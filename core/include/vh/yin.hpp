// vh/yin.hpp — F0 estimation. de Cheveigne & Kawahara, JASA 2002.
//
// WHY YIN AND NOT SOMETHING ELSE: low latency, robust, no upper frequency limit, and
// simple enough to debug by hand. pYIN adds HMM smoothing and some latency; CREPE is a
// CNN and heavier. Both are behind IAnalyzer if we want them later.
//
// THE COST THIS CLASS IMPOSES ON THE WHOLE SYSTEM, stated plainly:
// robust estimation wants 2-3 pitch periods of observation. At a low male fundamental
// (110 Hz, period 9.09 ms) that is 18-27 ms before the first confident estimate. That is
// the single largest term in the Mode A latency budget and it cannot be argued away —
// it is set by physics, not by implementation quality.
//
// BUT IT IS A CONVERGENCE COST, NOT A PER-BLOCK TAX. Once locked, tracking is
// incremental. The distinction is the difference between an instrument that feels
// sluggish on every note and one that feels sluggish only when you change pitch. See
// holdThroughUnvoiced below — that flag is where the distinction is cashed in.

#pragma once

#include "vh/analysis.hpp"
#include "vh/epoch.hpp"

#include <vector>

namespace vh {

struct YinConfig {
    // PROVISIONAL: 70 Hz lower bound. Low enough for a bass voice, high enough to keep
    // the difference function from getting expensive. Raise it if you never sing that
    // low — every Hz of lower bound costs window length, and window length is latency.
    float minHz = 70.0f;
    float maxHz = 1000.0f;

    // YIN's absolute threshold. Below this, the candidate is accepted.
    // PROVISIONAL: 0.15 is the paper's suggestion and a reasonable default. Lower =
    // stricter = more unvoiced frames; higher = more octave errors.
    float threshold = 0.15f;

    // Analysis hop. PROVISIONAL: 128 samples ~2.7 ms at 48 kHz. Independent of the audio
    // block size on purpose — the two should never be coupled.
    FrameCount hopSamples = 128;

    // THE DECISION FROM THE LATENCY ANALYSIS, made settable so it can be A/B'd:
    //
    // When a frame comes back unvoiced or low-confidence, hold the previous F0 and mark
    // it held, rather than reporting "no pitch". Without this, every consonant in a sung
    // lyric reads as lost lock and re-pays the 18-27 ms acquisition — the instrument
    // stutters through words while feeling fine on a held vowel.
    //
    // CERTAIN that holding is right. PROVISIONAL on how long to hold before giving up.
    bool holdThroughUnvoiced = true;

    // PROVISIONAL: 200 ms. Long enough to bridge any consonant; short enough that a
    // genuinely abandoned phrase does not leave a stale pitch lying around.
    float maxHoldMs = 200.0f;

    // --- the voicing GATE ---------------------------------------------------------
    //
    // WHY A GATE AND NOT A COMPARISON. The voicing decision used to be nothing but "did
    // this hop find a dip below threshold", evaluated every 128 samples. Near the
    // threshold — which is where consonants, note entries and quiet passages all live —
    // that flips back and forth every 2.7 ms. MEASURED on the melody recording: 27
    // passthrough flips in 5 seconds, six of them lasting under 8 ms. Every one of those
    // flips is a mode change in both shifters, and downstream of a hard switch it was a
    // click; downstream of a crossfade it is still a needless 8 ms of the shifted path
    // fading out and back in.
    //
    // ASYMMETRIC ON PURPOSE. Becoming voiced is immediate, because a late note entry is
    // the one thing an instrument may not do. Becoming unvoiced waits, because nothing
    // bad happens if the first few milliseconds of a fricative are still being shifted —
    // a fricative is broadband noise and the shift is inaudible over that span — whereas
    // dropping out on a momentary dip is very audible.
    //
    // The cost, stated plainly: a genuine consonant starts passing through
    // `releaseHops * hopSamples` late. At the defaults that is 10.7 ms. TUNE by counting
    // flips in a vh_trace CSV, not by ear — the ear cannot separate this from the
    // handover crossfade downstream of it.
    int releaseHops = 4;

    // --- F0 MOMENTUM --------------------------------------------------------------
    //
    // Believe small changes immediately; require a large one to be corroborated before
    // committing to it.
    //
    // WHY: a single-hop estimate is allowed to be wrong, and when it is wrong it is
    // usually wrong by an octave or by a formant — never by 30 cents. Committing to that
    // estimate moves the synthesis spacing, the grain size, the epoch tracker's cutoff
    // and (in Mode A) the ratio itself, all within one hop, and then moves them all back.
    // That is a discontinuity per wrong estimate.
    //
    // maxStepCents is deliberately generous: 120 cents per hop is 4400 cents/second,
    // which is faster than any real portamento and far faster than vibrato (~5 cents per
    // hop at 6 Hz and 100 cents depth). So ordinary singing never touches this path, and
    // what does touch it is estimator noise.
    //
    // Anything larger costs `jumpConfirmHops` hops before it is believed — 5.3 ms at the
    // defaults. This is the design-notes principle applied to the tracker: uncertainty
    // costs LATENCY, not correctness. A genuine octave leap still lands, 5 ms late; a
    // spurious one never lands at all.
    float maxStepCents = 120.0f;
    int jumpConfirmHops = 2;
};

class YinAnalyzer final : public IAnalyzer {
public:
    explicit YinAnalyzer(YinConfig cfg = {}) : cfg_(cfg) {}

    void prepare(double sampleRate, FrameCount maxBlock) override;
    void process(const AudioRing& ring, Pos upTo) noexcept override;
    const AnalysisFrame& current() const noexcept override { return frame_; }
    FrameCount latencySamples() const noexcept override { return windowSamples_; }
    void forceReacquire() noexcept override { holdSamples_ = 0; frame_.f0IsHeld = false; }

    // Exposed for tests and for the offline harness. Not used on the audio thread.
    FrameCount windowSamples() const noexcept { return windowSamples_; }

private:
    // Returns period in samples, or 0 if no confident candidate.
    //
    // TWO-STAGE: a coarse search on a decimated copy, then a refinement at full rate in a
    // narrow band around the winner. See yin.cpp for why.
    float estimatePeriod(const Sample* x, FrameCount n, float& confidenceOut) noexcept;
    void scanEpochs(const class AudioRing& ring, Pos upTo) noexcept;
    void pushEpoch(Pos p) noexcept;

    YinConfig cfg_{};
    double sampleRate_ = 48000.0;
    FrameCount windowSamples_ = 0;
    FrameCount minTau_ = 0, maxTau_ = 0;

    // Allocated once in prepare(). Never resized on the audio thread.
    std::vector<Sample> window_;
    std::vector<Sample> decimated_;
    std::vector<float> diff_;
    std::vector<float> cmnd_;
    FrameCount decimation_ = 4;

    AnalysisFrame frame_{};
    Pos nextAnalysisPos_ = 0;
    FrameCount holdSamples_ = 0;
    FrameCount maxHoldSamples_ = 0;

    // F0 momentum. `trackedPeriod_` is what we have COMMITTED to; `pendingPeriod_` is a
    // candidate large jump that has not yet been corroborated. See YinConfig.
    float trackedPeriod_ = 0.0f;
    float pendingPeriod_ = 0.0f;
    int pendingCount_ = 0;

    // Voicing gate. Counts consecutive hops with no confident estimate; voicing is only
    // reported Unvoiced once it reaches cfg_.releaseHops.
    int unvoicedRun_ = 0;

    // Epoch tracking runs sample-by-sample and therefore on its own clock, independent of
    // the F0 hop. Keeping them separate matters: F0 can afford to update every 2.7 ms,
    // but a phase reference that only updated that often would quantise grain placement
    // to hop boundaries, which is audible as roughness.
    EpochTracker epochs_;
    Pos epochScanPos_ = 0;
    std::vector<Sample> epochScratch_;
};

} // namespace vh
