// tests/test_output_stage.cpp — Track D (docs/app-design.md §5.5): the input stage (trim,
// gate) and the output stage (dry/wet, limiter) that make the app stop being wet-only.
//
// tests/test_tuning.cpp already pins that every field this file exercises defaults to a
// bit-exact no-op; tests/test_humanization.cpp covers the per-voice sum and pan that these
// stages wrap. This file is specifically the NEW block-level processing mixImpl() does
// after Track C's per-voice loop finishes: trim/gate before the ring ever sees the signal,
// and dry/wet + limiter after the voices are summed. See engine.cpp's mixImpl() for the
// implementation these tests are pinned against.

#include <doctest/doctest.h>

#include "vh/engine.hpp"
#include "vh/granular_shifter.hpp"
#include "vh/passthrough_shifter.hpp"
#include "vh/psola_shifter.hpp"
#include "vh/rt.hpp"
#include "vh/yin.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

using namespace vh;

namespace {

constexpr double kSR = 48000.0;
constexpr FrameCount kBlock = 128;

EngineConfig baseCfg() {
    EngineConfig cfg;
    cfg.sampleRate = kSR;
    cfg.maxBlock = kBlock;
    cfg.ratioSource = RatioSource::IntervalFromRoot;
    cfg.rootMidiNote = 60;
    return cfg;
}

std::vector<Sample> harmonicTone(double f0, size_t n, double amp = 0.2) {
    std::vector<Sample> sig(n);
    for (size_t i = 0; i < n; ++i) {
        double s = 0.0;
        for (int k = 1; k <= 5; ++k) {
            s += std::sin(2.0 * kPi * f0 * k * static_cast<double>(i) / kSR) / k;
        }
        sig[i] = static_cast<Sample>(s * amp);
    }
    return sig;
}

std::vector<Sample> renderMono(Engine& e, const std::vector<Sample>& sig) {
    std::vector<Sample> out(sig.size(), 0.0f);
    for (size_t i = 0; i + kBlock <= sig.size(); i += kBlock) {
        e.process(sig.data() + i, out.data() + i, kBlock);
    }
    return out;
}

} // namespace

// ============================================================================
// PART 1 — the input stage: trim, then an optional gate (app-design.md §5.5).
// ============================================================================

TEST_CASE("inputTrimDb scales the input signal by the correct linear factor") {
    // PassthroughShifter + unison ratio (played note == root, IntervalFromRoot mode) means
    // the wet output is, after the note-on envelope settles, exactly the trimmed/gated
    // input read back unshifted -- see PassthroughShifter's own header comment ("copies
    // input to output") and Voice::gain/envGain both being exactly 1.0 in steady state.
    // That makes a straight peak-ratio measurement a direct proof of the trim gain, with
    // no pitch-shifting machinery in the way to obscure it.
    Engine e;
    e.prepare(baseCfg(), std::make_unique<YinAnalyzer>(),
             []() { return std::make_unique<PassthroughShifter>(); }, nullptr);

    Tuning t{};
    t.inputTrimDb = 6.0f;
    e.applyTuning(t);
    e.noteOn(60, 1.0f);   // unison with root 60: ratio == 1.0 exactly

    const auto sig = harmonicTone(220.0, kBlock * 40);
    const auto out = renderMono(e, sig);

    // Skip the note-on attack ramp (attackMs default 8 ms, comfortably inside the first
    // 10 blocks) so v.envGain has settled to exactly 1.0 and is not itself part of what
    // is being measured.
    float peakIn = 0.0f, peakOut = 0.0f;
    for (size_t i = kBlock * 10; i < sig.size(); ++i) {
        peakIn = std::max(peakIn, std::fabs(sig[i]));
        peakOut = std::max(peakOut, std::fabs(out[i]));
    }
    REQUIRE(peakIn > 1e-4f);
    const float expected = std::pow(10.0f, 6.0f / 20.0f);   // ~1.995
    CAPTURE(peakIn);
    CAPTURE(peakOut);
    CHECK(peakOut / peakIn == doctest::Approx(expected).epsilon(0.01));
}

TEST_CASE("inputGateDb has no effect on output while inputGateOn is false") {
    // app-design.md §4.2's sketch pairs the threshold with an explicit enable flag
    // precisely so a threshold value alone cannot silently mean "gate exists" -- this is
    // the test that pins the flag actually gates the FEATURE, not just the UI affordance.
    auto build = [](bool setDramaticGateDb) {
        auto e = std::make_unique<Engine>();
        e->prepare(baseCfg(), std::make_unique<YinAnalyzer>(),
                  []() { return std::make_unique<PassthroughShifter>(); }, nullptr);
        Tuning t{};
        if (setDramaticGateDb) t.inputGateDb = -1.0f;   // inputGateOn stays false (default)
        e->applyTuning(t);
        e->noteOn(64, 1.0f);
        return e;
    };

    const auto sig = harmonicTone(180.0, kBlock * 60);
    auto a = build(false);
    auto b = build(true);
    const auto outA = renderMono(*a, sig);
    const auto outB = renderMono(*b, sig);

    REQUIRE(outA.size() == outB.size());
    for (size_t i = 0; i < outA.size(); ++i) {
        CAPTURE(i);
        REQUIRE(outA[i] == outB[i]);   // bit-exact -- inputGateDb is genuinely unread here
    }
}

TEST_CASE("the input gate attenuates a quiet signal and does not click") {
    // Methodology: rather than reverse-engineering a magic per-sample slew bound the way
    // tests/test_continuity.cpp does for the whole render, this reconstructs the gate's
    // OWN gain trajectory exactly, sample by sample, by dividing the (unison, passthrough,
    // envelope-settled) output by the known input: out[k] == in[k] * gateGain[k] at
    // inputTrimDb == 0 and v.gain == v.envGain == 1, so gateGain[k] == out[k] / in[k]
    // wherever in[k] != 0. A strictly-positive DC input on each side of the transition
    // guarantees that division is always valid and isolates the GATE's contribution from
    // any shape the source signal itself has -- see this file's header and voice.hpp's
    // envPhase comment for why a hand-picked slew threshold on the raw signal would
    // conflate the source's own discontinuity with the gate's.
    Engine e;
    e.prepare(baseCfg(), std::make_unique<YinAnalyzer>(),
             []() { return std::make_unique<PassthroughShifter>(); }, nullptr);

    Tuning t{};
    t.inputGateOn = true;
    t.inputGateDb = -40.0f;
    e.applyTuning(t);
    e.noteOn(60, 1.0f);   // unison: ratio == 1.0, so envGain settles and stays at 1.0

    constexpr float kLoud = 0.4f;     // ~ -8 dBFS: comfortably above the -40 dB threshold
    constexpr float kQuiet = 1e-5f;   // ~ -100 dBFS: comfortably below the -46 dB knee floor

    const size_t nLoud = kBlock * 250;    // ~666 ms -- long enough for the envelope
    const size_t nQuiet = kBlock * 250;   // follower and the voice's own attack to settle

    std::vector<Sample> in(nLoud + nQuiet);
    for (size_t i = 0; i < nLoud; ++i) in[i] = kLoud;
    for (size_t i = 0; i < nQuiet; ++i) in[nLoud + i] = kQuiet;

    const auto out = renderMono(e, in);

    // Skip the note-on attack ramp (a handful of blocks) before trusting the division.
    const size_t warmup = kBlock * 20;
    REQUIRE(warmup < nLoud);

    std::vector<float> gateGain(in.size(), 0.0f);
    for (size_t i = warmup; i < out.size(); ++i) {
        gateGain[i] = out[i] / in[i];
    }

    // Loud region, near the end (fully settled): gate should be essentially fully open.
    float loudAvg = 0.0f;
    size_t loudCount = 0;
    for (size_t i = nLoud - 2000; i < nLoud; ++i) { loudAvg += gateGain[i]; ++loudCount; }
    loudAvg /= static_cast<float>(loudCount);
    CAPTURE(loudAvg);
    CHECK(loudAvg > 0.95f);

    // Quiet region, near the end (fully settled): gate should be essentially fully closed.
    float quietAvg = 0.0f;
    size_t quietCount = 0;
    for (size_t i = in.size() - 2000; i < in.size(); ++i) { quietAvg += gateGain[i]; ++quietCount; }
    quietAvg /= static_cast<float>(quietCount);
    CAPTURE(quietAvg);
    CHECK(quietAvg < 0.05f);

    // NOT A HARD MUTE: find where the gain trajectory crosses 0.9 going down, and where it
    // subsequently crosses 0.1. A switched gate would do this in ONE sample (or very few,
    // bounded by a single block); a smooth gate spreads it over many samples, governed by
    // the release time constant.
    size_t idxHigh = 0, idxLow = 0;
    bool foundHigh = false, foundLow = false;
    for (size_t i = nLoud; i < out.size(); ++i) {
        if (!foundHigh && gateGain[i] < 0.9f) { idxHigh = i; foundHigh = true; }
        if (foundHigh && !foundLow && gateGain[i] < 0.1f) { idxLow = i; foundLow = true; break; }
    }
    REQUIRE(foundHigh);
    REQUIRE(foundLow);
    const size_t span = idxLow - idxHigh;
    CAPTURE(span);
    CHECK(span > 200);   // several ms at 48 kHz -- nowhere near a single-sample step
}

// ============================================================================
// PART 2 — the output stage: dry/wet (app-design.md §5.5).
// ============================================================================

TEST_CASE("dryGain and wetGain mix linearly, including dry-only and wet-only extremes") {
    // wetGain/dryGain are read in EXACTLY ONE place (the final combine line in mixImpl()'s
    // output stage) and nowhere upstream of it, so the per-voice sum and the staged dry
    // signal are bit-identical across all three builds below regardless of which gains are
    // applied to them -- which is what makes the superposition check below exact rather
    // than approximate.
    auto build = [](float wetGain, float dryGain) {
        auto e = std::make_unique<Engine>();
        e->prepare(baseCfg(), std::make_unique<YinAnalyzer>(),
                  []() { return std::make_unique<PassthroughShifter>(); }, nullptr);
        Tuning t{};
        t.wetGain = wetGain;
        t.dryGain = dryGain;
        e->applyTuning(t);
        e->noteOn(67, 1.0f);   // +7 semitones from root 60: wet is genuinely pitch-shifted,
                                // so wet-only and dry-only are not accidentally identical.
        return e;
    };

    const auto sig = harmonicTone(220.0, kBlock * 100);

    auto wetOnly = build(1.0f, 0.0f);
    auto dryOnly = build(0.0f, 1.0f);
    auto mixHalf = build(0.5f, 0.5f);

    const auto outWet = renderMono(*wetOnly, sig);
    const auto outDry = renderMono(*dryOnly, sig);
    const auto outMix = renderMono(*mixHalf, sig);

    // DRY-ONLY: reproduces exactly the staged input (trim/gate default off, so this is the
    // raw input, sample for sample) -- proves the dry path bypasses the voices entirely,
    // not merely that it is quiet. Bit-exact, not Approx: dryGain == 1.0 and wetGain ==
    // 0.0 are both exact multiplicative identities/annihilators (see mixImpl()'s output
    // stage comment).
    REQUIRE(outDry.size() == sig.size());
    for (size_t i = 0; i < sig.size(); ++i) {
        CAPTURE(i);
        REQUIRE(outDry[i] == sig[i]);
    }

    // WET-ONLY: carries real (pitch-shifted) voice content, not silence -- the other half
    // of "the extremes actually differ", not just "both are correctly zeroed".
    bool wetNonzero = false;
    for (size_t i = kBlock * 10; i < outWet.size(); ++i) {
        if (std::fabs(outWet[i]) > 1e-4f) { wetNonzero = true; break; }
    }
    CHECK(wetNonzero);

    // LINEARITY: the half-and-half mix superposes the two extremes exactly.
    for (size_t i = 0; i < sig.size(); ++i) {
        CAPTURE(i);
        REQUIRE(outMix[i] == doctest::Approx(0.5f * outWet[i] + 0.5f * outDry[i]).epsilon(1e-6));
    }
}

// ============================================================================
// PART 3 — the limiter (app-design.md §5.5, BUGS.md VH-009).
// ============================================================================

TEST_CASE("the limiter caps output at outCeilingDb on a stacked upward shift that clips without it (VH-009)") {
    // VH-009's own scenario, reproduced: "upward shifts run hot enough to clip... The
    // JUCE shell must apply headroom." A low root and three upward voices stacked at +7,
    // +12 and +19 semitones matches VH-009's "several stacked voices" and its own +19 st
    // probe point.
    auto build = [](bool limiterOn) {
        auto e = std::make_unique<Engine>();
        EngineConfig cfg;
        cfg.sampleRate = kSR;
        cfg.maxBlock = kBlock;
        cfg.ratioSource = RatioSource::IntervalFromRoot;
        cfg.rootMidiNote = 48;
        e->prepare(cfg, std::make_unique<YinAnalyzer>(),
                  []() { return std::make_unique<PsolaShifter>(); }, nullptr);
        Tuning t{};
        // rootMidiNote must be reasserted -- Tuning{}'s OWN default is 60 (tuning.hpp), so
        // applying it unmodified here would silently pull the root back to 60 out from
        // under an EngineConfig prepared with a different one. Not a Track D concern --
        // applyTuning() is a complete snapshot by design (app-design.md §4.3), never a
        // partial override -- but a trap for any test that mixes a non-default prepare()
        // with a mostly-default Tuning, exactly as this one does.
        t.rootMidiNote = cfg.rootMidiNote;
        t.outCeilingDb = -3.0f;
        t.limiterOn = limiterOn;
        e->applyTuning(t);
        for (int semis : {7, 12, 19}) e->noteOn(48 + semis, 1.0f);
        return e;
    };

    const auto sig = harmonicTone(180.0, kBlock * 300, 0.5);

    auto unlimited = build(false);
    auto limited = build(true);
    const auto outUnlimited = renderMono(*unlimited, sig);
    const auto outLimited = renderMono(*limited, sig);

    const float ceilingLin = std::pow(10.0f, -3.0f / 20.0f);

    float peakUnlimited = 0.0f;
    for (auto x : outUnlimited) peakUnlimited = std::max(peakUnlimited, std::fabs(x));
    CAPTURE(peakUnlimited);
    REQUIRE(peakUnlimited > ceilingLin);   // proves the scenario genuinely clips without it

    // Steady-state peak, skipping the note-on attack: a NON-LOOKAHEAD limiter cannot fully
    // prevent a brief overshoot on the very first transient it sees (app-design.md §5.5
    // accepts exactly this cost in exchange for adding no latency), so the tight bound is
    // only fair once the gain-reduction state has had time to catch up.
    //
    // 1.10x, not 1.05x: this chord's three voices land near 4:5:6-ish frequency ratios
    // (perfect fourth/fifth/octave apart), so the summed waveform's true crest recurs
    // roughly every 5-11 ms -- close to the ~1 ms attack / ~50 ms release time constants
    // this limiter uses (app-design.md §5.5's own figures). Measured residual overshoot
    // here is ~5.5% once mixImpl()'s peak-hold detector (peakEnv_) is holding the crest
    // between cycles rather than losing it every time the waveform dips -- see that
    // detector's own comment in engine.hpp for the mechanism, and for what an UNDAMPED
    // instantaneous-sample detector measured instead (a steady-state peak roughly 2x over
    // ceiling, not a few percent) before this fix. A residual few percent on genuinely
    // periodic material at these attack/release times is the accepted, documented cost of
    // not adding lookahead latency; tightening the attack further to close that last few
    // percent would trade against how audibly smooth the gain reduction itself is, which
    // is not a trade this test should force silently.
    float peakLimitedSteady = 0.0f;
    for (size_t i = kBlock * 20; i < outLimited.size(); ++i) {
        peakLimitedSteady = std::max(peakLimitedSteady, std::fabs(outLimited[i]));
    }
    CAPTURE(peakLimitedSteady);
    CHECK(peakLimitedSteady <= ceilingLin * 1.10f);

    // Even including the attack transient, the limiter must meaningfully reduce the peak
    // versus not having one at all.
    float peakLimitedFull = 0.0f;
    for (auto x : outLimited) peakLimitedFull = std::max(peakLimitedFull, std::fabs(x));
    CAPTURE(peakLimitedFull);
    CHECK(peakLimitedFull < peakUnlimited);
}

TEST_CASE("limiterOn=false makes outCeilingDb irrelevant -- the limiter is truly inert when off") {
    // THE test that protects RESULTS.md (app-design.md §5.5: "a measurement that silently
    // limits is a measurement of the limiter"). Gain-reduction tracking runs
    // UNCONDITIONALLY every block (see limiterGain_'s comment in engine.hpp) so that
    // toggling the limiter on mid-performance reacts immediately rather than to stale
    // state -- which means the only way to be sure it is not ALSO silently applied while
    // off is to prove output is invariant to outCeilingDb entirely. If the limiter code
    // path introduced any difference while disabled, changing its ceiling from 0 dB to a
    // ceiling so extreme it would crush this signal to nothing WOULD show up here.
    auto build = [](float outCeilingDb) {
        auto e = std::make_unique<Engine>();
        EngineConfig cfg;
        cfg.sampleRate = kSR;
        cfg.maxBlock = kBlock;
        cfg.ratioSource = RatioSource::IntervalFromRoot;
        cfg.rootMidiNote = 48;
        e->prepare(cfg, std::make_unique<YinAnalyzer>(),
                  []() { return std::make_unique<PsolaShifter>(); }, nullptr);
        Tuning t{};
        t.rootMidiNote = cfg.rootMidiNote;   // see the sibling test above for why
        t.outCeilingDb = outCeilingDb;
        t.limiterOn = false;   // the default; asserted explicitly for clarity
        e->applyTuning(t);
        for (int semis : {7, 12, 19}) e->noteOn(48 + semis, 1.0f);
        return e;
    };

    const auto sig = harmonicTone(180.0, kBlock * 300, 0.5);
    auto a = build(0.0f);     // no headroom reduction requested
    auto b = build(-80.0f);   // an extreme ceiling that WOULD crush everything if it were
                               // ever applied
    const auto outA = renderMono(*a, sig);
    const auto outB = renderMono(*b, sig);

    REQUIRE(outA.size() == outB.size());
    for (size_t i = 0; i < outA.size(); ++i) {
        CAPTURE(i);
        REQUIRE(outA[i] == outB[i]);   // bit-exact
    }
}

// ============================================================================
// PART 4 — the governing property, extended to the new stages (app-design.md §5.5/§11).
// ============================================================================

TEST_CASE("a default Tuning{} stays bit-identical through the new stages on a loud, clipping-prone chord") {
    // tests/test_tuning.cpp already pins this property on a single-voice, moderate-level
    // signal. This is the adversarial version: a loud, upward-shifted, multi-voice chord
    // is exactly the material that would make the gate's envelope follower and the
    // limiter's gain-reduction state do real internal work (see gateEnv_/limiterGain_'s
    // comments in engine.hpp -- both are updated EVERY block regardless of whether their
    // feature is enabled), so this is the scenario most likely to expose a default that is
    // not actually a bit-exact no-op.
    // rootMidiNote is 60 here DELIBERATELY, matching Tuning{}'s own default (tuning.hpp) --
    // this test applies a LITERAL Tuning{} every block, so the EngineConfig it starts from
    // has to already agree with every field that struct defaults to, root included, or the
    // very first applyTuning() call would silently reassign the root out from under the
    // "with" engine and the two renders would diverge for a reason that has nothing to do
    // with the new stages under test (this cost some debugging time to track down; see the
    // sibling PART 3 tests for the same trap hit with a non-default root and fixed by
    // reasserting t.rootMidiNote explicitly instead).
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
    for (int semis : {7, 12, 19}) {
        without->noteOn(60 + semis, 1.0f);
        with->noteOn(60 + semis, 1.0f);
    }

    const auto sig = harmonicTone(180.0, kBlock * 200, 0.5);
    std::vector<Sample> outA(sig.size(), 0.0f), outB(sig.size(), 0.0f);
    const Tuning defaultTuning{};
    for (size_t i = 0; i + kBlock <= sig.size(); i += kBlock) {
        without->process(sig.data() + i, outA.data() + i, kBlock);
        with->applyTuning(defaultTuning);   // every block -- the worst case for drift
        with->process(sig.data() + i, outB.data() + i, kBlock);
    }

    REQUIRE(outA.size() == outB.size());
    for (size_t i = 0; i < outA.size(); ++i) {
        CAPTURE(i);
        REQUIRE(outA[i] == outB[i]);   // BIT-IDENTICAL, not doctest::Approx
    }
}

TEST_CASE("the stereo path stays bit-identical too, at Tuning{} defaults, through the new stages") {
    // app-design.md's own summary of Track D's obligation: "your stages, at default
    // Tuning, must be provable no-ops in the mono path exactly as in the stereo path."
    // rootMidiNote == 60 here for the same reason as the mono version above: it must
    // agree with Tuning{}'s own default before the first applyTuning(Tuning{}) call.
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
    for (int semis : {7, 12, 19}) {
        without->noteOn(60 + semis, 1.0f);
        with->noteOn(60 + semis, 1.0f);
    }

    const auto sig = harmonicTone(180.0, kBlock * 200, 0.5);
    std::vector<Sample> aL(sig.size(), 0.0f), aR(sig.size(), 0.0f);
    std::vector<Sample> bL(sig.size(), 0.0f), bR(sig.size(), 0.0f);
    const Tuning defaultTuning{};
    for (size_t i = 0; i + kBlock <= sig.size(); i += kBlock) {
        without->process(sig.data() + i, aL.data() + i, aR.data() + i, kBlock);
        with->applyTuning(defaultTuning);
        with->process(sig.data() + i, bL.data() + i, bR.data() + i, kBlock);
    }

    for (size_t i = 0; i < sig.size(); ++i) {
        CAPTURE(i);
        REQUIRE(aL[i] == bL[i]);
        REQUIRE(aR[i] == bR[i]);
    }
}
