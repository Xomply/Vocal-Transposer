#include <doctest/doctest.h>

#include "vh/engine.hpp"
#include "vh/passthrough_shifter.hpp"
#include "vh/psola_shifter.hpp"
#include "vh/rt.hpp"
#include "vh/yin.hpp"

#include <cmath>
#include <vector>

using namespace vh;

namespace {

constexpr double kSR = 48000.0;
constexpr FrameCount kBlock = 128;

std::vector<Sample> tone(double f0, FrameCount n) {
    std::vector<Sample> v(n);
    for (FrameCount i = 0; i < n; ++i) {
        double s = 0.0;
        for (int k = 1; k <= 6; ++k)
            s += std::sin(2.0 * M_PI * f0 * k * static_cast<double>(i) / kSR) / k;
        v[i] = static_cast<Sample>(s * 0.2);
    }
    return v;
}

// A shifter that records what ratio it was asked for, so mode behaviour can be asserted
// without needing a real DSP implementation. The point of the IPitchShifter seam is that
// the orchestration can be tested before any engine exists.
class SpyShifter final : public IPitchShifter {
public:
    mutable double lastRatio = 0.0;
    mutable int calls = 0;

    void prepare(double, FrameCount) override {}
    void reset() noexcept override {}
    void process(const ShiftRequest& req) noexcept override {
        VH_RT_SECTION();
        lastRatio = req.ratio;
        ++calls;
        req.ring->read(req.cursor->next, req.out, req.numFrames);
        if (!req.cursor->frozen) req.cursor->next += req.numFrames;
    }
    FrameCount latencySamples() const noexcept override { return 0; }
    const char* name() const noexcept override { return "spy"; }
};

// Engine is deliberately non-copyable (it owns an atomic and unique_ptrs), so helpers
// hand out ownership rather than values. Fighting that with a copy would mean loosening
// the ownership model to suit a test, which is backwards.
std::unique_ptr<Engine> makeEngine(RatioSource mode, SpyShifter** spyOut = nullptr) {
    auto e = std::make_unique<Engine>();
    EngineConfig cfg;
    cfg.sampleRate = kSR;
    cfg.maxBlock = kBlock;
    cfg.ratioSource = mode;
    cfg.rootMidiNote = 60;

    SpyShifter* firstSpy = nullptr;
    e->prepare(cfg, std::make_unique<YinAnalyzer>(),
              [&firstSpy]() {
                  auto s = std::make_unique<SpyShifter>();
                  if (!firstSpy) firstSpy = s.get();
                  return s;
              },
              []() { return std::make_unique<PassthroughShifter>(); });
    if (spyOut) *spyOut = firstSpy;
    return e;
}

void pump(Engine& e, const std::vector<Sample>& sig, std::vector<Sample>& out) {
    out.assign(kBlock, 0.0f);
    for (FrameCount i = 0; i + kBlock <= sig.size(); i += kBlock) {
        e.process(sig.data() + i, out.data(), kBlock);
    }
}

} // namespace

TEST_CASE("silence in, silence out with no voices") {
    auto e = makeEngine(RatioSource::AbsoluteTarget);
    std::vector<Sample> in(kBlock, 0.3f), out(kBlock, 999.0f);
    e->process(in.data(), out.data(), kBlock);
    for (auto s : out) CHECK(s == doctest::Approx(0.0f));
}

TEST_CASE("Mode B computes its ratio without any pitch detection") {
    // The claim being pinned: Mode B's ratio is known the instant the key goes down.
    // Feeding SILENCE means the analyser can never lock — and the ratio must still be
    // exactly right. If this ever fails, Mode B has acquired a dependency on F0 and has
    // lost the latency advantage that justifies its existence.
    SpyShifter* spy = nullptr;
    auto e = makeEngine(RatioSource::IntervalFromRoot, &spy);
    REQUIRE(spy != nullptr);

    e->noteOn(67, 1.0f);                       // G4 against root C4 = +7 semitones
    std::vector<Sample> silence(kBlock * 4, 0.0f), out;
    pump(*e, silence, out);

    CHECK(spy->calls > 0);
    CHECK(spy->lastRatio == doctest::Approx(std::pow(2.0, 7.0 / 12.0)).epsilon(1e-9));
}

TEST_CASE("Mode A derives its ratio from the sung pitch") {
    SpyShifter* spy = nullptr;
    auto e = makeEngine(RatioSource::AbsoluteTarget, &spy);
    REQUIRE(spy != nullptr);

    e->noteOn(69, 1.0f);                       // target A4 = 440 Hz
    const auto sig = tone(220.0, 48000);      // singing A3
    std::vector<Sample> out;
    pump(*e, sig, out);

    CHECK(spy->lastRatio == doctest::Approx(2.0).epsilon(0.02));   // an octave up
}

TEST_CASE("Mode A degrades to unity rather than exploding when F0 is unavailable") {
    // Feeding silence gives the analyser nothing. A naive f_target/f_sung with f_sung
    // near zero produces an enormous ratio and a burst of noise in someone's ears.
    // Unity is the safe degradation.
    SpyShifter* spy = nullptr;
    auto e = makeEngine(RatioSource::AbsoluteTarget, &spy);
    e->noteOn(72, 1.0f);
    std::vector<Sample> silence(kBlock * 4, 0.0f), out;
    pump(*e, silence, out);
    CHECK(spy->lastRatio == doctest::Approx(1.0));
}

TEST_CASE("the blend policy is swappable at runtime") {
    // The load-bearing requirement: the split behaviour must be tweakable after the whole
    // system exists. Swapping mid-stream must not disturb voices, cursors or engines.
    auto e = makeEngine(RatioSource::IntervalFromRoot);
    FastOnlyPolicy fast;
    QualityOnlyPolicy qual;
    OnsetHandoverPolicy onset;

    e->setBlendPolicy(&fast);
    CHECK(std::string(e->blendPolicy()->name()) == "fast-only");

    e->noteOn(64, 1.0f);
    const auto sig = tone(200.0, 24000);
    std::vector<Sample> out;
    pump(*e, sig, out);

    e->setBlendPolicy(&qual);
    CHECK(std::string(e->blendPolicy()->name()) == "quality-only");
    pump(*e, sig, out);

    e->setBlendPolicy(&onset);
    pump(*e, sig, out);
    CHECK(e->activeVoiceCount() == 1);
}

TEST_CASE("onset handover moves from fast to quality over time") {
    OnsetHandoverPolicy p;
    p.handoverStartMs = 20.0f;
    p.handoverEndMs = 60.0f;

    BlendContext ctx{};
    ctx.sampleRate = kSR;

    ctx.samplesSinceNoteOn = 0.0f;
    CHECK(p.weightsFor(ctx).fast == doctest::Approx(1.0f));

    ctx.samplesSinceNoteOn = static_cast<float>(kSR * 0.040);   // 40 ms, mid-handover
    const auto mid = p.weightsFor(ctx);
    CHECK(mid.fast > 0.2f);
    CHECK(mid.quality > 0.2f);

    ctx.samplesSinceNoteOn = static_cast<float>(kSR * 0.100);   // 100 ms, past the end
    CHECK(p.weightsFor(ctx).quality == doctest::Approx(1.0f));
}

TEST_CASE("blend weights always sum to one") {
    // Guards against the gain dip or bump that an un-normalised crossfade produces, which
    // is heard as amplitude pumping at the handover rate rather than as an obvious bug.
    BlendWeights w{0.3f, 0.3f};
    w.normalize();
    CHECK(w.fast + w.quality == doctest::Approx(1.0f));

    BlendWeights zero{0.0f, 0.0f};
    zero.normalize();
    CHECK(zero.fast + zero.quality == doctest::Approx(1.0f));   // degenerate case is safe
}

TEST_CASE("sustain freezes a voice by stalling its cursor") {
    auto e = makeEngine(RatioSource::IntervalFromRoot);
    e->setSustain(true);
    e->noteOn(60, 1.0f);

    const auto sig = tone(180.0, 24000);
    std::vector<Sample> out;
    pump(*e, sig, out);

    e->noteOff(60);
    CHECK(e->activeVoiceCount() == 1);      // held, not released

    e->setSustain(false);
    pump(*e, sig, out);
    CHECK(e->activeVoiceCount() == 0);
}

TEST_CASE("a full block with many voices allocates nothing") {
    // The end-to-end version of the real-time claim: analyser, voices, two engines,
    // blending, mixing — all inside one callback, with the detector armed.
    auto e = makeEngine(RatioSource::AbsoluteTarget);
    for (int n = 0; n < 12; ++n) e->noteOn(52 + n * 2, 1.0f);
    CHECK(e->activeVoiceCount() == 12);

    const auto sig = tone(130.0, 48000);
    std::vector<Sample> out(kBlock, 0.0f);

    rt::resetViolationCount();
    for (FrameCount i = 0; i + kBlock <= sig.size(); i += kBlock) {
        e->process(sig.data() + i, out.data(), kBlock);
    }
    CHECK(rt::violationCount() == 0);
}

TEST_CASE("output stays finite under 16 voices of extreme shift") {
    // NaN and Inf in an audio path are not abstract: they are a loud noise in someone's
    // ears. Any engine added later should be run through this.
    auto e = makeEngine(RatioSource::AbsoluteTarget);
    for (int n = 0; n < kMaxVoices; ++n) e->noteOn(24 + n * 5, 1.0f);

    const auto sig = tone(90.0, 48000);
    std::vector<Sample> out(kBlock, 0.0f);
    for (FrameCount i = 0; i + kBlock <= sig.size(); i += kBlock) {
        e->process(sig.data() + i, out.data(), kBlock);
        for (auto s : out) REQUIRE(std::isfinite(s));
    }
}

TEST_CASE("note-on lookback satisfies the SLOWEST engine, not the fastest") {
    // REGRESSION TEST for a bug that cost real debugging time, and whose symptom pointed
    // nowhere near its cause.
    //
    // The Engine starts each voice reading some distance behind the write head so the
    // shifters have history to work from. Sizing that distance from the FAST engine alone
    // starves the quality engine: PSOLA's residency checks fail, it silently drops grains,
    // and the audible result is a blend roughly 6 dB quieter than either engine alone.
    // That reads as a mixing fault, so you go and inspect the blend weights — which are
    // perfectly correct.
    //
    // Asserted by giving the two slots very different latencies and checking what cursor
    // position each engine is actually handed.
    class LatentSpy final : public IPitchShifter {
    public:
        explicit LatentSpy(FrameCount lat) : lat_(lat) {}
        mutable Pos firstCursor = 0;
        mutable bool seen = false;
        void prepare(double, FrameCount) override {}
        void reset() noexcept override {}
        void process(const ShiftRequest& req) noexcept override {
            if (!seen) { firstCursor = req.cursor->next; seen = true; }
            for (FrameCount i = 0; i < req.numFrames; ++i) req.out[i] = 0.0f;
            if (!req.cursor->frozen) req.cursor->next += req.numFrames;
        }
        FrameCount latencySamples() const noexcept override { return lat_; }
        const char* name() const noexcept override { return "latent-spy"; }
    private:
        FrameCount lat_;
    };

    auto e = std::make_unique<Engine>();
    EngineConfig cfg;
    cfg.sampleRate = kSR;
    cfg.maxBlock = kBlock;
    cfg.ratioSource = RatioSource::IntervalFromRoot;

    LatentSpy* slowSpy = nullptr;
    e->prepare(cfg, std::make_unique<YinAnalyzer>(),
               []() { return std::make_unique<LatentSpy>(200); },        // fast: short
               [&slowSpy]() {
                   auto s = std::make_unique<LatentSpy>(4000);           // quality: long
                   if (!slowSpy) slowSpy = s.get();
                   return s;
               });

    QualityOnlyPolicy qual;
    e->setBlendPolicy(&qual);

    const auto sig = tone(180.0, 48000);
    std::vector<Sample> out;
    // Prime the ring so there is history to look back into.
    pump(*e, sig, out);
    e->noteOn(60, 1.0f);
    pump(*e, sig, out);

    REQUIRE(slowSpy != nullptr);
    REQUIRE(slowSpy->seen);
    const Pos lookback = e->ring().writePos() - slowSpy->firstCursor;
    CHECK(lookback >= 4000);   // the slow engine's requirement, not the fast one's
}

TEST_CASE("a note pressed on an EMPTY ring still produces sound — the history gate") {
    // REGRESSION TEST for a bug that never healed itself, found only on real recordings.
    //
    // noteOn sets cursor.next = noteOnPos - lookback. Pos is unsigned, so pressing a key
    // before the ring holds `lookback` samples clamps the cursor to zero — and from then
    // on the cursor and the write head advance in lockstep, one block apart. The lag stays
    // at one block FOREVER. PSOLA, starved of lookahead, fails its residency checks and
    // emits silence for the entire life of that voice.
    //
    // It bites exactly where you would least want it to: the first chord of a session,
    // pressed before anything has been sung. Measured in rendered output as 220 dB
    // frame-to-frame drops.
    //
    // The gate makes the voice wait until the lag is genuine. Starting a few tens of
    // milliseconds late is not a compromise — you cannot pitch-shift audio you have not
    // heard yet.
    Engine e;
    EngineConfig cfg;
    cfg.sampleRate = kSR;
    cfg.maxBlock = kBlock;
    cfg.ratioSource = RatioSource::IntervalFromRoot;
    cfg.rootMidiNote = 57;
    e.prepare(cfg, std::make_unique<YinAnalyzer>(),
              []() { return std::make_unique<PsolaShifter>(); }, nullptr);
    FastOnlyPolicy fast;
    e.setBlendPolicy(&fast);

    // Key pressed at the very first sample, before ANY audio exists.
    e.noteOn(64, 1.0f);

    const auto sig = tone(220.0, 48000 * 2);
    std::vector<Sample> out(kBlock, 0.0f);
    std::vector<double> blockRms;
    for (FrameCount i = 0; i + kBlock <= sig.size(); i += kBlock) {
        e.process(sig.data() + i, out.data(), kBlock);
        double s = 0.0;
        for (auto v : out) s += static_cast<double>(v) * v;
        blockRms.push_back(std::sqrt(s / kBlock));
    }

    // Once running, the voice must be continuously audible. Check the back half, well past
    // any legitimate startup gate and envelope ramp.
    const size_t half = blockRms.size() / 2;
    double quietest = 1e9, loudest = 0.0;
    for (size_t i = half; i < blockRms.size(); ++i) {
        quietest = std::min(quietest, blockRms[i]);
        loudest = std::max(loudest, blockRms[i]);
    }
    CHECK(loudest > 1e-3);            // it makes sound at all
    CHECK(quietest > loudest * 0.05); // and never drops into a hole (>-26 dB from peak)
}

TEST_CASE("reported latency reflects the mode") {
    // Mode B should not be paying the analyser's acquisition cost. This is the latency
    // argument for Mode B, asserted rather than believed.
    auto a = makeEngine(RatioSource::AbsoluteTarget);
    auto b = makeEngine(RatioSource::IntervalFromRoot);
    CHECK(a->latencySamples() > b->latencySamples());
    CHECK(b->latencySamples() == 0);
}
