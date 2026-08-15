#include "vh/psola_shifter.hpp"
#include "vh/rt.hpp"

#include <algorithm>
#include <cmath>

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

    // Grain half-length: at least one analysis period, so each grain carries a full cycle
    // and therefore the full formant structure; and at least the synthesis spacing, so
    // successive grains always overlap by 50% or more.
    //
    // WITHOUT the second condition a downward shift past about half-pitch spaces grains
    // further apart than the window is wide, and the output develops periodic holes —
    // heard as a rough buzz rather than a low voice.
    const FrameCount halfLen = static_cast<FrameCount>(
        std::min(std::max(period, synthSpacing), static_cast<double>(maxHalf_)));

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
            // Hann windows of half-length H at spacing S sum to H/S, so normalise by S/H.
            // Getting this wrong does not sound like a gain error — it sounds like the
            // shifter is louder at some intervals than others, which gets blamed on the
            // blend.
            const float gain = static_cast<float>(synthSpacing / static_cast<double>(halfLen));
            placeGrain(ring, centre, srcEpoch, halfLen, gain);
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
    // The honest test is whether the emitted audio is actually empty. If it is — the
    // source has fallen off the ring, the epochs are stale, the cursor lags too little —
    // then fall back to unshifted passthrough. A hole is far more audible and more
    // alarming than a moment at the wrong pitch, and it is indistinguishable from a crash.
    //
    // A backstop, not a design. If it fires often something upstream is wrong (see the
    // history gate in Engine::process), and the ratio being briefly ignored is the symptom
    // that should send you looking.
    float emitted = 0.0f;
    for (FrameCount i = 0; i < n; ++i) {
        const Sample v = ola_[(c + i) & olaMask_];
        req.out[i] = v;
        emitted += std::fabs(v);
    }
    if (emitted == 0.0f) ring.read(c, req.out, n);

    if (!cur.frozen) cur.next += n;
}

} // namespace vh
