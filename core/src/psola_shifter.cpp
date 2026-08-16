#include "vh/psola_shifter.hpp"
#include "vh/rt.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace vh {

namespace {
// Longest period we ever handle: the 70 Hz lower bound of the tracker.
constexpr double kLowestF0 = 70.0;
constexpr int kWindowTableSize = 2048;
} // namespace

void PsolaShifter::prepare(double sampleRate, FrameCount maxBlock) {
    sampleRate_ = sampleRate;
    maxHalf_ = static_cast<FrameCount>(sampleRate / kLowestF0) + 1;

    // Must hold a whole block plus grains reaching a half-length either side, with room
    // to spare so the modulo wrap never collides with data still owed to the output.
    // 10x rather than 6x: mu < 1 stretches a grain to halfSrc/mu, which the clamp in
    // process() caps at 2 * maxHalf_. The accumulator must hold a block plus grains
    // reaching that far either side, with room so the modulo wrap never collides with
    // data still owed to the output.
    FrameCount need = maxBlock + 10 * maxHalf_;
    FrameCount cap = 1;
    while (cap < need) cap <<= 1;
    ola_.assign(cap, Sample{0});
    olaMask_ = cap - 1;

    window_.resize(kWindowTableSize);
    for (int i = 0; i < kWindowTableSize; ++i) {
        const double t = static_cast<double>(i) / (kWindowTableSize - 1);
        window_[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * M_PI * t));
    }

    latency_ = 2 * maxHalf_;
    reset();
}

void PsolaShifter::reset() noexcept {
    std::fill(ola_.begin(), ola_.end(), Sample{0});
    nextSynthPos_ = 0.0;
    clearedUpTo_ = 0;
    started_ = false;
    lastGrainPos_ = 0;
    haveGrain_ = false;
}

// Read the ring at a FRACTIONAL offset from an anchor position.
//
// WHY THE OFFSET IS SEPARATE FROM THE ANCHOR: Pos is unsigned and the offset is signed.
// Building one double from `anchor + offset` and casting it back is exactly the undefined
// conversion that produced the `inf` bug in the granular shifter (RESULTS.md #5). Keeping
// the anchor an integer and the offset a signed double means the only cast is on a small
// number, and the wrap on `anchor + (Pos)i` is defined unsigned arithmetic.
Sample PsolaShifter::readFrac(const AudioRing& ring, Pos anchor, double offset) noexcept {
    const double fl = std::floor(offset);
    const Pos base = anchor + static_cast<Pos>(static_cast<std::int64_t>(fl));
    const float t = static_cast<float>(offset - fl);

    const float xm1 = ring.at(base - 1), x0 = ring.at(base);
    const float x1 = ring.at(base + 1), x2 = ring.at(base + 2);

    const float c0 = x0;
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + c0;
}

// Place one grain, resampling its CONTENT by mu on the way.
//
// THIS IS THE FORMANT KNOB, and it is the whole of it. See VOICE-MODEL.md §5.
//
// PSOLA sets PITCH by WHEN grains are emitted. Grain CONTENT was copied verbatim, but
// nothing ever required that. Reading the source at `k * mu` for an output offset k means
// the grain's own waveform is scaled in time by 1/mu, so every frequency inside it — the
// formants — scales by mu, while the pitch stays exactly where the synthesis spacing put
// it. Independent formant control, in the time domain, with no FFT and no filter model.
//
// AND IT PARTLY FILLS THE VH-008 GAP, not by luck. mu < 1 stretches the grain by 1/mu, so
// duty = 2 * ratio / mu. A longer vocal tract genuinely has lower formants AND narrower
// bandwidths AND therefore a longer ring; the thing we would otherwise be faking is the
// thing a bigger body actually does.
//
// THE HONEST LIMIT: this scales the WHOLE grain, so it also stretches the glottal pulse
// inside it, which is a change to the source character that VOICE-MODEL.md reading 3 says
// to hold. Source and filter cannot be separated by resampling. That is what an LPC
// residual/filter split would buy, and it is the sharpest argument for building one.
void PsolaShifter::placeGrain(const AudioRing& ring, Pos centre, Pos sourceEpoch,
                              FrameCount halfOut, double mu, float gain) noexcept {
    const int len = static_cast<int>(halfOut);
    if (len <= 1) return;

    // mu == 1 is the identity and must stay BIT-IDENTICAL to the pre-profile engine, so
    // that VoicingProfile::enabled = false is a real A/B control and not "almost the same
    // plus interpolator noise".
    const bool unity = std::abs(mu - 1.0) < 1e-12;

    for (int k = -len; k <= len; ++k) {
        // Normalised window position in [0,1].
        const float wt = (static_cast<float>(k + len) / static_cast<float>(2 * len));
        const int wi = static_cast<int>(wt * (kWindowTableSize - 1));
        const float w = window_[static_cast<size_t>(std::clamp(wi, 0, kWindowTableSize - 1))];

        const Sample s = unity
            ? ring.at(sourceEpoch + static_cast<Pos>(static_cast<std::int64_t>(k)))
            : readFrac(ring, sourceEpoch, static_cast<double>(k) * mu);

        ola_[(centre + static_cast<Pos>(static_cast<std::int64_t>(k))) & olaMask_] +=
            s * w * gain;
    }
}

void PsolaShifter::process(const ShiftRequest& req) noexcept {
    VH_RT_SECTION();

    const AudioRing& ring = *req.ring;
    const AnalysisFrame& a = *req.analysis;
    ReadCursor& cur = *req.cursor;
    const Pos c = cur.next;
    const FrameCount n = req.numFrames;

    const double period = static_cast<double>(a.periodSamples);
    const double ratio = req.ratio > 1e-6 ? req.ratio : 1e-6;
    const double synthSpacing = period / ratio;

    // Grain half-length: ONE analysis period, so each grain carries exactly one glottal
    // pulse plus the taper either side of it. That is the whole mechanism — PSOLA lowers
    // pitch by emitting one pulse LESS OFTEN, and raises it by emitting one pulse MORE
    // often. The grain must therefore hold one pulse, never several.
    //
    // THIS LINE PREVIOUSLY READ min(max(period, synthSpacing), maxHalf_) — see BUGS.md
    // VH-001. On a downward shift synthSpacing = period / ratio > period, so halfLen grew
    // with the spacing and each grain spanned 2/ratio periods OF THE ORIGINAL WAVEFORM: at
    // ratio 0.25, eight glottal pulses in one grain. A grain carrying eight pulses already
    // contains the source's periodicity, so re-spacing such grains cannot change the pitch
    // — overlapping them merely reconstructs the original. Measured output pitch was
    // EXACTLY the input pitch at ratios 1/2, 1/3 and 1/4, and erratic between them.
    //
    // The reason the old line existed was real but misdiagnosed: at large downward ratios
    // grains no longer overlap and the output develops periodic holes. THOSE HOLES ARE NOT
    // A BUG. The gap between glottal pulses at a lower rate is what a lower-pitched voice
    // IS. The old code suppressed the symptom and destroyed the function with it.
    //
    // The honest cost of the correct geometry is that large downward shifts get rougher as
    // the inter-pulse gap widens, because a single copied pulse cannot fill it. That is a
    // real limit of TD-PSOLA and one of the reasons a source-filter engine is on the
    // roadmap; it is not something to fix by widening the grain.
    const FrameCount halfSrc = static_cast<FrameCount>(
        std::min(period, static_cast<double>(maxHalf_)));

    // THE FORMANT CURVE. mu = ratio^k, evaluated per block from the voicing profile — not
    // a constant, because real voices do not vary linearly with F0 and a fixed number is
    // the wrong shape for the answer. k = 0 gives mu = 1 and this engine behaves exactly
    // as it did before the profile existed; k = 1 gives mu = ratio, which is the granular
    // engine. See vh/voicing.hpp for the derivation and VOICE-MODEL.md §6 for why.
    const double mu = req.preservation->voicing.muFor(ratio);

    // Output half-length. The grain reads halfOut * mu == halfSrc samples of SOURCE either
    // side of the epoch regardless of mu, so the ring lookahead in latencySamples() stays
    // correct and only our own accumulator grows — which prepare() sized for.
    const FrameCount halfOut = static_cast<FrameCount>(std::clamp(
        static_cast<double>(halfSrc) / (mu > 1e-6 ? mu : 1e-6),
        1.0, static_cast<double>(2 * maxHalf_)));

    // Zero forward to `upTo`, so grains always land in clean accumulator.
    auto clearTo = [&](Pos upTo) noexcept {
        // If we have fallen absurdly far behind (a long pass-through section, a reset),
        // do not walk the whole gap; jump.
        if (clearedUpTo_ + static_cast<Pos>(ola_.size()) < upTo) clearedUpTo_ = upTo - static_cast<Pos>(ola_.size());
        while (clearedUpTo_ < upTo) { ola_[clearedUpTo_ & olaMask_] = 0.0f; ++clearedUpTo_; }
    };

    const bool passThrough =
        (a.voicing == Voicing::Unvoiced && req.preservation->unvoiced != UnvoicedPolicy::Shift) ||
        a.periodSamples < 8.0f || a.epochCount == 0;

    if (passThrough) {
        // Unvoiced, or nothing to be pitch-synchronous with. Pass the audio through and
        // restart the synthesis clock so voicing resumes in phase with the output rather
        // than wherever it drifted to during the gap.
        ring.read(c, req.out, n);
        clearTo(c + n);
        nextSynthPos_ = static_cast<double>(c + n);
        started_ = true;
        if (!cur.frozen) cur.next += n;
        return;
    }

    if (!started_) {
        // Start the synthesis clock a half-grain BEHIND the emit point, so the very first
        // output samples already have grain tails overlapping them.
        nextSynthPos_ = static_cast<double>(c > halfOut ? c - halfOut : 0);
        clearedUpTo_ = static_cast<Pos>(nextSynthPos_);
        started_ = true;
    }

    // Never let the clock lag so far that grains would land in already-emitted output.
    if (nextSynthPos_ + static_cast<double>(halfOut) < static_cast<double>(c)) {
        nextSynthPos_ = static_cast<double>(c > halfOut ? c - halfOut : 0);
    }

    // Place every grain that can touch the block we are about to emit. Its centre may be
    // up to a half-length PAST the end of the block, because a grain's leading edge
    // reaches backwards.
    const double horizon = static_cast<double>(c + n + halfOut);
    while (nextSynthPos_ < horizon) {
        const Pos centre = static_cast<Pos>(nextSynthPos_);
        clearTo(centre + halfOut + 1);

        // The heart of PSOLA: for this synthesis instant, find the analysis epoch nearest
        // in time and copy ITS waveform. Timing is preserved (the grain lands where the
        // synthesis clock says); pitch changes (the clock ticks at a different rate); the
        // spectral envelope rides along untouched inside the grain.
        const Pos srcEpoch = a.nearestEpoch(centre);
        // Residency is a SOURCE-side question: the grain reads +/- halfSrc of input
        // regardless of mu, because halfOut * mu == halfSrc. Checking halfOut here
        // would refuse perfectly available audio at mu < 1. The +/- 2 margin is the
        // cubic interpolator's neighbourhood.
        if (srcEpoch > halfSrc + 2 && ring.contains(srcEpoch - halfSrc - 2, halfSrc * 2 + 5)) {
            // Overlap-add normalisation — DERIVED, see BUGS.md VH-003. The answer is 1.0,
            // and the derivation is worth keeping because the wrong answer looked right.
            //
            // The old line was min(S/H, 1.0), from "Hann grains of half-length H at
            // spacing S sum to H/S, so correct by S/H". That reasoning is about the
            // WINDOW ENVELOPE and PSOLA is not an envelope problem.
            //
            // What actually has to be preserved is the amplitude of each glottal PULSE.
            // A grain is centred on its epoch, so the pulse sits at the window's peak,
            // w(0) = 1. Neighbouring grains contribute their TAILS at that instant, which
            // are ring, not pulse, and small. So the pulse peak is already correct at
            // unity and any S-dependent scaling makes it wrong.
            //
            // Downward (S > H) the clamp already gave 1.0, which is why downward level
            // was fine. Upward S = period/ratio < H = period, so the old expression
            // returned 1/ratio and quietly attenuated every upward shift in proportion to
            // the shift: -3.5 dB at +7 st, -10.5 dB at +21 st. Measured loss was larger
            // still (-6.1 and -17.3), the remainder being genuine partial cancellation
            // between the tails of the many grains that overlap at high ratios. That part
            // is a property of the method, not of this line, and is not fixed here.
            //
            // PHYSICALLY this is also the right answer: raising pitch means firing the
            // glottis more often at the same strength, so more energy per second is
            // correct rather than something to normalise away.
            //
            // WATCH FOR: upward shifts are now louder than they were by up to ~17 dB.
            // Peaks are measured in RESULTS.md; if a future change makes them clip, the
            // fix is headroom in the mixer, NOT a scale factor back in here.
            constexpr float gain = 1.0f;
            placeGrain(ring, centre, srcEpoch, halfOut, mu, gain);
            lastGrainPos_ = centre;
            haveGrain_ = true;
        }
        nextSynthPos_ += synthSpacing;
    }

    clearTo(c + n);   // guarantee the emit span has been through the zeroing pass

    // SILENCE IS NEVER AN ACCEPTABLE OUTPUT — but detect it correctly.
    //
    // THE FIRST VERSION OF THIS BACKSTOP TESTED "were any grains placed this block?" and
    // was WRONG, in a way the test suite caught immediately and which is worth recording:
    // for a downward shift the synthesis spacing EXCEEDS a block (an octave down at 150 Hz
    // spaces grains 640 samples apart, against a 128-sample block), so most blocks
    // legitimately place no grains at all and emit the tails of grains placed earlier.
    // Zero grains is healthy operation, not starvation.
    //
    // The honest test WAS whether the emitted audio is actually empty. THAT IS NOW ALSO
    // WRONG, and the VH-001 fix is what made it wrong — the same mistake one level down.
    //
    // With grains correctly sized at one analysis period, a large downward shift spaces
    // them much further apart than the grain is wide: three octaves down on a 242 Hz voice
    // places a 396-sample grain every 1584 samples, leaving 1188 samples — NINE consecutive
    // 128-sample blocks — of genuine, correct silence between glottal pulses. That silence
    // IS the lower pitch. Treating it as starvation and substituting unshifted passthrough
    // filled every gap with the source, and the output came back at the SOURCE PITCH: the
    // exact VH-001 symptom, produced by a completely different line.
    //
    // So the test is not "is this block empty" but "has it been too long since any grain
    // landed at all". Two synthesis spacings is the threshold: one spacing is the normal
    // gap, two means the grain that should have arrived did not. Genuine starvation — the
    // source has fallen off the ring, the epochs are stale, the cursor lags too little —
    // still trips it, because in that state no grain lands for an unbounded time.
    //
    // A backstop, not a design. If it fires often something upstream is wrong (see the
    // history gate in Engine::process), and the ratio being briefly ignored is the symptom
    // that should send you looking.
    for (FrameCount i = 0; i < n; ++i) req.out[i] = ola_[(c + i) & olaMask_];

    const double sinceGrain = haveGrain_
        ? static_cast<double>(c + n) - static_cast<double>(lastGrainPos_)
        : std::numeric_limits<double>::infinity();
    if (sinceGrain > 2.0 * synthSpacing + static_cast<double>(halfOut)) {
        ring.read(c, req.out, n);
    }

    if (!cur.frozen) cur.next += n;
}

} // namespace vh
