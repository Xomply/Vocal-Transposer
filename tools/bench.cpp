// tools/bench.cpp — CPU cost and objective quality, measured rather than asserted.
//
// TWO QUESTIONS THIS ANSWERS:
//
// 1. Can it hold the deadline? At 48 kHz with a 128-sample block the audio callback has
//    2.667 ms of wall clock to produce 128 samples. Anything approaching that budget will
//    drop out on stage, where the machine is also running a DAW, a display, and whatever
//    Windows feels like doing. The number to watch is p99 as a fraction of the budget, and
//    the honest headroom target is well under 50%.
//
// 2. Does it hit the right notes? Rendering a fixed interval and measuring the output F0
//    against the target turns "sounds about right" into cents of error.
//
// WHAT THIS DOES NOT MEASURE, so the numbers are not over-read: worst-case latency under
// scheduler pressure, denormal stalls, or cache behaviour with a real plugin host in the
// same process. Those need LatencyMon and a real session on the target machine.

#include "vh/engine.hpp"
#include "vh/granular_shifter.hpp"
#include "vh/psola_shifter.hpp"
#include "vh/yin.hpp"

#include "voice.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

using namespace vh;
using namespace vhtools;

namespace {

constexpr double kSR = 48000.0;

struct Timing {
    double meanUs = 0.0;
    double p99Us = 0.0;
    double maxUs = 0.0;
};

// Forces both engines to run, so dual-engine timings are worst case rather than whatever
// the handover happens to be doing at that instant.
struct BothPolicy final : IBlendPolicy {
    BlendWeights weightsFor(const BlendContext&) const noexcept override { return {0.5f, 0.5f}; }
    const char* name() const noexcept override { return "both"; }
};

Timing benchmark(int voices, FrameCount block, bool dual, const std::vector<Sample>& sig) {
    Engine engine;
    EngineConfig cfg;
    cfg.sampleRate = kSR;
    cfg.maxBlock = block;
    cfg.ratioSource = RatioSource::AbsoluteTarget;

    auto makeGranular = []() -> std::unique_ptr<IPitchShifter> { return std::make_unique<GranularShifter>(); };
    auto makePsola    = []() -> std::unique_ptr<IPitchShifter> { return std::make_unique<PsolaShifter>(); };

    if (dual) engine.prepare(cfg, std::make_unique<YinAnalyzer>(), makeGranular, makePsola);
    else      engine.prepare(cfg, std::make_unique<YinAnalyzer>(), makePsola, nullptr);

    static BothPolicy both;
    static FastOnlyPolicy fast;
    engine.setBlendPolicy(dual ? static_cast<const IBlendPolicy*>(&both)
                               : static_cast<const IBlendPolicy*>(&fast));

    for (int v = 0; v < voices; ++v) engine.noteOn(40 + v * 3, 1.0f);

    std::vector<Sample> out(block, 0.0f);
    std::vector<double> times;
    times.reserve(sig.size() / block + 1);

    // Warm up: the first blocks pay page faults and cold caches. Real, but not what is
    // being measured.
    for (FrameCount i = 0; i + block <= std::min<FrameCount>(sig.size(), block * 50); i += block)
        engine.process(sig.data() + i, out.data(), block);

    for (FrameCount i = 0; i + block <= sig.size(); i += block) {
        const auto t0 = std::chrono::steady_clock::now();
        engine.process(sig.data() + i, out.data(), block);
        const auto t1 = std::chrono::steady_clock::now();
        times.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
    }

    std::sort(times.begin(), times.end());
    Timing t;
    for (double x : times) t.meanUs += x;
    t.meanUs /= static_cast<double>(times.size());
    t.p99Us = times[static_cast<size_t>(static_cast<double>(times.size()) * 0.99)];
    t.maxUs = times.back();
    return t;
}

double measureCentsError(IPitchShifter& sh, double f0, double ratio) {
    const double midi = 69.0 + 12.0 * std::log2(f0 / 440.0);
    const auto in = renderPhrase({{0.0, 2.5, midi, 0, 0.0}}, kSR, 2.5);

    YinAnalyzer y;
    y.prepare(kSR, 128);
    AudioRing ring(1 << 18);
    sh.prepare(kSR, 128);
    sh.reset();

    PreservationSpec pres;
    ReadCursor cur;
    std::vector<Sample> out(in.size(), 0.0f), blk(128, 0.0f);
    bool started = false;

    for (FrameCount i = 0; i + 128 <= in.size(); i += 128) {
        ring.write(in.data() + i, 128);
        y.process(ring, ring.writePos());
        if (!started) {
            if (y.current().f0Hz <= 0.0f) continue;
            const FrameCount lat = sh.latencySamples();
            cur.next = ring.writePos() > lat ? ring.writePos() - lat : 0;
            started = true;
        }
        ShiftRequest req{};
        req.ring = &ring; req.analysis = &y.current(); req.cursor = &cur;
        req.preservation = &pres; req.ratio = ratio; req.out = blk.data(); req.numFrames = 128;
        sh.process(req);
        for (FrameCount k = 0; k < 128; ++k) out[i + k] = blk[k];
    }

    YinAnalyzer m;
    m.prepare(kSR, 128);
    AudioRing r2(1 << 18);
    for (FrameCount i = 0; i + 128 <= out.size(); i += 128) {
        r2.write(out.data() + i, 128);
        m.process(r2, r2.writePos());
    }
    const float measured = m.current().f0Hz;
    if (measured < 20.0f) return 9999.0;
    return 1200.0 * std::log2(static_cast<double>(measured) / (f0 * ratio));
}

} // namespace

int main() {
    const auto sig = renderPhrase(defaultPhrase(), kSR, 5.2);

    std::printf("=== CPU: processing time per block, 48 kHz ===\n\n");
    std::printf("%-32s %8s %8s %8s %9s\n", "configuration", "mean us", "p99 us", "max us", "% budget");

    struct Case { const char* label; int voices; FrameCount block; bool dual; };
    const Case cases[] = {
        {"1 voice, psola, 128",          1, 128, false},
        {"4 voices, psola, 128",         4, 128, false},
        {"8 voices, psola, 128",         8, 128, false},
        {"16 voices, psola, 128",       16, 128, false},
        {"8 voices, both engines, 128",  8, 128, true},
        {"16 voices, both engines, 128",16, 128, true},
        {"8 voices, psola, 64",          8,  64, false},
        {"16 voices, both engines, 64", 16,  64, true},
    };

    for (const auto& c : cases) {
        const Timing t = benchmark(c.voices, c.block, c.dual, sig);
        const double budgetUs = static_cast<double>(c.block) / kSR * 1e6;
        std::printf("%-32s %8.1f %8.1f %8.1f %8.1f%%\n",
                    c.label, t.meanUs, t.p99Us, t.maxUs, 100.0 * t.p99Us / budgetUs);
    }

    std::printf("\n=== Pitch accuracy: cents error vs target ===\n");
    std::printf("Steady vowel. Under ~10 cents is inaudible in a chord; under ~25 is fine\n");
    std::printf("for a harmony voice, which real singers do not tune more tightly than.\n\n");
    std::printf("%-11s %-10s %11s %11s\n", "source F0", "interval", "granular", "psola");

    const struct { double f0; double semis; } probes[] = {
        {110.0, -12.0}, {110.0, -5.0}, {110.0,  3.0}, {110.0,  7.0}, {110.0, 12.0},
        {150.0,  -7.0}, {150.0,  4.0}, {150.0,  7.0},
        {220.0,  -5.0}, {220.0,  5.0}, {220.0, 12.0},
    };

    for (const auto& p : probes) {
        const double ratio = std::pow(2.0, p.semis / 12.0);
        const double target = p.f0 * ratio;

        // A LIMIT OF THE MEASUREMENT, not of the shifter: the target is measured by
        // running YIN over the output, and YIN's own lower bound is 70 Hz. Below that it
        // correctly reports "no pitch", which would otherwise appear here as a spectacular
        // error. The audio is fine; the ruler is too short. Marked rather than hidden,
        // because a blank in a results table invites the assumption that it was skipped
        // for a reason someone once knew.
        if (target < 75.0) {
            std::printf("%8.0f Hz %+8.0f st %10s %10s   (target %.0f Hz is below the analyser's 70 Hz floor)\n",
                        p.f0, p.semis, "n/a", "n/a", target);
            continue;
        }

        GranularShifter g;
        PsolaShifter ps;
        const double eg = measureCentsError(g, p.f0, ratio);
        const double ep = measureCentsError(ps, p.f0, ratio);
        std::printf("%8.0f Hz %+8.0f st %+10.1f %+10.1f\n", p.f0, p.semis, eg, ep);
    }

    std::printf("\n=== Algorithmic latency (excludes I/O and host buffering) ===\n");
    GranularShifter g;
    PsolaShifter ps;
    YinAnalyzer y;
    g.prepare(kSR, 128);
    ps.prepare(kSR, 128);
    y.prepare(kSR, 128);
    std::printf("  YIN acquisition (worst case)  %6zu samples  %6.1f ms\n",
                y.latencySamples(), static_cast<double>(y.latencySamples()) * 1000.0 / kSR);
    std::printf("  granular (at rest)            %6zu samples  %6.1f ms\n",
                g.latencySamples(), static_cast<double>(g.latencySamples()) * 1000.0 / kSR);
    std::printf("  psola (worst case)            %6zu samples  %6.1f ms\n",
                ps.latencySamples(), static_cast<double>(ps.latencySamples()) * 1000.0 / kSR);
    return 0;
}
