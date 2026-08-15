// vh/voice.hpp — one harmony voice: which note, reading from where, how humanized.
//
// SEED: press a key, get a voice. Eight-plus of them, each an independent "singer".
//
// A structural fact worth stating loudly because it is counter-intuitive and it governs
// the CPU/latency trade: VOICE COUNT COSTS ZERO LATENCY. Analysis is shared across all
// voices and synthesis is parallel across them, so sixteen voices is sixteen times the
// CPU and exactly the same milliseconds as one. Latency is set by the slowest stage,
// never by the number of voices. Do not economise on voices to reduce latency; it does
// not work that way.

#pragma once

#include "vh/shifter.hpp"
#include "vh/types.hpp"

namespace vh {

// How a voice decides its shift ratio. This IS the mode switch.
enum class RatioSource {
    // Mode A — detachment. Sing anything; the played note comes out.
    // ratio = f_target / f_sung. Requires F0, and F0 error passes straight through to
    // output pitch: measure 8 cents wrong, sing 8 cents wrong.
    //
    // Chosen deliberately over "correction" (nudging a note you are already aiming at).
    // Correction would mean sub-50-cent shifts and a small error budget; detachment means
    // arbitrary ratios and total dependence on the tracker. Do not let these drift into
    // each other — they are different instruments.
    AbsoluteTarget,

    // Mode B — interval transposition from a declared root (a key you set, or the lowest
    // held MIDI note). ratio = 2^(n/12), known the instant the key goes down.
    //
    // NEEDS NO PITCH DETECTION for the ratio, which is why it can land ~11 ms faster than
    // Mode A. It preserves your scoops, bends and back-phrasing, because your deviation
    // rides along untouched.
    //
    // PROVISIONAL — the declared root. If deriving the root from the voice turns out to be
    // musically necessary, this mode collapses into Mode A's latency profile and the
    // dual-engine blend becomes the only remaining latency lever.
    IntervalFromRoot,
};

// Per-voice deviations that turn N copies into N singers.
//
// WHY THIS IS NOT OPTIONAL POLISH: identical shifted copies sum into something that
// sounds like one flanged voice, not a choir. Real backing singers do not lock vibrato
// phase with the lead — independent, decorrelated vibrato is precisely what makes voices
// sound like people. This struct is the difference between "chorus of robots" and
// "backing vocalists", and it is cheap.
//
// NOTE the vibrato asymmetry: for harmony voices we SYNTHESISE vibrato rather than
// predicting the lead's. Predicting it would need 300-600 ms of F0 history to save ~10 ms
// — an estimator with half a second of lag, chasing a 10 ms saving. A harmony voice's
// vibrato needs to be plausible, not accurate. Rate and depth converge fast; phase we
// randomise, beneficially.
struct Humanization {
    float detuneCents = 0.0f;        // static, per voice
    float detuneDriftCents = 0.0f;   // slow random walk amplitude
    float onsetJitterMs = 0.0f;      // entry timing spread
    float vibratoRateHz = 0.0f;
    float vibratoDepthCents = 0.0f;
    float vibratoPhase = 0.0f;       // randomised per voice — this is the point
    float envelopeWarpOffset = 0.0f; // slight per-voice vocal-tract difference
    float pan = 0.0f;                // -1..+1
};

enum class VoiceState {
    Idle,
    Attacking,
    Sustaining,
    Releasing,
};

struct Voice {
    VoiceState state = VoiceState::Idle;

    int midiNote = -1;
    float velocity = 0.0f;

    RatioSource ratioSource = RatioSource::AbsoluteTarget;

    // The ratio actually applied this block, after humanization and glide.
    double ratio = 1.0;

    // Glide/portamento target. Collier's much-discussed "glide" is exactly this: a
    // portamento rate, not a special algorithm.
    double targetRatio = 1.0;
    float glideRateSemitonesPerSec = 0.0f;   // 0 = instant

    ReadCursor cursor{};
    Humanization hum{};

    float gain = 1.0f;

    // Amplitude envelope, applied by the Engine.
    //
    // WHY IT EXISTS: cutting a voice dead on note-off produces a step discontinuity, and a
    // step is broadband — it clicks. A few milliseconds of ramp removes it entirely. Also
    // used to fade a voice IN, which matters because a voice starting mid-vowel begins at
    // whatever amplitude the waveform happened to be at.
    float envGain = 0.0f;
    float envTarget = 1.0f;

    // Whether each engine ran last block. The Engine skips engines whose blend weight is
    // inaudible, as a free CPU governor — but a stateful shifter coming back after being
    // skipped has stale internals and must be reset, or it resumes from a grain phase that
    // no longer relates to the input.
    bool fastRan = false;
    bool qualityRan = false;

    // Absolute position at which this voice's note-on landed. Feeds the blend policy:
    // "time since note on" is one of the axes the fast/quality crossfade can key on.
    Pos noteOnPos = 0;

    bool active() const noexcept { return state != VoiceState::Idle; }
};

} // namespace vh
