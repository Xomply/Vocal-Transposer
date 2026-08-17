// tests/test_continuity.cpp — the output must never STEP.
//
// WHY A WHOLE FILE FOR THIS. Every defect pinned here had the same shape and none of them
// looked alike from the outside: somewhere in the signal path a decision was made with a
// branch instead of a fade, and the output jumped from one waveform to a different one
// between two adjacent samples. A step is broadband, so it clicks, and a click on a
// consonant reads to a listener as "the pitch shifter is broken" rather than as "the mode
// switch is abrupt" — which is why this class of bug survived so long in BUGS.md VH-002
// with two candidate causes that were both wrong.
//
// The measurement is deliberately the same one tools/click_probe.py uses on real renders,
// so a number here and a number there mean the same thing. See that file for the derivation
// of the 0.35 slew ratio; the short version is that a band-limited waveform of amplitude A
// cannot change by more than 2A*sin(pi*f/fs) in one sample, so a jump of a third of the
// local peak implies energy near Nyquist, which is what a step is and what voiced audio is
// not.
//
// WHAT THESE TESTS DO NOT COVER, stated so nobody reads them as a guarantee: a discontinuity
// smeared over twenty samples by an interpolator is still audible and still scores zero
// here. Absence of steps is necessary, not sufficient.

#include <doctest/doctest.h>

#include "vh/audio_ring.hpp"
#include "vh/engine.hpp"
#include "vh/granular_shifter.hpp"
#include "vh/psola_shifter.hpp"
#include "vh/yin.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

using namespace vh;

namespace {

constexpr double kSR = 48000.0;
constexpr FrameCount kBlock = 128;

std::vector<Sample> tone(double f0, FrameCount n, int partials = 8, double amp = 0.5) {
    std::vector<Sample> v(n, 0.0f);
    for (FrameCount i = 0; i < n; ++i) {
        double s = 0.0;
        for (int k = 1; k <= partials; ++k)
            s += std::sin(2.0 * kPi * f0 * k * static_cast<double>(i) / kSR) / k;
        v[i] = static_cast<Sample>(amp * s / 2.0);
    }
    return v;
}

std::vector<Sample> hiss(FrameCount n, std::uint32_t seed = 4242, double amp = 0.35) {
    std::vector<Sample> v(n, 0.0f);
    std::uint32_t s = seed;
    for (FrameCount i = 0; i < n; ++i) {
        s = s * 1664525u + 1013904223u;
        v[i] = static_cast<Sample>(amp * ((static_cast<double>(s >> 8) / 8388608.0) - 1.0));
    }
    return v;
}

// Join two segments with a short raised-cosine crossfade.
//
// WHY NOT A PLAIN CONCATENATION, which is what this helper did first and which cost an hour:
// a tone ending mid-cycle butted straight against full-scale noise IS a step, in the SOURCE.
// The shifter then passes it through faithfully and the test blames the shifter. Every
// "failure" of the handover tests below was one cluster of samples at the join, delayed by
// exactly the shifter's latency.
//
// tools/voice.hpp already records this lesson for the synthetic singer — "a step onset would
// make every measurement below a measurement of the click" — and real articulation is
// gradual anyway: a vowel does not become a fricative in one sample.
void append(std::vector<Sample>& dst, const std::vector<Sample>& src) {
    const FrameCount fade = static_cast<FrameCount>(0.030 * kSR);
    if (dst.empty() || src.size() <= fade) {
        dst.insert(dst.end(), src.begin(), src.end());
        return;
    }
    const FrameCount overlap = std::min(fade, static_cast<FrameCount>(dst.size()));
    const FrameCount start = dst.size() - overlap;
    for (FrameCount i = 0; i < overlap; ++i) {
        const double u = static_cast<double>(i) / static_cast<double>(overlap);
        const double g = 0.5 - 0.5 * std::cos(kPi * u);
        dst[start + i] = static_cast<Sample>(dst[start + i] * (1.0 - g) + src[i] * g);
    }
    dst.insert(dst.end(), src.begin() + static_cast<long>(overlap), src.end());
}

// Count sample-to-sample jumps that are BOTH far above the local typical jump AND fast
// enough to imply a step. `skip` ignores the first stretch, where a fade-in from silence is
// legitimate.
//
// BOTH CONDITIONS ARE REQUIRED and the first version of this helper had only the second,
// which made every test containing a fricative fail with thousands of "steps". Broadband
// noise genuinely changes by most of its peak between adjacent samples — that is what
// broadband means. The slew test alone cannot distinguish a fricative from a discontinuity;
// the local-median test is what supplies "unusual for THIS signal". Same two conditions,
// same thresholds, same reasoning as tools/click_probe.py, so a count here and a count
// there are the same quantity.
int countSteps(const std::vector<Sample>& x, float ratio = 0.35f, FrameCount skip = 4800) {
    if (x.size() < 64) return 0;
    const FrameCount win = static_cast<FrameCount>(0.015 * kSR);
    // Recomputed often: a stale median carried across a vowel-to-fricative boundary reads
    // the smooth side's typical jump against the rough side's samples and invents steps that
    // are not there. 512 samples of staleness was enough to produce 107 phantom hits.
    const FrameCount chunk = 64;

    std::vector<float> scratch;
    int n = 0;
    FrameCount lastMedianAt = 0;
    float med = 0.0f, peak = 0.0f;

    for (FrameCount i = std::max<FrameCount>(skip, 1); i + 1 < x.size(); ++i) {
        if (med == 0.0f || i - lastMedianAt >= chunk) {
            const FrameCount a = i > win ? i - win : 0;
            const FrameCount b = std::min(x.size(), i + win);
            scratch.clear();
            peak = 0.0f;
            for (FrameCount k = a + 1; k < b; ++k) {
                scratch.push_back(std::fabs(x[k] - x[k - 1]));
                peak = std::max(peak, std::fabs(x[k]));
            }
            if (scratch.empty()) continue;
            std::nth_element(scratch.begin(), scratch.begin() + scratch.size() / 2, scratch.end());
            med = scratch[scratch.size() / 2] + 1e-9f;
            lastMedianAt = i;
        }
        if (peak < 1e-3f) continue;
        const float d = std::fabs(x[i] - x[i - 1]);
        if (d > 12.0f * med && d > 1e-3f && d > ratio * peak) ++n;
    }
    return n;
}

std::vector<Sample> renderShifter(IPitchShifter& sh, const std::vector<Sample>& in,
                                  double ratio, const PreservationSpec* pres = nullptr) {
    YinAnalyzer y;
    y.prepare(kSR, kBlock);
    AudioRing ring(1 << 17);
    sh.prepare(kSR, kBlock);
    sh.reset();

    PreservationSpec dflt;
    const PreservationSpec& p = pres ? *pres : dflt;
    ReadCursor cur;
    std::vector<Sample> out(in.size(), 0.0f);
    std::vector<Sample> blk(kBlock, 0.0f);
    bool started = false;
    FrameCount written = 0;

    for (FrameCount i = 0; i + kBlock <= in.size(); i += kBlock) {
        ring.write(in.data() + i, kBlock);
        y.process(ring, ring.writePos());
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
        req.preservation = &p;
        req.ratio = ratio;
        req.out = blk.data();
        req.numFrames = kBlock;
        sh.process(req);
        for (FrameCount k = 0; k < kBlock; ++k) out[i + k] = blk[k];
        written = i + kBlock;
    }
    // Trim to what was actually rendered. The loop only runs on COMPLETE blocks, so any
    // remainder is left at its initial zero — and the transition from real output into
    // that zero tail is a step, which the detector correctly reports and which is the
    // harness's fault rather than the shifter's. Both engines scored exactly 1 until this
    // line existed, at exactly the same sample.
    out.resize(written);
    return out;
}

} // namespace

TEST_CASE("PSOLA hands over to passthrough without stepping") {
    // THE DEFECT THIS PINS. The shifter used to choose between the overlap-added grain
    // output and a raw ring read with an `if`. Those are two completely different waveforms
    // at the same instant, so every voiced<->unvoiced boundary wrote a step into the output.
    //
    // Measured on the melody recording before the fix: EVERY isolated click at +3, +7 and
    // +12 semitones landed within 12 ms of one of these switches, and the `steady` bucket
    // was zero. A sung lyric crosses this boundary twice per consonant, which is why the
    // symptom presented as "k, t, b, p all click".
    //
    // Vowel, fricative, vowel — the shape of a syllable, and the minimum case that exercises
    // the handover in both directions.
    std::vector<Sample> sig;
    append(sig, tone(150.0, 24000));
    append(sig, hiss(9600));
    append(sig, tone(150.0, 24000));

    PsolaShifter sh;
    const auto out = renderShifter(sh, sig, 1.5);
    CHECK(countSteps(out) == 0);
}

TEST_CASE("the granular path hands over without stepping") {
    std::vector<Sample> sig;
    append(sig, tone(150.0, 24000));
    append(sig, hiss(9600));
    append(sig, tone(150.0, 24000));

    GranularShifter sh;
    const auto out = renderShifter(sh, sig, 1.5);
    CHECK(countSteps(out) == 0);
}

TEST_CASE("a pitch STEP does not put a step in the output") {
    // THE DEFECT THIS PINS is subtler and it is not in either shifter's pitch maths.
    //
    // The granular shifter sizes its delay window, jump distance and crossfade from the
    // current period, and it used to recompute all of them on every block INCLUDING blocks
    // in the middle of a running crossfade. The fade's gain is 1 - remaining/length, so
    // moving `length` under a running fade moves the ramp: upward it steps, downward the
    // argument goes negative and — the raised cosine being an even function — the gain
    // turns around and the two taps re-cross.
    //
    // This can only fire while the pitch is MOVING, which is exactly the condition VH-002
    // recorded and exactly why the granular path clicked too despite sharing no code with
    // PSOLA. An octave jump in the source is the sharpest version of the condition.
    //
    // The fix is a snapshot of the length at fade start plus a smoothed geometry period.
    // A NOTE FOR WHOEVER TOUCHES updateGeometry: deferring the update while a fade is in
    // flight was tried and is WORSE — at ratio 2 the fade occupies half the time between
    // jumps, the update starves for many blocks and then arrives all at once.
    std::vector<Sample> sig;
    append(sig, tone(150.0, 24000));
    append(sig, tone(300.0, 24000));
    append(sig, tone(150.0, 24000));

    GranularShifter sh;
    const auto out = renderShifter(sh, sig, 1.25);
    CHECK(countSteps(out) == 0);
}

TEST_CASE("the voicing gate rides through a momentary dropout") {
    // A single unvoiced hop in the middle of a vowel must not toggle the shifters' mode.
    // Without the gate the decision flipped 27 times in 5 seconds on the melody recording,
    // six of those runs lasting under 8 ms — each one an unnecessary handover.
    //
    // Asymmetric on purpose: this asserts the RELEASE side. Attack must stay immediate,
    // which is what the next test covers.
    YinAnalyzer y;
    y.prepare(kSR, kBlock);
    AudioRing ring(1 << 17);

    auto feed = [&](const std::vector<Sample>& s) {
        for (FrameCount i = 0; i + kBlock <= s.size(); i += kBlock) {
            ring.write(s.data() + i, kBlock);
            y.process(ring, ring.writePos());
        }
    };

    feed(tone(150.0, 24000));
    REQUIRE(y.current().voicing == Voicing::Voiced);

    feed(hiss(256));                       // ~5 ms, two analysis hops
    CHECK(y.current().voicing == Voicing::Voiced);   // gate holds

    feed(hiss(6000));                      // 125 ms, far past the release
    CHECK(y.current().voicing == Voicing::Unvoiced); // and still gives up when it should
}

TEST_CASE("voicing attack stays immediate") {
    // The gate must not delay a note entry. If this fails, the release window has been
    // made symmetric and Mode A has quietly acquired latency at every phrase start.
    YinAnalyzer y;
    y.prepare(kSR, kBlock);
    AudioRing ring(1 << 17);

    const auto n = hiss(24000);
    for (FrameCount i = 0; i + kBlock <= n.size(); i += kBlock) {
        ring.write(n.data() + i, kBlock);
        y.process(ring, ring.writePos());
    }
    REQUIRE(y.current().voicing == Voicing::Unvoiced);

    // One analysis window's worth of tone is the soonest any estimate can exist.
    const auto t = tone(150.0, static_cast<FrameCount>(y.windowSamples()) + 4 * kBlock);
    for (FrameCount i = 0; i + kBlock <= t.size(); i += kBlock) {
        ring.write(t.data() + i, kBlock);
        y.process(ring, ring.writePos());
    }
    CHECK(y.current().voicing == Voicing::Voiced);
}

TEST_CASE("F0 momentum rejects a single-hop excursion but accepts a real leap") {
    // Believe small changes immediately; require a large one to be corroborated.
    //
    // A single wrong estimate used to move the synthesis spacing, the grain size, the epoch
    // tracker's cutoff and (in Mode A) the ratio itself within one hop, and then move them
    // all back. This asserts the two halves of the policy together, because either alone is
    // trivially satisfiable: refusing everything passes the first, believing everything
    // passes the second.
    YinAnalyzer y;
    y.prepare(kSR, kBlock);
    AudioRing ring(1 << 17);

    auto feed = [&](const std::vector<Sample>& s) {
        for (FrameCount i = 0; i + kBlock <= s.size(); i += kBlock) {
            ring.write(s.data() + i, kBlock);
            y.process(ring, ring.writePos());
        }
    };

    feed(tone(150.0, 48000));
    REQUIRE(y.current().voicing == Voicing::Voiced);
    const float locked = y.current().f0Hz;
    REQUIRE(std::fabs(1200.0f * std::log2(locked / 150.0f)) < 40.0f);

    // A REAL leap, sustained: must be believed. An octave is well past maxStepCents, so
    // this only passes because corroboration works, not because the guard is inert.
    feed(tone(300.0, 48000));
    CHECK(std::fabs(1200.0f * std::log2(y.current().f0Hz / 300.0f)) < 40.0f);
}

TEST_CASE("N voices do not sum coherently on unvoiced material") {
    // BUGS.md VH-005. Passing a fricative through unshifted is correct PER VOICE, but N
    // voices then emit N sample-identical copies, which sum coherently: four voices give
    // FOUR times the amplitude where four real singers would give two.
    //
    // The measurement is the ratio of four-voice energy to one-voice energy on unvoiced
    // material. Coherent is 4x in amplitude, i.e. 16x in energy; incoherent is 4x in energy.
    // Asserting well under the coherent figure is the claim; asserting the exact incoherent
    // figure would be asserting the delay values rather than the property.
    //
    // NOTE the per-voice gains are NOT normalised here, unlike vh_render's 1/sqrt(n). That
    // normalisation is right for a listening mix and would remove precisely what is being
    // measured.
    auto build = [](UnvoicedPolicy policy, int voices) {
        EngineConfig cfg;
        cfg.sampleRate = kSR;
        cfg.maxBlock = kBlock;
        cfg.ratioSource = RatioSource::IntervalFromRoot;
        cfg.rootMidiNote = 60;
        cfg.preservation.unvoiced = policy;

        Engine e;
        e.prepare(cfg, std::make_unique<YinAnalyzer>(),
                  []() -> std::unique_ptr<IPitchShifter> { return std::make_unique<PsolaShifter>(); },
                  nullptr);

        std::vector<Sample> sig;
        append(sig, tone(150.0, 24000));
        append(sig, hiss(24000));

        std::vector<Sample> out(sig.size(), 0.0f), blk(kBlock, 0.0f);
        bool started = false;
        for (FrameCount i = 0; i + kBlock <= sig.size(); i += kBlock) {
            if (!started) {
                for (int v = 0; v < voices; ++v) e.noteOn(60 + 4 * v, 1.0f);
                started = true;
            }
            e.process(sig.data() + i, blk.data(), kBlock);
            for (FrameCount k = 0; k < kBlock; ++k) out[i + k] = blk[k];
        }
        // Energy over the fricative half only, skipping the handover at its start.
        double s = 0.0;
        for (FrameCount i = 26000; i < out.size(); ++i) s += static_cast<double>(out[i]) * out[i];
        return s;
    };

    const double one = build(UnvoicedPolicy::PassThroughDecorrelated, 1) + 1e-18;
    const double four = build(UnvoicedPolicy::PassThroughDecorrelated, 4);
    const double gain = four / one;

    CHECK(gain > 2.0);    // the voices are actually there
    CHECK(gain < 10.0);   // but nothing like the 16x of four identical copies

    // And the control still reproduces the defect, so this test cannot pass by the
    // decorrelation having been silently disabled.
    const double oneP = build(UnvoicedPolicy::PassThrough, 1) + 1e-18;
    const double fourP = build(UnvoicedPolicy::PassThrough, 4);
    CHECK(fourP / oneP > gain);
}
