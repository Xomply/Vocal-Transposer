// tools/sweep.cpp — render ONE engine at ONE constant interval. Nothing else.
//
// WHY THIS EXISTS AND vh_render DOES NOT SUFFICE:
//
// The question is whether the QUALITY path degrades beyond +/- 6 semitones. Answering it
// needs a control rendered at the SAME RATIO by a different method, so that "this sounds
// bad" can be separated from "any shift this large sounds bad".
//
// vh_render cannot supply that control. Its variant table hard-codes RatioSource per
// variant: `_granular.wav` is AbsoluteTarget, and only `_modeB.wav` is IntervalFromRoot.
// So asking vh_render for "granular at -24 semitones" actually yields "granular targeting
// MIDI 36", which for an 787 Hz soprano is a completely different ratio. I made exactly
// that mistake first and the resulting table was nonsense in a way that looked plausible:
// the control column moved smoothly and in the right direction, it was simply measuring
// something else. Recorded here because the next person will reach for vh_render too.
//
// This tool therefore fixes the mode and varies only the engine and the interval.
//
// It is NOT a replacement for vh_render. vh_render renders the six-variant comparison a
// human listens to; this renders one axis of an experiment.
//
// usage: vh_sweep <in.wav> <out.wav> <granular|psola> <semitones>

#include "vh/engine.hpp"
#include "vh/granular_shifter.hpp"
#include "vh/psola_shifter.hpp"
#include "vh/yin.hpp"
#include "wav.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace vh;
using namespace vhtools;

namespace {

// Matches vh_render, so figures from the two tools are comparable. A different block
// size here would change grain scheduling and quietly invalidate cross-tool comparison.
constexpr FrameCount kBlock = 128;

// Mode B's root. The note sent is kRoot + semitones, so the interval IS the argument
// and the ratio is exactly 2^(n/12) with no dependence on what is being sung.
constexpr int kRoot = 60;

} // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr,
            "usage: vh_sweep <in.wav> <out.wav> <granular|psola> <semitones>\n"
            "  Renders ONE engine at ONE constant interval, Mode B (IntervalFromRoot).\n");
        return 1;
    }

    const std::string inPath = argv[1], outPath = argv[2], which = argv[3];
    const int semis = std::atoi(argv[4]);

    if (which != "granular" && which != "psola") {
        std::fprintf(stderr, "error: engine must be 'granular' or 'psola', got '%s'\n", which.c_str());
        return 1;
    }

    const int note = kRoot + semis;
    if (note < 0 || note > 127) {
        std::fprintf(stderr, "error: %+d st from root %d gives MIDI %d, outside 0..127\n",
                     semis, kRoot, note);
        return 1;
    }

    std::vector<Sample> dry;
    double sr = 48000.0;
    if (!readWav(inPath, dry, sr)) {
        std::fprintf(stderr, "error: could not read %s (16-bit PCM WAV expected)\n", inPath.c_str());
        return 1;
    }

    Engine engine;
    EngineConfig cfg;
    cfg.sampleRate = sr;
    cfg.maxBlock = kBlock;
    cfg.ratioSource = RatioSource::IntervalFromRoot;   // the whole point; see header comment
    cfg.rootMidiNote = kRoot;

    auto mkG = []() -> std::unique_ptr<IPitchShifter> { return std::make_unique<GranularShifter>(); };
    auto mkP = []() -> std::unique_ptr<IPitchShifter> { return std::make_unique<PsolaShifter>(); };

    // Single engine in the FAST slot with FastOnlyPolicy. Putting the engine under test in
    // the fast slot regardless of which engine it is keeps the blend weights identical
    // between the two runs, so the only difference between them is the algorithm.
    if (which == "granular") engine.prepare(cfg, std::make_unique<YinAnalyzer>(), mkG, nullptr);
    else                     engine.prepare(cfg, std::make_unique<YinAnalyzer>(), mkP, nullptr);

    FastOnlyPolicy fastOnly;
    engine.setBlendPolicy(&fastOnly);
    engine.setBlendAlignment(0.0f);

    std::vector<Sample> out(dry.size(), 0.0f);
    std::vector<Sample> blk(kBlock, 0.0f);

    bool started = false;
    for (FrameCount i = 0; i + kBlock <= dry.size(); i += kBlock) {
        if (!started) { engine.noteOn(note, 1.0f); started = true; }
        engine.process(dry.data() + i, blk.data(), kBlock);
        // NO per-voice gain scaling and NO dry mixed in. vh_render applies 1/sqrt(n) and
        // an 0.9 trim, which is right for a listening mix and wrong for measurement --
        // it would put a scale factor between the two engines' outputs that has nothing
        // to do with either algorithm.
        for (FrameCount k = 0; k < kBlock; ++k) out[i + k] = blk[k];
    }

    if (!writeWav(outPath, out, sr)) {
        std::fprintf(stderr, "error: could not write %s\n", outPath.c_str());
        return 1;
    }
    return 0;
}
