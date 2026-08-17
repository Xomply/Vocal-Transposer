// tools/voice.hpp — a synthetic singing voice, for testing and demos.
//
// WHY SYNTHESISE RATHER THAN RECORD:
//
// 1. GROUND TRUTH. We know the exact F0 contour and exact formant frequencies at every
//    sample. That makes claims like "PSOLA preserved the formants" measurable rather than
//    a matter of opinion. A recorded voice gives you neither number.
//
// 2. REPRODUCIBILITY. Byte-identical input on every run, so a regression is a regression
//    and not a different take.
//
// 3. CONTROL OF THE HARD CASES. Real recordings are mostly easy material. This generator
//    puts the difficult things in deliberately: a fricative (no F0 at all), a legato note
//    change (pitch discontinuity), vibrato, and vowel transitions (formants moving while
//    pitch holds still).
//
// WHAT IT IS NOT: a convincing human. It is a source-filter model — glottal pulse train
// through three formant resonators — which is the right physics but a crude realisation.
// It sounds like a synthesiser singing, and that is fine for measurement. Judge the
// PROCESSING by comparing dry against wet, not by whether the dry sounds like a person.

#pragma once

#include "vh/types.hpp"

#include <cmath>
#include <vector>

namespace vhtools {

struct Vowel {
    const char* name;
    double f1, f2, f3;
};

// Sung vowel formants, adult male, roughly per Peterson & Barney / Sundberg.
inline constexpr Vowel kVowels[] = {
    {"a", 700.0, 1220.0, 2600.0},   // "father"
    {"e", 500.0, 1750.0, 2500.0},   // "bed"
    {"i", 300.0, 2300.0, 3000.0},   // "see"
    {"o", 450.0,  850.0, 2600.0},   // "boat"
    {"u", 330.0,  700.0, 2400.0},   // "boot"
};

struct PhraseSegment {
    double startSec;
    double durSec;
    double midiNote;      // fractional, so glides are expressible
    int vowel;            // index into kVowels; -1 means unvoiced fricative
    double vibratoDepthCents = 0.0;
};

// A short sung phrase exercising the cases that matter.
inline std::vector<PhraseSegment> defaultPhrase() {
    return {
        // Held vowel with vibrato — the easy case the whole system should nail.
        {0.00, 1.20, 45.0, 0, 40.0},        // A2, "a"
        // Vowel change at CONSTANT pitch. Formants move while F0 sits still: the case
        // that tests whether the envelope is being tracked rather than assumed.
        {1.25, 0.80, 45.0, 2, 30.0},        // same pitch, "i"
        // Fricative. No F0 at all. Must pass through unshifted.
        {2.10, 0.35, 0.0, -1},
        // Legato note change — pitch discontinuity, the expensive case for the tracker.
        {2.50, 1.00, 48.0, 3, 45.0},        // C3, "o"
        // Lower note, longer, to exercise the low end where periods are longest.
        {3.60, 1.40, 41.0, 0, 35.0},        // F2, "a"
    };
}

// Render a phrase. `sr` is the sample rate.
inline std::vector<vh::Sample> renderPhrase(const std::vector<PhraseSegment>& phrase,
                                            double sr, double totalSec) {
    const size_t n = static_cast<size_t>(totalSec * sr);
    std::vector<double> f0(n, 0.0);
    std::vector<double> amp(n, 0.0);
    std::vector<double> fr1(n, 700.0), fr2(n, 1220.0), fr3(n, 2600.0);
    std::vector<double> noiseAmp(n, 0.0);

    for (const auto& seg : phrase) {
        const size_t a = static_cast<size_t>(seg.startSec * sr);
        const size_t b = std::min(n, static_cast<size_t>((seg.startSec + seg.durSec) * sr));
        for (size_t i = a; i < b; ++i) {
            const double t = static_cast<double>(i - a) / sr;
            const double u = static_cast<double>(i - a) / static_cast<double>(b - a);

            // Amplitude envelope with soft attack and release — a step onset would make
            // every measurement below a measurement of the click.
            const double env = std::min(1.0, u / 0.06) * std::min(1.0, (1.0 - u) / 0.10);

            if (seg.vowel < 0) {
                noiseAmp[i] = 0.35 * env;
                // A fricative is broadband noise shaped by front-cavity geometry. Reusing
                // the formant filters with high centres approximates /s/ well enough that
                // the voicing detector has something realistic to reject.
                fr1[i] = 4000.0; fr2[i] = 6500.0; fr3[i] = 8000.0;
                continue;
            }

            const double vib = seg.vibratoDepthCents *
                               std::sin(2.0 * vh::kPi * 5.6 * t) *
                               std::min(1.0, t / 0.25);   // vibrato fades in, as singers do
            f0[i] = 440.0 * std::pow(2.0, (seg.midiNote - 69.0 + vib / 100.0) / 12.0);
            amp[i] = env;

            const Vowel& v = kVowels[seg.vowel];
            fr1[i] = v.f1; fr2[i] = v.f2; fr3[i] = v.f3;
        }
    }

    // Smooth the formant trajectories. Articulators are physically slow — a tongue moving
    // from /a/ to /i/ takes 50-100 ms — so instantaneous formant jumps are not merely
    // unrealistic, they are a broadband click.
    auto smooth = [&](std::vector<double>& x, double tauMs) {
        const double k = 1.0 - std::exp(-1.0 / (tauMs * 0.001 * sr));
        double s = x.empty() ? 0.0 : x[0];
        for (size_t i = 0; i < x.size(); ++i) { s += k * (x[i] - s); x[i] = s; }
    };
    smooth(fr1, 45.0); smooth(fr2, 45.0); smooth(fr3, 45.0);
    smooth(amp, 8.0);  smooth(noiseAmp, 8.0);

    // Glottal excitation: one raised-cosine pulse per period. A bare impulse would have
    // energy at Nyquist and make the resonators ring unrealistically.
    std::vector<double> exc(n, 0.0);
    double phase = 0.0;
    std::uint32_t rng = 987654321u;
    for (size_t i = 0; i < n; ++i) {
        if (f0[i] > 20.0) {
            const double period = sr / f0[i];
            phase += 1.0 / period;
            if (phase >= 1.0) {
                phase -= 1.0;
                const int width = std::max(2, static_cast<int>(period * 0.35));
                for (int k = 0; k < width && i + static_cast<size_t>(k) < n; ++k) {
                    const double tt = static_cast<double>(k) / width;
                    exc[i + static_cast<size_t>(k)] += (0.5 - 0.5 * std::cos(2.0 * vh::kPi * tt)) * amp[i];
                }
            }
            // A little aspiration noise even on voiced frames — real voices are never
            // perfectly periodic, and a perfectly periodic input flatters the tracker.
            rng = rng * 1664525u + 1013904223u;
            exc[i] += ((static_cast<double>(rng >> 8) / 8388608.0) - 1.0) * 0.012 * amp[i];
        }
        if (noiseAmp[i] > 0.0) {
            rng = rng * 1664525u + 1013904223u;
            exc[i] += ((static_cast<double>(rng >> 8) / 8388608.0) - 1.0) * noiseAmp[i];
        }
    }

    // Time-varying resonators. State carries across samples so the filters glide with the
    // formants rather than being re-instantiated per block.
    std::vector<vh::Sample> out(n, 0.0f);
    auto resonate = [&](const std::vector<double>& fc, double bw, double gain) {
        double z1 = 0.0, z2 = 0.0;
        for (size_t i = 0; i < n; ++i) {
            const double r = std::exp(-vh::kPi * bw / sr);
            const double theta = 2.0 * vh::kPi * fc[i] / sr;
            const double a1 = -2.0 * r * std::cos(theta);
            const double a2 = r * r;
            const double y = exc[i] - a1 * z1 - a2 * z2;
            z2 = z1; z1 = y;
            out[i] += static_cast<vh::Sample>(y * gain * (1.0 - r));
        }
    };
    resonate(fr1, 80.0, 1.0);
    resonate(fr2, 100.0, 0.6);
    resonate(fr3, 140.0, 0.3);

    float peak = 1e-9f;
    for (auto s : out) peak = std::max(peak, std::fabs(s));
    for (auto& s : out) s *= 0.6f / peak;
    return out;
}

} // namespace vhtools
