// vh/granular_shifter.hpp — the fast path. Cheap, low latency, resamples.
//
// SEED: a read pointer traverses the input history at rate `ratio`. Because it moves at a
// different speed from the write head, it drifts, and periodically has to jump back into
// range. Two taps are crossfaded across the jump to hide the discontinuity. This is the
// classic hardware harmonizer (Eventide H910 lineage) and it is the only genuinely
// low-latency option.
//
// THE ONE IDEA THAT MAKES THIS SOUND ACCEPTABLE RATHER THAN METALLIC:
//
// Two taps reading at the same rate, offset by a constant delay difference, sum to a comb
// filter. Notches every 1/delta Hz, static and pitched, straight through the vowel range.
// The ear attributes a static comb to a filter, which is why naive implementations sound
// like a tube.
//
// The usual mitigations — more taps, jittered ramps — SMEAR the comb. Constraining the
// jump distance to an integer number of pitch periods instead makes the taps read
// phase-aligned copies of the waveform, so they sum constructively across the spectrum
// and THE COMB STOPS EXISTING. Not reduced: absent. This is the "synchronized" in SOLA
// and the "pitch-synchronous" in PSOLA, and it is why this shifter depends on the epoch
// tracker even though it never places a grain.
//
// WHAT THIS SHIFTER CANNOT DO, so nobody expects it to: it resamples, therefore it drags
// formants along with pitch. An octave up is a chipmunk. That is not a bug to be fixed
// here — it is the trade that buys the latency, and it is why there is a second engine.

#pragma once

#include "vh/shifter.hpp"

namespace vh {

struct GranularConfig {
    // Jump distance in pitch periods when voiced. PROVISIONAL: 1.
    // Larger jumps mean fewer crossfades (less smearing) but more latency, since the
    // delay must accommodate the jump.
    int jumpPeriods = 1;

    // Crossfade length as a fraction of a period. PROVISIONAL: 0.5.
    // Long fades hide the jump better and smear transients more.
    float fadeFraction = 0.5f;

    // Fallback grain size when there is no F0 to be synchronous with. PROVISIONAL: 8 ms.
    float unvoicedGrainMs = 8.0f;
};

class GranularShifter final : public IPitchShifter {
public:
    explicit GranularShifter(GranularConfig cfg = {}) : cfg_(cfg) {}

    void prepare(double sampleRate, FrameCount maxBlock) override;
    void reset() noexcept override;
    void process(const ShiftRequest& req) noexcept override;
    FrameCount latencySamples() const noexcept override { return baseDelay_; }
    const char* name() const noexcept override { return "granular"; }

private:
    // 4-point cubic Hermite. Linear interpolation is cheap and audibly bad here: it is a
    // lowpass whose cutoff moves with the fractional part of the read position, so a
    // sustained note acquires a shimmer at the drift rate. Cubic costs a few more
    // multiplies and removes it.
    static Sample interpolate(const AudioRing& ring, double pos) noexcept;

    // `elapsed` is the block length: this runs per block, so the smoothing coefficient
    // must be derived from block time, not sample time. See the .cpp.
    void updateGeometry(float periodSamples, double sampleRate, FrameCount elapsed) noexcept;

    GranularConfig cfg_{};
    double sampleRate_ = 48000.0;

    double pos_ = 0.0;        // primary read position, absolute, fractional
    double fadePos_ = 0.0;    // outgoing tap during a crossfade
    int fadeRemaining_ = 0;
    int fadeLength_ = 0;

    // The length the CURRENTLY RUNNING fade began with, which is not always fadeLength_.
    //
    // THE BUG THIS FIXES, found by instrumenting the shifter directly and worth recording
    // because the symptom is indistinguishable from a pitch-tracking fault:
    //
    // updateGeometry() recomputes fadeLength_ from the current period on every block,
    // including blocks in the middle of a fade. The gain is 1 - fadeRemaining_/fadeLength_,
    // so changing the denominator part-way through moves the ramp under the fade. If the
    // period rises the gain steps; if it falls, fadeLength_ drops below fadeRemaining_, the
    // argument goes NEGATIVE, and because the raised cosine is an even function the gain
    // turns around and heads back the way it came — the two taps re-cross and the envelope
    // stops being monotonic.
    //
    // MEASURED: at +12 st on the melody, an F0 estimate of 361 Hz on a 181 Hz voice halved
    // the period for a few hops; the block on which the geometry corrected itself carries
    // the largest discontinuity in the whole file (|dx| = 0.084 against a block peak of
    // 0.061 — a step bigger than the signal).
    //
    // This can only happen while the pitch is MOVING, which is exactly the condition
    // BUGS.md VH-002 records and exactly why the granular path showed clicks too despite
    // sharing no code with PSOLA. It is neither of VH-002's two candidates.
    //
    // A FIX THAT WAS TRIED AND WAS WORSE, recorded so it is not tried again: deferring
    // updateGeometry() until no fade is in flight. At ratio 2 the fade occupies half the
    // time between jumps, so block boundaries keep landing inside one and the update
    // starves for many blocks — then arrives all at once, which is a bigger step than the
    // one being avoided. The geometry must keep updating; it is the FADE that must be
    // insulated from it.
    int activeFadeLen_ = 1;

    FrameCount baseDelay_ = 0;
    FrameCount minDelay_ = 0;
    FrameCount maxDelay_ = 0;
    FrameCount jumpSize_ = 0;

    // Smoothed period driving the geometry. NOT the pitch — see updateGeometry().
    float geoPeriod_ = 0.0f;

    bool started_ = false;

    // Seeded from the cursor on the first block after a reset, so that N voices do not
    // share one random sequence. See granular_shifter.cpp.
    bool seeded_ = false;
    std::uint32_t rng_ = 22222u;
};

} // namespace vh
