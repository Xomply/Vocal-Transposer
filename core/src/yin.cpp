#include "vh/yin.hpp"
#include "vh/audio_ring.hpp"
#include "vh/rt.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vh {

void YinAnalyzer::prepare(double sampleRate, FrameCount /*maxBlock*/) {
    sampleRate_ = sampleRate;

    minTau_ = static_cast<FrameCount>(sampleRate / cfg_.maxHz);
    maxTau_ = static_cast<FrameCount>(sampleRate / cfg_.minHz) + 1;

    // Window must cover two full periods of the lowest pitch we claim to track: one to
    // hold the lag and one to correlate against. This is where the latency comes from,
    // and it is not negotiable without giving up the low end.
    windowSamples_ = maxTau_ * 2;

    window_.assign(windowSamples_, Sample{0});
    decimated_.assign(windowSamples_ / decimation_ + 4, Sample{0});
    diff_.assign(maxTau_ + 1, 0.0f);
    cmnd_.assign(maxTau_ + 1, 0.0f);

    maxHoldSamples_ = static_cast<FrameCount>(cfg_.maxHoldMs * 0.001 * sampleRate);

    epochs_.prepare(sampleRate);
    // 1024-sample scan chunks. Sized for cache friendliness, not for correctness — the
    // scan loop below chunks whatever it is given, so this number cannot cause a bug,
    // only a slowdown.
    epochScratch_.assign(1024, Sample{0});
    epochScanPos_ = 0;

    frame_ = AnalysisFrame{};
    nextAnalysisPos_ = 0;
    holdSamples_ = 0;
    trackedPeriod_ = 0.0f;
    pendingPeriod_ = 0.0f;
    pendingCount_ = 0;
    unvoicedRun_ = 0;
}

// Scan every new sample through the epoch tracker.
//
// WHY A SEPARATE PASS FROM THE F0 HOP LOOP: F0 estimation reads a long window every 128
// samples; epoch detection needs every single sample with one-sample lookahead. Trying to
// serve both from one loop means either running YIN far too often or quantising epochs to
// hop boundaries. They are different clocks and the code says so.
void YinAnalyzer::scanEpochs(const AudioRing& ring, Pos upTo) noexcept {
    if (epochScanPos_ == 0) epochScanPos_ = ring.oldestPos();
    if (epochScanPos_ < ring.oldestPos()) epochScanPos_ = ring.oldestPos();

    const float period = frame_.periodSamples;
    while (epochScanPos_ < upTo) {
        const FrameCount n = static_cast<FrameCount>(
            std::min<Pos>(epochScratch_.size(), upTo - epochScanPos_));
        if (!ring.read(epochScanPos_, epochScratch_.data(), n)) break;
        for (FrameCount i = 0; i < n; ++i) {
            if (epochs_.process(epochScratch_[i], epochScanPos_ + i + 1, period)) {
                pushEpoch(epochs_.lastEpoch());
            }
        }
        epochScanPos_ += n;
    }
}

void YinAnalyzer::pushEpoch(Pos p) noexcept {
    if (frame_.epochCount < AnalysisFrame::kEpochHistory) {
        frame_.epochs[frame_.epochCount++] = p;
    } else {
        // Shift down by one. O(64) per epoch, i.e. per pitch period — around 200 times a
        // second. Deliberately a memmove rather than a circular index: the consumers walk
        // this array looking for a nearest match, and a linear newest-last layout keeps
        // that code trivial. Trading a few hundred nanoseconds for not having modular
        // arithmetic in the grain-placement path is the right way round.
        std::memmove(frame_.epochs, frame_.epochs + 1,
                     sizeof(Pos) * (AnalysisFrame::kEpochHistory - 1));
        frame_.epochs[AnalysisFrame::kEpochHistory - 1] = p;
    }
    frame_.lastEpoch = p;
    frame_.epochValid = true;
}

float YinAnalyzer::estimatePeriod(const Sample* x, FrameCount n, float& confidenceOut) noexcept {
    // TWO-STAGE ESTIMATION, and the reason is a measured one rather than a theoretical
    // one.
    //
    // The naive single-stage version computes the difference function at full rate over
    // the whole lag range: O(n * tauMax), about 940k multiply-adds per hop at a 70 Hz
    // lower bound. Benchmarked, that alone consumed the ENTIRE 2.67 ms callback budget at
    // a 128-sample block — and, tellingly, the cost was identical for 1 voice and for 16,
    // because analysis is shared. The bottleneck was never the voices.
    //
    // Decimating by 4 for the coarse search cuts that by 16x (both the signal length and
    // the lag count shrink), at the cost of resolving the period only to within 4 samples.
    // A narrow full-rate refinement around the winner recovers the precision for a few
    // percent of the original work. Total speedup is roughly 10x for no accuracy loss that
    // survives the parabolic interpolation.
    //
    // The alternative — FFT-based autocorrelation, O(n log n) — is the textbook answer and
    // would also work. This is chosen because it is twenty lines with no dependency and no
    // window-length constraints, and because at these sizes it is competitive.

    const FrameCount D = decimation_;
    const FrameCount nd = n / D;
    const FrameCount maxTauD = std::min(maxTau_ / D, nd / 2);
    const FrameCount minTauD = std::max<FrameCount>(1, minTau_ / D);
    if (maxTauD <= minTauD + 2) { confidenceOut = 0.0f; return 0.0f; }

    // Decimate with a box average rather than by dropping samples. Plain subsampling
    // aliases everything above 6 kHz down into the range we are searching, which shows up
    // as spurious minima — i.e. as octave errors, the exact failure this whole design is
    // trying to avoid.
    for (FrameCount i = 0; i < nd; ++i) {
        float acc = 0.0f;
        for (FrameCount k = 0; k < D; ++k) acc += x[i * D + k];
        decimated_[i] = acc / static_cast<float>(D);
    }

    // Stage 1: coarse difference function on the decimated signal.
    diff_[0] = 1.0f;
    for (FrameCount tau = 1; tau <= maxTauD; ++tau) {
        float sum = 0.0f;
        const FrameCount lim = nd - tau;
        for (FrameCount i = 0; i < lim; ++i) {
            const float d = decimated_[i] - decimated_[i + tau];
            sum += d * d;
        }
        diff_[tau] = sum;
    }

    // Cumulative mean normalisation: YIN's actual contribution over plain autocorrelation.
    // It removes the trivial minimum at tau=0 and makes one absolute threshold meaningful
    // across signal levels.
    cmnd_[0] = 1.0f;
    float running = 0.0f;
    for (FrameCount tau = 1; tau <= maxTauD; ++tau) {
        running += diff_[tau];
        cmnd_[tau] = (running > 1e-12f) ? diff_[tau] * static_cast<float>(tau) / running : 1.0f;
    }

    // FIRST dip below threshold, not the global minimum.
    //
    // THE OCTAVE-ERROR GUARD, and the single most important line in this function: a
    // signal periodic at T is also periodic at 2T and 3T, so the global minimum is
    // frequently at a subharmonic. Taking the first qualifying dip picks the fundamental.
    // Getting this wrong puts a harmony voice a full octave adrift.
    FrameCount tauD = minTauD;
    bool found = false;
    while (tauD < maxTauD) {
        if (cmnd_[tauD] < cfg_.threshold) {
            while (tauD + 1 < maxTauD && cmnd_[tauD + 1] < cmnd_[tauD]) ++tauD;
            found = true;
            break;
        }
        ++tauD;
    }
    if (!found) { confidenceOut = 0.0f; return 0.0f; }

    // Stage 2: refine at full rate in a narrow band around the coarse winner.
    const FrameCount centre = tauD * D;
    const FrameCount lo = centre > 2 * D ? centre - 2 * D : 1;
    const FrameCount hi = std::min(maxTau_, centre + 2 * D);

    float bestVal = 1e30f;
    FrameCount bestTau = centre;
    for (FrameCount tau = lo; tau <= hi; ++tau) {
        float sum = 0.0f;
        const FrameCount lim = n - tau;
        for (FrameCount i = 0; i < lim; ++i) {
            const float d = x[i] - x[i + tau];
            sum += d * d;
        }
        diff_[tau] = sum;
        if (sum < bestVal) { bestVal = sum; bestTau = tau; }
    }

    // Normalise the refined minimum against the local mean so the confidence figure stays
    // comparable to the coarse stage's.
    float localMean = 0.0f;
    for (FrameCount tau = lo; tau <= hi; ++tau) localMean += diff_[tau];
    localMean /= static_cast<float>(hi - lo + 1);
    const float norm = localMean > 1e-12f ? bestVal / localMean : 1.0f;

    // Parabolic interpolation for sub-sample resolution. Without it the pitch quantises to
    // integer lags, which at high pitches is tens of cents of error — and in Mode A that
    // error passes straight to the output.
    float betterTau = static_cast<float>(bestTau);
    if (bestTau > lo && bestTau < hi) {
        const float s0 = diff_[bestTau - 1], s1 = diff_[bestTau], s2 = diff_[bestTau + 1];
        const float denom = 2.0f * (2.0f * s1 - s2 - s0);
        if (std::fabs(denom) > 1e-12f) betterTau += (s2 - s0) / denom;
    }

    confidenceOut = std::clamp(1.0f - norm, 0.0f, 1.0f);
    return betterTau;
}

void YinAnalyzer::process(const AudioRing& ring, Pos upTo) noexcept {
    VH_RT_SECTION();

    if (windowSamples_ == 0) return;
    scanEpochs(ring, upTo);
    if (nextAnalysisPos_ == 0) nextAnalysisPos_ = windowSamples_;

    // Catch up in hops. Usually zero or one iteration; more only if we were stalled.
    while (nextAnalysisPos_ + cfg_.hopSamples <= upTo) {
        const Pos winStart = nextAnalysisPos_ - windowSamples_;
        if (!ring.read(winStart, window_.data(), windowSamples_)) break;

        float conf = 0.0f;
        const float raw = estimatePeriod(window_.data(), windowSamples_, conf);

        // ---- F0 MOMENTUM -------------------------------------------------------
        //
        // `raw` is this hop's opinion. `period` is what we are willing to act on. They
        // differ only when the opinion is a large jump that nothing has corroborated.
        //
        // On ACQUISITION (nothing tracked yet) the first estimate is committed outright:
        // there is no momentum to weigh it against, and making a note entry wait for
        // corroboration would add the one latency this project spends everything to
        // avoid. Momentum guards the middle of a phrase, not its beginning.
        float period = 0.0f;
        if (raw > 0.0f) {
            if (trackedPeriod_ <= 0.0f) {
                trackedPeriod_ = raw;
                pendingCount_ = 0;
                period = raw;
            } else {
                const float cents = std::fabs(1200.0f * std::log2(raw / trackedPeriod_));
                if (cents <= cfg_.maxStepCents) {
                    trackedPeriod_ = raw;
                    pendingCount_ = 0;
                    period = raw;
                } else {
                    // A leap. Corroboration means the NEXT hop agreeing with the
                    // candidate, not merely disagreeing with the tracked value again —
                    // otherwise two unrelated wrong estimates would confirm each other.
                    const bool agrees =
                        pendingCount_ > 0 && pendingPeriod_ > 0.0f &&
                        std::fabs(1200.0f * std::log2(raw / pendingPeriod_)) <= cfg_.maxStepCents;
                    pendingPeriod_ = raw;
                    pendingCount_ = agrees ? pendingCount_ + 1 : 1;

                    if (pendingCount_ >= cfg_.jumpConfirmHops) {
                        trackedPeriod_ = raw;
                        pendingCount_ = 0;
                        period = raw;
                    } else {
                        // Not yet believed. Carry the tracked value forward — a confident
                        // hop, so this is NOT the unvoiced hold and must not consume the
                        // hold budget.
                        period = trackedPeriod_;
                    }
                }
            }
        }

        // ---- THE VOICING GATE --------------------------------------------------
        // Immediate to voiced, delayed to unvoiced. See YinConfig::releaseHops.
        if (raw > 0.0f) unvoicedRun_ = 0;
        else if (unvoicedRun_ < cfg_.releaseHops) ++unvoicedRun_;
        const bool gateVoiced = unvoicedRun_ < cfg_.releaseHops;

        // Copy rather than default-construct: the epoch history accumulated by
        // scanEpochs() lives in frame_ and must survive the F0 update.
        AnalysisFrame f = frame_;
        f.pos = nextAnalysisPos_;

        if (period > 0.0f) {
            f.voicing = Voicing::Voiced;
            f.f0Hz = static_cast<float>(sampleRate_) / period;
            f.periodSamples = period;
            f.confidence = conf;
            f.f0IsHeld = false;
            holdSamples_ = 0;
        } else if (cfg_.holdThroughUnvoiced && frame_.f0Hz > 0.0f && holdSamples_ < maxHoldSamples_) {
            // THE HOLD. See yin.hpp and the latency analysis: this is what stops every
            // consonant from re-paying the acquisition cost. The pitch is carried
            // forward, flagged as held, and the voicing reflects the GATE — during the
            // release window it is still Voiced, so a momentary dropout does not toggle
            // the shifters' handover, and after it the unvoiced path takes over as before.
            f.voicing = gateVoiced ? Voicing::Voiced : Voicing::Unvoiced;
            f.f0Hz = frame_.f0Hz;
            f.periodSamples = frame_.periodSamples;
            f.confidence = 0.0f;
            f.f0IsHeld = true;
            holdSamples_ += cfg_.hopSamples;
        } else {
            f.voicing = Voicing::Unvoiced;
            f.f0Hz = 0.0f;
            f.periodSamples = 0.0f;
            f.confidence = 0.0f;
            f.f0IsHeld = false;
            // The hold has been abandoned, so the tracked pitch is stale. Dropping it
            // means the next entry re-acquires rather than being dragged toward whatever
            // was sung before the silence — momentum must not survive a lost lock.
            trackedPeriod_ = 0.0f;
            pendingCount_ = 0;
        }

        frame_ = f;
        nextAnalysisPos_ += cfg_.hopSamples;
    }
}

} // namespace vh
