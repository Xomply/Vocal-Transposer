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

    // A fixed per-voice delay on the whole voice, in milliseconds. The first row of this
    // struct the Engine actually reads.
    //
    // WHY A STATIC DELAY IS THE FIX FOR THE SIBILANT COLLAPSE (BUGS.md VH-005). Passing a
    // fricative through unshifted is correct PER VOICE, but N voices then emit N sample-
    // identical copies. Identical copies sum COHERENTLY: four voices at 1/sqrt(4) gain
    // give twice the amplitude, which is the measured 1.7x energy in the demo's fricative
    // region, and the ensemble audibly collapses to unison and re-widens at every /s/.
    //
    // Broadband noise decorrelates within about 0.1 ms, so a per-voice offset of a few
    // milliseconds makes those copies sum in POWER instead — sqrt(N) rather than N. The
    // voiced parts are unaffected in any way that matters, because each voice is already
    // at a different pitch, and a pure delay changes no timbre at all: it is four singers
    // standing at four distances from one microphone, which is the physical situation
    // being modelled.
    //
    // WHY NOT AN ALLPASS, which is the other candidate the ledger names: an allpass is
    // flat in magnitude but disperses the glottal pulse on VOICED frames too, so it would
    // have to be switched in and out with voicing — and a switched filter is exactly the
    // class of defect the handover crossfade exists to remove. A static delay needs no
    // switching, so it cannot introduce one. If comb coloration between the delayed copies
    // turns out to be audible, an allpass is the next thing to try; it is not needed yet.
    float staticDelayMs = 0.0f;
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

    // --- hold/freeze bookkeeping (app-design.md §5.1) ---------------------------------
    //
    // WHY TWO FLAGS AND NOT ONE: hold and freeze are now separate controls (Engine::
    // setHold/setFreeze) that can be engaged independently or together, so a voice needs
    // to remember independently "am I being kept alive by hold" and "am I latched by
    // freeze" -- collapsing them back into one bool is exactly the two-names-one-behaviour
    // trap the split was written to avoid.
    //
    // holdDeferred: true from the moment a note-off arrives while Engine::setHold(true) is
    // in effect, until setHold(false) releases every voice with this flag set. The voice
    // itself is untouched while deferred -- state, cursor, everything keeps tracking live
    // input exactly as an unreleased note would. This is conventional sustain.
    bool holdDeferred = false;

    // freezeLatched: true for every voice that was SOUNDING at the instant
    // Engine::setFreeze(true) was called. While latched, the voice ignores note-off
    // entirely (see Engine::noteOff) -- not deferred, simply not listened to -- and its
    // cursor is stalled into a loop window (see ReadCursor::frozen). Cleared, and the
    // voice moved to Releasing, when setFreeze(false) is called. PROVISIONAL (app-design.md
    // §5.1): latching as well as stalling is what makes freeze a pad you can sing over
    // instead of a note that silently keeps chasing input queued up behind it, but it is
    // untested by ear. If it turns out wrong, the fix is one line in Engine::setFreeze:
    // stop setting this flag and freeze becomes a pure cursor-stall, with note-off staying
    // live exactly as it always could.
    bool freezeLatched = false;

    // --- humanization runtime state (app-design.md §5.2) -------------------------------
    //
    // Everything a voice needs to keep between blocks that is NOT itself a per-voice
    // "value" (those live in `hum` above, alongside the static per-voice detune/vibrato-
    // rate/depth that Engine::noteOn derives once from the voice index and never touches
    // again). These three are the exception: they change every block.
    //
    // humDriftValue: the CURRENT output of the slow random walk described by
    // Humanization::detuneDriftCents (the walk's amplitude). Starts at exactly 0 and the
    // recurrence in Engine::process is constructed so that it STAYS exactly 0, bit for
    // bit, for as long as the amplitude is exactly 0 -- see that function's comment. That
    // is what lets humanization-off renders stay bit-identical without a branch.
    float humDriftValue = 0.0f;

    // humDriftRng: a per-voice RNG seed for the drift walk, seeded from the voice index at
    // noteOn with a DIFFERENT multiplier than cursor.rngState (which seeds the freeze
    // restart jitter and the granular shifter's unvoiced-jump jitter) -- sharing one seed
    // across unrelated random processes would correlate them (the drift and the freeze
    // jitter would "roll the same dice"), which defeats the point of derandomising each
    // independently.
    std::uint32_t humDriftRng = 1;

    // startAtPos: onsetJitterMs's entry-timing spread. Set at noteOn to
    // `noteOnPos + jitterSamples`; Engine::process skips this voice (does not advance its
    // cursor, exactly like the pre-existing history gate) until the ring's write position
    // reaches it. At the default jitter of 0 this is exactly noteOnPos, and because
    // noteOnPos is captured from ring_->writePos() BEFORE that block's write() call runs,
    // writePos() has already moved past it by the time this is checked -- so the gate
    // never fires and this is a no-op, matching today's behaviour exactly.
    Pos startAtPos = 0;

    float gain = 1.0f;

    // Amplitude envelope, applied by the Engine.
    //
    // WHY IT EXISTS: cutting a voice dead on note-off produces a step discontinuity, and a
    // step is broadband — it clicks. A few milliseconds of ramp removes it entirely. Also
    // used to fade a voice IN, which matters because a voice starting mid-vowel begins at
    // whatever amplitude the waveform happened to be at.
    float envGain = 0.0f;
    float envTarget = 1.0f;

    // The envelope's RAMP POSITION, 0..1, from which envGain is derived through a raised
    // cosine. envGain itself is not ramped linearly any more.
    //
    // WHY: a linear ramp is C0, not C1 — its derivative steps at both ends. HANDOVER.md
    // states the rule ("every crossfade is at least C1: a fade with a kink in its
    // derivative clicks no matter how long you make it") and this envelope was the one
    // place in the codebase that broke it, on every note-on and every note-off.
    float envPhase = 0.0f;

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
