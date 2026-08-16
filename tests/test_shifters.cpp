// tests/test_shifters.cpp — do the engines actually shift, and does PSOLA actually
// preserve formants?
//
// The measurement approach matters here. Asserting "the code ran" proves nothing about a
// pitch shifter. These tests synthesise a voice with KNOWN F0 and KNOWN formants, run it
// through an engine, then MEASURE the output's F0 and spectral content. If PSOLA stops
// preserving formants, or the granular shifter stops shifting, the numbers move.

#include <doctest/doctest.h>

#include "vh/audio_ring.hpp"
#include "vh/granular_shifter.hpp"
#include "vh/psola_shifter.hpp"
#include "vh/rt.hpp"
#include "vh/yin.hpp"

#include <cmath>
#include <complex>
#include <vector>

using namespace vh;

namespace {

constexpr double kSR = 48000.0;
constexpr FrameCount kBlock = 128;

// A synthetic voice: glottal pulse train through three formant resonators.
//
// WHY BUILD THIS RATHER THAN USE A SINE: the whole point of PSOLA is that it moves the
// harmonics while leaving the formants alone. A sine has no formants, so the property
// under test does not exist in the signal and the test would pass vacuously. A
// source-filter synthetic gives ground truth for BOTH quantities — which a recorded voice
// would not.
std::vector<Sample> synthVoice(double f0, FrameCount n,
                               double f1 = 700.0, double f2 = 1220.0, double f3 = 2600.0) {
    std::vector<Sample> v(n, 0.0f);

    const double period = kSR / f0;
    std::vector<double> exc(n, 0.0);
    for (double p = 0.0; p < static_cast<double>(n); p += period) {
        const int start = static_cast<int>(p);
        const int width = static_cast<int>(period * 0.3);
        for (int k = 0; k < width && start + k < static_cast<int>(n); ++k) {
            const double t = static_cast<double>(k) / width;
            exc[static_cast<size_t>(start + k)] += 0.5 - 0.5 * std::cos(2.0 * M_PI * t);
        }
    }

    auto resonate = [&](double fc, double bw, double amp) {
        const double r = std::exp(-M_PI * bw / kSR);
        const double theta = 2.0 * M_PI * fc / kSR;
        const double a1 = -2.0 * r * std::cos(theta);
        const double a2 = r * r;
        double z1 = 0.0, z2 = 0.0;
        for (FrameCount i = 0; i < n; ++i) {
            const double y = exc[i] - a1 * z1 - a2 * z2;
            z2 = z1; z1 = y;
            v[i] += static_cast<Sample>(y * amp * (1.0 - r));
        }
    };
    resonate(f1, 80.0, 1.0);
    resonate(f2, 90.0, 0.5);
    resonate(f3, 120.0, 0.25);

    float peak = 1e-9f;
    for (auto s : v) peak = std::max(peak, std::fabs(s));
    for (auto& s : v) s *= 0.5f / peak;
    return v;
}

// Measure F0 of a rendered buffer with our own analyser. Legitimate as a measuring
// instrument because its accuracy is independently pinned in test_yin.cpp against tones
// of known frequency.
float measureF0(const std::vector<Sample>& sig) {
    YinAnalyzer y;
    y.prepare(kSR, kBlock);
    AudioRing ring(1 << 17);
    for (FrameCount i = 0; i + kBlock <= sig.size(); i += kBlock) {
        ring.write(sig.data() + i, kBlock);
        y.process(ring, ring.writePos());
    }
    return y.current().f0Hz;
}

// Energy in a frequency BAND, not at a single frequency.
//
// THE FIRST VERSION OF THIS HELPER MEASURED A SINGLE BIN AND WAS WRONG, in a way worth
// recording because it is an easy trap: a formant is a resonance of the vocal tract, and
// the signal only has energy where a HARMONIC happens to fall inside it. With a 300 Hz
// fundamental there is simply no partial at 700 Hz, so a single-frequency probe measures
// spectral leakage and returns whatever the window sidelobes happen to give — numbers that
// look like data and are noise. Summing across a band that is wide relative to the
// harmonic spacing always catches partials and measures the resonance itself.
double bandEnergy(const std::vector<Sample>& sig, double flo, double fhi,
                  FrameCount from, FrameCount len) {
    double total = 0.0;
    for (double f = flo; f <= fhi; f += 10.0) {
        std::complex<double> acc{0.0, 0.0};
        for (FrameCount i = 0; i < len && from + i < sig.size(); ++i) {
            const double t = static_cast<double>(i);
            // Hann, or the rectangular window's sidelobes swamp the measurement.
            const double w = 0.5 - 0.5 * std::cos(2.0 * M_PI * t / static_cast<double>(len));
            acc += std::polar(w * static_cast<double>(sig[from + i]), -2.0 * M_PI * f * t / kSR);
        }
        const double m = std::abs(acc) / static_cast<double>(len);
        total += m * m;
    }
    return total;
}

// Run one shifter standalone at a fixed ratio.
std::vector<Sample> render(IPitchShifter& sh, const std::vector<Sample>& in, double ratio,
                           const PreservationSpec* presOverride = nullptr) {
    YinAnalyzer y;
    y.prepare(kSR, kBlock);
    AudioRing ring(1 << 17);
    sh.prepare(kSR, kBlock);
    sh.reset();

    PreservationSpec presDefault;
    const PreservationSpec& pres = presOverride ? *presOverride : presDefault;
    ReadCursor cur;
    std::vector<Sample> out(in.size(), 0.0f);
    std::vector<Sample> blk(kBlock, 0.0f);
    bool started = false;

    for (FrameCount i = 0; i + kBlock <= in.size(); i += kBlock) {
        ring.write(in.data() + i, kBlock);
        y.process(ring, ring.writePos());

        // Wait for the analyser to lock before starting, mirroring the Engine's lookback
        // on note-on.
        if (!started) {
            if (y.current().f0Hz <= 0.0f) continue;
            const FrameCount lat = sh.latencySamples();
            cur.next = ring.writePos() > lat ? ring.writePos() - lat : 0;
            started = true;
        }

        ShiftRequest req{};
        req.ring = &ring;
        req.analysis = &y.current();
        req.cursor = &cur;
        req.preservation = &pres;
        req.ratio = ratio;
        req.out = blk.data();
        req.numFrames = kBlock;
        sh.process(req);

        for (FrameCount k = 0; k < kBlock; ++k) out[i + k] = blk[k];
    }
    return out;
}

float centsError(float measured, float expected) {
    return static_cast<float>(1200.0 * std::log2(measured / expected));
}

} // namespace

// ---------------------------------------------------------------------------
// Granular (fast path)
// ---------------------------------------------------------------------------

TEST_CASE("granular shifter moves pitch by the requested ratio") {
    const auto in = synthVoice(150.0, 60000);
    for (double semis : {-5.0, -3.0, 3.0, 5.0, 7.0, 12.0}) {
        const double ratio = std::pow(2.0, semis / 12.0);
        GranularShifter g;
        const auto out = render(g, in, ratio);
        const float f = measureF0(out);
        CAPTURE(semis);
        REQUIRE(f > 20.0f);
        CHECK(std::fabs(centsError(f, static_cast<float>(150.0 * ratio))) < 40.0f);
    }
}

TEST_CASE("the engines differ exactly as their algorithms predict, and mu sits between them") {
    // THE TEST THAT DISTINGUISHES THE ENGINES, and now also the test that pins the voicing
    // profile's central claim.
    //
    // Same input, same ratio, one octave up. Three renders:
    //
    //   granular            resamples, so the WHOLE spectrum stretches. Envelope tracks
    //                       the ratio: mu == ratio.
    //   psola, profile OFF  re-emits UNMODIFIED grains, so the harmonics move and the
    //                       envelope does not: mu == 1.
    //   psola, profile ON   re-emits grains whose CONTENT is resampled by mu = ratio^0.3,
    //                       so the envelope moves a little. Strictly between the two.
    //
    // THAT ORDERING IS THE WHOLE ARCHITECTURE OF VOICE-MODEL.md §6 IN ONE ASSERTION: the
    // exponent interpolates continuously between the two engines already in the repo. If
    // someone changes muFor() into something that is not monotone in k, this fails.
    //
    // Measured as spectral tilt — high-band energy over low-band — rather than as an
    // absolute formant position. An earlier version of this test probed single frequencies
    // and measured nothing but window leakage, because a formant only shows up where a
    // harmonic happens to land inside it. A comparison BETWEEN renders on identical input
    // is immune to that: whatever the measurement's quirks, all three suffer them equally.
    //
    // WHY ORDERING AND NOT ABSOLUTE THRESHOLDS: the previous version asserted
    // `tiltP < tiltIn * 3.0`, which was a threshold tuned to mu == 1 and broke the moment
    // mu became a curve — F1 at 700 Hz crosses the 800/900 Hz band edge under even a 23%
    // warp, so the ratio leaps by two orders of magnitude for a small, correct change.
    // The relationship being claimed was always an ordering. It is now written as one.
    const auto in = synthVoice(150.0, 60000, 700.0, 1220.0, 2600.0);

    PreservationSpec hold;
    hold.voicing.enabled = false;      // the A/B control: mu == 1, pre-profile behaviour

    GranularShifter g;
    PsolaShifter pHold, pWarp;
    const auto og      = render(g, in, 2.0);
    const auto opHold  = render(pHold, in, 2.0, &hold);
    const auto opWarp  = render(pWarp, in, 2.0);

    const FrameCount from = 30000, len = 8192;
    auto tilt = [&](const std::vector<Sample>& s) {
        return bandEnergy(s, 900.0, 1600.0, from, len) /
               (bandEnergy(s, 250.0, 800.0, from, len) + 1e-18);
    };

    const double tiltIn   = tilt(in);
    const double tiltG    = tilt(og);
    const double tiltHold = tilt(opHold);
    const double tiltWarp = tilt(opWarp);
    CAPTURE(tiltIn); CAPTURE(tiltG); CAPTURE(tiltHold); CAPTURE(tiltWarp);

    // Resampling drags the envelope up: markedly more high-band energy than the input had.
    CHECK(tiltG > tiltIn * 5.0);
    // With the profile off, PSOLA leaves the envelope where it was. This is the claim the
    // quality path was built on, and it must remain literally true, not approximately.
    CHECK(tiltHold < tiltIn * 3.0);
    // The profile moves it, but nothing like as far as resampling does.
    CHECK(tiltWarp > tiltHold);
    CHECK(tiltWarp < tiltG);
    // And the extremes are still unambiguously different, which is why both engines exist.
    CHECK(tiltG > tiltHold * 5.0);
}

TEST_CASE("voicing.enabled = false is BIT-IDENTICAL to the engine before the profile existed") {
    // A meta-test, in the spirit of "the guard detects an allocation".
    //
    // The A/B control is only a control if it is exact. mu == 1 takes an integer read path
    // in placeGrain specifically so that disabling the profile does not mean "the same
    // plus cubic interpolator noise" — which would make every listening comparison between
    // profile on and off a comparison of two changes rather than one.
    //
    // If someone removes the `unity` fast path as a simplification, this fails, and the
    // comment defending it has a test attached.
    const auto in = synthVoice(150.0, 40000, 700.0, 1220.0, 2600.0);

    PreservationSpec off;
    off.voicing.enabled = false;

    PreservationSpec zeroK;            // the other way of spelling mu == 1
    zeroK.voicing.muStrength = 0.0f;

    PsolaShifter a, b;
    const auto oa = render(a, in, 0.5, &off);
    const auto ob = render(b, in, 0.5, &zeroK);

    REQUIRE(oa.size() == ob.size());
    for (size_t i = 0; i < oa.size(); ++i) REQUIRE(oa[i] == ob[i]);
}

// ---------------------------------------------------------------------------
// PSOLA (quality path)
// ---------------------------------------------------------------------------

TEST_CASE("psola moves pitch by the requested ratio") {
    const auto in = synthVoice(150.0, 60000);
    for (double semis : {-5.0, -3.0, 2.0, 4.0, 7.0}) {
        const double ratio = std::pow(2.0, semis / 12.0);
        PsolaShifter p;
        const auto out = render(p, in, ratio);
        const float f = measureF0(out);
        CAPTURE(semis);
        REQUIRE(f > 20.0f);
        CHECK(std::fabs(centsError(f, static_cast<float>(150.0 * ratio))) < 40.0f);
    }
}

TEST_CASE("psola PRESERVES formants — the property the quality path exists for") {
    // THE CENTRAL CLAIM OF THIS ENGINE. Shift up a fifth; F1 must stay near 700 Hz rather
    // than sliding to ~1050 Hz the way the resampling path does.
    //
    // If this fails, the quality path has no reason to exist — it is just a slower
    // granular shifter.
    const auto in = synthVoice(150.0, 60000, 700.0, 1220.0, 2600.0);
    PsolaShifter p;
    const auto out = render(p, in, std::pow(2.0, 7.0 / 12.0));

    const FrameCount from = out.size() / 2, len = 8192;
    const double atF1     = bandEnergy(out, 600.0, 800.0, from, len);
    const double atScaled = bandEnergy(out, 900.0, 1150.0, from, len);   // where resampling would put it
    CHECK(atF1 > atScaled);
}

TEST_CASE("psola actually LOWERS pitch past half-pitch, not merely avoids gapping") {
    // THIS TEST PREVIOUSLY CHECKED ONLY FOR GAPS, and its comment named the
    // max(period, synthSpacing) grain sizing as the thing protecting against them. That
    // line was the VH-001 bug: it made each grain span 2/ratio SOURCE periods, so
    // re-spacing the grains could not change the pitch and the output came back at the
    // INPUT pitch. The absence of gaps was being used as a proxy for a working downward
    // shift, and the two came apart completely — a test that passed on broken output.
    //
    // Absence of gaps is still worth pinning, so both assertions live here now. But the
    // pitch assertion is the load-bearing one: it is the property the feature exists for.
    const double f0 = 200.0;
    const double ratio = 0.45;             // past half-pitch, where VH-001 lived
    const auto in = synthVoice(f0, 60000);
    PsolaShifter p;
    const auto out = render(p, in, ratio);

    FrameCount longestGap = 0, run = 0;
    for (FrameCount i = out.size() / 2; i < out.size() - kBlock; ++i) {
        if (std::fabs(out[i]) < 1e-4f) { ++run; longestGap = std::max(longestGap, run); }
        else run = 0;
    }
    // Gaps are now EXPECTED and correct: at ratio 0.45 grains land every period/0.45
    // samples and are only 2*period wide, so the space between glottal pulses is real
    // output, not starvation. The bound is one synthesis spacing plus slack — enough to
    // catch a shifter that has stopped emitting, loose enough not to punish the geometry.
    const FrameCount spacing = static_cast<FrameCount>((48000.0 / f0) / ratio);
    CHECK(longestGap < spacing + spacing / 2);

    // THE ASSERTION THAT WAS MISSING. measureF0 is YIN, which picks the FIRST dip below
    // threshold rather than the global minimum — that is precisely the choice that stops
    // it reporting a subharmonic, and it is what makes it a valid instrument here. An
    // estimator searching a narrow band around the EXPECTED output F0 would have scored
    // the broken shifter as healthy, which is exactly how VH-001 survived both this suite
    // and a measurement sweep. Do not "improve" this by narrowing the search.
    //
    // Output F0 is 90 Hz, comfortably above YIN's 70 Hz floor, so the instrument is in
    // range. Lower the ratio much further in a new test and it will not be.
    const double measured = measureF0(out);
    const double want = f0 * ratio;
    CHECK(measured == doctest::Approx(want).epsilon(0.08));

    // And explicitly NOT the input pitch, which is the specific VH-001 failure signature.
    CHECK(std::fabs(measured - f0) > 0.2 * f0);
}

TEST_CASE("both shifters handle unvoiced audio without exploding") {
    std::vector<Sample> nz(40000);
    std::uint32_t s = 7u;
    for (auto& x : nz) {
        s = s * 1664525u + 1013904223u;
        x = static_cast<Sample>((s >> 9) % 2000) / 1000.0f - 1.0f;
        x *= 0.3f;
    }

    GranularShifter g;
    PsolaShifter p;
    const auto og = render(g, nz, 2.0);
    const auto op = render(p, nz, 2.0);

    for (auto x : og) REQUIRE(std::isfinite(x));
    for (auto x : op) REQUIRE(std::isfinite(x));
}

TEST_CASE("neither shifter allocates on the audio thread") {
    const auto in = synthVoice(140.0, 24000);
    GranularShifter g;
    PsolaShifter p;

    rt::resetViolationCount();
    render(g, in, 1.5);
    render(p, in, 1.5);
    CHECK(rt::violationCount() == 0);
}

TEST_CASE("reported latency is honest, and quality is the slower path") {
    // The blender derives alignment from these numbers. A shifter that under-reports
    // produces a comb between the two engines — easy to misdiagnose as a DSP bug in
    // whichever engine you happened to inspect first.
    GranularShifter g;
    g.prepare(kSR, kBlock);
    CHECK(g.latencySamples() > 0);

    PsolaShifter p;
    p.prepare(kSR, kBlock);
    CHECK(p.latencySamples() > g.latencySamples());
}

TEST_CASE("signals that START WITH SILENCE stay finite — the real-recording case") {
    // REGRESSION TEST for a bug that 36 synthetic tests missed, because every one of them
    // began with a fully primed steady tone. Real recordings begin with silence, room
    // tone, or a breath.
    //
    // With no F0 the granular shifter's geometry falls back to a long grain; early in the
    // stream the read pointer's delay is briefly under the minimum, and the corrective
    // jump subtracts more than has been written. pos_ went NEGATIVE, the unsigned cast was
    // undefined behaviour, the interpolator's fractional part became enormous and the
    // cubic overflowed to -inf. Infinity in an audio buffer is the loudest possible sound.
    //
    // Any new shifter should be run through this case before it is trusted.
    for (double leadIn : {0.05, 0.25, 1.0}) {
        std::vector<Sample> in(static_cast<size_t>(kSR * leadIn), 0.0f);
        const auto voiced = synthVoice(150.0, 40000);
        in.insert(in.end(), voiced.begin(), voiced.end());

        for (double ratio : {0.5, 0.75, 1.5, 2.0}) {
            GranularShifter g;
            PsolaShifter p;
            CAPTURE(leadIn); CAPTURE(ratio);
            for (auto x : render(g, in, ratio)) REQUIRE(std::isfinite(x));
            for (auto x : render(p, in, ratio)) REQUIRE(std::isfinite(x));
        }
    }
}

TEST_CASE("very high fundamentals stay finite — soprano range") {
    // The real sustained sample is a soprano around 810 Hz, near YIN's 1000 Hz ceiling,
    // where the period is only ~59 samples and every grain-size calculation is at its
    // smallest. Worth pinning separately from the mid-range cases.
    const auto in = synthVoice(800.0, 40000, 900.0, 2200.0, 3100.0);
    for (double ratio : {0.5, 1.0, 1.25}) {
        GranularShifter g;
        PsolaShifter p;
        CAPTURE(ratio);
        for (auto x : render(g, in, ratio)) REQUIRE(std::isfinite(x));
        for (auto x : render(p, in, ratio)) REQUIRE(std::isfinite(x));
    }
}

TEST_CASE("output stays finite across extreme ratios") {
    const auto in = synthVoice(110.0, 24000);
    for (double r : {0.25, 0.5, 1.0, 2.0, 4.0}) {
        GranularShifter g;
        PsolaShifter p;
        for (auto x : render(g, in, r)) REQUIRE(std::isfinite(x));
        for (auto x : render(p, in, r)) REQUIRE(std::isfinite(x));
    }
}
