// vh/voicing.hpp — every voice parameter as a CURVE of the shift ratio, not a constant.
//
// SEED, and the reason this file exists at all: `PreservationSpec::envelopeWarp = 0.8f`
// was a single number standing in for a relationship. It suited an octave and was wrong
// everywhere else, and because nothing read it, nobody found out. See VOICE-MODEL.md §6.
//
// THE FRAMING THIS IMPLEMENTS (VOICE-MODEL.md §1, "reading 3"):
// the output should be believably the SAME VOICE at a pitch that voice cannot reach.
// Physically INFORMED, not physically RESTRICTED. A real singer cannot move three
// octaves; this instrument must. So the curves below describe how properties vary across
// F0 in real voices, and then simply do not stop where a real voice would.
//
// WHY A POWER LAW. From the population relations in the design notes:
//
//     male -> female:  F0 x1.75,  formants x1.167   ->  k = ln(1.167)/ln(1.75) = 0.28
//     male -> child:   F0 x2.50,  formants x1.400   ->  k = ln(1.400)/ln(2.50) = 0.37
//
// so mu = ratio^k with k ~ 0.3. An octave down gives mu = 0.5^0.3 = 0.81, which is the
// mu ~ 0.8 that US Patent 6,304,846 gives for a believable octave-down — DERIVED rather
// than inherited, and now correct at every ratio instead of at one.
//
// THE PROPERTY THAT MAKES ME BELIEVE THE FORM IS RIGHT: its endpoints are the two engines
// this project already has.
//
//     k = 0    mu = 1        formants held            = plain PSOLA
//     k = 0.3  mu = r^0.3    formants follow a body   = the default
//     k = 1    mu = ratio    formants track pitch     = the granular engine
//
// The exponent is a single dial that interpolates continuously between them — not by
// blending two outputs, but as one parameter with both behaviours as limits.
//
// KNOWN LIMIT, stated so it is not discovered as a surprise: a power law is linear in
// log-log, so it captures the smooth trend across F0 and NOT register breaks. It is a
// first form. That is why this is a struct with named exponents rather than a formula
// inlined at the call site — a breakpoint table can replace `muFor` later without
// touching a single caller.

#pragma once

#include <algorithm>
#include <cmath>

namespace vh {

struct VoicingProfile {
    // Master switch. Off = every curve returns its identity, so the engines behave
    // exactly as they did before this file existed. Keep it, it is the A/B control.
    bool enabled = true;

    // --- B1: vocal tract length (the mu warp) -------------------------------------
    //
    // DERIVED, and the only number here with data behind it. See the header derivation.
    float kMu = 0.30f;

    // Taste multiplier on the exponent. 0 = hold formants (PSOLA), 1 = the derived
    // curve, 1/kMu = full resampling (granular). PROVISIONAL: this is the knob to sweep
    // by ear first, because it is the one that changes character rather than level.
    float muStrength = 1.0f;

    // Clamps. NOT a physical statement — a numerical guard, because mu also sets the
    // grain's output length and an unbounded mu would blow the overlap-add accumulator.
    // At kMu 0.3 these bind only past about four octaves.
    float muMin = 0.45f;
    float muMax = 2.20f;

    // --- A4: source spectral tilt (open-quotient correction) ----------------------
    //
    // WHY THIS EXISTS AT ALL, and it is the least obvious row in the model:
    //
    // A PSOLA grain is copied verbatim, so the EXCITATION keeps its ABSOLUTE duration
    // while the output period grows by 1/ratio. The open quotient — open phase over
    // period, which is roughly scale-free at 0.4-0.6 in real voices — therefore collapses
    // BY ratio. At -24 st a soprano's ~0.6 ms open phase sits inside a 20 ms period:
    // open quotient ~0.03. That is not a voice, it is an impulse train, and an impulse
    // train has energy at every multiple of its rate.
    //
    // That is BUGS.md VH-008's symptom ("sounds like a square wave") and VH-008's own
    // measurement: `duty = 2 * ratio` IS the open quotient collapsing. The ledger read it
    // as a gap-filling problem. It is a SOURCE problem, and the gap is a second, separate
    // cause (VOICE-MODEL.md C1).
    //
    // The excitation's spectral contribution is approximately a TILT with a corner near
    // 1/(open phase) ~ F0/OQ. Holding the open quotient means that corner should scale
    // with the target pitch, i.e. by `ratio`. So the correction is a first-order shelf
    // with high-frequency gain ratio^kTilt: darker on the way down, brighter on the way
    // up. It is an approximation of a source model, not a source model.
    //
    // kTilt = 1.0 follows from open quotient being scale-free. UNTESTED BY EAR.
    float kTilt = 1.0f;

    // 0 disables the tilt while leaving mu alone — needed to answer VOICE-MODEL.md §7 Q4
    // (how much of the buzz is open quotient versus truncated ring) by measuring one
    // correction at a time.
    float tiltStrength = 1.0f;

    // Shelf gain clamps, in linear amplitude. -20 dB / +12 dB.
    float tiltGainMin = 0.10f;
    float tiltGainMax = 4.00f;

    // Corner frequency as a multiple of the TARGET fundamental. 1/OQ with OQ ~ 0.5.
    // PROVISIONAL: a guess with a plausible derivation, which is not the same as a
    // measurement.
    float tiltCornerInF0 = 2.0f;

    // --- curves -------------------------------------------------------------------

    // Formant scale factor for a given pitch ratio. 1.0 means "hold the envelope".
    double muFor(double ratio) const noexcept {
        if (!enabled || ratio <= 1e-6) return 1.0;
        const double k = static_cast<double>(kMu) * static_cast<double>(muStrength);
        if (std::abs(k) < 1e-9) return 1.0;
        const double mu = std::pow(ratio, k);
        return std::clamp(mu, static_cast<double>(muMin), static_cast<double>(muMax));
    }

    // High-frequency shelf gain approximating the open-quotient correction.
    double tiltGainFor(double ratio) const noexcept {
        if (!enabled || ratio <= 1e-6) return 1.0;
        const double k = static_cast<double>(kTilt) * static_cast<double>(tiltStrength);
        if (std::abs(k) < 1e-9) return 1.0;
        return std::clamp(std::pow(ratio, k),
                          static_cast<double>(tiltGainMin),
                          static_cast<double>(tiltGainMax));
    }

    // Shelf corner in Hz, from the TARGET fundamental. Returns 0 when there is nothing
    // to anchor to, which the caller must read as "bypass" — an unvoiced frame has no
    // open quotient to correct and filtering it would only dull the fricative.
    double tiltCornerHz(double sourceF0Hz, double ratio) const noexcept {
        if (!enabled || sourceF0Hz <= 1.0 || ratio <= 1e-6) return 0.0;
        return sourceF0Hz * ratio * static_cast<double>(tiltCornerInF0);
    }
};

} // namespace vh
