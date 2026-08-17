// app/src/LiveVoiceEstimate.h — a best-effort "where are we right now" for the curve
// displays' live dot (docs/app-design.md §6.3).
//
// WHY THIS IS AN APPROXIMATION, STATED UP FRONT: vh::Engine deliberately exposes no
// per-voice introspection — asking it "which voice is loudest" or "what ratio is voice N
// using" would mean either reaching into DSP internals from the UI (exactly what
// app/README.md forbids) or growing Engine's public surface for a display-only need. So
// this reconstructs the SAME public ratio formula Engine::ratioForVoice uses internally
// (core/src/engine.cpp) from data EngineAudioCallback already exposes as meter atomics:
// the most recently triggered MIDI note and the analyser's current F0. This is arithmetic
// for a plot, not DSP — it computes nothing that reaches an audio sample — but it is
// honest to record that with a chord held, it tracks the MOST RECENTLY TRIGGERED voice,
// not necessarily the loudest one, because no per-voice level meter exists anywhere in
// this codebase to do better with.
#pragma once

#include "EngineAudioCallback.h"
#include "vh/tuning.hpp"

#include <cmath>

namespace vhapp {

inline double midiToHzApprox(int note) noexcept {
    return 440.0 * std::pow(2.0, static_cast<double>(note - 69) / 12.0);
}

struct LiveVoiceEstimate {
    bool available = false;         // false: no active voice, or panic was just pressed --
                                     // curve displays should draw no dot, not a stale one.
    double ratio = 1.0;             // output/input pitch ratio (mirrors ratioForVoice()).
    double intervalSemitones = 0.0; // 12*log2(ratio); the mu-curve's x-axis quantity.
    double targetHz = 0.0;          // midiToHzApprox(lastNoteOn) -- the register-split axis.
    double sourceF0Hz = 0.0;        // analyser's current F0 -- the tilt shelf's anchor.
    double msSinceNoteOn = 0.0;     // the onset-handover axis.
};

inline LiveVoiceEstimate estimateLiveVoice(const EngineAudioCallback& engine, vh::RatioSource mode,
                                            int rootMidiNote) noexcept {
    LiveVoiceEstimate e;
    const int note = engine.lastNoteOn();
    if (note < 0 || engine.activeVoices() <= 0) return e;   // available stays false

    e.available = true;
    e.sourceF0Hz = engine.f0Hz();
    e.targetHz = midiToHzApprox(note);
    e.msSinceNoteOn = engine.msSinceLastNoteOn();

    if (mode == vh::RatioSource::IntervalFromRoot) {
        // Mode B: exactly Engine::ratioForVoice's own formula -- no F0 involved, known the
        // instant the key goes down.
        e.ratio = std::pow(2.0, static_cast<double>(note - rootMidiNote) / 12.0);
    } else {
        // Mode A: f_target / f_sung, same as ratioForVoice. Falls back to 1.0 (no shift)
        // when there is no confident F0 yet, matching the core's own fallback.
        e.ratio = (e.sourceF0Hz > 1.0) ? (e.targetHz / e.sourceF0Hz) : 1.0;
    }
    e.intervalSemitones = 12.0 * std::log2(e.ratio > 1e-9 ? e.ratio : 1e-9);
    return e;
}

} // namespace vhapp
