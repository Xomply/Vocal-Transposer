// tests/test_tuning.cpp — the property this whole file exists to protect: a default-
// constructed Tuning{} reproduces the Engine's behaviour EXACTLY as it stands before
// Tuning existed. Every CHECK below either compares a Tuning{} field against the actual
// live struct it was copied from (YinConfig{}, GranularConfig{}, VoicingProfile{},
// PreservationSpec{}, a default Engine's own accessors), or — for the handful of values
// that are genuinely just a bare literal inside a .cpp file with no struct behind them —
// against that literal, with a comment naming the file and line it came from. If any of
// these ever disagrees, applying a default Tuning to Engine would silently change rendered
// audio, which would invalidate RESULTS.md's measurements and click_sweep.py's regression
// grid without anyone asking for that change. See tuning.hpp's own file header for the
// full argument.

#include <doctest/doctest.h>

#include "vh/blend.hpp"
#include "vh/engine.hpp"
#include "vh/granular_shifter.hpp"
#include "vh/passthrough_shifter.hpp"
#include "vh/preservation.hpp"
#include "vh/psola_shifter.hpp"
#include "vh/rt.hpp"
#include "vh/tuning.hpp"
#include "vh/tuning_bus.hpp"
#include "vh/voice.hpp"
#include "vh/voicing.hpp"
#include "vh/yin.hpp"

#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace vh;

namespace {

// Records the ratio it was last asked for, so Mode B's root can be observed without
// reaching into Engine's private ratioForVoice(). Mirrors tests/test_engine.cpp's
// SpyShifter; kept separate because that one lives in an anonymous namespace this file
// does not share.
class RatioSpy final : public IPitchShifter {
public:
    mutable double lastRatio = 0.0;
    void prepare(double, FrameCount) override {}
    void reset() noexcept override {}
    void process(const ShiftRequest& req) noexcept override {
        lastRatio = req.ratio;
        req.ring->read(req.cursor->next, req.out, req.numFrames);
        if (!req.cursor->frozen) req.cursor->next += req.numFrames;
    }
    FrameCount latencySamples() const noexcept override { return 0; }
    const char* name() const noexcept override { return "ratio-spy"; }
};

// Field-by-field equality. NOT std::memcmp: Tuning is trivially copyable but the standard
// does not guarantee padding bytes are copied identically byte-for-byte by every compiler
// in every situation, so a memcmp-based idempotency check would risk failing (or, worse,
// passing by accident) on grounds that have nothing to do with the property under test.
// Comparing the fields that actually matter is slower to write and the only version that
// says what it means.
bool sameHumanize(const HumanizationSpec& a, const HumanizationSpec& b) {
    return a.detuneCents == b.detuneCents && a.detuneDriftCents == b.detuneDriftCents &&
           a.onsetJitterMs == b.onsetJitterMs && a.vibratoRateHz == b.vibratoRateHz &&
           a.vibratoDepthCents == b.vibratoDepthCents &&
           a.envelopeWarpSpread == b.envelopeWarpSpread && a.panSpread == b.panSpread;
}

bool sameShifters(const ShifterTuning& a, const ShifterTuning& b) {
    return a.granular.jumpPeriods == b.granular.jumpPeriods &&
           a.granular.fadeFraction == b.granular.fadeFraction &&
           a.granular.unvoicedGrainMs == b.granular.unvoicedGrainMs &&
           a.granular.geometrySmoothMs == b.granular.geometrySmoothMs &&
           a.psola.handoverMs == b.psola.handoverMs;
}

bool sameAnalyzer(const AnalyzerTuning& a, const AnalyzerTuning& b) {
    return a.threshold == b.threshold && a.hopSamples == b.hopSamples &&
           a.holdThroughUnvoiced == b.holdThroughUnvoiced && a.maxHoldMs == b.maxHoldMs &&
           a.releaseHops == b.releaseHops && a.maxStepCents == b.maxStepCents &&
           a.jumpConfirmHops == b.jumpConfirmHops &&
           a.epochCutoffRatio == b.epochCutoffRatio &&
           a.epochLoudnessFactor == b.epochLoudnessFactor &&
           a.epochRefractoryFactor == b.epochRefractoryFactor &&
           a.epochEnvelopeRelease == b.epochEnvelopeRelease &&
           a.epochCutoffMinHz == b.epochCutoffMinHz && a.epochCutoffMaxHz == b.epochCutoffMaxHz;
}

bool samePreservation(const PreservationSpec& a, const PreservationSpec& b) {
    const VoicingProfile& va = a.voicing;
    const VoicingProfile& vb = b.voicing;
    return a.preserveEnvelope == b.preserveEnvelope && a.envelopeWarp == b.envelopeWarp &&
           a.scaleWarpWithShift == b.scaleWarpWithShift &&
           a.preserveAperiodicity == b.preserveAperiodicity &&
           a.vibratoConstantInCents == b.vibratoConstantInCents &&
           a.unvoiced == b.unvoiced && a.preserveTiming == b.preserveTiming &&
           va.enabled == vb.enabled && va.kMu == vb.kMu && va.muStrength == vb.muStrength &&
           va.muMin == vb.muMin && va.muMax == vb.muMax && va.kTilt == vb.kTilt &&
           va.tiltStrength == vb.tiltStrength && va.tiltGainMin == vb.tiltGainMin &&
           va.tiltGainMax == vb.tiltGainMax && va.tiltCornerInF0 == vb.tiltCornerInF0;
}

// Used only by the idempotency test, which needs to know clamp() changed NOTHING on its
// second call — every field, not just the ones a particular clamp rule targets, because an
// idempotency bug could just as easily show up as an unrelated field drifting.
bool sameTuning(const Tuning& a, const Tuning& b) {
    return a.schemaVersion == b.schemaVersion && a.mode == b.mode &&
           a.rootMidiNote == b.rootMidiNote && a.voiceCap == b.voiceCap &&
           a.attackMs == b.attackMs && a.releaseMs == b.releaseMs &&
           a.glideSemisPerSec == b.glideSemisPerSec && a.freezeWindowMs == b.freezeWindowMs &&
           a.velocityToGainMin == b.velocityToGainMin && a.velocityCurve == b.velocityCurve &&
           samePreservation(a.preservation, b.preservation) && a.kind == b.kind &&
           a.rsCrossoverHz == b.rsCrossoverHz && a.rsWidthOctaves == b.rsWidthOctaves &&
           a.ohStartMs == b.ohStartMs && a.ohEndMs == b.ohEndMs &&
           a.alignment == b.alignment && sameHumanize(a.humanize, b.humanize) &&
           a.decorMinMs == b.decorMinMs && a.decorMaxMs == b.decorMaxMs &&
           sameShifters(a.shifters, b.shifters) && sameAnalyzer(a.analyzer, b.analyzer) &&
           a.inputTrimDb == b.inputTrimDb && a.inputGateOn == b.inputGateOn &&
           a.inputGateDb == b.inputGateDb && a.dryGain == b.dryGain &&
           a.wetGain == b.wetGain && a.outCeilingDb == b.outCeilingDb &&
           a.limiterOn == b.limiterOn;
}

} // namespace

// ============================================================================
// PART 1 — every default reproduces the live constant it replaces.
// ============================================================================

TEST_CASE("Tuning{} instrument fields match EngineConfig's own defaults") {
    EngineConfig ref{};   // engine.hpp:44-57
    const Tuning t{};
    CHECK(t.mode == ref.ratioSource);
    CHECK(t.rootMidiNote == ref.rootMidiNote);

    // voiceCap: no cap exists in Engine today (§4.1 promotes this as a NEW runtime limit,
    // capped at kMaxVoices per types.hpp:51). The default must therefore equal kMaxVoices
    // -- anything smaller would silence voices Engine currently makes available.
    CHECK(t.voiceCap == kMaxVoices);

    // glideSemisPerSec: Voice::glideRateSemitonesPerSec default is 0 ("0 = instant"),
    // voice.hpp:113 -- today's engine has no portamento at all.
    const Voice v{};
    CHECK(t.glideSemisPerSec == v.glideRateSemitonesPerSec);
}

TEST_CASE("Tuning{} timbre field is byte-for-byte a default PreservationSpec") {
    const PreservationSpec ref{};
    CHECK(samePreservation(Tuning{}.preservation, ref));
}

TEST_CASE("Tuning{} envelope times match the hardcoded 8 ms in Engine::process") {
    // engine.cpp:240 -- `const float envStep = 1.0f / (0.008 * cfg_.sampleRate);` is the
    // ONE envelope constant Engine has today, used for both attack and release slewing of
    // v.envPhase. Both new fields must default to the SAME number, in both directions, or
    // a default Tuning already changes release behaviour that nobody asked to change.
    CHECK(Tuning{}.attackMs == doctest::Approx(8.0f));
    CHECK(Tuning{}.releaseMs == doctest::Approx(8.0f));
}

TEST_CASE("Tuning{} velocity curve defaults to a bypass, matching today's unused velocity") {
    // app-design.md §5.6 (Track C): Voice::velocity is stored and was never read before this
    // track. velocityToGainMin == 1.0 makes the gain formula
    // (velocityToGainMin + (1-velocityToGainMin)*velocity^curve) return exactly 1.0 for
    // EVERY velocity and EVERY curve exponent -- the same "defaults are the null
    // hypothesis" rule as dryGain/wetGain/limiterOn.
    CHECK(Tuning{}.velocityToGainMin == 1.0f);
    CHECK(Tuning{}.velocityCurve == 1.0f);
}

TEST_CASE("Tuning{} freeze window matches the hardcoded 500 ms in Engine::noteOff") {
    // engine.cpp:183 -- `const Pos span = static_cast<Pos>(cfg_.sampleRate * 0.5);`
    CHECK(Tuning{}.freezeWindowMs == doctest::Approx(500.0f));
}

TEST_CASE("Tuning{} decorrelation spread matches the hardcoded 0.5-4 ms in Engine::noteOn") {
    // engine.cpp:164-165 -- `v.hum.staticDelayMs = 0.5 + 3.5 * frac;`, frac in [0, 1), so
    // the achieved range is [0.5, 4.0) ms. decorMinMs/decorMaxMs are the ENDPOINTS of that
    // range, not the formula itself -- the golden-ratio derivation stays Engine's job.
    CHECK(Tuning{}.decorMinMs == doctest::Approx(0.5f));
    CHECK(Tuning{}.decorMaxMs == doctest::Approx(4.0f));
}

TEST_CASE("Tuning{} blend fields match the concrete policies' own defaults") {
    const RegisterSplitPolicy rs{};       // blend.hpp:104-105
    const OnsetHandoverPolicy oh{};       // blend.hpp:139-140
    const Tuning t{};
    CHECK(t.rsCrossoverHz == doctest::Approx(rs.crossoverHz));
    CHECK(t.rsWidthOctaves == doctest::Approx(rs.widthOctaves));
    CHECK(t.ohStartMs == doctest::Approx(oh.handoverStartMs));
    CHECK(t.ohEndMs == doctest::Approx(oh.handoverEndMs));

    // alignment and kind against a freshly constructed Engine's OWN defaults, not just
    // literals -- these two are reachable without prepare().
    Engine e;
    CHECK(t.alignment == doctest::Approx(e.blendAlignment()));   // engine.hpp:148
    // No enum<->policy mapping exists yet (that is Track C's applyTuning() wiring); the
    // closest honest check available today is that both independently name "fast-only" as
    // the thing that runs before anyone calls setBlendPolicy() (engine.cpp:17,
    // engine.hpp:158).
    CHECK(std::string(e.blendPolicy()->name()) == "fast-only");
    CHECK(t.kind == BlendKind::FastOnly);
}

TEST_CASE("Tuning{} humanization is entirely OFF") {
    // §11: "every humanization default" is PROVISIONAL and meant to move to something
    // "modest, not zero" -- but that belongs in a factory PROFILE, not the struct default.
    // See tuning.hpp's file header for why: a zero default is what makes a default Tuning a
    // no-op against today's engine, which reads Humanization almost nowhere yet.
    const HumanizationSpec h{};
    CHECK(h.detuneCents == 0.0f);
    CHECK(h.detuneDriftCents == 0.0f);
    CHECK(h.onsetJitterMs == 0.0f);
    CHECK(h.vibratoRateHz == 0.0f);
    CHECK(h.vibratoDepthCents == 0.0f);
    CHECK(h.envelopeWarpSpread == 0.0f);
    CHECK(h.panSpread == 0.0f);
}

TEST_CASE("Tuning{} granular section matches GranularConfig{}'s own defaults") {
    const GranularConfig ref{};   // granular_shifter.hpp:34-44
    const auto& g = Tuning{}.shifters.granular;
    CHECK(g.jumpPeriods == ref.jumpPeriods);
    CHECK(g.fadeFraction == doctest::Approx(ref.fadeFraction));
    CHECK(g.unvoicedGrainMs == doctest::Approx(ref.unvoicedGrainMs));

    // geometrySmoothMs has no struct counterpart -- it is a bare literal inside
    // GranularShifter::updateGeometry. granular_shifter.cpp:57:
    // `... / (0.020 * sampleRate)` -- 20 ms.
    CHECK(g.geometrySmoothMs == doctest::Approx(20.0f));
}

TEST_CASE("Tuning{} PSOLA handover matches the hardcoded kHandoverMs") {
    // psola_shifter.cpp:25 -- `constexpr double kHandoverMs = 8.0;`, an anonymous-
    // namespace local with no struct behind it at all -- the single most-wanted
    // provisional constant in HANDOVER.md §8 ("TUNE BY EAR on /t/ and /k/ before anything
    // else in this file").
    CHECK(Tuning{}.shifters.psola.handoverMs == doctest::Approx(8.0));
}

TEST_CASE("Tuning{} analyser behavioural fields match YinConfig{}'s own defaults") {
    const YinConfig ref{};   // yin.hpp:27-102. Deliberately NOT checking ref.minHz/maxHz --
                              // those are restart-only (§4.1) and absent from Tuning on
                              // purpose; see AnalyzerTuning's header comment.
    const auto& a = Tuning{}.analyzer;
    CHECK(a.threshold == doctest::Approx(ref.threshold));
    CHECK(a.hopSamples == ref.hopSamples);
    CHECK(a.holdThroughUnvoiced == ref.holdThroughUnvoiced);
    CHECK(a.maxHoldMs == doctest::Approx(ref.maxHoldMs));
    CHECK(a.releaseHops == ref.releaseHops);
    CHECK(a.maxStepCents == doctest::Approx(ref.maxStepCents));
    CHECK(a.jumpConfirmHops == ref.jumpConfirmHops);
}

TEST_CASE("Tuning{} epoch tracker constants match the hardcoded values in epoch.hpp") {
    // None of these are struct fields today; EpochTracker computes them as bare literals
    // inline. Citations are to core/include/vh/epoch.hpp.
    const auto& a = Tuning{}.analyzer;
    CHECK(a.epochCutoffRatio == doctest::Approx(1.8f));        // epoch.hpp:92
    CHECK(a.epochLoudnessFactor == doctest::Approx(0.25f));    // epoch.hpp:110
    CHECK(a.epochRefractoryFactor == doctest::Approx(0.6f));   // epoch.hpp:116
    CHECK(a.epochEnvelopeRelease == doctest::Approx(0.9995f)); // epoch.hpp:105
    CHECK(a.epochCutoffMinHz == doctest::Approx(120.0f));      // epoch.hpp:56
    CHECK(a.epochCutoffMaxHz == doctest::Approx(1500.0f));     // epoch.hpp:56
}

TEST_CASE("Tuning{} signal-path fields are unity/bypass, not a new audio change") {
    // Nothing here has a "live counterpart" to compare against, because none of this
    // exists in Engine yet -- these are literal assertions on the documented no-op values.
    const Tuning t{};
    CHECK(t.inputTrimDb == 0.0f);
    CHECK(t.inputGateOn == false);
    CHECK(t.dryGain == 0.0f);
    CHECK(t.wetGain == 1.0f);
    CHECK(t.outCeilingDb == 0.0f);
    CHECK(t.limiterOn == false);
}

TEST_CASE("Tuning{}'s schema version is set and non-zero") {
    CHECK(Tuning{}.schemaVersion == kTuningSchemaVersion);
    CHECK(Tuning{}.schemaVersion != 0);
}

// ============================================================================
// PART 2 — clamp() enforces every documented bound.
// ============================================================================

TEST_CASE("clamp() floors hopSamples at 32") {
    // app-design.md §4.5 item 1 / yin.cpp:209 -- at zero, the analysis hop loop never
    // advances. This is the one clamp in the whole struct that prevents a HANG, not a
    // wrong number.
    Tuning t{};
    t.analyzer.hopSamples = 0;
    t.clamp();
    CHECK(t.analyzer.hopSamples >= 32);

    Tuning u{};
    u.analyzer.hopSamples = 5;
    u.clamp();
    CHECK(u.analyzer.hopSamples >= 32);

    // Above the floor, clamp() must not touch it.
    Tuning v{};
    v.analyzer.hopSamples = 512;
    v.clamp();
    CHECK(v.analyzer.hopSamples == 512);
}

TEST_CASE("clamp() keeps the decorrelation spread under the buffer's sizing bound") {
    // engine.cpp sizes decorStride_ from a 50 ms literal (raised from 10 ms in this same
    // change, app-design.md §4.5 item 2 -- "raise the prepare-time bound to 50 ms and
    // hard-clamp the live control to it"). Both bounds moved together; see
    // "clamp() keeps a spread at the new 50 ms maximum inside the buffer it asked for"
    // below for the Engine-level proof that raising the clamp alone would not have been
    // enough without also raising engine.cpp's sizing literal.
    Tuning t{};
    t.decorMinMs = -5.0f;
    t.decorMaxMs = 999.0f;
    t.clamp();
    CHECK(t.decorMinMs >= 0.0f);
    CHECK(t.decorMaxMs <= 50.0f);
    CHECK(t.decorMinMs <= t.decorMaxMs);

    // Inverted min/max from a hand-edited profile must come out ordered, not merely
    // in-range.
    Tuning u{};
    u.decorMinMs = 8.0f;
    u.decorMaxMs = 1.0f;
    u.clamp();
    CHECK(u.decorMinMs <= u.decorMaxMs);
}

TEST_CASE("clamp() keeps mu and tilt clamps ordered") {
    // voicing.hpp's muFor()/tiltGainFor() call std::clamp(v, min, max) unconditionally;
    // min > max is undefined behaviour there, not merely a wrong answer.
    Tuning t{};
    t.preservation.voicing.muMin = 2.0f;
    t.preservation.voicing.muMax = 0.5f;
    t.preservation.voicing.tiltGainMin = 4.0f;
    t.preservation.voicing.tiltGainMax = 0.1f;
    t.clamp();
    CHECK(t.preservation.voicing.muMin <= t.preservation.voicing.muMax);
    CHECK(t.preservation.voicing.tiltGainMin <= t.preservation.voicing.tiltGainMax);
}

TEST_CASE("clamp() range-limits gains and 0..1 amounts") {
    Tuning t{};
    t.dryGain = -3.0f;
    t.wetGain = 999.0f;
    t.alignment = 5.0f;
    t.rsWidthOctaves = 0.0f;
    t.analyzer.threshold = -1.0f;
    t.shifters.granular.fadeFraction = 3.0f;
    t.analyzer.epochLoudnessFactor = -2.0f;
    t.humanize.panSpread = 7.0f;
    t.clamp();

    CHECK(t.dryGain >= 0.0f);
    CHECK(t.dryGain <= kTuningMaxLinearGain);
    CHECK(t.wetGain <= kTuningMaxLinearGain);
    CHECK(t.alignment >= 0.0f);
    CHECK(t.alignment <= 1.0f);
    CHECK(t.rsWidthOctaves > 0.0f);            // RegisterSplitPolicy divides by this
    CHECK(t.analyzer.threshold >= 0.0f);
    CHECK(t.analyzer.threshold <= 1.0f);
    CHECK(t.shifters.granular.fadeFraction <= 1.0f);
    CHECK(t.analyzer.epochLoudnessFactor >= 0.0f);
    CHECK(t.humanize.panSpread <= 1.0f);
}

TEST_CASE("clamp() limits voiceCap to 1..kMaxVoices") {
    Tuning t{};
    t.voiceCap = 0;
    t.clamp();
    CHECK(t.voiceCap >= 1);

    Tuning u{};
    u.voiceCap = -5;
    u.clamp();
    CHECK(u.voiceCap >= 1);

    Tuning v{};
    v.voiceCap = kMaxVoices + 100;
    v.clamp();
    CHECK(v.voiceCap <= kMaxVoices);
}

TEST_CASE("clamp() makes every millisecond field non-negative") {
    Tuning t{};
    t.attackMs = -1.0f;
    t.releaseMs = -1.0f;
    t.freezeWindowMs = -1.0f;
    t.ohStartMs = -1.0f;
    t.analyzer.maxHoldMs = -1.0f;
    t.shifters.granular.unvoicedGrainMs = -1.0f;
    t.shifters.granular.geometrySmoothMs = -1.0f;
    t.shifters.psola.handoverMs = -1.0f;
    t.humanize.onsetJitterMs = -1.0f;
    t.clamp();

    CHECK(t.attackMs >= 0.0f);
    CHECK(t.releaseMs >= 0.0f);
    CHECK(t.freezeWindowMs >= 0.0f);
    CHECK(t.ohStartMs >= 0.0f);
    CHECK(t.analyzer.maxHoldMs >= 0.0f);
    CHECK(t.shifters.granular.unvoicedGrainMs >= 0.0f);
    CHECK(t.shifters.granular.geometrySmoothMs >= 0.0f);
    CHECK(t.shifters.psola.handoverMs >= 0.0f);
    CHECK(t.humanize.onsetJitterMs >= 0.0f);
}

TEST_CASE("clamp() keeps the onset-handover window ordered") {
    // ohEndMs < ohStartMs would run OnsetHandoverPolicy's crossfade backwards.
    Tuning t{};
    t.ohStartMs = 70.0f;
    t.ohEndMs = 25.0f;
    t.clamp();
    CHECK(t.ohEndMs >= t.ohStartMs);
}

TEST_CASE("clamp() range-limits the velocity curve") {
    Tuning t{};
    t.velocityToGainMin = -3.0f;
    t.velocityCurve = -5.0f;
    t.clamp();
    CHECK(t.velocityToGainMin >= 0.0f);
    CHECK(t.velocityToGainMin <= 1.0f);
    CHECK(t.velocityCurve > 0.0f);   // floored above zero -- pow(vel, <=0) is not sane at vel==0

    Tuning u{};
    u.velocityToGainMin = 5.0f;
    u.clamp();
    CHECK(u.velocityToGainMin <= 1.0f);
}

TEST_CASE("clamp() keeps the epoch cutoff range ordered and sane") {
    Tuning t{};
    t.analyzer.epochCutoffMinHz = -50.0f;
    t.analyzer.epochCutoffMaxHz = -10.0f;
    t.clamp();
    CHECK(t.analyzer.epochCutoffMinHz >= 20.0f);
    CHECK(t.analyzer.epochCutoffMaxHz >= t.analyzer.epochCutoffMinHz);
}

TEST_CASE("clamp() is idempotent") {
    // A hand-edited profile only ever gets clamped once by applyTuning(), but the property
    // has to hold regardless -- an idempotency bug is a clamp that keeps nudging a value a
    // little further every time it is applied, which would be a slow drift bug indistin-
    // guishable from the profile itself changing.
    Tuning t{};
    t.analyzer.hopSamples = 0;
    t.decorMinMs = 999.0f;
    t.decorMaxMs = -5.0f;
    t.preservation.voicing.muMin = 9.0f;
    t.preservation.voicing.muMax = 0.1f;
    t.voiceCap = 999;
    t.dryGain = -50.0f;
    t.ohStartMs = 999.0f;
    t.ohEndMs = -50.0f;
    t.velocityToGainMin = -50.0f;
    t.velocityCurve = -50.0f;

    t.clamp();
    Tuning once = t;
    t.clamp();
    CHECK(sameTuning(t, once));
}

// ============================================================================
// PART 3 — TuningBus.
// ============================================================================

TEST_CASE("TuningBus::poll before any publish returns nullptr") {
    TuningBus bus;
    CHECK(bus.poll() == nullptr);
}

TEST_CASE("TuningBus::poll after publish returns the published value") {
    TuningBus bus;
    Tuning t{};
    t.rootMidiNote = 67;
    bus.publish(t);

    const Tuning* got = bus.poll();
    REQUIRE(got != nullptr);
    CHECK(got->rootMidiNote == 67);
}

TEST_CASE("TuningBus::poll a second time with no new publish returns nullptr") {
    TuningBus bus;
    Tuning t{};
    t.rootMidiNote = 67;
    bus.publish(t);

    REQUIRE(bus.poll() != nullptr);
    CHECK(bus.poll() == nullptr);   // nothing changed since the first poll claimed it
}

TEST_CASE("several publishes then one poll yields the LATEST value") {
    // The whole point of a snapshot channel over a queue (tuning_bus.hpp's file header):
    // intermediate values are dropped, not queued. B below must never be observed.
    TuningBus bus;
    Tuning a{}; a.rootMidiNote = 1;
    Tuning b{}; b.rootMidiNote = 2;
    Tuning c{}; c.rootMidiNote = 3;
    bus.publish(a);
    bus.publish(b);
    bus.publish(c);

    const Tuning* got = bus.poll();
    REQUIRE(got != nullptr);
    CHECK(got->rootMidiNote == 3);
    CHECK(bus.poll() == nullptr);   // that was the only new thing to see
}

TEST_CASE("the published value survives the publisher mutating its own copy afterwards") {
    // publish(const Tuning&) must copy, not alias -- a caller that keeps its local Tuning
    // around and edits it after publishing (the natural shape of a UI parameter object)
    // must not retroactively change what the audio thread already saw.
    TuningBus bus;
    Tuning t{};
    t.rootMidiNote = 60;
    bus.publish(t);

    t.rootMidiNote = 127;   // mutate the caller's own copy after publishing

    const Tuning* got = bus.poll();
    REQUIRE(got != nullptr);
    CHECK(got->rootMidiNote == 60);
}

TEST_CASE("TuningBus::poll never allocates") {
    // Same mechanism as tests/test_rt_safety.cpp: the test binary's global operator new
    // reports into vh::rt when called inside a real-time section, and poll() marks itself
    // with VH_RT_SECTION() (mirroring AudioRing::read/write). publish() is deliberately
    // included in the loop unguarded, same as the AudioRing precedent measuring both
    // write and read together -- it is the UI-thread half and is not itself under test,
    // but interleaving it is what makes each poll() actually have something new to claim.
    TuningBus bus;
    Tuning t{};
    rt::resetViolationCount();
    for (int i = 0; i < 100; ++i) {
        t.rootMidiNote = i % 128;
        bus.publish(t);
        const Tuning* got = bus.poll();
        (void)got;
    }
    CHECK(rt::violationCount() == 0);
}

TEST_CASE("TuningBus::poll is silent when there is genuinely nothing to allocate for") {
    // Steady-state RT-safety: poll()'d repeatedly with no intervening publish should be
    // even cheaper (a single load, per the header comment) and certainly no less safe.
    TuningBus bus;
    rt::resetViolationCount();
    for (int i = 0; i < 100; ++i) {
        const Tuning* got = bus.poll();
        (void)got;
    }
    CHECK(rt::violationCount() == 0);
}

// ============================================================================
// PART 4 — Engine::applyTuning(): the fan-out, and THE GOVERNING PROPERTY.
// ============================================================================

TEST_CASE("applying a default Tuning{} does not change rendered audio by one sample") {
    // THE GOVERNING PROPERTY of this whole track. PART 1 above already pins that every
    // INDIVIDUAL field of Tuning{} matches the live constant it replaces; this is the
    // end-to-end version of the same claim, run through a REAL Engine with REAL shifters
    // (granular + PSOLA) and a REAL analyser (YIN) — not "every field matches in
    // isolation" but "calling applyTuning(Tuning{}) every single block, the worst case for
    // drift, produces BIT-IDENTICAL output to never calling it at all". If this test ever
    // fails, RESULTS.md's measurements and click_sweep.py's regression grid are both
    // invalidated the moment applyTuning() is wired into anything that renders — see this
    // file's own header and tuning.hpp's file header for why that must not happen silently.
    constexpr double kSR = 48000.0;
    constexpr FrameCount kBlock = 128;

    auto makeEngine = []() {
        auto e = std::make_unique<Engine>();
        EngineConfig cfg;
        cfg.sampleRate = kSR;
        cfg.maxBlock = kBlock;
        cfg.ratioSource = RatioSource::IntervalFromRoot;
        cfg.rootMidiNote = 60;
        e->prepare(cfg, std::make_unique<YinAnalyzer>(),
                  []() { return std::make_unique<GranularShifter>(); },
                  []() { return std::make_unique<PsolaShifter>(); });
        return e;
    };

    auto without = makeEngine();
    auto with = makeEngine();
    without->noteOn(64, 1.0f);
    with->noteOn(64, 1.0f);

    // A real-ish harmonic tone, not silence — silence would trivially satisfy bit-identity
    // without exercising any of the code paths the setTuning() fan-out actually touches
    // (voiced/unvoiced handovers, epoch tracking, grain placement, the decorrelation line).
    std::vector<Sample> sig(kBlock * 200);
    for (size_t i = 0; i < sig.size(); ++i) {
        double s = 0.0;
        for (int k = 1; k <= 5; ++k) {
            s += std::sin(2.0 * kPi * 180.0 * k * static_cast<double>(i) / kSR) / k;
        }
        sig[i] = static_cast<Sample>(s * 0.2);
    }

    std::vector<Sample> outA(sig.size(), 0.0f), outB(sig.size(), 0.0f);
    const Tuning defaultTuning{};
    for (size_t i = 0; i + kBlock <= sig.size(); i += kBlock) {
        without->process(sig.data() + i, outA.data() + i, kBlock);
        with->applyTuning(defaultTuning);   // called EVERY block -- the worst case
        with->process(sig.data() + i, outB.data() + i, kBlock);
    }

    REQUIRE(outA.size() == outB.size());
    for (size_t i = 0; i < outA.size(); ++i) {
        CAPTURE(i);
        REQUIRE(outA[i] == outB[i]);   // BIT-IDENTICAL. Not doctest::Approx.
    }
}

TEST_CASE("applyTuning fans mode and rootMidiNote out to Mode B's ratio computation") {
    // Also pins Engine::setRootMidiNote(), the setter app-design.md §4.3 names as missing
    // despite rootMidiNote being read every block.
    RatioSpy* spy = nullptr;
    Engine e;
    EngineConfig cfg;
    cfg.sampleRate = 48000.0;
    cfg.maxBlock = 128;
    cfg.ratioSource = RatioSource::IntervalFromRoot;
    cfg.rootMidiNote = 60;
    e.prepare(cfg, std::make_unique<YinAnalyzer>(),
             // Engine::prepare() calls this factory once PER VOICE SLOT (kMaxVoices times),
             // not once total -- the `if (!spy)` guard keeps `spy` pointing at the first one
             // constructed, which is the one voice index 0 (allocated by the single noteOn()
             // below) actually uses. Without the guard, `spy` ends up aliasing the LAST
             // slot's shifter, which never runs. Mirrors test_engine.cpp's `firstSpy` pattern.
             [&spy]() { auto s = std::make_unique<RatioSpy>(); if (!spy) spy = s.get(); return s; },
             nullptr);
    e.noteOn(67, 1.0f);   // +7 semitones from root 60
    std::vector<Sample> silence(128, 0.0f), out(128, 0.0f);
    e.process(silence.data(), out.data(), 128);
    REQUIRE(spy != nullptr);
    CHECK(spy->lastRatio == doctest::Approx(std::pow(2.0, 7.0 / 12.0)));

    e.setRootMidiNote(67);   // root now equals the played note: ratio should collapse to 1
    e.process(silence.data(), out.data(), 128);
    CHECK(spy->lastRatio == doctest::Approx(1.0));

    // applyTuning() must reach the SAME field, not just the dedicated setter.
    Tuning t{};
    t.mode = RatioSource::IntervalFromRoot;
    t.rootMidiNote = 55;   // +12 semitones from the played note 67
    e.applyTuning(t);
    e.process(silence.data(), out.data(), 128);
    CHECK(spy->lastRatio == doctest::Approx(2.0));   // one octave
}

TEST_CASE("applyTuning selects the blend policy from Tuning::kind") {
    Engine e;
    EngineConfig cfg;
    cfg.sampleRate = 48000.0;
    cfg.maxBlock = 128;
    e.prepare(cfg, std::make_unique<YinAnalyzer>(),
             []() { return std::make_unique<PassthroughShifter>(); },
             []() { return std::make_unique<PassthroughShifter>(); });

    Tuning t{};
    t.kind = BlendKind::QualityOnly;
    e.applyTuning(t);
    CHECK(std::string(e.blendPolicy()->name()) == "quality-only");

    t.kind = BlendKind::RegisterSplit;
    t.rsCrossoverHz = 300.0f;
    t.rsWidthOctaves = 2.0f;
    e.applyTuning(t);
    CHECK(std::string(e.blendPolicy()->name()) == "register-split");
    const auto* rs = dynamic_cast<const RegisterSplitPolicy*>(e.blendPolicy());
    REQUIRE(rs != nullptr);
    CHECK(rs->crossoverHz == doctest::Approx(300.0f));
    CHECK(rs->widthOctaves == doctest::Approx(2.0f));

    t.kind = BlendKind::OnsetHandover;
    t.ohStartMs = 10.0f;
    t.ohEndMs = 40.0f;
    e.applyTuning(t);
    CHECK(std::string(e.blendPolicy()->name()) == "onset-handover");
    const auto* oh = dynamic_cast<const OnsetHandoverPolicy*>(e.blendPolicy());
    REQUIRE(oh != nullptr);
    CHECK(oh->handoverStartMs == doctest::Approx(10.0f));
    CHECK(oh->handoverEndMs == doctest::Approx(40.0f));

    t.kind = BlendKind::FastOnly;
    e.applyTuning(t);
    CHECK(std::string(e.blendPolicy()->name()) == "fast-only");
}

TEST_CASE("a custom blend policy set via setBlendPolicy takes precedence over Tuning::kind") {
    // app-design.md §4.4: "a custom policy takes precedence over the enum."
    Engine e;
    EngineConfig cfg;
    cfg.sampleRate = 48000.0;
    cfg.maxBlock = 128;
    e.prepare(cfg, std::make_unique<YinAnalyzer>(),
             []() { return std::make_unique<PassthroughShifter>(); },
             []() { return std::make_unique<PassthroughShifter>(); });

    QualityOnlyPolicy custom;
    e.setBlendPolicy(&custom);

    Tuning t{};
    t.kind = BlendKind::FastOnly;   // would normally select fast-only
    e.applyTuning(t);
    CHECK(e.blendPolicy() == &custom);   // override still active, untouched by applyTuning

    e.setBlendPolicy(nullptr);   // release the override
    e.applyTuning(t);
    CHECK(std::string(e.blendPolicy()->name()) == "fast-only");   // the enum takes over again
}

TEST_CASE("clamp() keeps a spread at the new 50 ms maximum inside the buffer it asked for") {
    // app-design.md §4.5 item 2's hazard, proven end-to-end rather than merely at the
    // clamp() unit level: pushing a live decorrelation spread past the buffer's ACTUAL
    // sizing bound makes `dline[(dw - decorDelay) & dmask]` (engine.cpp) wrap and read the
    // WRONG-AGED sample -- a much SHORTER, silently incorrect delay, not a crash. This is
    // the test that would have failed had engine.cpp's decorStride_ sizing literal been
    // left at its old 0.010 while only tuning.hpp's clamp() bound was raised to 50 ms --
    // exactly the "two bounds move together or they silently disagree again" hazard
    // app-design.md's own audit calls out.
    Engine e;
    EngineConfig cfg;
    cfg.sampleRate = 48000.0;
    cfg.maxBlock = 128;
    cfg.preservation.unvoiced = UnvoicedPolicy::PassThroughDecorrelated;
    e.prepare(cfg, std::make_unique<YinAnalyzer>(),
             []() { return std::make_unique<PassthroughShifter>(); }, nullptr);

    Tuning t{};
    t.decorMinMs = 50.0f;
    t.decorMaxMs = 50.0f;   // pinned at the new ceiling: every voice gets exactly 50 ms
    e.applyTuning(t);
    e.noteOn(60, 1.0f);

    constexpr FrameCount kBlock = 128;
    constexpr double kSR = 48000.0;
    const size_t totalFrames = kBlock * 300;
    const size_t impulseAt = kBlock * 10;   // runway before it, past note-on's lookback gate

    std::vector<Sample> in(totalFrames, 0.0f);
    in[impulseAt] = 1.0f;
    std::vector<Sample> out(totalFrames, 0.0f);
    for (size_t i = 0; i + kBlock <= totalFrames; i += kBlock) {
        e.process(in.data() + i, out.data() + i, kBlock);
    }

    size_t peakIdx = 0;
    float peakVal = 0.0f;
    for (size_t i = impulseAt; i < totalFrames; ++i) {
        if (std::fabs(out[static_cast<size_t>(i)]) > peakVal) {
            peakVal = std::fabs(out[static_cast<size_t>(i)]);
            peakIdx = i;
        }
    }
    REQUIRE(peakVal > 0.1f);   // the impulse's decorrelated copy actually arrived
    const double gotDelayMs = static_cast<double>(peakIdx - impulseAt) * 1000.0 / kSR;
    CHECK(gotDelayMs > 40.0);   // near the requested 50 ms, not wrapped to a tiny offset
    CHECK(gotDelayMs < 60.0);
}

TEST_CASE("Engine::applyTuning allocates nothing on the audio thread") {
    // The end-to-end version of tests/test_rt_safety.cpp's extension for applyTuning():
    // included here too so this file's own governing-property test and the RT-safety
    // claim for the same function sit next to each other.
    Engine e;
    EngineConfig cfg;
    cfg.sampleRate = 48000.0;
    cfg.maxBlock = 128;
    e.prepare(cfg, std::make_unique<YinAnalyzer>(),
             []() { return std::make_unique<GranularShifter>(); },
             []() { return std::make_unique<PsolaShifter>(); });
    e.noteOn(64, 1.0f);

    Tuning t{};
    t.kind = BlendKind::RegisterSplit;
    t.shifters.psola.handoverMs = 12.0f;
    t.analyzer.maxHoldMs = 150.0f;

    std::vector<Sample> in(128, 0.1f), out(128, 0.0f);
    rt::resetViolationCount();
    for (int i = 0; i < 50; ++i) {
        t.rootMidiNote = 60 + (i % 12);
        e.applyTuning(t);
        e.process(in.data(), out.data(), 128);
    }
    CHECK(rt::violationCount() == 0);
}
