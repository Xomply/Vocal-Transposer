// vh/analysis.hpp — what the pipeline knows about the singer's voice, right now.
//
// SEED: analysis is SHARED. Every voice reads the same F0, the same epochs, the same
// spectral envelope. This is why voice count costs CPU but zero latency, and it is the
// single most important structural fact about the whole system. One analyser, N
// synthesisers.
//
// So AnalysisFrame is the narrow waist of the architecture: everything upstream produces
// it, everything downstream consumes it, and the two halves know nothing else about each
// other. Adding a field here is cheap; making synthesis reach back into an analyser's
// internals is the thing to never do.

#pragma once

#include "vh/types.hpp"

#include <cstdint>

namespace vh {

enum class Voicing : std::uint8_t {
    Unvoiced,   // fricative, plosive, silence — no periodicity
    Voiced,     // periodic, F0 meaningful
};

struct AnalysisFrame {
    // Absolute position this frame describes. Frames are timestamped rather than
    // implicitly ordered because the analysis hop and the audio block size are unrelated,
    // and pretending otherwise causes drift that shows up as slow detuning over minutes.
    Pos pos = 0;

    Voicing voicing = Voicing::Unvoiced;

    // Best current estimate of fundamental frequency, in Hz.
    //
    // READ THE NEXT FIELD BEFORE USING THIS ONE.
    float f0Hz = 0.0f;

    // TRUE when f0Hz is a HELD value from the last confident estimate rather than a
    // fresh measurement.
    //
    // WHY THIS FIELD EXISTS — this is a real design decision, not bookkeeping:
    // F0 acquisition costs 18-27 ms at a low male fundamental (2-3 pitch periods before
    // YIN can commit). That cost is a CONVERGENCE cost, paid when the tracker locks —
    // NOT a per-block tax. If the tracker treats every unvoiced frame as lost lock, then
    // every consonant in a sung lyric re-pays 27 ms and the instrument stutters through
    // words while feeling fine on a held vowel.
    //
    // So: on unvoiced or low-confidence frames we HOLD the previous F0 and set this flag,
    // rather than reporting zero. Consumers that need a shift ratio can carry on
    // unbothered through a /t/. Consumers that care whether the estimate is real (a
    // re-acquisition policy, a confidence-gated blend) check the flag.
    //
    // CERTAIN that holding is correct. PROVISIONAL on when to give up holding and force
    // re-acquisition — that is a tuning question for the analyser, see IAnalyzer.
    bool f0IsHeld = false;

    // 0..1. Analyser-specific but monotonic: higher means more trustworthy.
    // Used for adaptive behaviour (widen search, duck the wet signal, force
    // re-acquisition) and as a blend input.
    float confidence = 0.0f;

    // Period in samples, derived from f0Hz. Cached because grain sizing needs it on every
    // sample of every voice, and a divide per voice per sample is real money.
    float periodSamples = 0.0f;

    // Most recent glottal closure instant at or before `pos`, absolute.
    //
    // WHY SEPARATE FROM f0Hz: F0 is a RATE; epochs are a PHASE. Pitch-synchronous methods
    // (PSOLA, and the pitch-synchronous delay-line fix that makes the comb filter stop
    // existing rather than merely smearing) need to know WHEN the glottis closed, not
    // just how often. A MIDI keyboard can tell you the rate; nothing but the signal can
    // tell you the phase. Zero means "not established".
    Pos lastEpoch = 0;
    bool epochValid = false;

    // Recent epoch history, newest last.
    //
    // WHY A HISTORY AND NOT JUST THE LATEST: PSOLA does not place one grain at the newest
    // epoch. For each synthesis instant it looks for the analysis epoch nearest in time,
    // and under a downward shift the synthesis instants are further apart than the
    // analysis ones, so it must reach back several periods.
    //
    // WHY IT LIVES IN THE SHARED FRAME rather than inside each shifter: every voice needs
    // the same list. Sixteen voices each maintaining a private copy of identical data is
    // sixteen chances for them to disagree. Shared analysis, per-voice synthesis — the
    // same principle that makes voice count free of latency.
    //
    // PROVISIONAL at 64 entries: ~0.9 s at a 70 Hz fundamental, far more than any grain
    // search needs. Costs 512 bytes in a struct only ever passed by pointer.
    static constexpr int kEpochHistory = 64;
    Pos epochs[kEpochHistory]{};
    int epochCount = 0;   // valid entries, newest at epochCount-1

    // Nearest recorded epoch to `target`, or 0 if none.
    Pos nearestEpoch(Pos target) const noexcept {
        if (epochCount == 0) return 0;
        Pos best = epochs[0];
        Pos bestDist = target > best ? target - best : best - target;
        for (int i = 1; i < epochCount; ++i) {
            const Pos e = epochs[i];
            const Pos d = target > e ? target - e : e - target;
            if (d < bestDist) { bestDist = d; best = e; }
        }
        return best;
    }
};

// Interface, not a base class to inherit behaviour from.
//
// WHY AN INTERFACE HERE (this is a deliberate dial, per the handover discipline):
// we are confident the analyser will be swapped. YIN is the default; pYIN adds HMM
// smoothing at some latency cost; CREPE is a heavier neural option; and a MIDI-informed
// analyser that constrains the search around the played notes is an explicit item on the
// roadmap. Four known candidates is enough certainty to justify the indirection.
//
// Contrast with the FFT backend, which is deliberately NOT abstracted — we will pick one
// and live with it.
class IAnalyzer {
public:
    virtual ~IAnalyzer() = default;

    // Called once, off the audio thread. All allocation happens here.
    virtual void prepare(double sampleRate, FrameCount maxBlock) = 0;

    // Audio thread. Consume newly available input up to `upTo` and update internal state.
    // Must not allocate.
    virtual void process(const class AudioRing& ring, Pos upTo) noexcept = 0;

    // Most recent frame. Cheap; callers may call per voice per block.
    virtual const AnalysisFrame& current() const noexcept = 0;

    // How far behind `upTo` the analyser's estimate necessarily lags, in samples.
    //
    // WHY THIS IS ON THE INTERFACE: the blender aligns engine outputs in time, and the
    // engines' latencies depend on their analyser's. A latency figure that lives only in
    // a comment gets out of date the first time someone changes a window size. Making it
    // a queryable property means the alignment maths is derived, never hand-maintained.
    virtual FrameCount latencySamples() const noexcept = 0;

    // Force the next frame to be a fresh measurement rather than a held value.
    // Used when something upstream knows the pitch has genuinely changed.
    virtual void forceReacquire() noexcept = 0;
};

} // namespace vh
