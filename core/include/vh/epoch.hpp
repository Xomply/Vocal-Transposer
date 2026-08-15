// vh/epoch.hpp — WHEN the glottis closed, not just how often.
//
// SEED: F0 is a RATE. Pitch-synchronous methods need a PHASE. PSOLA has to place grains
// on glottal closure instants, and the delay-line fix that makes the comb filter stop
// existing (rather than merely smearing it) needs jump distances that are an integer
// number of periods measured from a consistent phase reference. A MIDI keyboard can tell
// you the rate; only the signal can tell you the phase.
//
// THE HONEST SCOPE OF THIS CLASS, because it would be easy to oversell:
// this is NOT a glottal closure instant detector in the speech-science sense. DYPSA and
// SEDREAMS solve that problem properly, validated against electroglottography, and they
// are not causal in the form we would need.
//
// What PSOLA actually requires is weaker and this delivers it: a phase reference that is
// CONSISTENT from period to period. Grains must land at the same point of successive
// cycles; whether that point is anatomically the moment of closure is irrelevant to the
// overlap-add. A peak tracker on a lowpassed signal, predicted forward by the known
// period, gives exactly that.
//
// The failure mode to watch for is the one that matters: if the reference drifts by a
// fraction of a period between grains, the overlap-add partially cancels and the output
// gets a hollow, phasey quality. Consistency is the whole specification.

#pragma once

#include "vh/types.hpp"

#include <cmath>

namespace vh {

class EpochTracker {
public:
    void prepare(double sampleRate) {
        sr_ = sampleRate;
        setCutoff(900.0);
        reset();
    }

    // Lowpass cutoff, tracking the fundamental.
    //
    // A FIXED CUTOFF WAS THE FIRST DESIGN AND IT BROKE AT HIGH PITCHES, in a way worth
    // recording: at 900 Hz fixed, a 110 Hz voice is isolated nicely, but a 220 Hz voice
    // still has its first formant (~700 Hz) passing through almost unattenuated. The peak
    // picker then sometimes locks onto an F1 crest instead of the glottal one, and because
    // F1 crests recur several times per period, the chosen phase JITTERS between cycles.
    //
    // Jittered phase is not a small error. PSOLA aligns grains on these instants, so the
    // grains stop being coherent with one another and the overlap-add partially cancels —
    // producing an output an octave or a fifth away from the requested pitch. The symptom
    // looks like a pitch-shifting bug and lives entirely in the phase reference.
    //
    // Tracking the fundamental at ~1.8x keeps the glottal periodicity and pushes F1 well
    // into the stopband at every pitch we handle.
    void setCutoff(double fc) noexcept {
        fc = fc < 120.0 ? 120.0 : (fc > 1500.0 ? 1500.0 : fc);
        cutoff_ = fc;
        const double w = 2.0 * M_PI * fc / sr_;
        const double q = 0.7071;
        const double alpha = std::sin(w) / (2.0 * q);
        const double cosw = std::cos(w);
        const double a0 = 1.0 + alpha;
        b0_ = static_cast<float>((1.0 - cosw) / 2.0 / a0);
        b1_ = static_cast<float>((1.0 - cosw) / a0);
        b2_ = b0_;
        a1_ = static_cast<float>(-2.0 * cosw / a0);
        a2_ = static_cast<float>((1.0 - alpha) / a0);
    }

    void reset() noexcept {
        z1_ = z2_ = 0.0f;
        prev_ = prev2_ = 0.0f;
        envelope_ = 0.0f;
        lastEpoch_ = 0;
        haveEpoch_ = false;
        primed_ = 0;
    }

    // Feed one sample. `pos` is its absolute position. `periodSamples` is the current
    // estimate, 0 if unknown. Returns true when an epoch is confirmed — at position
    // pos-1, because confirming a local maximum requires seeing the sample after it.
    //
    // ONE SAMPLE OF LOOKAHEAD. That is the entire latency cost of this class, which is
    // why it is worth doing this way rather than with a windowed method.
    bool process(Sample x, Pos pos, float periodSamples) noexcept {
        // Retune only on a meaningful pitch change. Recomputing coefficients per sample
        // would cost two transcendentals per sample per voice; 5% hysteresis makes it
        // essentially never happen during a sustained note, and the filter's exact cutoff
        // is not critical — only that it sits between F0 and F1.
        if (periodSamples > 8.0f) {
            const double f0 = sr_ / static_cast<double>(periodSamples);
            const double want = 1.8 * f0;
            if (want > cutoff_ * 1.05 || want < cutoff_ * 0.95) setCutoff(want);
        }

        // Transposed direct form II biquad.
        const float y = b0_ * x + z1_;
        z1_ = b1_ * x - a1_ * y + z2_;
        z2_ = b2_ * x - a2_ * y;

        // Envelope follower, fast attack / slow release, for an adaptive threshold.
        // A fixed threshold would either miss quiet passages or fire on noise in loud
        // ones; singing dynamics span far too much range for a constant.
        const float mag = std::fabs(y);
        envelope_ = mag > envelope_ ? mag : envelope_ * 0.9995f;

        bool detected = false;
        if (primed_ >= 2) {
            const bool isPeak = prev_ > prev2_ && prev_ >= y;
            const bool loudEnough = prev_ > envelope_ * 0.25f;

            // Refractory period. Without it, formant ripple riding on the fundamental
            // produces two or three "epochs" per cycle, and PSOLA emits grains at the
            // wrong rate — which sounds like a pitch error, not like a detection bug, so
            // it is easy to misdiagnose.
            const float refractory = periodSamples > 1.0f ? periodSamples * 0.6f : 32.0f;
            const bool farEnough = !haveEpoch_ ||
                                   static_cast<float>(pos - 1 - lastEpoch_) >= refractory;

            if (isPeak && loudEnough && farEnough) {
                lastEpoch_ = pos - 1;
                haveEpoch_ = true;
                detected = true;
            }
        } else {
            ++primed_;
        }

        prev2_ = prev_;
        prev_ = y;
        return detected;
    }

    Pos lastEpoch() const noexcept { return lastEpoch_; }
    bool valid() const noexcept { return haveEpoch_; }

private:
    double sr_ = 48000.0;
    double cutoff_ = 900.0;
    float b0_ = 0, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0;
    float z1_ = 0, z2_ = 0;
    float prev_ = 0, prev2_ = 0;
    float envelope_ = 0;
    Pos lastEpoch_ = 0;
    bool haveEpoch_ = false;
    int primed_ = 0;
};

} // namespace vh
