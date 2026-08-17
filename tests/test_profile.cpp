// tests/test_profile.cpp — vh_profile: does a .vhprofile file actually mean what it says.
//
// THE PROPERTY THIS FILE EXISTS TO PROTECT (docs/app-design.md §7's own payoff): a profile
// tuned by ear in the app must become the LITERAL input to vh_render/vh_sweep, which only
// holds if save()-then-load() reproduces every field of vh::Tuning exactly, if a profile
// saved by an older build still loads sanely under a newer one (missing keys keep their
// compiled default), and if a hand-edited file that is wrong in some way FAILS instead of
// quietly feeding the audio thread garbage — hopSamples = 0 is app-design.md §4.5 item 1's
// own example of what "quietly" can cost.
//
// Every file-based test below writes into a per-test path under the system temp directory
// rather than anywhere in the repo, so this file behaves the same whether ctest is run from
// the build directory or vh_tests.exe is run directly, per README.md's documented commands.

#include <doctest/doctest.h>

#include "vh/profile.hpp"
#include "vh/tuning.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

using namespace vh;
using namespace vh::profile;

namespace {

// One path per test case name, so parallel/rerun test invocations do not collide and a
// failed run's file is easy to find by name if it needs inspecting by hand.
std::filesystem::path tempProfilePath(const char* tag) {
    return std::filesystem::temp_directory_path() /
           (std::string("vh_profile_test_") + tag + ".vhprofile");
}

void writeRawFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    REQUIRE(static_cast<bool>(f));
    f << content;
}

// Field-by-field equality, mirroring tests/test_tuning.cpp's own sameTuning() helper (not
// reused directly -- that one is file-local to a different translation unit). NOT memcmp,
// for the identical reason given there: padding bytes are not part of the property under
// test.
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

// Every field set to a value distinct from Tuning{}'s default AND already inside every
// bound Tuning::clamp() enforces -- see the file header on load()'s unconditional clamp()
// call. Picking values that clamp() would leave untouched is what makes this struct usable
// for an EXACT round-trip test: if clamp() legitimately altered a field, a mismatch after
// load() would not distinguish "the serializer lost a field" from "clamp() did its job",
// which is a different property, covered by its own tests further down.
Tuning distinctiveTuning() {
    Tuning t{};
    t.schemaVersion = kTuningSchemaVersion;

    t.mode = RatioSource::IntervalFromRoot;
    t.rootMidiNote = 55;
    t.voiceCap = 9;
    t.attackMs = 12.5f;
    t.releaseMs = 33.25f;
    t.glideSemisPerSec = 4.75f;
    t.freezeWindowMs = 321.0f;
    t.velocityToGainMin = 0.4f;   // clamped to [0,1] -- already inside, unaffected by clamp()
    t.velocityCurve = 2.5f;       // floored at 0.01 -- already above it, unaffected

    t.preservation.preserveEnvelope = false;
    t.preservation.envelopeWarp = 0.42f;
    t.preservation.scaleWarpWithShift = false;
    t.preservation.preserveAperiodicity = false;
    t.preservation.vibratoConstantInCents = false;
    t.preservation.unvoiced = UnvoicedPolicy::Shift;
    t.preservation.preserveTiming = false;

    auto& vp = t.preservation.voicing;
    vp.enabled = false;
    vp.kMu = 0.123456f;
    vp.muStrength = 0.654321f;
    vp.muMin = 0.111f;
    vp.muMax = 3.333f;
    vp.kTilt = 0.777f;
    vp.tiltStrength = 0.222f;
    vp.tiltGainMin = 0.05f;
    vp.tiltGainMax = 6.75f;
    vp.tiltCornerInF0 = 1.5f;

    t.kind = BlendKind::OnsetHandover;
    t.rsCrossoverHz = 314.159f;
    t.rsWidthOctaves = 2.5f;
    t.ohStartMs = 12.0f;
    t.ohEndMs = 88.0f;
    t.alignment = 0.375f;

    t.humanize.detuneCents = 6.25f;
    t.humanize.detuneDriftCents = 1.5f;
    t.humanize.onsetJitterMs = 7.0f;
    t.humanize.vibratoRateHz = 5.75f;
    t.humanize.vibratoDepthCents = 18.0f;
    t.humanize.envelopeWarpSpread = 0.9f;
    t.humanize.panSpread = 0.6f;

    t.decorMinMs = 1.25f;
    t.decorMaxMs = 9.0f;

    t.shifters.granular.jumpPeriods = 3;
    t.shifters.granular.fadeFraction = 0.35f;
    t.shifters.granular.unvoicedGrainMs = 11.0f;
    t.shifters.granular.geometrySmoothMs = 27.0f;
    t.shifters.psola.handoverMs = 5.0f;

    t.analyzer.threshold = 0.22f;
    t.analyzer.hopSamples = 256;
    t.analyzer.holdThroughUnvoiced = false;
    t.analyzer.maxHoldMs = 175.0f;
    t.analyzer.releaseHops = 6;
    t.analyzer.maxStepCents = 90.0f;
    t.analyzer.jumpConfirmHops = 5;
    t.analyzer.epochCutoffRatio = 2.1f;
    t.analyzer.epochLoudnessFactor = 0.4f;
    t.analyzer.epochRefractoryFactor = 0.5f;
    t.analyzer.epochEnvelopeRelease = 0.777f;
    t.analyzer.epochCutoffMinHz = 100.0f;
    t.analyzer.epochCutoffMaxHz = 1800.0f;

    t.inputTrimDb = -6.0f;
    t.inputGateOn = true;
    t.inputGateDb = -45.0f;
    t.dryGain = 0.3f;
    t.wetGain = 0.8f;
    t.outCeilingDb = -1.0f;
    t.limiterOn = true;

    return t;
}

} // namespace

// ============================================================================
// PART 1 — exact round-trip.
// ============================================================================

TEST_CASE("save() then load() reproduces every Tuning field exactly") {
    const auto path = tempProfilePath("roundtrip");
    const Tuning original = distinctiveTuning();
    ProfileMeta meta;
    meta.name = "Round-Trip Test Profile";
    meta.notes = "Line one.\nLine two with a \\backslash\\ and \"quotes\".\nFinal line.";

    std::string err;
    REQUIRE(save(original, meta, path.string(), err));

    Tuning loaded{};
    ProfileMeta loadedMeta;
    LoadReport report;
    REQUIRE(load(path.string(), loaded, loadedMeta, report));
    CHECK(report.ok);
    CHECK(report.warnings.empty());   // every field known, version matches -- nothing to warn about

    CHECK(sameTuning(original, loaded));
    CHECK(loadedMeta.name == meta.name);
    CHECK(loadedMeta.notes == meta.notes);

    std::filesystem::remove(path);
}

TEST_CASE("Tuning{} itself round-trips (the common case, not just the stress case)") {
    const auto path = tempProfilePath("roundtrip_default");
    const Tuning original{};
    const ProfileMeta meta{};   // empty name/notes -- must also round-trip

    std::string err;
    REQUIRE(save(original, meta, path.string(), err));

    Tuning loaded{};
    ProfileMeta loadedMeta;
    LoadReport report;
    REQUIRE(load(path.string(), loaded, loadedMeta, report));
    CHECK(sameTuning(original, loaded));
    CHECK(loadedMeta.name.empty());
    CHECK(loadedMeta.notes.empty());

    std::filesystem::remove(path);
}

TEST_CASE("float precision survives round-trip for values default formatting would mangle") {
    // Values chosen to need more than a handful of significant digits to round-trip exactly
    // -- the reason profile.cpp uses std::to_chars' shortest-round-trip mode rather than a
    // fixed precision. If a fixed precision were too low, THIS test is the one that would
    // catch it (the struct-default values are mostly short decimals that a low precision
    // would pass by accident).
    const auto path = tempProfilePath("float_precision");
    Tuning original{};
    // Runtime-computed rather than literal, so the compiler cannot quietly round these to
    // whatever "nice" decimal a hand-picked literal would already be -- each is whatever
    // float32 the division/multiplication actually produces, needing the full mantissa's
    // worth of decimal digits to round-trip exactly.
    original.preservation.voicing.kMu = 1.0f / 3.0f;
    original.rsCrossoverHz = 2.0f / 7.0f;
    // Stays safely under clamp()'s 0.999999 ceiling for this field (see Tuning::clamp()
    // item 8) -- this test is about PRECISION surviving, not about probing that bound,
    // which the dedicated PART 6 tests already cover.
    original.analyzer.epochEnvelopeRelease = 5.0f / 7.0f;

    std::string err;
    REQUIRE(save(original, ProfileMeta{}, path.string(), err));

    Tuning loaded{};
    ProfileMeta m;
    LoadReport report;
    REQUIRE(load(path.string(), loaded, m, report));

    // Bit-exact, not doctest::Approx -- that is the whole property under test.
    CHECK(loaded.preservation.voicing.kMu == original.preservation.voicing.kMu);
    CHECK(loaded.rsCrossoverHz == original.rsCrossoverHz);
    CHECK(loaded.analyzer.epochEnvelopeRelease == original.analyzer.epochEnvelopeRelease);

    std::filesystem::remove(path);
}

// ============================================================================
// PART 2 — missing keys keep the struct default (the migration guarantee).
// ============================================================================

TEST_CASE("missing keys keep vh::Tuning{}'s compiled default, not the caller's prior value") {
    const auto path = tempProfilePath("missing_keys");
    // A minimal, well-formed file: just schema_version and ONE field. Everything else this
    // build knows about must come out equal to Tuning{}'s own default.
    writeRawFile(path,
        "schema_version = " + std::to_string(kTuningSchemaVersion) + "\n" +
        "root_midi_note = 67\n");

    // Seed the output with values that are NOT the default and NOT what the file sets, so a
    // bug that left stale caller state in place (rather than starting from Tuning{}) would
    // be caught rather than accidentally matching.
    Tuning t{};
    t.rootMidiNote = 1;
    t.attackMs = 999.0f;
    t.voiceCap = 1;
    ProfileMeta m;
    m.name = "stale";

    LoadReport report;
    REQUIRE(load(path.string(), t, m, report));
    CHECK(report.ok);

    CHECK(t.rootMidiNote == 67);          // the one field the file DID set
    const Tuning defaults{};
    CHECK(t.attackMs == defaults.attackMs);       // untouched fields: compiled default
    CHECK(t.voiceCap == defaults.voiceCap);
    CHECK(sameHumanize(t.humanize, defaults.humanize));
    CHECK(samePreservation(t.preservation, defaults.preservation));
    CHECK(m.name.empty());                // meta.name absent from file -> default (empty),
                                           // not the caller's stale "stale"

    std::filesystem::remove(path);
}

// ============================================================================
// PART 3 — unknown keys warn, but the file still loads.
// ============================================================================

TEST_CASE("an unrecognized key is ignored with a warning, not a failure") {
    const auto path = tempProfilePath("unknown_key");
    // schema_version is interpolated from kTuningSchemaVersion, not hardcoded, so this
    // fixture keeps testing exactly the two UNKNOWN-key warnings below rather than
    // silently growing a third "schema_version mismatch" warning the next time a track
    // bumps the schema (Track C already did once — see tuning.hpp).
    writeRawFile(path,
        "schema_version = " + std::to_string(kTuningSchemaVersion) + "\n" +
        "root_midi_note = 72\n"
        "this_key_does_not_exist = 42\n"
        "shifters.granular.made_up_field = 1.0\n");

    Tuning t{};
    ProfileMeta m;
    LoadReport report;
    REQUIRE(load(path.string(), t, m, report));
    CHECK(report.ok);
    CHECK(t.rootMidiNote == 72);   // the recognized key still applied

    REQUIRE(report.warnings.size() == 2);
    bool sawFirst = false, sawSecond = false;
    for (const auto& w : report.warnings) {
        if (w.find("this_key_does_not_exist") != std::string::npos) sawFirst = true;
        if (w.find("shifters.granular.made_up_field") != std::string::npos) sawSecond = true;
    }
    CHECK(sawFirst);
    CHECK(sawSecond);

    std::filesystem::remove(path);
}

// ============================================================================
// PART 4 — malformed files fail cleanly: an error, not a crash, not a half-load.
// ============================================================================

TEST_CASE("a line with no '=' is a hard parse error") {
    const auto path = tempProfilePath("malformed_no_equals");
    writeRawFile(path,
        "schema_version = " + std::to_string(kTuningSchemaVersion) + "\n" +
        "this line has no equals sign\n");

    Tuning t{};
    t.rootMidiNote = 123;   // sentinel: must survive UNTOUCHED on failure
    ProfileMeta m;
    m.name = "sentinel";
    LoadReport report;
    CHECK_FALSE(load(path.string(), t, m, report));
    CHECK_FALSE(report.ok);
    CHECK_FALSE(report.error.empty());
    CHECK(report.error.find("line 2") != std::string::npos);

    // NO HALF-LOAD: the caller's struct must be exactly what it was before the call.
    CHECK(t.rootMidiNote == 123);
    CHECK(m.name == "sentinel");

    std::filesystem::remove(path);
}

TEST_CASE("missing schema_version is a hard error") {
    const auto path = tempProfilePath("no_schema_version");
    writeRawFile(path, "root_midi_note = 60\n");

    Tuning t{};
    ProfileMeta m;
    LoadReport report;
    CHECK_FALSE(load(path.string(), t, m, report));
    CHECK_FALSE(report.ok);
    CHECK(report.error.find("schema_version") != std::string::npos);

    std::filesystem::remove(path);
}

TEST_CASE("a duplicate key is a hard error, not first-wins or last-wins") {
    const auto path = tempProfilePath("duplicate_key");
    writeRawFile(path,
        "schema_version = " + std::to_string(kTuningSchemaVersion) + "\n" +
        "root_midi_note = 10\n"
        "root_midi_note = 20\n");

    Tuning t{};
    ProfileMeta m;
    LoadReport report;
    CHECK_FALSE(load(path.string(), t, m, report));
    CHECK_FALSE(report.ok);
    CHECK(report.error.find("root_midi_note") != std::string::npos);

    std::filesystem::remove(path);
}

TEST_CASE("an unparsable number fails cleanly and names the offending key") {
    const auto path = tempProfilePath("bad_number");
    writeRawFile(path,
        "schema_version = " + std::to_string(kTuningSchemaVersion) + "\n" +
        "attack_ms = not_a_number\n");

    Tuning t{};
    ProfileMeta m;
    LoadReport report;
    CHECK_FALSE(load(path.string(), t, m, report));
    CHECK_FALSE(report.ok);
    CHECK(report.error.find("attack_ms") != std::string::npos);

    std::filesystem::remove(path);
}

TEST_CASE("an unrecognized enum value fails cleanly and names the offending key") {
    const auto path = tempProfilePath("bad_enum");
    writeRawFile(path,
        "schema_version = " + std::to_string(kTuningSchemaVersion) + "\n" +
        "mode = SidewaysTarget\n");

    Tuning t{};
    ProfileMeta m;
    LoadReport report;
    CHECK_FALSE(load(path.string(), t, m, report));
    CHECK_FALSE(report.ok);
    CHECK(report.error.find("mode") != std::string::npos);

    std::filesystem::remove(path);
}

TEST_CASE("an invalid bool value fails cleanly") {
    const auto path = tempProfilePath("bad_bool");
    writeRawFile(path,
        "schema_version = " + std::to_string(kTuningSchemaVersion) + "\n" +
        "limiter_on = maybe\n");

    Tuning t{};
    ProfileMeta m;
    LoadReport report;
    CHECK_FALSE(load(path.string(), t, m, report));
    CHECK_FALSE(report.ok);
    CHECK(report.error.find("limiter_on") != std::string::npos);

    std::filesystem::remove(path);
}

TEST_CASE("a non-finite float value is rejected rather than accepted") {
    const auto path = tempProfilePath("bad_finite");
    writeRawFile(path,
        "schema_version = " + std::to_string(kTuningSchemaVersion) + "\n" +
        "attack_ms = nan\n");

    Tuning t{};
    ProfileMeta m;
    LoadReport report;
    CHECK_FALSE(load(path.string(), t, m, report));
    CHECK_FALSE(report.ok);

    std::filesystem::remove(path);
}

TEST_CASE("a nonexistent file fails cleanly") {
    Tuning t{};
    ProfileMeta m;
    LoadReport report;
    CHECK_FALSE(load((std::filesystem::temp_directory_path() / "vh_profile_does_not_exist.vhprofile").string(),
                      t, m, report));
    CHECK_FALSE(report.ok);
    CHECK_FALSE(report.error.empty());
}

TEST_CASE("save() to an unwritable path fails cleanly instead of crashing") {
    const auto path = std::filesystem::temp_directory_path() /
                       "vh_profile_test_nonexistent_dir_xyz" / "x.vhprofile";
    std::string err;
    CHECK_FALSE(save(Tuning{}, ProfileMeta{}, path.string(), err));
    CHECK_FALSE(err.empty());
}

// ============================================================================
// PART 5 — enum names survive as NAMES, not integers.
// ============================================================================

TEST_CASE("every RatioSource value round-trips through its name") {
    for (RatioSource mode : {RatioSource::AbsoluteTarget, RatioSource::IntervalFromRoot}) {
        const auto path = tempProfilePath("enum_ratio_source");
        Tuning t{};
        t.mode = mode;
        std::string err;
        REQUIRE(save(t, ProfileMeta{}, path.string(), err));

        Tuning loaded{};
        ProfileMeta m;
        LoadReport report;
        REQUIRE(load(path.string(), loaded, m, report));
        CHECK(loaded.mode == mode);
        std::filesystem::remove(path);
    }
}

TEST_CASE("every BlendKind value round-trips through its name") {
    for (BlendKind kind : {BlendKind::FastOnly, BlendKind::QualityOnly,
                            BlendKind::RegisterSplit, BlendKind::OnsetHandover}) {
        const auto path = tempProfilePath("enum_blend_kind");
        Tuning t{};
        t.kind = kind;
        std::string err;
        REQUIRE(save(t, ProfileMeta{}, path.string(), err));

        Tuning loaded{};
        ProfileMeta m;
        LoadReport report;
        REQUIRE(load(path.string(), loaded, m, report));
        CHECK(loaded.kind == kind);
        std::filesystem::remove(path);
    }
}

TEST_CASE("every UnvoicedPolicy value round-trips through its name") {
    for (UnvoicedPolicy pol : {UnvoicedPolicy::PassThrough, UnvoicedPolicy::PassThroughDecorrelated,
                                UnvoicedPolicy::Shift}) {
        const auto path = tempProfilePath("enum_unvoiced_policy");
        Tuning t{};
        t.preservation.unvoiced = pol;
        std::string err;
        REQUIRE(save(t, ProfileMeta{}, path.string(), err));

        Tuning loaded{};
        ProfileMeta m;
        LoadReport report;
        REQUIRE(load(path.string(), loaded, m, report));
        CHECK(loaded.preservation.unvoiced == pol);
        std::filesystem::remove(path);
    }
}

TEST_CASE("saved enum values are written as stable names, not integers") {
    // Directly inspects the file text -- the actual guarantee app-design.md §7 asks for
    // ("an integer enum in a hand-editable file is a trap"), not just that round-tripping
    // happens to work.
    const auto path = tempProfilePath("enum_is_text");
    Tuning t{};
    t.mode = RatioSource::IntervalFromRoot;
    t.kind = BlendKind::RegisterSplit;
    t.preservation.unvoiced = UnvoicedPolicy::PassThrough;
    std::string err;
    REQUIRE(save(t, ProfileMeta{}, path.string(), err));

    std::ifstream f(path, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    // Pre-existing bug found in passing while verifying Track C's schema bump did not break
    // this file: f was never closed before remove() below, and on Windows a still-open
    // ifstream handle can make DeleteFile fail with ERROR_SHARING_VIOLATION -- reproduced
    // deterministically (not flaky), every run, once actually exercised. Unrelated to
    // anything Track C changed; fixed here rather than left for the next person to
    // rediscover, since it is one line.
    f.close();
    CHECK(text.find("mode = IntervalFromRoot") != std::string::npos);
    CHECK(text.find("blend.kind = RegisterSplit") != std::string::npos);
    CHECK(text.find("preservation.unvoiced = PassThrough") != std::string::npos);

    std::filesystem::remove(path);
}

// ============================================================================
// PART 6 — clamp() is applied on load. This is the safety property, not a nicety.
// ============================================================================

TEST_CASE("load() clamps hopSamples = 0 to a safe floor instead of hanging the audio thread") {
    // app-design.md §4.5 item 1: at hopSamples == 0, yin.cpp's analysis hop loop never
    // advances -- an infinite loop INSIDE THE AUDIO CALLBACK, not a click. A .vhprofile is
    // exactly the hand-editable, untrusted input that hazard is about.
    const auto path = tempProfilePath("clamp_hop_zero");
    writeRawFile(path,
        "schema_version = " + std::to_string(kTuningSchemaVersion) + "\n" +
        "analyzer.hop_samples = 0\n");

    Tuning t{};
    ProfileMeta m;
    LoadReport report;
    REQUIRE(load(path.string(), t, m, report));
    CHECK(report.ok);
    CHECK(t.analyzer.hopSamples >= 32);   // Tuning::clamp()'s own floor

    std::filesystem::remove(path);
}

TEST_CASE("load() clamps an inverted mu range instead of leaving undefined-behaviour bait") {
    // voicing.hpp's muFor() calls std::clamp(v, muMin, muMax) unconditionally; muMin > muMax
    // is undefined behaviour there, not merely a wrong number.
    const auto path = tempProfilePath("clamp_mu_inverted");
    writeRawFile(path,
        "schema_version = " + std::to_string(kTuningSchemaVersion) + "\n" +
        "preservation.voicing.mu_min = 5.0\n"
        "preservation.voicing.mu_max = 0.1\n");

    Tuning t{};
    ProfileMeta m;
    LoadReport report;
    REQUIRE(load(path.string(), t, m, report));
    CHECK(t.preservation.voicing.muMin <= t.preservation.voicing.muMax);

    std::filesystem::remove(path);
}

TEST_CASE("load() clamps an out-of-range voiceCap") {
    const auto path = tempProfilePath("clamp_voice_cap");
    writeRawFile(path,
        "schema_version = " + std::to_string(kTuningSchemaVersion) + "\n" +
        "voice_cap = 999\n");

    Tuning t{};
    ProfileMeta m;
    LoadReport report;
    REQUIRE(load(path.string(), t, m, report));
    CHECK(t.voiceCap <= kMaxVoices);

    std::filesystem::remove(path);
}

// ============================================================================
// PART 7 — schema version mismatch: warn and load, per profile.cpp's documented policy.
// ============================================================================

TEST_CASE("a schema_version below this build's is a warning, and the file still loads") {
    const auto path = tempProfilePath("schema_older");
    writeRawFile(path,
        "schema_version = 0\n"
        "root_midi_note = 61\n");

    Tuning t{};
    ProfileMeta m;
    LoadReport report;
    REQUIRE(load(path.string(), t, m, report));
    CHECK(report.ok);
    CHECK(report.fileSchemaVersion == 0);
    REQUIRE(report.warnings.size() == 1);
    CHECK(report.warnings[0].find("schema_version") != std::string::npos);
    CHECK(t.rootMidiNote == 61);   // the file still loaded and applied normally

    std::filesystem::remove(path);
}

TEST_CASE("a schema_version above this build's is a warning, and the file still loads") {
    const auto path = tempProfilePath("schema_newer");
    writeRawFile(path,
        "schema_version = 999999\n"
        "root_midi_note = 61\n");

    Tuning t{};
    ProfileMeta m;
    LoadReport report;
    REQUIRE(load(path.string(), t, m, report));
    CHECK(report.ok);
    CHECK(report.fileSchemaVersion == 999999);
    CHECK(report.warnings.size() == 1);
    CHECK(t.rootMidiNote == 61);

    std::filesystem::remove(path);
}

TEST_CASE("a matching schema_version produces no version warning") {
    const auto path = tempProfilePath("schema_match");
    Tuning t{};
    t.schemaVersion = kTuningSchemaVersion;
    std::string err;
    REQUIRE(save(t, ProfileMeta{}, path.string(), err));

    Tuning loaded{};
    ProfileMeta m;
    LoadReport report;
    REQUIRE(load(path.string(), loaded, m, report));
    CHECK(report.fileSchemaVersion == kTuningSchemaVersion);
    CHECK(report.warnings.empty());

    std::filesystem::remove(path);
}
