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

    void updateGeometry(float periodSamples, double sampleRate) noexcept;

    GranularConfig cfg_{};
    double sampleRate_ = 48000.0;

    double pos_ = 0.0;        // primary read position, absolute, fractional
    double fadePos_ = 0.0;    // outgoing tap during a crossfade
    int fadeRemaining_ = 0;
    int fadeLength_ = 0;

    FrameCount baseDelay_ = 0;
    FrameCount minDelay_ = 0;
    FrameCount maxDelay_ = 0;
    FrameCount jumpSize_ = 0;

    bool started_ = false;
    std::uint32_t rng_ = 22222u;
};

} // namespace vh
