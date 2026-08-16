// tools/trace.cpp — vh_sweep, plus a CSV of what the analyser believed at every block.
//
// WHY THIS EXISTS. BUGS.md VH-002 ("clicks when the source pitch moves quickly") names two
// candidate causes and says how to tell them apart: "log the epoch series and the per-block
// period against click timestamps. If clicks align with epoch-interval discontinuities it
// is (1); if they are spread evenly through glides it is (2)." That experiment needs a log
// that does not exist. This is the log.
//
// WHAT IT IS NOT. It is not an instrumented Engine. Nothing in core/ knows this tool
// exists, no diagnostic hook was added to a shifter, and the audio path is bit-identical to
// vh_sweep's — which matters, because a defect that only appears when tracing is off is the
// classic way an instrumented build wastes a day. Everything below is read from
// `Engine::analysis()`, the public narrow waist, once per block.
//
// THE ONE INFERENCE IT MAKES, and the one thing to keep in sync by hand: `passthrough` is
// recomputed here from the same expression the shifters use. If that expression changes in
// psola_shifter.cpp or granular_shifter.cpp, change it here too, or the attribution in
// click_probe.py silently starts describing a decision the engine is not making. It is
// duplicated rather than exported because exporting it would mean a diagnostic accessor on
// the audio path, and this is a measurement tool, not a feature.
//
// usage: vh_trace <in.wav> <out.wav> <out.csv> <granular|psola> <semitones>
//                 [muStrength] [tiltStrength]

#include "vh/engine.hpp"
#include "vh/granular_shifter.hpp"
#include "vh/psola_shifter.hpp"
#include "vh/yin.hpp"
#include "wav.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace vh;
using namespace vhtools;

namespace {
constexpr FrameCount kBlock = 128;   // matches vh_sweep and vh_render; see sweep.cpp
constexpr int kRoot = 60;
} // namespace

int main(int argc, char** argv) {
    if (argc < 6) {
        std::fprintf(stderr,
            "usage: vh_trace <in.wav> <out.wav> <out.csv> <granular|psola> <semitones>"
            " [muStrength] [tiltStrength]\n");
        return 1;
    }

    const std::string inPath = argv[1], outPath = argv[2], csvPath = argv[3], which = argv[4];
    const int semis = std::atoi(argv[5]);
    const float muStrength   = argc > 6 ? static_cast<float>(std::atof(argv[6])) : 1.0f;
    const float tiltStrength = argc > 7 ? static_cast<float>(std::atof(argv[7])) : 1.0f;

    if (which != "granular" && which != "psola") {
        std::fprintf(stderr, "error: engine must be 'granular' or 'psola'\n");
        return 1;
    }

    std::vector<Sample> dry;
    double sr = 48000.0;
    if (!readWav(inPath, dry, sr)) {
        std::fprintf(stderr, "error: could not read %s\n", inPath.c_str());
        return 1;
    }

    Engine engine;
    EngineConfig cfg;
    cfg.sampleRate = sr;
    cfg.maxBlock = kBlock;
    cfg.ratioSource = RatioSource::IntervalFromRoot;
    cfg.rootMidiNote = kRoot;
    cfg.preservation.voicing.muStrength = muStrength;
    cfg.preservation.voicing.tiltStrength = tiltStrength;

    auto mkG = []() -> std::unique_ptr<IPitchShifter> { return std::make_unique<GranularShifter>(); };
    auto mkP = []() -> std::unique_ptr<IPitchShifter> { return std::make_unique<PsolaShifter>(); };
    if (which == "granular") engine.prepare(cfg, std::make_unique<YinAnalyzer>(), mkG, nullptr);
    else                     engine.prepare(cfg, std::make_unique<YinAnalyzer>(), mkP, nullptr);

    FastOnlyPolicy fastOnly;
    engine.setBlendPolicy(&fastOnly);
    engine.setBlendAlignment(0.0f);

    std::FILE* csv = std::fopen(csvPath.c_str(), "w");
    if (!csv) { std::fprintf(stderr, "error: could not write %s\n", csvPath.c_str()); return 1; }
    std::fprintf(csv, "time_s,f0_hz,period_samples,voiced,held,confidence,"
                      "epoch_interval,epoch_count,passthrough\n");

    std::vector<Sample> out(dry.size(), 0.0f);
    std::vector<Sample> blk(kBlock, 0.0f);

    bool started = false;
    for (FrameCount i = 0; i + kBlock <= dry.size(); i += kBlock) {
        if (!started) { engine.noteOn(kRoot + semis, 1.0f); started = true; }
        engine.process(dry.data() + i, blk.data(), kBlock);
        for (FrameCount k = 0; k < kBlock; ++k) out[i + k] = blk[k];

        const AnalysisFrame& a = engine.analysis();

        // Interval between the two most recent epochs. Compared against periodSamples in
        // click_probe.py: a phase reference that has relocked onto a different crest shows
        // up as an interval that is not the period, and that is candidate (1) in VH-002.
        double epochInterval = 0.0;
        if (a.epochCount >= 2) {
            const Pos b = a.epochs[a.epochCount - 1], c = a.epochs[a.epochCount - 2];
            epochInterval = static_cast<double>(b - c);
        }

        const bool passthrough =
            (a.voicing == Voicing::Unvoiced &&
             cfg.preservation.unvoiced != UnvoicedPolicy::Shift) ||
            a.periodSamples < 8.0f || a.epochCount == 0;

        std::fprintf(csv, "%.6f,%.3f,%.3f,%d,%d,%.4f,%.1f,%d,%d\n",
                     static_cast<double>(i + kBlock) / sr,
                     static_cast<double>(a.f0Hz),
                     static_cast<double>(a.periodSamples),
                     a.voicing == Voicing::Voiced ? 1 : 0,
                     a.f0IsHeld ? 1 : 0,
                     static_cast<double>(a.confidence),
                     epochInterval,
                     a.epochCount,
                     passthrough ? 1 : 0);
    }
    std::fclose(csv);

    if (!writeWav(outPath, out, sr)) {
        std::fprintf(stderr, "error: could not write %s\n", outPath.c_str());
        return 1;
    }
    return 0;
}
