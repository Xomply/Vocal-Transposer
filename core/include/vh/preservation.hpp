// vh/preservation.hpp — "keep it the same voice" is not one decision. It is seven.
//
// SEED: transposing a voice while keeping it recognisably the same voice requires
// deciding, per component, whether "same" means preserve, warp, or pass through. The
// design document works this out as a table; this file is that table as code, so the
// decisions live in one place instead of being scattered as implicit behaviour across
// three engines.
//
// WHY A STRUCT RATHER THAN CONSTANTS: every row here is a thing we expect to tune by ear,
// per voice, during phase 6. That is a high-confidence bet about what varies, which is
// exactly when the handover discipline says to build a dial.

#pragma once

#include "vh/voicing.hpp"

namespace vh {

enum class UnvoicedPolicy {
    // CERTAIN this is correct per voice: fricatives are shaped by front-cavity geometry
    // (tongue, teeth), not larynx rate. A real singer's /s/ does not transpose when they
    // sing a fifth higher. Shifting it is simply the wrong operation.
    PassThrough,

    // ...BUT see the open issue. With N voices all passing the SAME /s/ through
    // unmodified, the ensemble collapses to N identical mono copies at every sibilant and
    // re-widens when voicing resumes — an audible pumping of stereo width. Real backing
    // singers sibilate decorrelated.
    // This option exists to hold the place for the fix (per-voice allpass / micro-delay /
    // mild spectral warp). NOT YET IMPLEMENTED — deliberately deferred, see ARCHITECTURE.
    PassThroughDecorrelated,

    // Escape hatch for A/B listening. Almost certainly sounds wrong; keep it so the
    // wrongness can be demonstrated rather than asserted.
    Shift,
};

struct PreservationSpec {
    // Preserve the spectral envelope under pitch shift. This is what makes the output
    // your voice rather than a chipmunk. CERTAIN: on, always, for any serious path.
    bool preserveEnvelope = true;

    // Vocal-tract-length warp applied to the envelope, as a multiplier on the frequency
    // axis. 1.0 = strict preservation.
    //
    // WHY IT IS NOT SIMPLY 1.0: F0 and formants are governed by different physiology and
    // do not scale together. Preserving the envelope exactly under an octave drop implies
    // a small vocal tract producing an impossibly low fundamental — the ear reads that as
    // buzzy and synthetic rather than as a bass singer. Literature puts a believable
    // octave-down at mu ~0.75-1.0.
    //
    // SUPERSEDED, AND LEFT HERE ON PURPOSE. This is a single number standing in for a
    // relationship: it suits an octave and is wrong at every other interval, which nobody
    // discovered because nothing read it. `voicing` below replaces it with a curve, and
    // the curve reproduces this value at an octave (0.5^0.3 = 0.81) from population data
    // rather than from a citation. This field is now only used when `voicing.enabled` is
    // false — the A/B control. See VOICE-MODEL.md §6.
    float envelopeWarp = 0.8f;

    // Apply envelopeWarp progressively with shift magnitude rather than as a constant.
    //
    // WHY: at a shift of a whole tone you want strict preservation; at an octave you want
    // the warp. A fixed warp makes small shifts sound subtly wrong in order to make large
    // ones sound right. CERTAIN in principle; the curve is provisional.
    //
    // SUPERSEDED by `voicing`, which IS this idea done properly: the curve is a derived
    // power law rather than an unspecified "progressively".
    bool scaleWarpWithShift = true;

    // How each voice property varies with the shift ratio. This is the model; the flags
    // above are the legacy constants it replaces. See vh/voicing.hpp and VOICE-MODEL.md.
    VoicingProfile voicing{};

    // Keep breath/noise energy unshifted while harmonics move.
    // CERTAIN: shifting aperiodicity with the harmonics tonalizes breath noise, which is
    // one of the classic giveaways of a cheap harmonizer.
    bool preserveAperiodicity = true;

    // Vibrato depth is preserved in CENTS, not Hz.
    //
    // WHY: vibrato is a musical interval, not a frequency excursion. Preserved in Hz, an
    // octave-up halves the perceived depth and an octave-down doubles it. CERTAIN.
    bool vibratoConstantInCents = true;

    UnvoicedPolicy unvoiced = UnvoicedPolicy::PassThrough;

    // Nothing time-stretches. Ever. Present as a named field rather than an unwritten
    // assumption so that a future reader knows it was considered and settled, not
    // overlooked. CERTAIN — this is a live instrument; time is not ours to move.
    bool preserveTiming = true;
};

} // namespace vh
