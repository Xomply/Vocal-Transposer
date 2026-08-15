// tools/render.cpp — run a REAL recording through the engine.
//
// This is the tool that matters most going forward. Everything validated so far used a
// synthetic singer, which was the right choice for measurement (it gives ground truth for
// F0 and formants that a recording cannot) but is also the friendliest possible input:
// perfectly periodic, clean voicing boundaries, no room, no breath, no creak.
//
// Usage:
//   vh_render <in.wav> <outdir> <prefix> "<t>:<n>,<n>,<n>; <t>:<n>,<n>,<n>; ..."
//
// where <t> is seconds and <n> are MIDI note numbers held from that time until the next
// entry. Example: "0:60,64,67; 2.5:59,62,67"
//
// Renders the same variant set as the synthetic demo, so the two are directly comparable.

#include "vh/engine.hpp"
#include "vh/granular_shifter.hpp"
#include "vh/psola_shifter.hpp"
#include "vh/yin.hpp"

#include "wav.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace vh;
using namespace vhtools;

namespace {

constexpr FrameCount kBlock = 128;

struct Chord {
    double timeSec;
    std::vector<int> notes;
};

std::vector<Chord> parseChords(const std::string& spec) {
    std::vector<Chord> out;
    size_t i = 0;
    while (i < spec.size()) {
        size_t semi = spec.find(';', i);
        if (semi == std::string::npos) semi = spec.size();
        std::string part = spec.substr(i, semi - i);
        i = semi + 1;

        const size_t colon = part.find(':');
        if (colon == std::string::npos) continue;
        Chord c;
        c.timeSec = std::atof(part.substr(0, colon).c_str());
        std::string notes = part.substr(colon + 1);
        size_t j = 0;
        while (j < notes.size()) {
            size_t comma = notes.find(',', j);
            if (comma == std::string::npos) comma = notes.size();
            const std::string tok = notes.substr(j, comma - j);
            if (!tok.empty()) c.notes.push_back(std::atoi(tok.c_str()));
            j = comma + 1;
        }
        if (!c.notes.empty()) out.push_back(c);
    }
    return out;
}

enum class Choice { Granular, Psola, Blend };

std::vector<Sample> renderVariant(const std::vector<Sample>& dry, double sr,
                                  Choice choice, const std::vector<Chord>& chords,
                                  RatioSource mode, const IBlendPolicy& policy,
                                  float alignment, bool includeDry, int rootNote) {
    Engine engine;
    EngineConfig cfg;
    cfg.sampleRate = sr;
    cfg.maxBlock = kBlock;
    cfg.ratioSource = mode;
    cfg.rootMidiNote = rootNote;

    auto mkG = []() -> std::unique_ptr<IPitchShifter> { return std::make_unique<GranularShifter>(); };
    auto mkP = []() -> std::unique_ptr<IPitchShifter> { return std::make_unique<PsolaShifter>(); };

    switch (choice) {
        case Choice::Granular: engine.prepare(cfg, std::make_unique<YinAnalyzer>(), mkG, nullptr); break;
        case Choice::Psola:    engine.prepare(cfg, std::make_unique<YinAnalyzer>(), mkP, nullptr); break;
        case Choice::Blend:    engine.prepare(cfg, std::make_unique<YinAnalyzer>(), mkG, mkP); break;
    }
    engine.setBlendPolicy(&policy);
    engine.setBlendAlignment(alignment);

    std::vector<Sample> out(dry.size(), 0.0f);
    std::vector<Sample> blk(kBlock, 0.0f);

    size_t next = 0;
    std::vector<int> held;

    for (FrameCount i = 0; i + kBlock <= dry.size(); i += kBlock) {
        const double t = static_cast<double>(i) / sr;
        while (next < chords.size() && chords[next].timeSec <= t) {
            for (int n : held) engine.noteOff(n);
            held = chords[next].notes;
            for (int n : held) engine.noteOn(n, 1.0f);
            ++next;
        }
        engine.process(dry.data() + i, blk.data(), kBlock);
        const float g = 1.0f / std::sqrt(static_cast<float>(std::max<size_t>(1, held.size())));
        for (FrameCount k = 0; k < kBlock; ++k) {
            out[i + k] = blk[k] * g * 0.9f + (includeDry ? dry[i + k] * 0.5f : 0.0f);
        }
    }
    return out;
}

void report(const char* label, const std::vector<Sample>& s) {
    double sum = 0.0;
    float peak = 0.0f;
    for (auto x : s) { sum += static_cast<double>(x) * x; peak = std::max(peak, std::fabs(x)); }
    std::printf("  %-26s rms %.4f  peak %.3f\n", label, std::sqrt(sum / s.size()), peak);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr,
            "usage: vh_render <in.wav> <outdir> <prefix> \"<t>:<n>,<n>; <t>:<n>,<n>\" [rootMidi]\n");
        return 1;
    }
    const std::string in = argv[1], outDir = argv[2], prefix = argv[3];
    const auto chords = parseChords(argv[4]);
    const int root = argc > 5 ? std::atoi(argv[5]) : 60;

    std::vector<Sample> dry;
    double sr = 48000.0;
    if (!readWav(in, dry, sr)) {
        std::fprintf(stderr, "error: could not read %s (16-bit PCM WAV expected)\n", in.c_str());
        return 1;
    }
    std::printf("%s: %.2f s @ %.0f Hz, %zu chords\n", in.c_str(), dry.size() / sr, sr, chords.size());

    FastOnlyPolicy fastOnly;
    OnsetHandoverPolicy handover;

    writeWav(outDir + "/" + prefix + "_dry.wav", dry, sr);
    report("dry", dry);

    struct V { const char* suffix; const char* label; Choice c; const IBlendPolicy* p; float a; bool d; RatioSource m; };
    const V variants[] = {
        {"_granular.wav",     "granular (fast)",    Choice::Granular, &fastOnly, 0.0f, false, RatioSource::AbsoluteTarget},
        {"_psola.wav",        "psola (quality)",    Choice::Psola,    &fastOnly, 0.0f, false, RatioSource::AbsoluteTarget},
        {"_blend.wav",        "blend, no align",    Choice::Blend,    &handover, 0.0f, false, RatioSource::AbsoluteTarget},
        {"_blend_aligned.wav","blend, aligned",     Choice::Blend,    &handover, 1.0f, false, RatioSource::AbsoluteTarget},
        {"_mix.wav",          "psola + dry",        Choice::Psola,    &fastOnly, 0.0f, true,  RatioSource::AbsoluteTarget},
        {"_modeB.wav",        "mode B (interval)",  Choice::Psola,    &fastOnly, 0.0f, false, RatioSource::IntervalFromRoot},
    };

    for (const auto& v : variants) {
        const auto wet = renderVariant(dry, sr, v.c, chords, v.m, *v.p, v.a, v.d, root);
        writeWav(outDir + "/" + prefix + v.suffix, wet, sr);
        report(v.label, wet);
    }
    return 0;
}
