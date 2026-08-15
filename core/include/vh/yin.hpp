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

    // Epoch tracking runs sample-by-sample and therefore on its own clock, independent of
    // the F0 hop. Keeping them separate matters: F0 can afford to update every 2.7 ms,
    // but a phase reference that only updated that often would quantise grain placement
    // to hop boundaries, which is audible as roughness.
    EpochTracker epochs_;
    Pos epochScanPos_ = 0;
    std::vector<Sample> epochScratch_;
};

} // namespace vh
