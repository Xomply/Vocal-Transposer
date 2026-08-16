// vh/engine.hpp — the orchestrator. Owns nothing conceptual; wires everything concrete.
//
// SIGNAL FLOW, and the reason it is shaped this way:
//
//   mic -> AudioRing (shared history, one writer)
//            |
//            +-> IAnalyzer            ONE analyser for all voices. Shared analysis is
//            |                        why voice count costs CPU but zero latency.
//            |
//            +-> for each active Voice:
//                   fast   engine ---+
//                                    +--> IBlendPolicy --> mix
//                   quality engine --+
//
// The Engine deliberately knows no concrete DSP. Analyser and shifters arrive by
// injection, the blend policy is swappable at runtime. That is not abstraction for its
// own sake: the whole project is an experiment in which engine and which split sound
// best, so "swap the engine, keep everything else" is the primary operation this codebase
// exists to support.
//
// WHAT IS DELIBERATELY *NOT* GENERALISED (so nobody adds it later thinking it was an
// oversight): there is no effect graph, no node system, no plugin registry, no
// abstraction over sample rate or channel count. Those are the standard ways an audio
// project like this drowns. Mono in, mixed out, two engines, one analyser.

#pragma once

#include "vh/analysis.hpp"
#include "vh/audio_ring.hpp"
#include "vh/blend.hpp"
#include "vh/preservation.hpp"
#include "vh/shifter.hpp"
#include "vh/types.hpp"
#include "vh/voice.hpp"

#include <functional>
#include <memory>
#include <vector>

namespace vh {

using ShifterFactory = std::function<std::unique_ptr<IPitchShifter>()>;

struct EngineConfig {
    double sampleRate = 48000.0;
    FrameCount maxBlock = 128;

    // Mode A or Mode B. See RatioSource — these are different instruments, not settings.
    RatioSource ratioSource = RatioSource::AbsoluteTarget;

    // Mode B only: the declared root the intervals are measured from.
    // PROVISIONAL that a declared root is musically sufficient; if it is not, Mode B loses
    // its latency advantage entirely.
    int rootMidiNote = 60;

    PreservationSpec preservation{};
};

class Engine {
public:
    Engine();
    ~Engine();

    // Off the audio thread. Everything that allocates happens here and only here.
    void prepare(const EngineConfig& cfg,
                 std::unique_ptr<IAnalyzer> analyzer,
                 ShifterFactory makeFast,
                 ShifterFactory makeQuality);

    // Audio thread. in and out may not alias. Writes exactly numFrames.
    void process(const Sample* in, Sample* out, FrameCount numFrames) noexcept;

    // Audio thread, called from the MIDI queue before process().
    void noteOn(int midiNote, float velocity) noexcept;
    void noteOff(int midiNote) noexcept;
    void setSustain(bool held) noexcept;

    // Runtime-swappable. This is the dial the whole architecture was shaped around: the
    // blend policy can be replaced while playing, without touching engines or voices.
    //
    // Ownership is caller-side and the pointer must outlive the Engine. Deliberately a
    // raw pointer rather than a shared_ptr swap: refcount traffic on the audio thread is
    // exactly the sort of thing that is invisible until it is a dropout.
    void setBlendPolicy(const IBlendPolicy* policy) noexcept { blend_ = policy; }
    const IBlendPolicy* blendPolicy() const noexcept { return blend_; }

    void setPreservation(const PreservationSpec& p) noexcept { cfg_.preservation = p; }
    void setRatioSource(RatioSource s) noexcept { cfg_.ratioSource = s; }

    // How much of the latency difference between the two engines to compensate, 0..1.
    //
    // THIS IS THE PARAMETER THE ARCHITECTURE DOC FLAGGED AS "the first thing that will
    // break when a real quality engine lands", and it is a genuine musical trade rather
    // than a bug to be fixed:
    //
    //   0.0 — no compensation. The fast engine's output arrives early, exactly as intended;
    //         you hear the note when you press the key. The two engines are misaligned in
    //         time, so the blend is a smear rather than a coherent sum. Bloomberg's rig
    //         works this way and the smear is the point.
    //   1.0 — full compensation. The fast path is delayed to match the quality path, the
    //         sum is phase-coherent, and the entire latency advantage is thrown away.
    //
    // Anywhere in between trades playability against coherence. There is no correct value;
    // there is a value you prefer, which is why it is exposed and why the default is 0.
    void setBlendAlignment(float amount) noexcept {
        alignAmount_ = amount < 0.0f ? 0.0f : (amount > 1.0f ? 1.0f : amount);
    }
    float blendAlignment() const noexcept { return alignAmount_; }

    // Introspection for tests, the offline harness, and the UI meters.
    const AudioRing& ring() const noexcept { return ring_; }
    const AnalysisFrame& analysis() const noexcept;
    int activeVoiceCount() const noexcept;

    // Total algorithmic latency of the currently-selected path, in samples.
    // Derived from the engines rather than hand-maintained, so it cannot go stale.
    FrameCount latencySamples() const noexcept;

private:
    int allocateVoice(int midiNote) noexcept;
    double ratioForVoice(const Voice& v, const AnalysisFrame& a) const noexcept;

    EngineConfig cfg_{};
    AudioRing ring_;
    std::unique_ptr<IAnalyzer> analyzer_;

    // One shifter pair PER VOICE, not one pair shared by all voices.
    //
    // WHY: shifters carry per-voice transient state — grain phase, crossfade position,
    // accumulated phase in the vocoder case. Sharing one instance across voices would
    // have every voice trampling the others' state. The cost is kMaxVoices * 2 objects,
    // which is nothing; the alternative is a class of bug that manifests only when two
    // notes are held at once.
    std::vector<std::unique_ptr<IPitchShifter>> fast_;
    std::vector<std::unique_ptr<IPitchShifter>> quality_;

    std::vector<Voice> voices_;

    // Scratch, sized in prepare(). Never resized on the audio thread.
    std::vector<Sample> fastBuf_, qualityBuf_;

    // Per-voice delay line used to time-align the fast path against the quality path.
    // Flat storage with a stride rather than a vector-of-vectors: one allocation, and the
    // audio thread never touches a pointer it did not already have.
    std::vector<Sample> alignBuf_;
    FrameCount alignStride_ = 0;
    std::vector<FrameCount> alignWrite_;
    float alignAmount_ = 0.0f;

    // Per-voice decorrelation delay lines. See Humanization::staticDelayMs and BUGS.md
    // VH-005: N voices passing the same fricative through unshifted sum coherently, and a
    // few milliseconds of per-voice offset is what turns that back into an ensemble.
    std::vector<Sample> decorBuf_;
    std::vector<FrameCount> decorWrite_;
    FrameCount decorStride_ = 0;

    const IBlendPolicy* blend_ = nullptr;
    FastOnlyPolicy defaultBlend_{};   // used until someone sets a real one

    bool sustain_ = false;
    bool prepared_ = false;
};

} // namespace vh
