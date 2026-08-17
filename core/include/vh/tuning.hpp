// vh/tuning.hpp — every LIVE engine parameter, gathered into one POD.
//
// SEED: app-design.md §4.1 works out a fact that is not intuitive — almost nothing in
// this engine sizes a buffer, so almost everything is safe to change while sound is
// coming out. The restart-only exceptions (sampleRate, maxBlock, YIN's minHz/maxHz,
// PSOLA's kLowestF0/kWindowTableSize, kMaxVoices, kInputHistoryFrames) stay OUT of this
// struct on purpose — see the DELIBERATELY ABSENT notes below. Everything else that
// HANDOVER.md §8 calls "provisional, meant to be tuned by ear" belongs here, because a
// provisional constant nobody can turn into a live knob never actually gets tuned.
//
// THE CENTRAL PROPERTY THIS FILE MUST HAVE, and the one the test file exists to pin:
// a default-constructed Tuning{} reproduces the Engine's behaviour EXACTLY as it stands
// today, before this file existed. Every default below was copied from the live constant
// it replaces, not invented and not rounded — see the file:line citation on each one.
// That is what makes "apply a default Tuning" provably not an audio change, which is what
// keeps RESULTS.md's measurements and click_sweep.py's regression grid valid once this
// struct is actually wired into Engine (a later track — this file only adds the struct).
//
// CONSEQUENTLY, humanization ships OFF. Every field in HumanizationSpec defaults to zero,
// even though app-design.md §5.2 and §11 both say the eventual musical default should be
// "a few cents of detune and 15-25 cents of vibrato" — modest, not zero. That nicer-
// sounding default belongs in a factory PROFILE (app-design.md §7), applied on top of
// Tuning{}, not in the struct default. If it were the struct default, "load a Tuning and
// render" would no longer be a no-op against today's engine, and the entire evidence base
// this project stands on (HANDOVER.md §9: "a measured claim... a line in RESULTS.md...
// citing a number that came from running the tools in the patch") would need
// re-baselining for a change nobody asked to measure yet. Defaults are the null hypothesis;
// taste is a profile.
//
// FOR THE SAME REASON, every new signal-path field defaults to unity/bypass: dryGain 0
// (today's Engine emits no dry signal at all), wetGain 1 (today's Engine is 100% wet),
// limiterOn false (no limiter exists today), inputTrimDb/inputGateOn/outCeilingDb all
// no-ops. None of these fields change what Engine::process emits today, because none of
// them are read by Engine::process today — but the day they ARE wired in (app-design.md
// Track D), the default must still be "no change", not "the new feature turned on".
//
// CORE STAYS DEPENDENCY-FREE: standard library only. This header does not know that
// files, JSON, or a UI exist — see CMakeLists.txt's rationale for why that is the load-
// bearing build decision it is. Serialization is `vh_profile`'s job (app-design.md §7),
// entirely outside `core`.

#pragma once

#include "vh/analysis.hpp"    // AnalyzerTuning, IAnalyzer::setTuning
#include "vh/preservation.hpp"
#include "vh/shifter.hpp"     // ShifterTuning (granular + psola), IPitchShifter::setTuning
#include "vh/types.hpp"
#include "vh/voice.hpp"       // RatioSource — the mode switch Mode A / Mode B is built on

#include <algorithm>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace vh {

// Bumped whenever a field is added, removed, renamed, reordered, or reinterpreted.
//
// HOW A LOADER USES THIS (vh_profile, app-design.md §7 — not this file; core does not
// know profiles exist): read this field FIRST, before touching anything else in the
// blob. A mismatch means the bytes that follow may not mean what their offsets suggest —
// Tuning is a POD, so an old-schema blob loaded against a newer layout does not fail to
// parse, it silently reinterprets a float as part of an enum or a shifted struct. The
// loader's job is to notice the version differs and either migrate field-by-field or
// refuse the file outright; Tuning itself never migrates anything — it is the payload,
// not the loader.
inline constexpr std::uint32_t kTuningSchemaVersion = 2;   // bumped: Track C (app-design.md
                                                            // §5) added velocityToGainMin
                                                            // and velocityCurve.

// A sanity ceiling on linear gain fields, shared so dryGain/wetGain read the same bound
// and a reader does not have to wonder whether they were meant to differ. +12 dB is
// generous headroom for a mix decision and still catches a JSON typo (a 10 for a 1.0)
// before it becomes a burst of full-scale output. Not physically load-bearing the way the
// hazards in clamp() are — see the clamp() comments below for the ones that actually
// prevent a hang or a corrupted buffer read.
inline constexpr float kTuningMaxLinearGain = 4.0f;   // +12 dBFS

// --- blend --------------------------------------------------------------------------
//
// One enum value per concrete IBlendPolicy (blend.hpp). app-design.md §4.4: Engine will
// own one instance of each and select by this enum, rather than refcounting a runtime
// policy object on the audio thread. `setBlendPolicy()` remains as an override for
// experimental policies from tools/tests; a custom policy set that way takes precedence
// over this enum. That wiring is a later track — this is only the selector value.
enum class BlendKind : std::uint8_t {
    FastOnly,       // FastOnlyPolicy — DEFAULT. Matches Engine's own current default:
                     // Engine::Engine() sets blend_ = &defaultBlend_, and defaultBlend_ is
                     // a FastOnlyPolicy (engine.cpp:17, engine.hpp:158) until someone calls
                     // setBlendPolicy(). This is the value that makes a default Tuning
                     // reproduce TODAY'S engine rather than some other engine.
    QualityOnly,    // QualityOnlyPolicy
    RegisterSplit,  // RegisterSplitPolicy — Collier's split, reproduced to be measured.
    OnsetHandover,  // OnsetHandoverPolicy — masks latency by covering the attack.
};

// --- ensemble: per-voice SPREAD, not per-voice values ---------------------------------
//
// app-design.md §5.2: per-voice Humanization values (voice.hpp) are derived at noteOn
// from the voice INDEX, deterministically — the same choice already made for the
// decorrelation delay at engine.cpp:164, and for the same reason: a given chord must
// humanize identically on every render, or a before/after comparison is comparing two
// different performances instead of two engines. HumanizationSpec is therefore the WIDTH
// of the ensemble, not any one voice's number; the derivation from index to per-voice
// value is Engine's job (a later track), not this struct's.
//
// vibratoPhase has NO field here on purpose: voice.hpp:56 is explicit that randomising
// phase per voice — never sharing it — is "the entire point", so there is nothing to make
// tunable. It is derived, not configured.
struct HumanizationSpec {
    // Ensemble width of the static per-voice detune, in cents. Each voice's own offset is
    // derived from its index within roughly +-detuneCents/2 — see Humanization::detuneCents.
    float detuneCents = 0.0f;

    // Amplitude of each voice's independent slow random walk (Humanization::detuneDriftCents).
    // Every voice runs its OWN walk, seeded from its index, so two voices sharing this same
    // depth still drift apart — the decorrelation comes from the per-voice seed, not from
    // this number differing per voice.
    float detuneDriftCents = 0.0f;

    // Entry-timing spread, in ms (Humanization::onsetJitterMs). noteOn records
    // startAtPos = noteOnPos + jitter, jitter derived per voice within [0, onsetJitterMs].
    float onsetJitterMs = 0.0f;

    // Per-voice LFO. Rate and depth are shared by every voice on purpose — a chorus of
    // different vibrato RATES beats against a held chord rather than reading as one
    // ensemble breathing together; PHASE (randomised, not configurable — see above) is
    // what decorrelates the voices, not the rate.
    float vibratoRateHz = 0.0f;
    float vibratoDepthCents = 0.0f;

    // Ensemble width of the per-voice vocal-tract difference (Humanization::
    // envelopeWarpOffset, applied as ShiftRequest::muScale after muFor() per app-design.md
    // §5.2). Dimensionless, symmetric around zero — a small offset on mu, not a warp ratio
    // in its own right.
    float envelopeWarpSpread = 0.0f;

    // 0..1: how far per-voice pan positions spread across the stereo field (app-design.md
    // §5.3), 0 = every voice centred (audibly identical to the mono path, which is the
    // point of defaulting here to 0 — see the file header on why every humanization
    // default is off), 1 = voices spread across the full field by index. Equal-power law;
    // NOT the blend's equal-gain law — see blend.hpp's normalize() for why those two
    // crossfades are deliberately different laws for deliberately different reasons.
    float panSpread = 0.0f;
};

// --- engines: granular section + psola section ----------------------------------------
//
// GranularTuning, PsolaTuning and ShifterTuning now live in shifter.hpp, not here — see
// that file's own comment for the full reasoning. Short version: this header includes
// voice.hpp, voice.hpp includes shifter.hpp, so shifter.hpp reaching back here for
// ShifterTuning would close a circular include. An earlier track placed them here because
// it was forbidden from touching shifter.hpp; that constraint does not apply to this one,
// so they moved to their correct home — a cut-and-paste with no logic change, plus the
// IPitchShifter::setTuning() overrides that place now makes possible.
//
// GranularTuning and the old GranularConfig no longer both exist holding the same fields:
// granular_shifter.hpp now has `using GranularConfig = GranularTuning;`.

// --- analyser: YIN's behavioural fields + the epoch tracker's constants ---------------
//
// AnalyzerTuning now lives in analysis.hpp, for the identical reason ShifterTuning lives in
// shifter.hpp: IAnalyzer::setTuning(const AnalyzerTuning&) needs the struct visible from
// analysis.hpp, and this header sits downstream of analysis.hpp in the include graph
// (voice.hpp -> shifter.hpp -> analysis.hpp), so defining it here would be the same
// circular include one level further out.

// --- the struct -------------------------------------------------------------------------
//
// Flat, trivially copyable, no pointers (see the static_asserts below), no strings. Every
// field is either a scalar or one of the small structs above, so TuningBus can move a
// whole Tuning with a single struct assignment — no per-field marshalling, no allocation.
struct Tuning {
    std::uint32_t schemaVersion = kTuningSchemaVersion;

    // --- instrument -----------------------------------------------------------------
    RatioSource mode = RatioSource::AbsoluteTarget;   // EngineConfig::ratioSource default
                                                       // (engine.hpp:49) — Mode A.
    int rootMidiNote = 60;                            // EngineConfig::rootMidiNote (engine.hpp:54)
    int voiceCap = kMaxVoices;                        // no cap exists today; kMaxVoices (16,
                                                       // types.hpp:51) is every voice Engine
                                                       // currently makes available.

    // BOTH default to the SAME 8 ms: today there is only one envelope time constant,
    // envStep in Engine::process (engine.cpp:240), used for both attack and release
    // slewing of v.envPhase. Splitting it into two dials is new surface area this struct
    // adds, but the DEFAULT must reproduce the single current number in both directions,
    // or a default-constructed Tuning already changes the release behaviour.
    float attackMs = 8.0f;
    float releaseMs = 8.0f;

    float glideSemisPerSec = 0.0f;    // Voice::glideRateSemitonesPerSec default (voice.hpp:113)
                                       // — "0 = instant", i.e. no portamento, today's behaviour.
    float freezeWindowMs = 500.0f;    // was the hardcoded 500 ms freeze window
                                       // (engine.cpp:183: `sampleRate * 0.5`).

    // Velocity -> gain curve (app-design.md §5.6): Voice::velocity is stored at noteOn and
    // was never read anywhere. gain = velocityToGainMin + (1 - velocityToGainMin) *
    // velocity^velocityCurve. DEFAULT IS BYPASS: velocityToGainMin = 1.0 makes the gain
    // exactly 1.0 for EVERY velocity and EVERY curve exponent (1 + 0*x == 1 is bit-exact
    // regardless of x), which is IDENTICAL to today's Engine, where velocity is simply
    // never consulted. Same "defaults are the null hypothesis" rule as dryGain/wetGain/
    // limiterOn below — a nicer, velocity-sensitive default belongs in a factory profile,
    // not in the struct a bare `Tuning{}` reproduces.
    float velocityToGainMin = 1.0f;   // gain at velocity == 0. 1.0 = velocity has no effect.
    float velocityCurve = 1.0f;       // exponent on velocity before the lerp. 1 = linear.

    // --- timbre -----------------------------------------------------------------------
    // Carries VoicingProfile. Embedded wholesale rather than re-declared field-by-field,
    // matching app-design.md §4.2's own sketch exactly — preservation.hpp is a different
    // track's file, so its Tier-0 dead dials (preserveEnvelope, envelopeWarp, ...) travel
    // along inside this struct whether or not they have readers. That is not the same
    // thing as ADDING them to Tuning as first-class dials: see the DELIBERATELY ABSENT
    // note at the bottom of this file for what that distinction means in practice.
    PreservationSpec preservation{};

    // --- blend --------------------------------------------------------------------------
    BlendKind kind = BlendKind::FastOnly;
    float rsCrossoverHz = 220.0f;     // RegisterSplitPolicy::crossoverHz default (blend.hpp:104)
    float rsWidthOctaves = 1.0f;      // RegisterSplitPolicy::widthOctaves default (blend.hpp:105)
    float ohStartMs = 25.0f;          // OnsetHandoverPolicy::handoverStartMs default (blend.hpp:139)
    float ohEndMs = 70.0f;            // OnsetHandoverPolicy::handoverEndMs default (blend.hpp:140)
    float alignment = 0.0f;           // Engine::alignAmount_ default (engine.hpp:148) — 0 =
                                       // no compensation, the fast path arrives early exactly
                                       // as intended today.

    // --- ensemble -------------------------------------------------------------------
    HumanizationSpec humanize{};      // all zero — see the file header on why.
    float decorMinMs = 0.5f;          // was the hardcoded 0.5-4 ms decorrelation spread
    float decorMaxMs = 4.0f;          // (engine.cpp:164-165: `0.5 + 3.5 * frac`, frac in [0,1)).

    // --- engines + analyser (non-allocating fields only) ------------------------------
    ShifterTuning shifters{};
    AnalyzerTuning analyzer{};

    // --- signal path --------------------------------------------------------------------
    // None of this exists in Engine today, so every default is a literal no-op: applying
    // it changes nothing about what Engine::process emits, because Engine::process does
    // not read any of these fields yet (app-design.md Track D wires them in later).
    float inputTrimDb = 0.0f;         // 0 dB = unity, no trim.
    bool  inputGateOn = false;        // gate OFF. NOTE: app-design.md §4.2's sketch shows
                                       // only `inputGateDb` (a threshold), with no separate
                                       // enable flag — see this file's own header comment
                                       // and the delivery report for why a bool was added:
                                       // a threshold alone cannot express "gate does not
                                       // exist" without an out-of-band sentinel value, and
                                       // a sentinel is a worse interface than a flag.
    float inputGateDb = -60.0f;       // threshold WHEN ENABLED; a plausible quiet-room
                                       // floor, PROVISIONAL like every other never-yet-
                                       // measured level in this codebase.
    float dryGain = 0.0f;             // 0 = no dry signal mixed in, matching today's engine,
                                       // which emits no dry path at all.
    float wetGain = 1.0f;             // 1 = unity, matching today's 100%-wet output.
    float outCeilingDb = 0.0f;        // 0 dBFS = no headroom reduction.
    bool  limiterOn = false;          // no limiter exists today; app-design.md §5.5 also
                                       // requires this default OFF in the offline tools
                                       // specifically so a measurement render is never a
                                       // measurement of the limiter (VH-009 is the reason
                                       // the limiter exists in the first place).

    // Enforces every safety bound a live parameter can violate. LOAD-BEARING, not
    // defensive programming: app-design.md §4.5 is a five-item audit of ways the "obvious"
    // implementation of a live parameter is quietly wrong, and profiles are hand-editable
    // JSON files (app-design.md §7), so a UI widget's min/max is not a defence — a zero or
    // a swapped min/max can arrive from a text editor exactly as easily as from a slider.
    // The rule this generalises (app-design.md §4.5, closing paragraph): "applyTuning()
    // clamps everything it receives."
    void clamp() noexcept {
        // (1) hopSamples floored at 32. AT ZERO, yin.cpp:209's
        //     `while (nextAnalysisPos_ + cfg_.hopSamples <= upTo)` never advances the
        //     analysis clock — an INFINITE LOOP INSIDE THE AUDIO CALLBACK, not a click or
        //     a wrong number. app-design.md §4.5 item 1 names this exact hazard and the
        //     exact fix.
        if (analyzer.hopSamples < FrameCount{32}) analyzer.hopSamples = FrameCount{32};

        // (2) Decorrelation spread kept under the buffer's ACTUAL sizing assumption.
        //     engine.cpp sizes decorStride_ from a literal (`cfg_.sampleRate * 0.050`, raised
        //     from 0.010 in this same change — app-design.md §4.5 item 2 is explicit that the
        //     two bounds "must move together or they silently disagree again"); a live spread
        //     pushed past it would make `dline[(dw - decorDelay) & dmask]` (engine.cpp) wrap
        //     and read the wrong-aged sample — quietly wrong audio, not a crash. kDecorBoundMs
        //     below and engine.cpp's sizing literal are two independent copies of 50.0 by
        //     necessity (core stays dependency-free, so engine.cpp cannot include this header
        //     just for one named constant, and this header must not depend on engine.cpp) —
        //     the coupling is enforced by this comment and by
        //     "clamp() keeps a 50 ms spread inside the buffer it asked for" in
        //     tests/test_tuning.cpp, not by the type system. If you raise one, raise the other.
        decorMinMs = std::clamp(decorMinMs, 0.0f, kDecorBoundMs);
        decorMaxMs = std::clamp(decorMaxMs, 0.0f, kDecorBoundMs);
        if (decorMinMs > decorMaxMs) std::swap(decorMinMs, decorMaxMs);

        // (3) mu and tilt clamps kept ordered. voicing.hpp's muFor()/tiltGainFor() both
        //     call std::clamp(value, min, max) unconditionally — std::clamp's precondition
        //     is min <= max, and violating it is UNDEFINED BEHAVIOUR, not merely a wrong
        //     answer. A hand-edited profile can set muMin > muMax as easily as any other
        //     transposed pair of numbers.
        if (preservation.voicing.muMin > preservation.voicing.muMax) {
            std::swap(preservation.voicing.muMin, preservation.voicing.muMax);
        }
        if (preservation.voicing.tiltGainMin > preservation.voicing.tiltGainMax) {
            std::swap(preservation.voicing.tiltGainMin, preservation.voicing.tiltGainMax);
        }

        // (4) Gains and 0..1 amounts, range-limited. Not a hang or a corrupted-read hazard
        //     like (1) and (2) — these are sanity bounds against a fat-fingered profile
        //     value driving the output stage, a divide, or a crossfade shape somewhere
        //     nonsensical.
        dryGain = std::clamp(dryGain, 0.0f, kTuningMaxLinearGain);
        wetGain = std::clamp(wetGain, 0.0f, kTuningMaxLinearGain);
        alignment = std::clamp(alignment, 0.0f, 1.0f);       // Engine::setBlendAlignment's
                                                              // own range (engine.hpp:106).
        rsWidthOctaves = std::max(rsWidthOctaves, 1e-3f);    // RegisterSplitPolicy divides
                                                              // by this (blend.hpp:109).
        analyzer.threshold = std::clamp(analyzer.threshold, 0.0f, 1.0f);
        shifters.granular.fadeFraction = std::clamp(shifters.granular.fadeFraction, 0.0f, 1.0f);
        analyzer.epochLoudnessFactor = std::clamp(analyzer.epochLoudnessFactor, 0.0f, 1.0f);
        analyzer.epochEnvelopeRelease = std::clamp(analyzer.epochEnvelopeRelease, 0.0f, 0.999999f);
        humanize.panSpread = std::clamp(humanize.panSpread, 0.0f, 1.0f);

        // (5) voiceCap limited to 1..kMaxVoices. Below 1 there is no instrument; above
        //     kMaxVoices there is no voice to steal into — Engine::prepare() allocates
        //     exactly kMaxVoices voices and sizes every per-voice buffer (alignBuf_,
        //     decorBuf_) to match (types.hpp:51, engine.cpp:28-62), so a cap above that
        //     number would claim voices that were never allocated.
        voiceCap = std::clamp(voiceCap, 1, kMaxVoices);

        // (6) Every millisecond field non-negative. A negative envelope time would make
        //     Engine::process's envStep (`1.0 / (attackMs * 0.001 * sr)`) divide by a
        //     negative and slew the WRONG direction; the rest have no physical meaning
        //     below zero even where nothing downstream would crash on them.
        attackMs = std::max(attackMs, 0.0f);
        releaseMs = std::max(releaseMs, 0.0f);
        freezeWindowMs = std::max(freezeWindowMs, 0.0f);
        ohStartMs = std::max(ohStartMs, 0.0f);
        ohEndMs = std::max(ohEndMs, ohStartMs);   // end before start would run the onset
                                                   // handover crossfade backwards.
        analyzer.maxHoldMs = std::max(analyzer.maxHoldMs, 0.0f);
        shifters.granular.unvoicedGrainMs = std::max(shifters.granular.unvoicedGrainMs, 0.0f);
        shifters.granular.geometrySmoothMs = std::max(shifters.granular.geometrySmoothMs, 0.0f);

        // handoverMs gets a floor ABOVE zero, unlike its neighbours in this block. At
        // exactly 0, PsolaShifter::setTuning() would compute mixStep_ = 1 / (0 * sr) = +inf.
        // That is not a memory-safety hazard on its own (every use of mix_ downstream is
        // clamped against a finite target before it can reach req.out — see that function's
        // own comment), but it silently defeats the whole point of the field: it turns the
        // C1 crossfade that replaced VH-002's four hard switches back into a one-sample
        // step. PsolaShifter::setTuning() carries an identical defensive floor of its own
        // (1e-3 ms) for callers that reach it directly without going through Tuning::clamp()
        // at all; this one is what a Tuning-based caller sees.
        shifters.psola.handoverMs = std::max(shifters.psola.handoverMs, 0.1f);
        humanize.onsetJitterMs = std::max(humanize.onsetJitterMs, 0.0f);

        // (7) Non-negative counts and ensemble-spread magnitudes. A negative hop count or
        //     jump count has no meaning; a negative "spread" would make Engine's eventual
        //     per-voice derivation produce the opposite of what the width control claims.
        analyzer.releaseHops = std::max(analyzer.releaseHops, 0);
        analyzer.jumpConfirmHops = std::max(analyzer.jumpConfirmHops, 0);
        shifters.granular.jumpPeriods = std::max(shifters.granular.jumpPeriods, 1);
        humanize.detuneCents = std::max(humanize.detuneCents, 0.0f);
        humanize.detuneDriftCents = std::max(humanize.detuneDriftCents, 0.0f);
        humanize.vibratoRateHz = std::max(humanize.vibratoRateHz, 0.0f);
        humanize.vibratoDepthCents = std::max(humanize.vibratoDepthCents, 0.0f);
        humanize.envelopeWarpSpread = std::max(humanize.envelopeWarpSpread, 0.0f);

        // (8) Epoch cutoff RANGE kept sane and ordered. EpochTracker::setCutoff clamps the
        //     RUNTIME value to [epochCutoffMinHz, epochCutoffMaxHz] (defaults [120, 1500]) as
        //     of the most recent setTuning() call, so an inverted or negative range here is
        //     not itself a hang or corruption hazard — it is against publishing a range the
        //     tracker will silently second-guess (min > max reads, in practice, as "always
        //     clamp to min").
        analyzer.epochCutoffMinHz = std::max(analyzer.epochCutoffMinHz, 20.0f);
        analyzer.epochCutoffMaxHz = std::max(analyzer.epochCutoffMaxHz, analyzer.epochCutoffMinHz);
        analyzer.epochRefractoryFactor = std::max(analyzer.epochRefractoryFactor, 0.0f);

        // (10) Velocity curve. velocityToGainMin is a gain, not a 0..1 amount, but it is a
        //      MIX gain (like dryGain/wetGain above) rather than a hazard-class bound, so it
        //      gets the same modest sanity range rather than kTuningMaxLinearGain — a
        //      velocity floor above 1.0 (louder at velocity 0 than at velocity 1) has no
        //      sane reading. velocityCurve is floored above zero: pow(0, <=0) is undefined
        //      or diverges at velocity == 0, and a curve of exactly 0 would make gain
        //      independent of velocity in the OTHER direction (constant at the ceiling)
        //      rather than the intended "no curve shaping" of exponent 1.
        velocityToGainMin = std::clamp(velocityToGainMin, 0.0f, 1.0f);
        velocityCurve = std::max(velocityCurve, 0.01f);

        // (9) Input/output stage sanity — no counterpart exists in Engine today, so these
        //     bounds are precautionary rather than pinned against a known failure.
        inputTrimDb = std::clamp(inputTrimDb, -60.0f, 24.0f);
        inputGateDb = std::clamp(inputGateDb, -100.0f, 0.0f);
        outCeilingDb = std::clamp(outCeilingDb, -60.0f, 0.0f);
    }

private:
    // The buffer-sizing bound clamp() enforces for (2) above. Named so the 50.0f cannot
    // silently drift out of sync with its own comment; still just a literal copy of
    // engine.cpp's decorStride_ sizing factor, not a shared constant with that file — see
    // (2)'s comment on why the two are two copies rather than one definition.
    static constexpr float kDecorBoundMs = 50.0f;
};

// --- shape guarantees -------------------------------------------------------------------
//
// Trivially copyable: TuningBus moves a whole Tuning with a plain struct assignment (see
// tuning_bus.hpp), which is only well-defined — and only cheap — if this holds. Any field
// with a user-defined copy constructor (a std::string, a std::vector, anything with a
// destructor that matters) would break both the RT-safety guarantee and the "one memcpy"
// cost model TuningBus is built around.
static_assert(std::is_trivially_copyable_v<Tuning>,
              "Tuning crosses the UI/audio boundary by value inside TuningBus; anything "
              "that is not trivially copyable would need synchronization TuningBus does "
              "not provide, or would allocate on assignment, which is illegal from the "
              "audio thread when TuningBus::publish() one day gets called near it.");

namespace tuning_detail {
template <typename... Ts>
inline constexpr bool none_are_pointers = (!std::is_pointer_v<Ts> && ...);
} // namespace tuning_detail

// "No pointers" cannot be expressed as one whole-struct trait in C++20 without reflection
// (there is no std::is_pointer_free_v that walks nested aggregates), so this is the
// closest honest approximation: every DIRECT member of Tuning is asserted individually,
// which rules out a raw pointer at the top level. HumanizationSpec (this file) is checked
// one level down below; ShifterTuning's two halves and AnalyzerTuning carry their own
// equivalent static_asserts in shifter.hpp and analysis.hpp respectively, next to where
// they are now defined. PreservationSpec/VoicingProfile are declared in a file this track
// does not own (preservation.hpp/voicing.hpp) and were confirmed pointer-free by
// inspection while writing this header — every field in both is a float, bool, or enum. If
// that ever changes, this static_assert will not catch it; a reviewer touching
// preservation.hpp is the actual backstop for that one struct.
static_assert(tuning_detail::none_are_pointers<
    decltype(Tuning::schemaVersion), decltype(Tuning::mode), decltype(Tuning::rootMidiNote),
    decltype(Tuning::voiceCap), decltype(Tuning::attackMs), decltype(Tuning::releaseMs),
    decltype(Tuning::glideSemisPerSec), decltype(Tuning::freezeWindowMs),
    decltype(Tuning::velocityToGainMin), decltype(Tuning::velocityCurve),
    decltype(Tuning::preservation), decltype(Tuning::kind), decltype(Tuning::rsCrossoverHz),
    decltype(Tuning::rsWidthOctaves), decltype(Tuning::ohStartMs), decltype(Tuning::ohEndMs),
    decltype(Tuning::alignment), decltype(Tuning::humanize), decltype(Tuning::decorMinMs),
    decltype(Tuning::decorMaxMs), decltype(Tuning::shifters), decltype(Tuning::analyzer),
    decltype(Tuning::inputTrimDb), decltype(Tuning::inputGateOn), decltype(Tuning::inputGateDb),
    decltype(Tuning::dryGain), decltype(Tuning::wetGain), decltype(Tuning::outCeilingDb),
    decltype(Tuning::limiterOn)>,
    "Tuning must contain no pointers -- a pointer field would let the audio thread "
    "dereference something the UI thread owns, which is exactly the class of bug the "
    "snapshot bus exists to make impossible.");

static_assert(tuning_detail::none_are_pointers<
    decltype(HumanizationSpec::detuneCents), decltype(HumanizationSpec::detuneDriftCents),
    decltype(HumanizationSpec::onsetJitterMs), decltype(HumanizationSpec::vibratoRateHz),
    decltype(HumanizationSpec::vibratoDepthCents), decltype(HumanizationSpec::envelopeWarpSpread),
    decltype(HumanizationSpec::panSpread)>,
    "HumanizationSpec must contain no pointers.");

// --- DELIBERATELY ABSENT, so a future reader does not treat the omission as a bug -------
//
// The Tier-0 dead dials from app-design.md §5.8: preserveEnvelope, envelopeWarp,
// scaleWarpWithShift, preserveAperiodicity, vibratoConstantInCents, preserveTiming. Every
// one of them has NO READER anywhere in the DSP — they arrive along for the ride inside
// `preservation` (PreservationSpec is embedded wholesale, per app-design.md §4.2's own
// sketch, and preservation.hpp is a different track's file), but nothing here promotes
// any of them into an independent Tuning field, a UI control, or a profile-schema entry.
// A knob that does nothing is worse than a missing knob, and this file's whole purpose is
// to be the place where "is this knob real" has one authoritative answer.
//
// Two of the six are already explained as superseded rather than vestigial
// (preservation.hpp: envelopeWarp and scaleWarpWithShift are kept as the A/B control for
// `voicing`) — they are still not promoted, because an A/B control being useful for
// developers is not the same claim as it being a musical parameter worth exposing.
//
// Also absent for the SAME reason, spelled out in AnalyzerTuning's own header comment:
// YIN's minHz/maxHz and any PSOLA constant that only sizes a prepare()-time buffer. Those
// are not vestigial — they matter — they are simply not LIVE, per app-design.md §4.1.

} // namespace vh
