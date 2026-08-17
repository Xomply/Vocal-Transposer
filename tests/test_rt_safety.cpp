#include <doctest/doctest.h>

#include "vh/audio_ring.hpp"
#include "vh/engine.hpp"
#include "vh/granular_shifter.hpp"
#include "vh/psola_shifter.hpp"
#include "vh/rt.hpp"
#include "vh/tuning.hpp"
#include "vh/yin.hpp"

#include <memory>
#include <vector>

using namespace vh;

TEST_CASE("the guard detects an allocation inside a real-time section") {
    // Meta-test. A safety net that does not actually catch anything is worse than none,
    // because it produces confidence without evidence. This proves the net has holes of
    // the right size before any other test relies on it.
    rt::resetViolationCount();
    {
        VH_RT_SECTION();
        std::vector<float> naughty(64);   // exactly the sort of thing we are hunting
        (void)naughty;
    }
    CHECK(rt::violationCount() > 0);
    rt::resetViolationCount();
}

TEST_CASE("the guard is silent outside a real-time section") {
    rt::resetViolationCount();
    std::vector<float> fine(1024);
    (void)fine;
    CHECK(rt::violationCount() == 0);
}

TEST_CASE("the suspend hatch works and restores") {
    rt::resetViolationCount();
    {
        VH_RT_SECTION();
        {
            rt::RtScopeSuspend allowThisOne;
            std::vector<float> deliberate(16);
            (void)deliberate;
        }
        CHECK(rt::violationCount() == 0);
        std::vector<float> accidental(16);   // outside the hatch — should be caught
        (void)accidental;
    }
    CHECK(rt::violationCount() > 0);
    rt::resetViolationCount();
}

TEST_CASE("AudioRing read and write allocate nothing") {
    AudioRing r(4096);
    std::vector<Sample> in(128, 0.5f), out(128);
    rt::resetViolationCount();
    for (int i = 0; i < 100; ++i) {
        r.write(in.data(), 128);
        r.read(r.writePos() - 128, out.data(), 128);
    }
    CHECK(rt::violationCount() == 0);
}

TEST_CASE("Engine::applyTuning allocates nothing on the audio thread") {
    // app-design.md §4.3/§6.1: applyTuning() is meant to be called from the audio callback
    // every block that tuningBus.poll() returns something new. It fans out to the
    // analyser, both shifters of EVERY voice, and the four owned blend policies — the
    // widest-reaching new audio-thread entry point this track adds, so it gets its own
    // dedicated RT-safety proof here alongside AudioRing's, rather than only living next to
    // the governing-property test in tests/test_tuning.cpp.
    Engine e;
    EngineConfig cfg;
    cfg.sampleRate = 48000.0;
    cfg.maxBlock = 128;
    e.prepare(cfg, std::make_unique<YinAnalyzer>(),
             []() { return std::make_unique<GranularShifter>(); },
             []() { return std::make_unique<PsolaShifter>(); });

    // Several voices held at once: setTuning() fans out to fast_[i]/quality_[i] for every
    // voice SLOT, not just active ones, so this exercises that loop at more than one entry.
    for (int n = 0; n < 6; ++n) e.noteOn(52 + n * 3, 1.0f);

    Tuning t{};
    t.kind = BlendKind::OnsetHandover;
    t.analyzer.epochCutoffRatio = 2.0f;
    t.shifters.granular.jumpPeriods = 2;
    t.shifters.psola.handoverMs = 5.0f;

    std::vector<Sample> in(128, 0.2f), out(128);
    rt::resetViolationCount();
    for (int i = 0; i < 50; ++i) {
        t.rsCrossoverHz = 200.0f + static_cast<float>(i);   // a genuinely changing value
        e.applyTuning(t);
        e.process(in.data(), out.data(), 128);
    }
    CHECK(rt::violationCount() == 0);
}

TEST_CASE("the stereo process() overload allocates nothing on the audio thread") {
    // app-design.md §5.3/§5.4: the new stereo entry point, with humanization (pan, detune,
    // vibrato, drift, onset jitter) actually engaged, so this exercises every new per-block
    // code path mixImpl() adds, not just the pre-existing mono one.
    Engine e;
    EngineConfig cfg;
    cfg.sampleRate = 48000.0;
    cfg.maxBlock = 128;
    e.prepare(cfg, std::make_unique<YinAnalyzer>(),
             []() { return std::make_unique<GranularShifter>(); },
             []() { return std::make_unique<PsolaShifter>(); });

    Tuning t{};
    t.humanize.detuneCents = 20.0f;
    t.humanize.detuneDriftCents = 8.0f;
    t.humanize.vibratoRateHz = 5.5f;
    t.humanize.vibratoDepthCents = 15.0f;
    t.humanize.onsetJitterMs = 10.0f;
    t.humanize.envelopeWarpSpread = 0.3f;
    t.humanize.panSpread = 1.0f;
    e.applyTuning(t);

    for (int n = 0; n < 6; ++n) e.noteOn(52 + n * 3, 1.0f);

    std::vector<Sample> in(128, 0.2f), outL(128), outR(128);
    rt::resetViolationCount();
    for (int i = 0; i < 100; ++i) {
        e.process(in.data(), outL.data(), outR.data(), 128);
    }
    CHECK(rt::violationCount() == 0);
}

TEST_CASE("the input gate and the limiter allocate nothing on the audio thread") {
    // app-design.md §5.5, Track D: the new per-block state (Engine::inputStageBuf_,
    // gateEnv_, limiterGain_) is preallocated in prepare() like fastBuf_/qualityBuf_, and
    // the gate/limiter math itself (pow, exp, fabs, a handful of comparisons) allocates
    // nothing -- but this is the widest-reaching new audio-thread code Track D adds, so it
    // gets the same dedicated proof as applyTuning() and the stereo overload above rather
    // than relying on the absence of an obvious std::vector in the diff.
    Engine e;
    EngineConfig cfg;
    cfg.sampleRate = 48000.0;
    cfg.maxBlock = 128;
    e.prepare(cfg, std::make_unique<YinAnalyzer>(),
             []() { return std::make_unique<GranularShifter>(); },
             []() { return std::make_unique<PsolaShifter>(); });

    Tuning t{};
    t.inputGateOn = true;
    t.inputGateDb = -30.0f;
    t.limiterOn = true;
    t.outCeilingDb = -3.0f;
    t.dryGain = 0.3f;
    t.wetGain = 0.8f;
    e.applyTuning(t);

    for (int n = 0; n < 4; ++n) e.noteOn(52 + n * 3, 1.0f);

    std::vector<Sample> in(128, 0.6f), outL(128), outR(128);
    rt::resetViolationCount();
    for (int i = 0; i < 100; ++i) {
        e.process(in.data(), outL.data(), outR.data(), 128);
    }
    CHECK(rt::violationCount() == 0);
}

TEST_CASE("setHold/setFreeze/allNotesOff allocate nothing on the audio thread") {
    // app-design.md §5.1: the hold/freeze split and panic are all called from the MIDI
    // queue before process(), per the app's threading model (app-design.md §6.1) -- same
    // real-time obligation as noteOn/noteOff above.
    Engine e;
    EngineConfig cfg;
    cfg.sampleRate = 48000.0;
    cfg.maxBlock = 128;
    e.prepare(cfg, std::make_unique<YinAnalyzer>(),
             []() { return std::make_unique<PsolaShifter>(); }, nullptr);

    std::vector<Sample> in(128, 0.1f), out(128, 0.0f);
    rt::resetViolationCount();
    for (int i = 0; i < 20; ++i) {
        e.setHold(i % 2 == 0);
        e.noteOn(60 + (i % 5), 1.0f);
        e.process(in.data(), out.data(), 128);
        e.setFreeze(i % 3 == 0);
        e.process(in.data(), out.data(), 128);
        e.noteOff(60 + (i % 5));
    }
    e.allNotesOff();
    e.process(in.data(), out.data(), 128);
    CHECK(rt::violationCount() == 0);
}
