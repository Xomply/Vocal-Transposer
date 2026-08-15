#include "vh/granular_shifter.hpp"
#include "vh/rt.hpp"

#include <algorithm>
#include <cmath>

namespace vh {

void GranularShifter::prepare(double sampleRate, FrameCount /*maxBlock*/) {
    sampleRate_ = sampleRate;
    reset();
}

void GranularShifter::reset() noexcept {
    pos_ = 0.0;
    fadePos_ = 0.0;
    fadeRemaining_ = 0;
    started_ = false;
    updateGeometry(0.0f, sampleRate_);
}

void GranularShifter::updateGeometry(float periodSamples, double sampleRate) noexcept {
    // Geometry follows the singer. A shifter whose delay is a compile-time constant is
    // either too long for a soprano or too short for a bass; sized in periods it is
    // correct for both, and latencySamples() reports the truth to the blender.
    const float fallback = static_cast<float>(cfg_.unvoicedGrainMs * 0.001 * sampleRate);
    const float p = periodSamples > 8.0f ? periodSamples : fallback;

    fadeLength_ = std::max(8, static_cast<int>(p * cfg_.fadeFraction));
    jumpSize_ = static_cast<FrameCount>(p * static_cast<float>(cfg_.jumpPeriods));

    // The delay must be deep enough that a backward jump plus the outgoing tap's fade
    // still lands inside written history, with a few samples of margin for the cubic
    // interpolator's neighbours.
    minDelay_ = static_cast<FrameCount>(fadeLength_) + 4;
    baseDelay_ = minDelay_ + jumpSize_;
    maxDelay_ = baseDelay_ + jumpSize_;
}

Sample GranularShifter::interpolate(const AudioRing& ring, double pos) noexcept {
    // std::floor, NOT a direct cast to Pos.
    //
    // THE BUG THIS FIXES, found only on real audio and worth spelling out:
    // Pos is unsigned. Casting a NEGATIVE double to an unsigned integer is undefined
    // behaviour, and on x86 at -O2 it yields a huge value. The fractional part `t` — which
    // the cubic polynomial below assumes lies in [0,1) — then became astronomically large,
    // and ((c3*t + c2)*t + c1)*t + c0 overflowed to +/-inf. Infinity in an audio buffer is
    // not a subtle artefact; it is the loudest possible sound.
    //
    // pos_ went negative at startup, when the recording begins with silence: with no F0
    // the geometry falls back to a long grain, the read pointer's delay is briefly under
    // the minimum, and the jump subtracts more than has been written. Every synthetic test
    // began with a fully primed steady tone, so this never appeared. Real recordings start
    // with silence, room tone, or a breath.
    const double fl = std::floor(pos);
    const Pos i = static_cast<Pos>(fl < 0.0 ? 0.0 : fl);
    const float t = static_cast<float>(pos - fl);
    const float xm1 = ring.at(i - 1), x0 = ring.at(i), x1 = ring.at(i + 1), x2 = ring.at(i + 2);

    const float c0 = x0;
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + c0;
}

void GranularShifter::process(const ShiftRequest& req) noexcept {
    VH_RT_SECTION();

    const AudioRing& ring = *req.ring;
    const AnalysisFrame& a = *req.analysis;
    ReadCursor& cur = *req.cursor;
    const Pos writePos = ring.writePos();

    updateGeometry(a.periodSamples, sampleRate_);

    if (!started_) {
        pos_ = static_cast<double>(writePos > baseDelay_ ? writePos - baseDelay_ : 0);
        fadePos_ = pos_;
        fadeRemaining_ = 0;
        started_ = true;
    }

    // Legal window for the read pointer: inside written history, with margin for the
    // cubic interpolator's four-sample neighbourhood.
    const double oldestOk = static_cast<double>(ring.oldestPos()) + 2.0;
    const double newestOk = static_cast<double>(writePos) - 2.0;
    if (pos_ < oldestOk) pos_ = oldestOk;
    if (fadePos_ < oldestOk) fadePos_ = oldestOk;

    // UNVOICED: pass through, do not shift.
    //
    // Fricatives are shaped by front-cavity geometry, not larynx rate — a singer's /s/
    // does not transpose when they sing a fifth higher. Advancing at 1.0 rather than
    // `ratio` also keeps the read position at a stable delay, so voicing resuming does not
    // find the pointer somewhere unexpected.
    const bool passThrough =
        a.voicing == Voicing::Unvoiced &&
        req.preservation->unvoiced != UnvoicedPolicy::Shift;

    const double rate = passThrough ? 1.0 : req.ratio;
    const bool pitchSynchronous = a.voicing == Voicing::Voiced && a.periodSamples > 8.0f;

    for (FrameCount n = 0; n < req.numFrames; ++n) {
        // Trigger a jump when the read pointer has drifted out of its working range and
        // we are not already mid-fade.
        if (fadeRemaining_ <= 0) {
            const double delay = static_cast<double>(writePos) - pos_;
            if (delay < static_cast<double>(minDelay_) || delay > static_cast<double>(maxDelay_)) {
                FrameCount jump = jumpSize_;

                if (!pitchSynchronous) {
                    // No F0 means no phase to be synchronous with, and noise is where comb
                    // coloration is MOST audible. So randomise the jump: a time-varying
                    // comb is heard as movement (chorus/flanging, which the ear forgives)
                    // rather than as timbre (a filter, which it does not).
                    //
                    // NOTE what is randomised: the jump DISTANCE, never the read RATE. The
                    // read rate *is* the pitch ratio; modulating it frequency-modulates the
                    // output into uncontrolled vibrato on top of the transposition.
                    rng_ = rng_ * 1664525u + 1013904223u;
                    const float jitter = 0.75f + 0.5f * (static_cast<float>(rng_ >> 8) / 16777216.0f);
                    jump = static_cast<FrameCount>(static_cast<float>(jump) * jitter);
                }

                // Refuse a jump that would land outside written history. Early in a
                // stream — or after a long unvoiced stretch changes the geometry — the
                // required jump can exceed everything that exists. Skipping is correct:
                // the pointer simply drifts until there is enough material, which is
                // inaudible, whereas jumping out of bounds is not.
                const double want = (delay < static_cast<double>(minDelay_))
                                        ? pos_ - static_cast<double>(jump)
                                        : pos_ + static_cast<double>(jump);
                if (want >= oldestOk && want <= newestOk) {
                    fadePos_ = pos_;
                    pos_ = want;
                    fadeRemaining_ = fadeLength_;
                }
            }
        }

        Sample s = interpolate(ring, pos_);

        if (fadeRemaining_ > 0) {
            const float x = 1.0f - static_cast<float>(fadeRemaining_) / static_cast<float>(fadeLength_);
            // Raised cosine: zero slope at both ends. The Shepard-tone lesson is that a
            // partial can be born and die inaudibly if the envelope is C1 at the
            // boundaries; a fade with a kink in its derivative clicks no matter how long
            // it is.
            const float g = 0.5f - 0.5f * std::cos(static_cast<float>(M_PI) * x);

            const Sample old = interpolate(ring, fadePos_);

            // EQUAL-GAIN, not equal-power. Once the jump is pitch-synchronous the two taps
            // are reading phase-aligned copies of the same waveform — strongly correlated
            // — and an equal-power law would put a +3 dB bump in the middle of every fade,
            // heard as pumping at the jump rate. When the jump is randomised the taps are
            // less correlated and equal-power would be closer to right; the error there is
            // a mild dip rather than a bump, which is the better failure.
            s = s * g + old * (1.0f - g);

            fadePos_ += rate;
            --fadeRemaining_;
        }

        req.out[n] = s;
        pos_ += rate;
    }

    // The cursor tracks wall-clock input consumption, not the read pointer — the read
    // pointer moves at `ratio` and jumps around, which is exactly why the two are separate
    // quantities. The blender aligns on the cursor.
    if (!cur.frozen) cur.next += req.numFrames;
}

} // namespace vh
