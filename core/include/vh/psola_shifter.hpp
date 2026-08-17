// vh/psola_shifter.hpp — TD-PSOLA. Formant preservation, for free.
//
// SEED: cut the input into grains centred on glottal epochs, window them, and overlap-add
// them at a DIFFERENT spacing. Closer together = higher pitch. Further apart = lower.
//
// WHY THE FORMANTS SURVIVE, which is the whole reason this engine exists:
// each grain is an unmodified slice of the original waveform, so its spectral envelope is
// the original envelope. Changing the RATE at which grains are emitted changes the
// harmonic spacing — the pitch — while every grain still carries the vocal tract's
// response untouched. You are changing how often the glottis fires, not the shape of the
// tract it fires into. That is exactly the physical operation a singer performs, which is
// why it sounds like a voice rather than like tape.
//
// Contrast the granular shifter, which resamples and therefore scales the whole spectrum,
// formants included. Same input, same ratio, completely different failure mode.
//
// THE COST: grains must be placed on epochs, so this engine is only as good as the epoch
// tracker. Epoch errors are audible as GLITCHES, not as graceful degradation — a
// misplaced grain is a discontinuity, not a slightly wrong note. And it needs about two
// pitch periods of lookahead, which at a low male fundamental is ~18 ms against the
// granular shifter's ~13.
//
// KNOWN LIMIT, restated after measurement — the inherited "+/- 6 semitones" was not what
// this code does in either direction (BUGS.md VH-007). What actually bounds it:
//
//   UPWARD    tail cancellation between the many grains that overlap at high ratios.
//             Level, not pitch. VH-003.
//   DOWNWARD  two separate causes, often conflated. (1) The tract ring is truncated at
//             T_source because a grain cannot be wider than one period without dragging
//             in the next glottal pulse, so the deficit is worse at HIGH source pitch,
//             not at low ratio. (2) The open quotient collapses: the excitation keeps its
//             absolute duration while the period grows by 1/ratio, so the source becomes
//             ever more impulse-like. VH-008 measured (2) and read it as (1).
//
// Only (1) needs a source-filter method. (2) is approximated here by the voicing profile's
// tilt correction, and mu < 1 lengthens the grain and so partly mitigates (1) as well.
// See VOICE-MODEL.md.

#pragma once

#include "vh/shifter.hpp"

#include <vector>

namespace vh {

class PsolaShifter final : public IPitchShifter {
public:
    void prepare(double sampleRate, FrameCount maxBlock) override;
    void reset() noexcept override;
    void process(const ShiftRequest& req) noexcept override;

    // Two periods: one for the grain half-length that must exist ahead of the synthesis
    // instant, one for the epoch tracker's own working margin.
    FrameCount latencySamples() const noexcept override { return latency_; }

    // Audio thread. Non-allocating. Recomputes mixStep_ from the new handoverMs — a
    // prepare()-baked derived value, per app-design.md §4.1 — or the field is silently
    // INERT despite the number visibly changing in a UI. See the comment on handoverMs_
    // below for the rest of the reasoning (this is HANDOVER.md §8's single most-wanted
    // provisional constant, now actually turnable without a rebuild).
    void setTuning(const ShifterTuning& t) noexcept override;

    // Exposed for tests: proves setTuning() actually recomputed the derived mixStep_
    // rather than leaving it silently inert. Not used on the audio thread by anything
    // other than process() itself.
    double mixStepPerSample() const noexcept { return mixStep_; }

    const char* name() const noexcept override { return "psola"; }

private:
    static Sample readFrac(const AudioRing& ring, Pos anchor, double offset) noexcept;

    void placeGrain(const AudioRing& ring, Pos centre, Pos sourceEpoch,
                    FrameCount halfOut, double mu, float gain) noexcept;

    void applySourceTilt(const ShiftRequest& req, const AnalysisFrame& a,
                         double ratio, FrameCount n) noexcept;

    double sampleRate_ = 48000.0;

    // THE HANDOVER between the grain path and passthrough, as a continuous mix rather
    // than a branch. 1 = grains, 0 = unshifted source.
    //
    // WHY THIS IS NOT A BOOLEAN, which is what it was: at a voicing edge the two paths
    // carry completely different waveforms — one is overlap-added grains at the target
    // pitch, the other is the raw input — and switching between them writes a step into
    // the output. A step is broadband; it clicks. MEASURED: every isolated click in the
    // melody render, at every interval, landed within 12 ms of one of these switches
    // (`steady` was 0 at +3, +7 and +12 st). The voicing edge is not where clicks are
    // WORST, it is where they ALL are.
    //
    // A sung lyric crosses this boundary on every consonant, twice, which is why the
    // symptom reads as "k, t, b, p all click" rather than as a pitch-shifting fault.
    //
    // The mix is stepped linearly and then shaped by a raised cosine, so the applied gain
    // is C1 at both ends — the same requirement HANDOVER.md states for every other
    // crossfade in this codebase, and for the same reason: a fade with a kink in its
    // derivative clicks no matter how long you make it.
    double mix_ = 0.0;
    double mixStep_ = 0.0;        // per-sample, derived from handoverMs_ -- see setTuning()
    bool parked_ = true;          // mix_ == 0 and staying there: grain path is idle

    // PROVISIONAL: 8 ms to hand over between the grain path and passthrough.
    //
    // Chosen to match the Engine's note envelope, which has been long enough to remove a
    // click since the first milestone, and short enough that a plosive still reads as a
    // plosive. The trade is explicit: longer is smoother and smears the consonant, shorter
    // is tighter and eventually clicks again. The two paths carry the SAME event at
    // slightly different pitches, so the smear is a doubling rather than a blur — which is
    // why this can be as short as it is.
    //
    // TUNE BY EAR on /t/ and /k/ before anything else in this file. Was a compile-time-only
    // constant (kHandoverMs in psola_shifter.cpp); promoted to a live field per
    // app-design.md §5.7/§4.3 so setTuning() can change it without a rebuild. mixStep_ (the
    // derived per-sample step) is recomputed in setTuning() every time this changes — see
    // the .cpp — or the knob would be silently inert (app-design.md §4.1).
    double handoverMs_ = 8.0;

    // Passthrough is rendered every block, not only when it is needed, because the
    // starvation backstop cannot know it will fire until after grains have been placed.
    // It is a ring read of one block; making it conditional would buy nothing and would
    // add a state where the fallback has no material.
    std::vector<Sample> through_;

    // Overlap-add accumulator, addressed by absolute position modulo its size.
    //
    // WHY AN ACCUMULATOR AND NOT A PER-BLOCK BUFFER: grains straddle block boundaries by
    // design — a grain centred near the end of one block writes into the next. A
    // per-block buffer would truncate them, which is heard as a buzz at the block rate.
    // Absolute addressing makes block boundaries invisible to grain placement.
    std::vector<Sample> ola_;
    FrameCount olaMask_ = 0;

    double nextSynthPos_ = 0.0;   // next synthesis instant, absolute, fractional

    // Everything below this position has been zeroed exactly once since it was last
    // emitted.
    //
    // WHY AN EXPLICIT WATERMARK rather than zeroing each block as it is read: a grain
    // centred near the emit point writes BACKWARDS by a half-length, into audio already
    // sent to the output. Clearing on read therefore cannot be the whole story — the
    // accumulator must be zeroed AHEAD of where grains will land, not behind. Getting this
    // wrong discards the first half of every grain, which breaks the overlap-add sum and
    // produces a strong subharmonic: the output sits an octave below the requested pitch
    // while every individual grain looks correct in isolation.
    Pos clearedUpTo_ = 0;

    bool started_ = false;

    // Absolute position of the most recently placed grain's centre, and whether one has
    // ever been placed. Exists so the silence backstop can tell a LEGITIMATE gap between
    // glottal pulses from genuine starvation — after the VH-001 fix, large downward shifts
    // space grains further apart than a block, so blocks of true silence are correct
    // output. See the backstop in psola_shifter.cpp.
    Pos lastGrainPos_ = 0;
    bool haveGrain_ = false;

    FrameCount latency_ = 0;
    FrameCount maxHalf_ = 0;

    // Open-quotient correction state: a one-pole lowpass whose output is recombined with
    // the input to form a first-order shelf. Per SHIFTER, therefore per voice — two voices
    // at different intervals need different corners, and sharing this state would smear
    // one voice's filter across another's audio.
    //
    // tiltPrimed_ is deliberately cleared whenever the filter is bypassed (unvoiced,
    // backstop, no F0) so that re-entry snaps the coefficients to their target instead of
    // slewing from stale values that relate to audio from before the gap. Same reasoning
    // as Engine's shifter re-entry reset.
    double tiltLpT_ = 0.0;   // one-pole at the TARGET voice's excitation corner
    double tiltLpS_ = 0.0;   // one-pole at the SOURCE voice's excitation corner
    double tiltG_ = 1.0;
    double tiltKT_ = 0.0;
    double tiltKS_ = 0.0;
    bool tiltPrimed_ = false;

    // Hann window, precomputed at unit length and sampled by normalised position, so
    // changing grain size with F0 costs no recomputation.
    std::vector<float> window_;
};

} // namespace vh
