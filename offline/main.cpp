// offline/main.cpp — WAV in, WAV out, through the real engine. No audio hardware.
//
// WHY THIS EXISTS BEFORE ANY PLUGIN OR APP:
//
// Iterating on DSP through a plugin host is miserable and slow: you cannot diff the
// output, you cannot regression-test it, you cannot run it in CI, and you cannot bisect a
// bug because every run is a different performance. Rendering a fixed file through the
// same Engine the audio callback will use gives you a deterministic, byte-comparable
// artefact for every change.
//
// It also gives you golden-file testing for free: render a reference, commit the hash,
// and any accidental change in the DSP shows up as a failed comparison rather than as
// "did that always sound like that?".
//
// Minimal WAV support on purpose — 16-bit PCM mono in, float32 mono out. A dependency on
// libsndfile would be more capable and is not worth the build complexity for a harness.

#include "vh/engine.hpp"
#include "vh/passthrough_shifter.hpp"
#include "vh/yin.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// fread returns a count we must check; ignoring it trips -Wunused-result and, more to the
// point, silently produces a half-read header on a truncated file. rd() makes the check
// impossible to forget.
template <typename T>
bool rd(FILE* f, T* dst, size_t count = 1) {
    return std::fread(dst, sizeof(T), count, f) == count;
}

#pragma pack(push, 1)
struct WavHeader {
    char     riff[4]{'R','I','F','F'};
    uint32_t riffSize = 0;
    char     wave[4]{'W','A','V','E'};
    char     fmt[4]{'f','m','t',' '};
    uint32_t fmtSize = 16;
    uint16_t format = 1;          // PCM
    uint16_t channels = 1;
    uint32_t sampleRate = 48000;
    uint32_t byteRate = 96000;
    uint16_t blockAlign = 2;
    uint16_t bitsPerSample = 16;
    char     data[4]{'d','a','t','a'};
    uint32_t dataSize = 0;
};
#pragma pack(pop)

bool readWavMono16(const std::string& path, std::vector<vh::Sample>& out, double& sr) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;

    char id[4];
    uint32_t sz = 0;
    if (!rd(f, id, 4) || std::memcmp(id, "RIFF", 4) != 0) { std::fclose(f); return false; }
    if (!rd(f, &sz)) { std::fclose(f); return false; }
    if (!rd(f, id, 4) || std::memcmp(id, "WAVE", 4) != 0) { std::fclose(f); return false; }

    uint16_t channels = 1, bits = 16, format = 1;
    // Chunk walk rather than assuming fmt is immediately followed by data — plenty of
    // real files carry LIST/fact chunks in between, and a harness that chokes on your own
    // recordings is a harness you stop using.
    while (rd(f, id, 4) && rd(f, &sz)) {
        const long next = std::ftell(f) + static_cast<long>(sz) + (sz & 1);
        if (std::memcmp(id, "fmt ", 4) == 0) {
            uint16_t blockAlign; uint32_t rate, byteRate;
            if (!rd(f, &format) || !rd(f, &channels) || !rd(f, &rate) ||
                !rd(f, &byteRate) || !rd(f, &blockAlign) || !rd(f, &bits)) {
                std::fclose(f); return false;
            }
            sr = rate;
        } else if (std::memcmp(id, "data", 4) == 0) {
            const size_t frames = sz / (bits / 8) / (channels ? channels : 1);
            out.resize(frames);
            std::vector<int16_t> raw(sz / 2);
            if (!rd(f, raw.data(), raw.size())) { std::fclose(f); return false; }
            for (size_t i = 0; i < frames; ++i) {
                // Downmix by taking channel 0. Analysis is mono by design.
                out[i] = raw[i * channels] / 32768.0f;
            }
            std::fclose(f);
            return format == 1 && bits == 16;
        }
        std::fseek(f, next, SEEK_SET);
    }
    std::fclose(f);
    return false;
}

bool writeWavMono16(const std::string& path, const std::vector<vh::Sample>& in, double sr) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    WavHeader h;
    h.sampleRate = static_cast<uint32_t>(sr);
    h.byteRate = h.sampleRate * 2;
    h.dataSize = static_cast<uint32_t>(in.size() * 2);
    h.riffSize = 36 + h.dataSize;
    std::fwrite(&h, sizeof(h), 1, f);
    std::vector<int16_t> raw(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        float s = in[i];
        s = s > 1.0f ? 1.0f : (s < -1.0f ? -1.0f : s);
        raw[i] = static_cast<int16_t>(s * 32767.0f);
    }
    std::fwrite(raw.data(), 2, raw.size(), f);
    std::fclose(f);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "usage: vh_offline <in.wav> <out.wav> [midiNote ...]\n"
            "  Renders the input through the engine with the given MIDI notes held.\n"
            "  With no notes, renders a single unison voice.\n");
        return 1;
    }

    std::vector<vh::Sample> input;
    double sr = 48000.0;
    if (!readWavMono16(argv[1], input, sr)) {
        std::fprintf(stderr, "error: could not read %s (16-bit PCM WAV expected)\n", argv[1]);
        return 1;
    }

    constexpr vh::FrameCount kBlock = 128;

    vh::Engine engine;
    vh::EngineConfig cfg;
    cfg.sampleRate = sr;
    cfg.maxBlock = kBlock;
    cfg.ratioSource = vh::RatioSource::AbsoluteTarget;

    engine.prepare(cfg, std::make_unique<vh::YinAnalyzer>(),
                   []() { return std::make_unique<vh::PassthroughShifter>(); },
                   []() { return std::make_unique<vh::PassthroughShifter>(); });

    vh::FastOnlyPolicy blend;
    engine.setBlendPolicy(&blend);

    if (argc > 3) {
        for (int i = 3; i < argc; ++i) engine.noteOn(std::atoi(argv[i]), 1.0f);
    } else {
        engine.noteOn(60, 1.0f);
    }

    std::vector<vh::Sample> output(input.size(), 0.0f);
    for (size_t i = 0; i + kBlock <= input.size(); i += kBlock) {
        engine.process(input.data() + i, output.data() + i, kBlock);
    }

    if (!writeWavMono16(argv[2], output, sr)) {
        std::fprintf(stderr, "error: could not write %s\n", argv[2]);
        return 1;
    }

    // Two figures, printed separately and never summed — BUGS.md VH-012. Steady state is
    // "how far behind the live edge is a locked, running voice" (correct for both modes);
    // acquisition adds Mode A's F0-lock convergence cost on top (zero for Mode B, which
    // needs no F0). The old single "algorithmic latency" line conflated the two, over-
    // reporting Mode A's steady-state figure by roughly YIN's whole analysis window.
    const vh::FrameCount steady = engine.steadyStateLatencySamples();
    const vh::FrameCount acquire = engine.acquisitionLatencySamples();
    std::printf("rendered %zu frames at %.0f Hz | voices=%d | "
                "steady-state latency=%zu samples (%.1f ms) | "
                "acquisition latency (Mode A onset only, VH-012)=%zu samples (%.1f ms)\n",
                input.size(), sr, engine.activeVoiceCount(),
                steady, steady * 1000.0 / sr,
                acquire, acquire * 1000.0 / sr);
    return 0;
}
