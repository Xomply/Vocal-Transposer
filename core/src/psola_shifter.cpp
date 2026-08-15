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
    FrameCount need = maxBlock + 6 * maxHalf_;
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

void PsolaShifter::placeGrain(const AudioRing& ring, Pos centre, Pos sourceEpoch,
                              FrameCount halfLen, float gain) noexcept {
    const int len = static_cast<int>(halfLen);
    if (len <= 1) return;

    for (int k = -len; k <= len; ++k) {
        // Normalised window position in [0,1].
        const float wt = (static_cast<float>(k + len) / static_cast<float>(2 * len));
        const int wi = static_cast<int>(wt * (kWindowTableSize - 1));
        const float w = window_[static_cast<size_t>(std::clamp(wi, 0, kWindowTableSize - 1))];

        const Pos src = sourceEpoch + static_cast<Pos>(static_cast<std::int64_t>(k));
        ola_[(centre + static_cast<Pos>(static_cast<std::int64_t>(k))) & olaMask_] +=
            ring.at(src) * w * gain;
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
    const FrameCount halfLen = static_cast<FrameCount>(
        std::min(period, static_cast<double>(maxHalf_)));

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
        nextSynthPos_ = static_cast<double>(c > halfLen ? c - halfLen : 0);
        clearedUpTo_ = static_cast<Pos>(nextSynthPos_);
        started_ = true;
    }

    // Never let the clock lag so far that grains would land in already-emitted output.
    if (nextSynthPos_ + static_cast<double>(halfLen) < static_cast<double>(c)) {
        nextSynthPos_ = static_cast<double>(c > halfLen ? c - halfLen : 0);
    }

    // Place every grain that can touch the block we are about to emit. Its centre may be
    // up to a half-length PAST the end of the block, because a grain's leading edge
    // reaches backwards.
    const double horizon = static_cast<double>(c + n + halfLen);
    while (nextSynthPos_ < horizon) {
        const Pos centre = static_cast<Pos>(nextSynthPos_);
        clearTo(centre + halfLen + 1);

        // The heart of PSOLA: for this synthesis instant, find the analysis epoch nearest
        // in time and copy ITS waveform. Timing is preserved (the grain lands where the
        // synthesis clock says); pitch changes (the clock ticks at a different rate); the
        // spectral envelope rides along untouched inside the grain.
        const Pos srcEpoch = a.nearestEpoch(centre);
        if (srcEpoch > halfLen && ring.contains(srcEpoch - halfLen, halfLen * 2 + 1)) {
            // Overlap-add normalisation. Hann grains of half-length H laid down at spacing
            // S sum to H/S *while they still overlap*, so the correction is S/H.
            //
            // THE CLAMP IS LOAD-BEARING AND IS PART OF THE VH-001 FIX. S/H is only valid
            // for S <= 2H. Beyond that the grains no longer touch at all: each one stands
            // alone and already peaks at unity, so scaling it by S/H (which reaches 4.0 at
            // two octaves down and 8.0 at three) simply makes it that many times too loud.
            // Before the clamp, -24 st and below hit full scale and clipped.
            //
            // Between S = H and S = 2H the true sum ripples below unity, so clamping there
            // leaves the output slightly quiet rather than slightly loud. That is the right
            // direction to err: quiet is a mix decision, clipped is destroyed samples.
            //
            // Getting this wrong does not sound like a gain error — it sounds like the
            // shifter is louder at some intervals than others, which gets blamed on the
            // blend.
            const float gain = static_cast<float>(
                std::min(synthSpacing / static_cast<double>(halfLen), 1.0));
            placeGrain(ring, centre, srcEpoch, halfLen, gain);
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
    if (sinceGrain > 2.0 * synthSpacing + static_cast<double>(halfLen)) {
        ring.read(c, req.out, n);
    }

    if (!cur.frozen) cur.next += n;
}

} // namespace vh
