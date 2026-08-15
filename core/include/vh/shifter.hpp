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

namespace vh {

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

    // For logging, test names, and the UI. Not used for dispatch — no string comparisons
    // on the audio thread.
    virtual const char* name() const noexcept = 0;
};

} // namespace vh
