#include <doctest/doctest.h>

#include "vh/audio_ring.hpp"
#include "vh/rt.hpp"
#include "vh/yin.hpp"

#include <cmath>
#include <vector>

using namespace vh;

namespace {

constexpr double kSR = 48000.0;

// A synthetic voice: a fundamental plus harmonics with a decaying spectrum.
//
// WHY NOT A PURE SINE: a pure sine is the easy case, and passing on it proves almost
// nothing about a tracker that will meet a real voice. The interesting failure — the
// octave error — happens because a signal periodic at T is also periodic at 2T, and that
// only bites when harmonics are present. Testing on a sine would let an octave-error bug
// through.
std::vector<Sample> harmonicTone(double f0, FrameCount n, int partials = 8) {
    std::vector<Sample> v(n);
    for (FrameCount i = 0; i < n; ++i) {
        double s = 0.0;
        for (int k = 1; k <= partials; ++k) {
            s += std::sin(2.0 * M_PI * f0 * k * static_cast<double>(i) / kSR) / k;
        }
        v[i] = static_cast<Sample>(s * 0.2);
    }
    return v;
}

std::vector<Sample> noise(FrameCount n, std::uint32_t seed = 12345) {
    std::vector<Sample> v(n);
    std::uint32_t s = seed;
    for (FrameCount i = 0; i < n; ++i) {
        s = s * 1664525u + 1013904223u;
        v[i] = static_cast<Sample>((s >> 8) & 0xFFFF) / 32768.0f - 1.0f;
        v[i] *= 0.05f;
    }
    return v;
}

float centsError(float measured, float expected) {
    return static_cast<float>(1200.0 * std::log2(measured / expected));
}

// Feed a whole signal through in blocks, as the audio thread would.
void run(YinAnalyzer& y, AudioRing& ring, const std::vector<Sample>& sig, FrameCount block = 128) {
    for (FrameCount i = 0; i + block <= sig.size(); i += block) {
        ring.write(sig.data() + i, block);
        y.process(ring, ring.writePos());
    }
}

} // namespace

TEST_CASE("recovers the fundamental within a few cents across the vocal range") {
    // Sweeping the range matters: the design document is explicit that low fundamentals
    // are the expensive case, and a tracker tuned on a comfortable tenor note can be
    // quietly useless at the bottom of a bass range — which is exactly where the harmony
    // voices will sit.
    for (double f0 : {82.4, 110.0, 146.8, 220.0, 440.0}) {
        YinAnalyzer y;
        y.prepare(kSR, 128);
        AudioRing ring(1 << 16);
        run(y, ring, harmonicTone(f0, 24000));

        const auto& f = y.current();
        CAPTURE(f0);
        CHECK(f.voicing == Voicing::Voiced);
        CHECK(std::fabs(centsError(f.f0Hz, static_cast<float>(f0))) < 10.0f);
    }
}

TEST_CASE("does not make octave errors on a harmonically rich tone") {
    // The catastrophic failure mode: not a slight detune, but a harmony voice a full
    // octave adrift. This test pins the "first dip below threshold, not global minimum"
    // choice in yin.cpp. If someone optimises that loop into a global-minimum search,
    // this fails.
    YinAnalyzer y;
    y.prepare(kSR, 128);
    AudioRing ring(1 << 16);
    run(y, ring, harmonicTone(98.0, 24000, 16));

    const auto& f = y.current();
    REQUIRE(f.voicing == Voicing::Voiced);
    CHECK(f.f0Hz > 90.0f);
    CHECK(f.f0Hz < 106.0f);           // not 49, not 196
}

TEST_CASE("reports noise as unvoiced") {
    YinAnalyzer y;
    y.prepare(kSR, 128);
    AudioRing ring(1 << 16);
    run(y, ring, noise(24000));
    CHECK(y.current().voicing == Voicing::Unvoiced);
}

TEST_CASE("holds F0 through an unvoiced gap instead of re-acquiring") {
    // THE BEHAVIOUR THE LATENCY ANALYSIS DEPENDS ON.
    //
    // Sing a vowel, then a consonant, then the vowel again. Without holding, the
    // consonant reads as lost lock and the tracker re-pays 18-27 ms of acquisition on
    // every syllable — the instrument stutters through words while feeling fine on a
    // sustained note. With holding, the pitch sails through.
    //
    // If this test fails, Mode A's responsiveness claim in latency-budget.md is void.
    YinAnalyzer y;
    y.prepare(kSR, 128);
    AudioRing ring(1 << 16);

    run(y, ring, harmonicTone(120.0, 16000));
    REQUIRE(y.current().voicing == Voicing::Voiced);
    const float lockedF0 = y.current().f0Hz;
    REQUIRE(lockedF0 > 100.0f);

    run(y, ring, noise(2400));                       // ~50 ms of fricative
    const auto& during = y.current();
    CHECK(during.voicing == Voicing::Unvoiced);      // honestly reported...
    CHECK(during.f0IsHeld);                          // ...but the pitch is carried

    // NOT bit-exact equality, and the reason is worth recording because the first version
    // of this test asserted it and failed:
    // the held value is the last CONFIDENT estimate, and as the analysis window slides
    // into the fricative it still contains mostly voiced material for a few hops. Those
    // hops produce legitimate, slightly different estimates before voicing drops out. So
    // the held pitch is the estimate from the last good hop, not the one we happened to
    // sample. A few cents of difference is correct behaviour; a large jump would not be.
    CHECK(std::fabs(1200.0f * std::log2(during.f0Hz / lockedF0)) < 20.0f);
}

TEST_CASE("gives up holding after maxHoldMs") {
    // Holding forever would leave a stale pitch lying around after a phrase genuinely
    // ends, which would make the next entry shift against a pitch nobody is singing.
    YinConfig cfg;
    cfg.maxHoldMs = 20.0f;
    YinAnalyzer y(cfg);
    y.prepare(kSR, 128);
    AudioRing ring(1 << 17);

    run(y, ring, harmonicTone(120.0, 16000));
    REQUIRE(y.current().voicing == Voicing::Voiced);

    run(y, ring, noise(24000));                      // 500 ms, far beyond the 20 ms hold
    CHECK_FALSE(y.current().f0IsHeld);
    CHECK(y.current().f0Hz == doctest::Approx(0.0f));
}

TEST_CASE("analysis allocates nothing on the audio thread") {
    YinAnalyzer y;
    y.prepare(kSR, 128);                             // allocation belongs here
    AudioRing ring(1 << 16);
    const auto sig = harmonicTone(150.0, 12000);

    rt::resetViolationCount();
    run(y, ring, sig);
    CHECK(rt::violationCount() == 0);
}

TEST_CASE("reported latency matches the window it actually needs") {
    // The blender derives its alignment from latencySamples(). A figure that drifts from
    // reality shows up as a comb filter between the two engines, which is easy to
    // misdiagnose as a shifter bug.
    YinAnalyzer y;
    y.prepare(kSR, 128);
    CHECK(y.latencySamples() == y.windowSamples());
    // Two periods of the 70 Hz lower bound, give or take the +1 in maxTau.
    CHECK(y.latencySamples() > static_cast<FrameCount>(2.0 * kSR / 70.0) - 4);
}
