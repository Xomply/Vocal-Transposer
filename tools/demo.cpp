// tools/demo.cpp — render dry and wet audio through the real Engine.
//
// Produces the listening material. Every variant goes through the same code path the
// audio callback will: same Engine, same block size, same shifters. Nothing here is a
// special offline mode, which is the point — if it sounds right here it will sound right
// live, modulo I/O latency.

#include "vh/engine.hpp"
#include "vh/granular_shifter.hpp"
#include "vh/psola_shifter.hpp"
#include "vh/yin.hpp"

#include "wav.hpp"
#include "voice.hpp"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace vh;
using namespace vhtools;

namespace {

constexpr double kSR = 48000.0;
constexpr FrameCount kBlock = 128;

enum class EngineChoice { Granular, Psola, Blend };

struct NoteEvent {
    double timeSec;
    int note;
    bool on;
};

std::vector<Sample> renderEngine(const std::vector<Sample>& dry,
                                 EngineChoice choice,
                                 const std::vector<NoteEvent>& events,
                                 RatioSource mode,
                                 const IBlendPolicy& policy,
                                 float alignment,
                                 bool includeDry) {
    Engine engine;
    EngineConfig cfg;
    cfg.sampleRate = kSR;
    cfg.maxBlock = kBlock;
    cfg.ratioSource = mode;
    cfg.rootMidiNote = 45;

    auto makeGranular = []() -> std::unique_ptr<IPitchShifter> { return std::make_unique<GranularShifter>(); };
    auto makePsola    = []() -> std::unique_ptr<IPitchShifter> { return std::make_unique<PsolaShifter>(); };

    switch (choice) {
        case EngineChoice::Granular: engine.prepare(cfg, std::make_unique<YinAnalyzer>(), makeGranular, nullptr); break;
        case EngineChoice::Psola:    engine.prepare(cfg, std::make_unique<YinAnalyzer>(), makePsola, nullptr); break;
        case EngineChoice::Blend:    engine.prepare(cfg, std::make_unique<YinAnalyzer>(), makeGranular, makePsola); break;
    }
    engine.setBlendPolicy(&policy);
    engine.setBlendAlignment(alignment);

    std::vector<Sample> out(dry.size(), 0.0f);
    std::vector<Sample> blk(kBlock, 0.0f);

    size_t nextEvent = 0;
    for (FrameCount i = 0; i + kBlock <= dry.size(); i += kBlock) {
        const double t = static_cast<double>(i) / kSR;
        while (nextEvent < events.size() && events[nextEvent].timeSec <= t) {
            const auto& e = events[nextEvent++];
            if (e.on) engine.noteOn(e.note, 1.0f);
            else      engine.noteOff(e.note);
        }
        engine.process(dry.data() + i, blk.data(), kBlock);
        for (FrameCount k = 0; k < kBlock; ++k) {
            // Voices are summed at unity inside the Engine; scale here so a six-voice
            // chord does not clip. A real mixer would have per-voice gain; this is the
            // demo's stand-in.
            out[i + k] = blk[k] * 0.42f + (includeDry ? dry[i + k] * 0.5f : 0.0f);
        }
    }
    return out;
}

void report(const char* label, const std::vector<Sample>& s) {
    double sum = 0.0;
    float peak = 0.0f;
    for (auto x : s) { sum += static_cast<double>(x) * x; peak = std::max(peak, std::fabs(x)); }
    std::printf("  %-28s rms %.4f  peak %.3f\n", label, std::sqrt(sum / s.size()), peak);
}

} // namespace

int main(int argc, char** argv) {
    const std::string outDir = argc > 1 ? argv[1] : ".";

    // A four-note sung phrase: held vowel with vibrato, a vowel change at constant pitch,
    // a fricative, a legato note change, and a low sustained note.
    const auto dry = renderPhrase(defaultPhrase(), kSR, 5.2);
    std::printf("Rendered %.1f s of synthetic voice\n", dry.size() / kSR);

    // Chord progression on the keyboard, independent of what is being sung. This is the
    // instrument: the singer supplies timbre and articulation, the keyboard supplies
    // harmony.
    //
    // A minor -> F major -> C major -> G major, four voices each.
    const std::vector<NoteEvent> chords = {
        {0.05, 45, true}, {0.05, 52, true}, {0.05, 57, true}, {0.05, 60, true},   // Am
        {1.35, 45, false},{1.35, 52, false},{1.35, 57, false},{1.35, 60, false},
        {1.40, 41, true}, {1.40, 48, true}, {1.40, 53, true}, {1.40, 57, true},   // F
        {2.45, 41, false},{2.45, 48, false},{2.45, 53, false},{2.45, 57, false},
        {2.50, 43, true}, {2.50, 52, true}, {2.50, 55, true}, {2.50, 60, true},   // C/G
        {3.55, 43, false},{3.55, 52, false},{3.55, 55, false},{3.55, 60, false},
        {3.60, 38, true}, {3.60, 45, true}, {3.60, 50, true}, {3.60, 57, true},   // D-ish
        {5.00, 38, false},{5.00, 45, false},{5.00, 50, false},{5.00, 57, false},
    };

    FastOnlyPolicy fastOnly;
    QualityOnlyPolicy qualityOnly;
    OnsetHandoverPolicy handover;

    struct Variant {
        const char* file;
        const char* label;
        EngineChoice engine;
        const IBlendPolicy* policy;
        float alignment;
        bool withDry;
    };

    const Variant variants[] = {
        {"wet_granular.wav",  "granular only (fast)",   EngineChoice::Granular, &fastOnly,    0.0f, false},
        {"wet_psola.wav",     "psola only (quality)",   EngineChoice::Psola,    &fastOnly,    0.0f, false},
        {"wet_blend.wav",     "blend, no alignment",    EngineChoice::Blend,    &handover,    0.0f, false},
        {"wet_blend_aligned.wav", "blend, aligned",     EngineChoice::Blend,    &handover,    1.0f, false},
        {"mix_psola.wav",     "psola + dry (the mix)",  EngineChoice::Psola,    &fastOnly,    0.0f, true},
    };

    writeWav(outDir + "/dry.wav", dry, kSR);
    std::printf("Levels:\n");
    report("dry", dry);

    for (const auto& v : variants) {
        const auto wet = renderEngine(dry, v.engine, chords, RatioSource::AbsoluteTarget,
                                      *v.policy, v.alignment, v.withDry);
        writeWav(outDir + "/" + v.file, wet, kSR);
        report(v.label, wet);
    }

    // Mode B for comparison: intervals from a declared root rather than absolute targets.
    // The singer's own intonation, scoops and vibrato ride through untouched.
    {
        const auto wet = renderEngine(dry, EngineChoice::Psola, chords, RatioSource::IntervalFromRoot,
                                      fastOnly, 0.0f, false);
        writeWav(outDir + "/wet_modeB_interval.wav", wet, kSR);
        report("mode B (interval)", wet);
    }

    std::printf("Written to %s\n", outDir.c_str());
    return 0;
}
