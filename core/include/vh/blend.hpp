// vh/blend.hpp — how the fast and quality engines hand over. THE dial of this project.
//
// SEED: Bloomberg's rig for Collier blended four Antares instances (better low notes,
// 40-70 ms) with a TC-Helicon unit (better high notes, 10-20 ms), run in parallel
// specifically to give the impression of lower latency. That split was by REGISTER.
//
// THE REQUIREMENT THAT SHAPES THIS FILE, stated by the user and load-bearing:
//   "the system of splitting into two engines, fast-but-bad and slow-but-good, is a good
//    idea regardless of where the split lies. The important factor is that the exact
//    waveform / behaviour of how the split occurs must be tweakable after creating the
//    whole system."
//
// Two consequences, both CERTAIN:
//
//   1. The split must NOT be a binary switch. It is a continuous crossfade whose shape is
//      the thing being tuned. "Waveform" is the user's word and it is the right one.
//
//   2. The split must not assume a playing style. Collier's register split was safe
//      because his set was known. Ours is not — the stated requirement is explicitly that
//      the system not rely on assumptions about how it is played. So register is ONE
//      available axis among several, not the design.
//
// Available axes, all exposed: register (target pitch), time since note-on, shift
// magnitude, voicing state, analysis confidence, per-voice manual override.
//
// WHY A POLICY INTERFACE RATHER THAN A PARAMETER STRUCT: we do not yet know which axis
// matters, and finding out is the point. A struct of gains would force us to guess the
// functional form now. An interface lets a new policy be written in twenty lines and
// A/B'd without touching the engines, the voices, or the mixer.

#pragma once

#include "vh/analysis.hpp"

#include <cmath>
#include "vh/types.hpp"
#include "vh/voice.hpp"

namespace vh {

// Everything a policy is allowed to know. Deliberately a flat value type: a policy that
// reaches into engine internals cannot be swapped, which defeats the purpose.
struct BlendContext {
    double ratio = 1.0;             // shift ratio being applied
    float sourceF0Hz = 0.0f;        // what the singer is doing
    float targetF0Hz = 0.0f;        // what comes out — the "register" axis
    float shiftSemitones = 0.0f;    // signed magnitude of the shift
    float samplesSinceNoteOn = 0.0f;
    Voicing voicing = Voicing::Unvoiced;
    float confidence = 0.0f;
    double sampleRate = 48000.0;
};

struct BlendWeights {
    float fast = 1.0f;
    float quality = 0.0f;

    // WHY EQUAL-GAIN NORMALISATION AND NOT EQUAL-POWER:
    // equal-power (sin/cos) crossfades assume uncorrelated sources and give a +3 dB bump
    // mid-fade on correlated ones — heard as amplitude pumping at the crossfade rate, and
    // a good chunk of the perceived warble in naive implementations. Our two engines are
    // processing the SAME voice with the SAME ratio, so their outputs are strongly
    // correlated. Equal-gain (summing to 1) is correct here.
    // IF the engines ever diverge enough to decorrelate, revisit — but measure the
    // correlation first rather than assuming.
    void normalize() noexcept {
        const float s = fast + quality;
        if (s > 1e-9f) { fast /= s; quality /= s; }
        else { fast = 1.0f; quality = 0.0f; }
    }
};

class IBlendPolicy {
public:
    virtual ~IBlendPolicy() = default;
    virtual BlendWeights weightsFor(const BlendContext& ctx) const noexcept = 0;
    virtual const char* name() const noexcept = 0;
};

// --- Concrete policies. Each is small on purpose; adding one should be trivial. ---

// Baseline for phase 2 and for A/B reference. Also the correct fallback if the quality
// engine ever fails to meet its deadline.
class FastOnlyPolicy final : public IBlendPolicy {
public:
    BlendWeights weightsFor(const BlendContext&) const noexcept override { return {1.0f, 0.0f}; }
    const char* name() const noexcept override { return "fast-only"; }
};

class QualityOnlyPolicy final : public IBlendPolicy {
public:
    BlendWeights weightsFor(const BlendContext&) const noexcept override { return {0.0f, 1.0f}; }
    const char* name() const noexcept override { return "quality-only"; }
};

// Collier's split, reproduced so it can be measured rather than admired.
// Fast engine takes the high voices, quality engine the low ones.
//
// PROVISIONAL in every parameter. The crossover and width are guesses; the ORDERING
// (fast=high, quality=low) is the part with evidence behind it, and even that is evidence
// about a specific pair of commercial units, not about our engines.
class RegisterSplitPolicy final : public IBlendPolicy {
public:
    float crossoverHz = 220.0f;
    float widthOctaves = 1.0f;

    BlendWeights weightsFor(const BlendContext& ctx) const noexcept override {
        const float f = ctx.targetF0Hz > 1.0f ? ctx.targetF0Hz : 1.0f;
        const float octavesAbove = log2f(f / crossoverHz) / (widthOctaves > 1e-6f ? widthOctaves : 1e-6f);
        BlendWeights w;
        w.fast = smoothstep01(octavesAbove * 0.5f + 0.5f);
        w.quality = 1.0f - w.fast;
        return w;
    }
    const char* name() const noexcept override { return "register-split"; }

private:
    // C1-continuous. Zero slope at both ends.
    //
    // WHY THE CURVE SHAPE MATTERS AND IS NOT A DETAIL: the lesson from Shepard tones is
    // that partials can be born and die inaudibly if the envelope has zero slope at birth
    // and death. A crossfade with a discontinuous derivative is audible as a click or a
    // seam no matter how long you make it. Every crossfade in this codebase should be at
    // least C1.
    static float smoothstep01(float x) noexcept {
        if (x <= 0.0f) return 0.0f;
        if (x >= 1.0f) return 1.0f;
        return x * x * (3.0f - 2.0f * x);
    }
};

// The latency-masking split: the fast engine covers the attack, the quality engine takes
// over once it has arrived. This is the policy that most directly implements "hear the
// note when you press the key, hear the good sound a moment later".
//
// PROVISIONAL in its parameters, and the ones most worth tuning by ear first.
class OnsetHandoverPolicy final : public IBlendPolicy {
public:
    float handoverStartMs = 25.0f;   // quality engine starts to appear
    float handoverEndMs   = 70.0f;   // fast engine fully gone

    BlendWeights weightsFor(const BlendContext& ctx) const noexcept override {
        const float ms = static_cast<float>(ctx.samplesSinceNoteOn / ctx.sampleRate * 1000.0);
        const float span = handoverEndMs - handoverStartMs;
        const float x = span > 1e-6f ? (ms - handoverStartMs) / span : 1.0f;
        BlendWeights w;
        w.quality = smoothstep01(x);
        w.fast = 1.0f - w.quality;
        return w;
    }
    const char* name() const noexcept override { return "onset-handover"; }

private:
    static float smoothstep01(float x) noexcept {
        if (x <= 0.0f) return 0.0f;
        if (x >= 1.0f) return 1.0f;
        return x * x * (3.0f - 2.0f * x);
    }
};

} // namespace vh
