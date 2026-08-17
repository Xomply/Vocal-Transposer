// tests/test_humanization.cpp — Track C (docs/app-design.md §5): does Humanization
// actually change the sound (§5.2), does stereo pan follow the equal-power law (§5.3), does
// the mono path stay bit-identical when humanization is off (§5.4), and do velocity /
// voiceCap (§5.6) actually work now that Voice::velocity and Tuning::voiceCap have readers.
//
// tests/test_engine.cpp covers hold/freeze (§5.1) and the alignAmount_ snap/ramp (§4.5 item
// 5); tests/test_tuning.cpp covers Tuning{}'s own defaults and clamp() bounds. This file is
// specifically the NEW per-voice application Engine::noteOn/mixImpl do with those values.

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

// Captures every ratio and the most recent muScale a voice was processed with, so a test
// can look at a whole trajectory (needed for vibrato/drift, which move block to block) as
// well as the latest value. Deliberately does NOT wrap process() in VH_RT_SECTION() the way
// the production shifters do — this is a test instrument, not a claim about real-time
// safety, and `ratios.push_back` is expected to allocate.
class SpyShifter final : public IPitchShifter {
public:
    mutable double lastRatio = 1.0;
    mutable double lastMuScale = 1.0;
    mutable std::vector<double> ratios;
    mutable int calls = 0;

    void prepare(double, FrameCount) override {}
    void reset() noexcept override {}
    void process(const ShiftRequest& req) noexcept override {
        lastRatio = req.ratio;
        lastMuScale = req.muScale;
        ratios.push_back(req.ratio);
        ++calls;
        req.ring->read(req.cursor->next, req.out, req.numFrames);
        if (!req.cursor->frozen) req.cursor->next += req.numFrames;
    }
    FrameCount latencySamples() const noexcept override { return 0; }
    const char* name() const noexcept override { return "spy"; }
};

// Mode B (IntervalFromRoot), root == 60: noteOn(60, ...) gives a base ratio of EXACTLY 1.0
// (2^(0/12)), with zero lookback (SpyShifter::latencySamples() == 0), so any deviation from
// 1.0 seen by the spy is entirely humanization, not F0 tracking noise or a lookback delay.
std::unique_ptr<Engine> makeSpyEngine(SpyShifter** spyOut) {
    auto e = std::make_unique<Engine>();
    EngineConfig cfg;
    cfg.sampleRate = kSR;
    cfg.maxBlock = kBlock;
    cfg.ratioSource = RatioSource::IntervalFromRoot;
    cfg.rootMidiNote = 60;

    SpyShifter* firstSpy = nullptr;
    e->prepare(cfg, std::make_unique<YinAnalyzer>(),
              [&firstSpy]() {
                  auto s = std::make_unique<SpyShifter>();
                  if (!firstSpy) firstSpy = s.get();
                  return s;
              },
              nullptr);
    *spyOut = firstSpy;
    return e;
}

void pumpSilence(Engine& e, int numBlocks, std::vector<Sample>& out) {
    std::vector<Sample> silence(kBlock, 0.0f);
    out.assign(kBlock, 0.0f);
    for (int i = 0; i < numBlocks; ++i) e.process(silence.data(), out.data(), kBlock);
}

EngineConfig baseCfg() {
    EngineConfig cfg;
    cfg.sampleRate = kSR;
    cfg.maxBlock = kBlock;
    cfg.ratioSource = RatioSource::IntervalFromRoot;
    cfg.rootMidiNote = 60;
    return cfg;
}

std::vector<Sample> harmonicTone(double f0, size_t n) {
    std::vector<Sample> sig(n);
    for (size_t i = 0; i < n; ++i) {
        double s = 0.0;
        for (int k = 1; k <= 5; ++k) {
            s += std::sin(2.0 * kPi * f0 * k * static_cast<double>(i) / kSR) / k;
        }
        sig[i] = static_cast<Sample>(s * 0.2);
    }
    return sig;
}

} // namespace

// ============================================================================
// PART 1 — Humanization is actually applied (app-design.md §5.2).
// ============================================================================

TEST_CASE("detuneCents applies a static per-voice offset to the ratio") {
    SpyShifter* spy = nullptr;
    auto e = makeSpyEngine(&spy);
    REQUIRE(spy != nullptr);

    Tuning t{};
    t.humanize.detuneCents = 40.0f;   // +-20 cents ensemble width
    e->applyTuning(t);

    e->noteOn(60, 1.0f);   // unison with root: base ratio is exactly 1.0
    std::vector<Sample> out;
    pumpSilence(*e, 1, out);

    REQUIRE(spy->calls > 0);
    const double cents = 1200.0 * std::log2(spy->lastRatio);
    CAPTURE(cents);
    CHECK(std::fabs(cents) > 0.01);          // genuinely nonzero
    CHECK(std::fabs(cents) <= 20.0 + 1e-6);  // within the +-detuneCents/2 ensemble width
}

TEST_CASE("detuneCents is exactly zero when HumanizationSpec::detuneCents is zero") {
    // The no-op half of the same property: at the default (all-zero) humanize spread, the
    // per-voice detune must be EXACTLY 0.0f, not merely small, or the mono bit-identical
    // property fails silently at every interval instead of holding exactly.
    SpyShifter* spy = nullptr;
    auto e = makeSpyEngine(&spy);
    REQUIRE(spy != nullptr);
    e->noteOn(60, 1.0f);
    std::vector<Sample> out;
    pumpSilence(*e, 1, out);
    REQUIRE(spy->calls > 0);
    CHECK(spy->lastRatio == 1.0);   // bit-exact, not doctest::Approx
}

TEST_CASE("vibrato periodically modulates the ratio around the base") {
    SpyShifter* spy = nullptr;
    auto e = makeSpyEngine(&spy);
    REQUIRE(spy != nullptr);

    Tuning t{};
    t.humanize.vibratoRateHz = 6.0f;
    t.humanize.vibratoDepthCents = 30.0f;
    e->applyTuning(t);

    e->noteOn(60, 1.0f);
    std::vector<Sample> out;
    // 6 Hz at a 128-sample/48 kHz block is ~2.67 ms/block, so a full cycle is ~62 blocks;
    // 80 blocks covers more than one full cycle.
    pumpSilence(*e, 80, out);

    REQUIRE(spy->ratios.size() >= 80);
    double minCents = 1e9, maxCents = -1e9;
    for (double r : spy->ratios) {
        const double c = 1200.0 * std::log2(r);
        minCents = std::min(minCents, c);
        maxCents = std::max(maxCents, c);
    }
    CAPTURE(minCents);
    CAPTURE(maxCents);
    CHECK(maxCents - minCents > 20.0);   // it genuinely swings, not merely nudges once
    CHECK(maxCents <= 30.0 + 0.5);       // and stays within the depth (small slack for the
    CHECK(minCents >= -30.0 - 0.5);      // block-rate quantisation noted in mixImpl()'s comment
}

TEST_CASE("detuneDriftCents produces a bounded random walk, not an unbounded drift") {
    SpyShifter* spy = nullptr;
    auto e = makeSpyEngine(&spy);
    REQUIRE(spy != nullptr);

    Tuning t{};
    t.humanize.detuneDriftCents = 15.0f;
    e->applyTuning(t);

    e->noteOn(60, 1.0f);
    std::vector<Sample> out;
    pumpSilence(*e, 2000, out);   // many blocks so the walk has time to wander

    REQUIRE(!spy->ratios.empty());
    double lo = 1e9, hi = -1e9;
    for (double r : spy->ratios) {
        const double c = 1200.0 * std::log2(r);
        CAPTURE(c);
        CHECK(std::fabs(c) <= 15.0 + 1e-3);   // never exceeds the amplitude bound
        lo = std::min(lo, c);
        hi = std::max(hi, c);
    }
    CHECK(hi - lo > 0.5);   // and it actually moves, rather than sitting at one offset
}

TEST_CASE("onsetJitterMs delays a voice's first processed block") {
    SpyShifter* spy = nullptr;
    auto e = makeSpyEngine(&spy);
    REQUIRE(spy != nullptr);

    Tuning t{};
    t.humanize.onsetJitterMs = 50.0f;
    e->applyTuning(t);

    e->noteOn(60, 1.0f);
    std::vector<Sample> out;
    pumpSilence(*e, 1, out);
    CHECK(spy->calls == 0);   // gated: startAtPos has not been reached yet

    pumpSilence(*e, 30, out);   // 30*128 = 3840 samples, comfortably past 50ms of jitter
    CHECK(spy->calls > 0);      // and now it has started
}

TEST_CASE("onsetJitterMs is a no-op at zero -- a voice starts on the very first block") {
    SpyShifter* spy = nullptr;
    auto e = makeSpyEngine(&spy);
    REQUIRE(spy != nullptr);
    e->noteOn(60, 1.0f);
    std::vector<Sample> out;
    pumpSilence(*e, 1, out);
    CHECK(spy->calls > 0);   // no gate fires when onsetJitterMs defaults to 0
}

TEST_CASE("envelopeWarpSpread is carried into ShiftRequest::muScale") {
    SpyShifter* spy = nullptr;
    auto e = makeSpyEngine(&spy);
    REQUIRE(spy != nullptr);

    Tuning t{};
    t.humanize.envelopeWarpSpread = 0.5f;
    e->applyTuning(t);

    e->noteOn(60, 1.0f);
    std::vector<Sample> out;
    pumpSilence(*e, 1, out);

    REQUIRE(spy->calls > 0);
    CHECK(spy->lastMuScale != doctest::Approx(1.0));
}

TEST_CASE("muScale defaults to exactly 1.0 when envelopeWarpSpread is zero") {
    SpyShifter* spy = nullptr;
    auto e = makeSpyEngine(&spy);
    REQUIRE(spy != nullptr);
    e->noteOn(60, 1.0f);
    std::vector<Sample> out;
    pumpSilence(*e, 1, out);
    REQUIRE(spy->calls > 0);
    CHECK(spy->lastMuScale == 1.0);   // bit-exact
}

// ============================================================================
// PART 2 — the mono path stays bit-identical (app-design.md §5.4).
// ============================================================================

TEST_CASE("mono output is deterministic with bare EngineConfig defaults (no Tuning at all)") {
    // This is the path every offline tool actually uses: vh_render, vh_trace, vh_sweep and
    // vh_demo all call setBlendAlignment() exactly once and never call applyTuning() at
    // all — so the property that matters for click_sweep.py's regression is that Track C's
    // new EngineConfig fields (humanize, voiceCap, velocityToGainMin/velocityCurve) default
    // to values that make every new code path in Engine::noteOn/mixImpl an EXACT no-op, not
    // merely that applying a default Tuning{} happens to be a no-op (tests/test_tuning.cpp
    // already covers that separately). Two identically-configured engines fed the same
    // input must therefore produce bit-identical output: the real proof that this is also
    // bit-identical to the PRE-Track-C engine is the full pre-existing suite (unmodified
    // except the hold/freeze split) continuing to pass, plus click_sweep.py's regression
    // count — see HANDOVER.md's own report on that run.
    auto build = []() {
        auto e = std::make_unique<Engine>();
        e->prepare(baseCfg(), std::make_unique<YinAnalyzer>(),
                  []() { return std::make_unique<GranularShifter>(); },
                  []() { return std::make_unique<PsolaShifter>(); });
        return e;
    };

    auto a = build();
    auto b = build();
    a->noteOn(67, 1.0f);
    b->noteOn(67, 1.0f);

    const auto sig = harmonicTone(210.0, kBlock * 300);
    std::vector<Sample> outA(sig.size(), 0.0f), outB(sig.size(), 0.0f);
    for (size_t i = 0; i + kBlock <= sig.size(); i += kBlock) {
        a->process(sig.data() + i, outA.data() + i, kBlock);
        b->process(sig.data() + i, outB.data() + i, kBlock);
    }

    REQUIRE(outA.size() == outB.size());
    for (size_t i = 0; i < outA.size(); ++i) {
        CAPTURE(i);
        REQUIRE(outA[i] == outB[i]);   // BIT-IDENTICAL, not doctest::Approx
    }
}

TEST_CASE("humanization ON changes mono output relative to humanization OFF") {
    // The contrast to the test above: proves the feature is real, not merely that the
    // no-op case is a no-op.
    auto build = [](const Tuning* t) {
        auto e = std::make_unique<Engine>();
        e->prepare(baseCfg(), std::make_unique<YinAnalyzer>(),
                  []() { return std::make_unique<GranularShifter>(); },
                  []() { return std::make_unique<PsolaShifter>(); });
        if (t) e->applyTuning(*t);
        return e;
    };

    Tuning hum{};
    hum.humanize.detuneCents = 30.0f;
    hum.humanize.vibratoRateHz = 5.5f;
    hum.humanize.vibratoDepthCents = 20.0f;

    auto off = build(nullptr);
    auto on = build(&hum);
    off->noteOn(67, 1.0f);
    on->noteOn(67, 1.0f);

    const auto sig = harmonicTone(210.0, kBlock * 200);
    std::vector<Sample> outOff(sig.size(), 0.0f), outOn(sig.size(), 0.0f);
    for (size_t i = 0; i + kBlock <= sig.size(); i += kBlock) {
        off->process(sig.data() + i, outOff.data() + i, kBlock);
        on->process(sig.data() + i, outOn.data() + i, kBlock);
    }

    bool anyDifference = false;
    for (size_t i = 0; i < outOff.size(); ++i) {
        if (outOff[i] != outOn[i]) { anyDifference = true; break; }
    }
    CHECK(anyDifference);
}

// ============================================================================
// PART 3 — stereo output and equal-power pan (app-design.md §5.3).
// ============================================================================

TEST_CASE("stereo pan follows the equal-power law: L^2+R^2 is conserved regardless of pan") {
    // cos^2(a) + sin^2(a) == 1 identically, so for a SINGLE voice, L^2+R^2 must equal the
    // (mono-equivalent) voice contribution squared at every sample, for ANY pan value. This
    // holds without needing to know what that pan value actually is, which is what makes it
    // a robust test of the LAW rather than of one particular derived pan number.
    const auto cfg = baseCfg();
    Engine eStereo, eMono;
    eStereo.prepare(cfg, std::make_unique<YinAnalyzer>(),
                    []() { return std::make_unique<PassthroughShifter>(); }, nullptr);
    eMono.prepare(cfg, std::make_unique<YinAnalyzer>(),
                  []() { return std::make_unique<PassthroughShifter>(); }, nullptr);

    Tuning t{};
    t.humanize.panSpread = 1.0f;   // full width, so pan is genuinely nonzero for voice 0
    eStereo.applyTuning(t);
    eMono.applyTuning(t);
    eStereo.noteOn(60, 1.0f);
    eMono.noteOn(60, 1.0f);

    const auto sig = harmonicTone(220.0, kBlock * 20);
    std::vector<Sample> outL(sig.size(), 0.0f), outR(sig.size(), 0.0f), outMono(sig.size(), 0.0f);
    for (size_t i = 0; i + kBlock <= sig.size(); i += kBlock) {
        eStereo.process(sig.data() + i, outL.data() + i, outR.data() + i, kBlock);
        eMono.process(sig.data() + i, outMono.data() + i, kBlock);
    }

    int checked = 0;
    for (size_t i = kBlock * 5; i < sig.size(); ++i) {   // past the attack ramp
        const double lhs = static_cast<double>(outL[i]) * outL[i] + static_cast<double>(outR[i]) * outR[i];
        const double rhs = static_cast<double>(outMono[i]) * outMono[i];
        CAPTURE(i);
        CHECK(lhs == doctest::Approx(rhs).epsilon(1e-4));
        ++checked;
    }
    CHECK(checked > 0);
}

TEST_CASE("stereo pan defaults to centred (L == R) when panSpread is 0") {
    const auto cfg = baseCfg();
    Engine e;
    e.prepare(cfg, std::make_unique<YinAnalyzer>(),
             []() { return std::make_unique<PassthroughShifter>(); }, nullptr);
    // humanize.panSpread defaults to 0 -- no Tuning applied at all, matching how every
    // offline tool actually configures the engine.
    e.noteOn(60, 1.0f);
    e.noteOn(64, 1.0f);
    e.noteOn(67, 1.0f);

    const auto sig = harmonicTone(220.0, kBlock * 20);
    std::vector<Sample> outL(sig.size(), 0.0f), outR(sig.size(), 0.0f);
    for (size_t i = 0; i + kBlock <= sig.size(); i += kBlock) {
        e.process(sig.data() + i, outL.data() + i, outR.data() + i, kBlock);
    }

    for (size_t i = 0; i < sig.size(); ++i) {
        CAPTURE(i);
        CHECK(outL[i] == doctest::Approx(outR[i]).epsilon(1e-5));
    }
}

TEST_CASE("stereo pan spreads a chord across the field when panSpread > 0") {
    const auto cfg = baseCfg();
    Engine e;
    e.prepare(cfg, std::make_unique<YinAnalyzer>(),
             []() { return std::make_unique<PassthroughShifter>(); }, nullptr);
    Tuning t{};
    t.humanize.panSpread = 1.0f;
    e.applyTuning(t);
    e.noteOn(60, 1.0f);
    e.noteOn(64, 1.0f);
    e.noteOn(67, 1.0f);

    const auto sig = harmonicTone(220.0, kBlock * 20);
    std::vector<Sample> outL(sig.size(), 0.0f), outR(sig.size(), 0.0f);
    for (size_t i = 0; i + kBlock <= sig.size(); i += kBlock) {
        e.process(sig.data() + i, outL.data() + i, outR.data() + i, kBlock);
    }

    bool anyAsymmetry = false;
    for (size_t i = kBlock * 5; i < sig.size(); ++i) {
        if (std::fabs(outL[i] - outR[i]) > 1e-4f) { anyAsymmetry = true; break; }
    }
    CHECK(anyAsymmetry);
}

TEST_CASE("stereo rendering is deterministic across repeated renders of the same chord") {
    auto build = []() {
        auto e = std::make_unique<Engine>();
        e->prepare(baseCfg(), std::make_unique<YinAnalyzer>(),
                  []() { return std::make_unique<GranularShifter>(); },
                  []() { return std::make_unique<PsolaShifter>(); });
        Tuning t{};
        t.humanize.detuneCents = 25.0f;
        t.humanize.vibratoRateHz = 5.0f;
        t.humanize.vibratoDepthCents = 15.0f;
        t.humanize.panSpread = 0.8f;
        e->applyTuning(t);
        return e;
    };

    auto a = build();
    auto b = build();
    for (int n : {60, 64, 67}) { a->noteOn(n, 1.0f); b->noteOn(n, 1.0f); }

    const auto sig = harmonicTone(180.0, kBlock * 100);
    std::vector<Sample> aL(sig.size(), 0.0f), aR(sig.size(), 0.0f);
    std::vector<Sample> bL(sig.size(), 0.0f), bR(sig.size(), 0.0f);
    for (size_t i = 0; i + kBlock <= sig.size(); i += kBlock) {
        a->process(sig.data() + i, aL.data() + i, aR.data() + i, kBlock);
        b->process(sig.data() + i, bL.data() + i, bR.data() + i, kBlock);
    }
    for (size_t i = 0; i < sig.size(); ++i) {
        CAPTURE(i);
        REQUIRE(aL[i] == bL[i]);
        REQUIRE(aR[i] == bR[i]);
    }
}

// ============================================================================
// PART 4 — velocity, voice cap (app-design.md §5.6).
// ============================================================================

TEST_CASE("velocity has no effect on gain by default -- a bypass, matching today's Engine") {
    const auto cfg = baseCfg();
    const auto sig = harmonicTone(220.0, kBlock * 20);

    auto render = [&](float velocity) {
        Engine e;
        e.prepare(cfg, std::make_unique<YinAnalyzer>(),
                 []() { return std::make_unique<PassthroughShifter>(); }, nullptr);
        e.noteOn(60, velocity);
        std::vector<Sample> out(sig.size(), 0.0f);
        for (size_t i = 0; i + kBlock <= sig.size(); i += kBlock) {
            e.process(sig.data() + i, out.data() + i, kBlock);
        }
        return out;
    };

    const auto quiet = render(0.1f);
    const auto loud = render(1.0f);
    REQUIRE(quiet.size() == loud.size());
    for (size_t i = 0; i < quiet.size(); ++i) {
        CAPTURE(i);
        CHECK(quiet[i] == loud[i]);   // velocity ignored -- bit-identical to today's Engine
    }
}

TEST_CASE("a live velocity curve maps velocity to gain when enabled via Tuning") {
    const auto cfg = baseCfg();
    const auto sig = harmonicTone(220.0, kBlock * 20);

    Tuning t{};
    t.velocityToGainMin = 0.0f;   // full range: gain == velocity^curve
    t.velocityCurve = 1.0f;       // linear

    auto render = [&](float velocity) {
        Engine e;
        e.prepare(cfg, std::make_unique<YinAnalyzer>(),
                 []() { return std::make_unique<PassthroughShifter>(); }, nullptr);
        e.applyTuning(t);
        e.noteOn(60, velocity);
        std::vector<Sample> out(sig.size(), 0.0f);
        for (size_t i = 0; i + kBlock <= sig.size(); i += kBlock) {
            e.process(sig.data() + i, out.data() + i, kBlock);
        }
        return out;
    };

    const auto half = render(0.5f);
    const auto full = render(1.0f);

    float peakHalf = 0.0f, peakFull = 0.0f;
    for (size_t i = kBlock * 5; i < half.size(); ++i) {
        peakHalf = std::max(peakHalf, std::fabs(half[i]));
        peakFull = std::max(peakFull, std::fabs(full[i]));
    }
    REQUIRE(peakFull > 1e-4f);
    CHECK(peakHalf / peakFull == doctest::Approx(0.5).epsilon(0.02));
}

TEST_CASE("voiceCap limits how many NEW voices can be allocated") {
    Engine e;
    e.prepare(baseCfg(), std::make_unique<YinAnalyzer>(),
             []() { return std::make_unique<PassthroughShifter>(); }, nullptr);

    Tuning t{};
    t.voiceCap = 3;
    e.applyTuning(t);

    for (int i = 0; i < 8; ++i) e.noteOn(40 + i, 1.0f);
    CHECK(e.activeVoiceCount() == 3);   // stealing kicks in past the CAP, not past kMaxVoices
}

TEST_CASE("voiceCap defaults to kMaxVoices -- no cap unless configured") {
    Engine e;
    e.prepare(baseCfg(), std::make_unique<YinAnalyzer>(),
             []() { return std::make_unique<PassthroughShifter>(); }, nullptr);
    for (int i = 0; i < kMaxVoices; ++i) e.noteOn(40 + i, 1.0f);
    CHECK(e.activeVoiceCount() == kMaxVoices);
}
