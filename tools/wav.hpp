// tools/wav.hpp — minimal 16-bit PCM mono WAV read/write.
//
// Deliberately not libsndfile: the harness needs exactly one format, and a build
// dependency for that is a worse trade than sixty lines. If stereo, 24-bit or float WAV
// ever becomes necessary, that is the moment to take the dependency, not before.

#pragma once

#include "vh/types.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace vhtools {

#pragma pack(push, 1)
struct WavHeader {
    char     riff[4]{'R','I','F','F'};
    uint32_t riffSize = 0;
    char     wave[4]{'W','A','V','E'};
    char     fmt[4]{'f','m','t',' '};
    uint32_t fmtSize = 16;
    uint16_t format = 1;
    uint16_t channels = 1;
    uint32_t sampleRate = 48000;
    uint32_t byteRate = 96000;
    uint16_t blockAlign = 2;
    uint16_t bitsPerSample = 16;
    char     data[4]{'d','a','t','a'};
    uint32_t dataSize = 0;
};
#pragma pack(pop)

template <typename T>
inline bool rd(FILE* f, T* dst, size_t count = 1) {
    return std::fread(dst, sizeof(T), count, f) == count;
}

inline bool readWav(const std::string& path, std::vector<vh::Sample>& out, double& sr) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char id[4];
    uint32_t sz = 0;
    if (!rd(f, id, 4) || std::memcmp(id, "RIFF", 4) != 0) { std::fclose(f); return false; }
    if (!rd(f, &sz)) { std::fclose(f); return false; }
    if (!rd(f, id, 4) || std::memcmp(id, "WAVE", 4) != 0) { std::fclose(f); return false; }

    uint16_t channels = 1, bits = 16, format = 1;
    while (rd(f, id, 4) && rd(f, &sz)) {
        const long next = std::ftell(f) + static_cast<long>(sz) + (sz & 1);
        if (std::memcmp(id, "fmt ", 4) == 0) {
            uint16_t blockAlign; uint32_t rate, byteRate;
            if (!rd(f, &format) || !rd(f, &channels) || !rd(f, &rate) ||
                !rd(f, &byteRate) || !rd(f, &blockAlign) || !rd(f, &bits)) { std::fclose(f); return false; }
            sr = rate;
        } else if (std::memcmp(id, "data", 4) == 0) {
            const size_t frames = sz / (bits / 8) / (channels ? channels : 1);
            out.resize(frames);
            std::vector<int16_t> raw(sz / 2);
            if (!rd(f, raw.data(), raw.size())) { std::fclose(f); return false; }
            for (size_t i = 0; i < frames; ++i) out[i] = raw[i * channels] / 32768.0f;
            std::fclose(f);
            return format == 1 && bits == 16;
        }
        std::fseek(f, next, SEEK_SET);
    }
    std::fclose(f);
    return false;
}

inline bool writeWav(const std::string& path, const std::vector<vh::Sample>& in, double sr) {
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

} // namespace vhtools
