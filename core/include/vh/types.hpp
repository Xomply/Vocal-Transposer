// vh/types.hpp — the vocabulary every other header speaks.
//
// SEED: a MIDI-driven vocal harmonizer. You sing; a MIDI keyboard says which notes
// come out. See ARCHITECTURE.md for the full why.
//
// This header exists so that "a position in the input stream" has exactly one meaning
// across the whole codebase. Getting that wrong is the classic way real-time audio
// projects rot: two modules disagree about whether an index is relative to the current
// block or to the start of time, and the bug only shows up as a click every few minutes.

#pragma once

#include <cstdint>
#include <cstddef>

namespace vh {

// CERTAIN: float, not double. Audio DSP at 32-bit float has ~144 dB of headroom, which
// is far beyond anything audible, and float halves cache pressure — which matters more
// than precision here, because we are latency-bound, not accuracy-bound.
using Sample = float;

// CERTAIN: absolute sample position since stream start, never block-relative.
//
// WHY THIS IS THE MOST IMPORTANT DECISION IN THE FILE: voices read the input history at
// independent lags. A frozen voice sits still while the writer runs on; the fast and
// quality engines read the same audio at different delays; the blender has to align two
// results that were computed from the same source samples. All of that is trivial when
// every module names a sample by its absolute index and impossible to keep straight when
// they name it by "12 samples into the current block".
//
// uint64 at 48 kHz wraps after ~12 million years. Not a concern.
using Pos = std::uint64_t;

// Frames, not samples, when counting time. The distinction is pedantic in a mono
// pipeline but stops the classic stereo off-by-two when output goes stereo (it will —
// per-voice panning is in the humanization spec).
using FrameCount = std::size_t;

// CERTAIN (for now): the analysis pipeline is mono. The microphone is one capsule and
// every analysis stage (F0, epochs, envelope) is defined on a single voice signal.
// Output may become stereo for per-voice panning; input will not become stereo.
inline constexpr int kAnalysisChannels = 1;

// PROVISIONAL: 16 simultaneous voices.
// Requirement was "at least 4, preferably 8+". 16 gives headroom for sustain-pedal
// stacking (holding a chord and adding another on top), which is a stated interest.
// Voice count costs CPU but *zero latency* — analysis is shared, synthesis is parallel —
// so the only thing this number trades against is CPU budget.
// IF THIS CHANGES: nothing structural. It sizes arrays. Raise it freely.
inline constexpr int kMaxVoices = 16;

// PROVISIONAL: 2 seconds of input history at 48 kHz.
// Sized by the *freeze* feature, not by the shifters: a frozen voice keeps re-reading
// captured material, and under ~1 s of source the loop becomes audibly repetitive.
// The shifters need only tens of milliseconds.
// IF THIS CHANGES: memory only. 2 s mono float = 384 kB. Cheap. Raise it if freeze
// sounds looped.
inline constexpr FrameCount kInputHistoryFrames = 96000;

// Musical constants.
inline constexpr double kSemitoneRatio = 1.0594630943592953;  // 2^(1/12)
inline constexpr double kCentsPerOctave = 1200.0;

} // namespace vh
