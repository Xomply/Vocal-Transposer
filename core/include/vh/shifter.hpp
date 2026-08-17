// vh/shifter.hpp — the seam between "how we shift" and everything else.
//
// SEED: the architecture runs TWO shifters concurrently on the same input and blends
// them — a cheap fast one that arrives in ~13 ms and gives the instrument its
// playability, and a slower high-quality one that arrives ~30-50 ms later and gives it
// its sound. This is lifted from Ben Bloomberg's rig for Jacob Collier, which ran four
// instances of Antares Harmony Engine in parallel with a TC-Helicon unit specifically to
// give the impression of lower latency.
//
// WHY THIS INTERFACE IS A HIGH-CONFIDENCE DIAL: we know of at least four implementations
// we intend to write (delay-line/granular, TD-PSOLA, phase vocoder + true envelope,
// WORLD source-filter) and we know at least two must run simultaneously. That is not a
// speculative abstraction; it is the shape of the problem.

#pragma once

#include "vh/analysis.hpp"
#include "vh/audio_ring.hpp"
#include "vh/preservation.hpp"
#include "vh/types.hpp"

#include <type_traits>

namespace vh {

// --- ShifterTuning: the live parameters of the two concrete shifters --------------------
//
// LAYERING, resolved the hard way (app-design.md §4.3's own account of this, kept here
// because it is the reason these three structs live in THIS file and not in tuning.hpp,
// where an earlier track first put them). tuning.hpp includes voice.hpp; voice.hpp
// includes THIS file; so shifter.hpp reaching back into tuning.hpp for ShifterTuning would
// close a circular include. Defining the tuning structs here and having tuning.hpp include
// this file and aggregate them keeps the dependency one-directional. If a future reader
// finds GranularTuning/PsolaTuning/ShifterTuning declared in a file that otherwise talks
// about DSP interfaces rather than parameters, this paragraph is why.
//
// GranularConfig, below in granular_shifter.hpp, is now `using GranularConfig =
// GranularTuning;` rather than a second struct with the same fields — two structs holding
// the same data was the thing app-design.md §4.3 flagged as needing collapsing.
struct GranularTuning {
    // Jump distance in pitch periods when voiced. PROVISIONAL: 1. Larger jumps mean fewer
    // crossfades (less smearing) but more latency, since the delay must accommodate the
    // jump.
    int jumpPeriods = 1;

    // Crossfade length as a fraction of a period. PROVISIONAL: 0.5. Long fades hide the
    // jump better and smear transients more.
    float fadeFraction = 0.5f;

    // Fallback grain size when there is no F0 to be synchronous with. PROVISIONAL: 8 ms.
    float unvoicedGrainMs = 8.0f;

    // The one-pole time constant smoothing the geometry period in
    // GranularShifter::updateGeometry (granular_shifter.cpp). Was a hardcoded 20 ms;
    // promoted per app-design.md §5.7 so it can be swept by ear instead of edited and
    // rebuilt. See updateGeometry()'s own comment for why this needs to be a LAG, not an
    // instantaneous follow.
    float geometrySmoothMs = 20.0f;
};

struct PsolaTuning {
    // Handover time between the grain path and unshifted passthrough (psola_shifter.cpp's
    // mix_/mixStep_). Was a hardcoded kHandoverMs = 8.0 ("TUNE BY EAR on /t/ and /k/ before
    // anything else in this file" — HANDOVER.md §8 names this the single most-wanted
    // provisional constant in the whole codebase). Feeds
    // mixStep_ = 1 / (handoverMs * 0.001 * sampleRate); PsolaShifter::setTuning() recomputes
    // mixStep_ whenever this changes — the field alone would otherwise be silently INERT,
    // not wrong, exactly like YinConfig::maxHoldMs (app-design.md §4.1).
    float handoverMs = 8.0f;
};

struct ShifterTuning {
    GranularTuning granular{};
    PsolaTuning psola{};
};

namespace shifter_tuning_detail {
template <typename... Ts>
inline constexpr bool none_are_pointers = (!std::is_pointer_v<Ts> && ...);
} // namespace shifter_tuning_detail

static_assert(shifter_tuning_detail::none_are_pointers<
    decltype(GranularTuning::jumpPeriods), decltype(GranularTuning::fadeFraction),
    decltype(GranularTuning::unvoicedGrainMs), decltype(GranularTuning::geometrySmoothMs),
    decltype(PsolaTuning::handoverMs)>,
    "ShifterTuning's sections must contain no pointers -- Tuning (tuning.hpp) crosses the "
    "UI/audio boundary by value inside TuningBus, and a pointer field here would let the "
    "audio thread dereference something the UI thread owns.");

// Where a voice is reading from, and whether it is still following the live input.
//
// FREEZE LIVES HERE, and nowhere else. The AudioRing has no concept of it; the shifters
// have no concept of it. A frozen voice is a cursor that has stopped chasing the write
// head and instead loops within a captured window. That is the entire feature.
//
// WHY IT IS PASSED AS MUTABLE STATE RATHER THAN OWNED BY THE SHIFTER: two shifters
// process the SAME voice concurrently for blending. If each owned its own cursor they
// would drift apart, and the blend would crossfade between two different moments of the
// performance. One cursor per voice, borrowed by both engines.
struct ReadCursor {
    Pos next = 0;          // absolute position of the next input sample this voice wants
    bool frozen = false;

    // Valid only while frozen: the captured window to loop within.
    Pos freezeBegin = 0;
    Pos freezeEnd = 0;

    // PROVISIONAL, and the trap worth knowing about: looping a frozen window at a fixed
    // period produces an obviously-looped buzz within about a second. The fix is
    // randomised grain selection within [freezeBegin, freezeEnd) plus slow per-voice
    // detune drift, so held voices sound like sustained singers rather than a stuck
    // sample. This seed exists so each voice randomises differently.
    // IF THE FIX TURNS OUT UNNECESSARY: delete the field, nothing depends on it.
    std::uint32_t rngState = 1;
};

struct ShiftRequest {
    const AudioRing* ring = nullptr;
    const AnalysisFrame* analysis = nullptr;
    ReadCursor* cursor = nullptr;          // mutable: advanced by the shifter
    const PreservationSpec* preservation = nullptr;

    // Output pitch / input pitch. 1.0 = no shift, 2.0 = octave up.
    //
    // WHY A RATIO AND NOT A TARGET NOTE: it is the one quantity both modes agree on.
    // Mode A (detachment: sing anything, the played notes come out) computes it as
    // f_target / f_sung and therefore needs F0. Mode B (interval transposition from a
    // declared root) computes it as 2^(n/12) and needs no pitch detection at all. The
    // shifter should not know or care which mode produced the number — that decision
    // belongs to the voice, not the DSP.
    double ratio = 1.0;

    // Per-voice multiplier on the formant-scale factor mu, AFTER VoicingProfile::muFor()
    // (app-design.md §5.2). Carries Humanization::envelopeWarpOffset -- a slight per-voice
    // vocal-tract difference, symmetric around zero, so the ensemble does not sound like N
    // copies of one throat. Defaults to 1.0 (no-op): PsolaShifter computes
    // `clamp(muFor(ratio) * muScale, muMin, muMax)`, and multiplying by exactly 1.0 then
    // re-clamping an already-in-range value is a bit-exact identity, which is what keeps a
    // humanization-off render bit-identical without needing a branch anywhere on this path.
    //
    // Read by PSOLA only. Granular has no mu warp to offset -- it resamples by the full
    // ratio unconditionally -- so it simply ignores this field; ShiftRequest is shared by
    // both engines and a field one shifter does not need costs it nothing to carry.
    double muScale = 1.0;

    Sample* out = nullptr;
    FrameCount numFrames = 0;
};

class IPitchShifter {
public:
    virtual ~IPitchShifter() = default;

    // Off the audio thread. All allocation here.
    virtual void prepare(double sampleRate, FrameCount maxBlock) = 0;

    // Reset transient state without reallocating. Called on voice start.
    virtual void reset() noexcept = 0;

    // Audio thread. Must not allocate, lock, or block.
    // Writes exactly req.numFrames samples to req.out. Advances req.cursor.
    virtual void process(const ShiftRequest& req) noexcept = 0;

    // Algorithmic latency in samples, EXCLUDING host buffering.
    //
    // WHY QUERYABLE RATHER THAN DOCUMENTED: the blender must time-align two engines whose
    // latencies differ by tens of milliseconds and depend on the current F0 (a grain is
    // sized in pitch periods, so a shifter's latency changes as the singer moves). A
    // constant in a header would be wrong the moment the singer changed note.
    //
    // May be called per block. Must be cheap.
    virtual FrameCount latencySamples() const noexcept = 0;

    // Audio thread, via Engine::applyTuning(). Update the shifter's own live parameters.
    // Non-allocating, no locks. Defaulted to a no-op so PassthroughShifter (and any future
    // shifter with nothing tunable) needs no change — app-design.md §4.3. Concrete shifters
    // read only their own section of ShifterTuning (granular vs psola); a shifter must
    // never reach across into the other's section, which is why the sections are separate
    // structs rather than one flat one.
    //
    // A shifter whose prepare()-baked derived values depend on a field in here (PSOLA's
    // mixStep_ from handoverMs, see PsolaTuning) MUST recompute that derived value in its
    // override, not just copy the field — see app-design.md §4.1: the field alone is
    // otherwise silently INERT, which is worse than a crash because the knob appears to
    // work.
    virtual void setTuning(const ShifterTuning&) noexcept {}

    // For logging, test names, and the UI. Not used for dispatch — no string comparisons
    // on the audio thread.
    virtual const char* name() const noexcept = 0;
};

} // namespace vh
