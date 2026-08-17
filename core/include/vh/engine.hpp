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
#include "vh/tuning.hpp"
#include "vh/types.hpp"
#include "vh/voice.hpp"

#include <algorithm>
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

    // How many frames of input history the AudioRing holds. Was baked into Engine's
    // constructor initialiser list (`ring_(kInputHistoryFrames)`), which meant changing it
    // needed Engine *reconstruction* rather than a re-prepare() — the one piece of sizing
    // that happened before prepare() got to see a config at all. app-design.md §4.1/§6
    // asks for this to join the ordinary restart tier; it now does, at the cost of
    // AudioRing construction moving into prepare() (see Engine::prepare() in the .cpp).
    // Restart-only: sized once, at prepare(), like every other buffer in this struct.
    FrameCount inputHistoryFrames = kInputHistoryFrames;

    // --- live, but stored here rather than as bare Engine members ------------------------
    //
    // These five are mutated post-prepare() by Engine::applyTuning() (and, for the first
    // three, are read every block) exactly the way ratioSource/rootMidiNote/preservation
    // already were before this struct grew a Tuning-driven fan-out — "EngineConfig" names
    // where prepare() gets ITS config from, not "config that only changes at prepare()".
    // Splitting live fields into a second struct next to this one would separate two
    // things that are read from the same call sites for the same reason and buys nothing.
    //
    // Defaults match the literals they replace exactly (8 ms envelope, 500 ms freeze
    // window, 0.5-4 ms decorrelation spread) so a default-constructed EngineConfig, and
    // therefore a default-constructed Tuning applied on top of it, changes no audio -- the
    // governing property tests/test_tuning.cpp exists to pin.
    float attackMs = 8.0f;       // was Engine::process's hardcoded envStep (8 ms, both ways)
    float releaseMs = 8.0f;
    float freezeWindowMs = 500.0f;   // was noteOff()'s hardcoded `sampleRate * 0.5`
    float decorMinMs = 0.5f;         // was noteOn()'s hardcoded `0.5 + 3.5 * frac`
    float decorMaxMs = 4.0f;         // achieved range of that formula, frac in [0, 1)

    // Ensemble SPREAD (app-design.md §5.2) -- per-voice values are derived from this at
    // noteOn, deterministically by voice index; see Engine::noteOn. Default-constructed
    // HumanizationSpec{} is all zero, so every per-voice derivation below collapses to an
    // exact algebraic no-op (a multiplier of 1.0, an additive offset of 0.0) rather than a
    // branch — see engine.cpp for why that is what keeps a humanization-off render bit-
    // identical to the pre-Track-C engine.
    HumanizationSpec humanize{};

    // Runtime voice cap, honoured by allocateVoice() (app-design.md §5.6). Defaults to
    // kMaxVoices, i.e. every voice slot prepare() allocates is usable — identical to
    // today's uncapped behaviour. Tuning::clamp() keeps a Tuning-sourced value inside
    // [1, kMaxVoices]; allocateVoice() clamps defensively again in case a caller sets this
    // field directly on an EngineConfig without going through Tuning at all.
    int voiceCap = kMaxVoices;

    // Velocity -> gain curve (app-design.md §5.6). See Tuning::velocityToGainMin/
    // velocityCurve for the formula and why the defaults are a bypass.
    float velocityToGainMin = 1.0f;
    float velocityCurve = 1.0f;

    // --- signal path (app-design.md §5.5, Track D) -------------------------------------
    //
    // Input stage (trim, then gate) and output stage (dry/wet, then a limiter). Every
    // default below is the literal no-op value from Tuning's own file header: applying a
    // default-constructed Tuning must not change what Engine::process emits, because
    // Engine::process did not read any of these fields before this track existed. See
    // mixImpl() in the .cpp for where each one is actually consumed.
    float inputTrimDb = 0.0f;    // 0 dB = unity.
    bool  inputGateOn = false;   // gate OFF.
    float inputGateDb = -60.0f;  // threshold WHEN ENABLED; unread while inputGateOn is false.
    float dryGain = 0.0f;        // no dry signal mixed in -- today's engine emits none.
    float wetGain = 1.0f;        // unity -- today's engine is 100% wet.
    float outCeilingDb = 0.0f;   // 0 dBFS; unread while limiterOn is false.
    bool  limiterOn = false;     // no limiter -- app-design.md §5.5 requires this OFF by
                                  // default specifically so an offline measurement render
                                  // is never a measurement of the limiter (BUGS.md VH-009
                                  // is the reason the limiter exists in the first place).
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
    //
    // THE MONO OVERLOAD MUST STAY BIT-IDENTICAL (app-design.md §5.4, CERTAIN — a hard
    // constraint). RESULTS.md's measurements and click_sweep.py's regression grid are
    // rendered-mono-audio comparisons against thresholds; changing this path invalidates
    // the whole evidence base for no benefit. It does NOT route through the pan stage —
    // see mixImpl() in the .cpp, templated on channel count so this instantiation is
    // exactly the pre-stereo mix loop and the stereo instantiation below is purely an
    // ADDED path, never a shared one with a runtime branch in it.
    void process(const Sample* in, Sample* out, FrameCount numFrames) noexcept;

    // Stereo overload (app-design.md §5.3). outL and outR may not alias each other or in.
    // Per-voice equal-power pan, derived deterministically from voice index exactly like
    // the decorrelation delay and the rest of Humanization — see Engine::noteOn. At
    // Tuning::humanize.panSpread == 0 (the default) every voice's pan is exactly 0.0f, so
    // every voice sits dead centre and L == R for the whole block.
    void process(const Sample* in, Sample* outL, Sample* outR, FrameCount numFrames) noexcept;

    // Audio thread, called from the MIDI queue before process().
    void noteOn(int midiNote, float velocity) noexcept;
    void noteOff(int midiNote) noexcept;

    // --- hold and freeze: two controls, not one (app-design.md §5.1) -------------------
    //
    // WHY SPLIT: the old setSustain(true) + note-off detached a voice from live input and
    // looped a captured window — a genuine feature, but not what a keyboard player's hands
    // expect from a sustain pedal (CC64). Two names for divergent behaviours is how that
    // becomes a bug in six months, so setSustain is REMOVED rather than kept as an alias.
    //
    // setHold(true): conventional sustain. A note-off arriving while held is DEFERRED —
    // the voice keeps tracking live input, state and cursor untouched — until
    // setHold(false), at which point every deferred voice moves to Releasing. This is what
    // CC64 should be wired to.
    void setHold(bool held) noexcept;

    // setFreeze(true): its own control, its own assignable CC (not CC64). On engage, every
    // currently SOUNDING voice latches — stops responding to note-off entirely, not merely
    // deferring it — AND stalls its cursor into a loop window of Tuning::freezeWindowMs
    // ending at the voice's position right now (see ReadCursor::frozen). On
    // setFreeze(false), every latched voice moves to Releasing. Hold and freeze may both be
    // engaged at once; freeze wins on CURSOR behaviour (a voice cannot both track live
    // input and loop a captured window), but each still keeps its own note-off bookkeeping.
    //
    // PROVISIONAL (app-design.md §5.1): the latch half. Stalling the cursor is what makes
    // freeze a captured loop; latching against note-off ADDITIONALLY is what makes it a pad
    // you can keep singing over after releasing the key, but that combination is untested
    // by ear. The two halves separate cleanly — see Voice::freezeLatched's comment — if
    // latching turns out to feel wrong.
    void setFreeze(bool engaged) noexcept;

    // Panic (app-design.md §5.6). Every active voice is moved to Releasing immediately,
    // bypassing hold/freeze latching entirely — this is an emergency stop, not a note-off,
    // so it must cut through both. Still goes through the release ENVELOPE rather than
    // hard-muting: HANDOVER.md's "every crossfade is at least C1" rule applies to silence
    // too, and a panic button that clicks on every press is worse than one that takes 8 ms.
    void allNotesOff() noexcept;

    // Runtime-swappable. This is the dial the whole architecture was shaped around: the
    // blend policy can be replaced while playing, without touching engines or voices.
    //
    // Ownership is caller-side and the pointer must outlive the Engine. Deliberately a
    // raw pointer rather than a shared_ptr swap: refcount traffic on the audio thread is
    // exactly the sort of thing that is invisible until it is a dropout. THAT REASONING
    // STILL HOLDS after applyTuning() below — app-design.md §4.4 is explicit this is a
    // documented decision deviated from ONLY for policy *parameters*, not for policy
    // *ownership*. Ownership of ALL FOUR concrete policies moved to Engine (see the
    // members near the bottom of this class) precisely so parameters reachable from
    // Tuning::kind can be written on the audio thread without any refcounting; this
    // pointer-swap mechanism is untouched and still the only way to run a CUSTOM policy
    // (one of the four owned instances is not what you want) from a tool or test.
    //
    // PRECEDENCE: calling this with a non-null pointer marks a custom override as active,
    // and applyTuning() will not touch blend_ again until this is called with nullptr,
    // which hands control back to Tuning::kind's enum-driven selection. Every existing
    // caller (the offline tools, the test suite) calls this exactly once and never calls
    // applyTuning() at all, so nothing about today's behaviour changes: the override, once
    // set, simply has nothing to compete with.
    void setBlendPolicy(const IBlendPolicy* policy) noexcept {
        blend_ = policy;
        customBlendPolicy_ = (policy != nullptr);
    }
    const IBlendPolicy* blendPolicy() const noexcept { return blend_; }

    void setPreservation(const PreservationSpec& p) noexcept { cfg_.preservation = p; }
    void setRatioSource(RatioSource s) noexcept { cfg_.ratioSource = s; }

    // Mode B's declared root. Read every block (ratioForVoice()) but, before this, only
    // settable at prepare() — app-design.md §4.3 names this the missing setter.
    void setRootMidiNote(int n) noexcept { cfg_.rootMidiNote = n; }

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
    //
    // RESOLVED (app-design.md §4.5 item 5 / §5's Track C item): a previous track correctly
    // declined to ramp this, because every existing tool sets alignment EXACTLY ONCE,
    // before any audio flows (tools/render.cpp, tools/demo.cpp, tools/sweep.cpp,
    // tools/trace.cpp all call this immediately after prepare()), and ramping from zero
    // unconditionally would have silently changed the first ~50 ms of every render
    // RESULTS.md depends on while the ramp caught up.
    //
    // The resolution: SNAP when the engine has not yet processed a block since prepare()
    // (or this is the first call to set alignment after prepare()), and RAMP only on
    // subsequent changes made while audio is already flowing. Every existing tool's single
    // pre-audio call takes the snap branch — bit-identical to the old step-only behaviour —
    // and a UI slider moved mid-performance now ramps over ~50 ms instead of jumping the
    // delay tap. See mixImpl() in the .cpp for the ramp itself and prepare() for where the
    // "not yet processed" flag resets. CERTAIN that the split is right; the ~50 ms ramp
    // TIME is PROVISIONAL, same status as every other un-auditioned millisecond constant in
    // this codebase.
    void setBlendAlignment(float amount) noexcept { setAlignmentTarget(std::clamp(amount, 0.0f, 1.0f)); }
    float blendAlignment() const noexcept { return alignAmount_; }

    // Audio thread, non-allocating, called from the app's audio callback after
    // tuningBus.poll() returns something new (app-design.md §4.3/§6.1). The Engine does
    // NOT own a TuningBus — the app owns it and hands snapshots in here; keeping the bus
    // out of core means core stays exercisable by the exact same call pattern whether the
    // caller is the JUCE app, a tool, or a test.
    //
    // Clamps its own COPY of `t` first (never the caller's) — app-design.md §4.5's closing
    // rule, "applyTuning() clamps everything it receives", because a profile is
    // hand-editable JSON and a UI widget's min/max is not a defence against that. Then fans
    // out: analyzer_->setTuning(), every shifter's setTuning() (fast and quality, every
    // voice slot — shifters carry no per-voice tuning, so one clamped ShifterTuning serves
    // all of them), the four owned blend policies' parameters, and Engine's own fields.
    //
    // setPreservation()/setRatioSource()/setBlendAlignment()/setBlendPolicy() all keep
    // working unchanged for tools and tests that never touch Tuning at all — applyTuning()
    // is an alternative entry point into the same state, not a replacement for those.
    void applyTuning(const Tuning& t) noexcept;

    // Introspection for tests, the offline harness, and the UI meters.
    const AudioRing& ring() const noexcept { return *ring_; }
    const AnalysisFrame& analysis() const noexcept;
    int activeVoiceCount() const noexcept;

    // --- latency, split per BUGS.md VH-012 ------------------------------------------------
    //
    // The single function this used to be conflated two different questions: "how far
    // behind the live edge is a locked, running voice" (steady state) and "how long before
    // Mode A can produce a trustworthy ratio AT ALL" (acquisition, Mode A only — Mode B
    // needs no F0 so pays none of this). Summing them into one FrameCount, as the original
    // latencySamples() does, answers neither question correctly: it over-reports Mode A's
    // steady-state figure by YIN's whole analysis window (~28.6 ms at the 70 Hz floor)
    // because that window is a CONVERGENCE cost paid once when the tracker locks, not a
    // per-block tax — docs/latency-budget.md calls conflating these two "the single most
    // important thing in this document and the one most often got wrong".
    //
    // steadyStateLatencySamples(): max(fast, quality) + decorrelation delay. Correct for
    // BOTH modes once the analyser (if Mode A) has locked — this is "how far behind the
    // live edge is the output right now", the number a host would want for delay
    // compensation once a voice is actually running.
    //
    // acquisitionLatencySamples(): steadyStateLatencySamples() plus YIN's analysis window,
    // for Mode A only (zero added for Mode B — "this asymmetry is the whole latency
    // argument for Mode B", per the .cpp). This is "how long before Mode A can be trusted
    // at all", paid once per pitch change, not once per block.
    //
    // latencySamples() is KEPT, UNCHANGED, equal to acquisitionLatencySamples() — this is
    // the worst-case figure the original function always returned, so every existing
    // caller (offline/main.cpp, app/src/EngineAudioCallback.cpp, tests/test_engine.cpp)
    // keeps compiling and keeps seeing the exact number it saw before this change. It is
    // not deprecated in the sense of being wrong; it answers a real question (worst case
    // for delay compensation) honestly, per the counter-argument in BUGS.md VH-012 — it
    // was only ever mislabelled by being the ONLY number offered. New callers should reach
    // for the two named accessors and print both, labelled, never summed.
    FrameCount latencySamples() const noexcept;
    FrameCount steadyStateLatencySamples() const noexcept;
    FrameCount acquisitionLatencySamples() const noexcept;

private:
    int allocateVoice(int midiNote) noexcept;
    double ratioForVoice(const Voice& v, const AnalysisFrame& a) const noexcept;
    const IBlendPolicy* policyForKind(BlendKind k) noexcept;

    // Shared body of both setBlendAlignment() and applyTuning()'s alignment field, so the
    // snap-vs-ramp decision (app-design.md §4.5 item 5) is made in exactly one place rather
    // than reimplemented at both call sites and risking them drifting apart.
    void setAlignmentTarget(float clamped01) noexcept;

    // The templated mix loop itself (app-design.md §5.4). Defined in engine.cpp and
    // explicitly instantiated there for Stereo = false/true — kept out of this header so
    // core's implementation stays in .cpp files like everywhere else, and because only
    // these two instantiations will ever exist.
    template <bool Stereo>
    void mixImpl(const Sample* in, Sample* outL, Sample* outR, FrameCount numFrames) noexcept;

    EngineConfig cfg_{};

    // Constructed in prepare(), not here — see EngineConfig::inputHistoryFrames above.
    // unique_ptr rather than a value member: AudioRing holds a std::atomic<Pos>, which is
    // neither copy- nor move-assignable, so "reconstruct in place" needs either placement
    // new or a level of indirection. A unique_ptr matches how analyzer_ and every shifter
    // below are already handled — allocated once in prepare(), left alone after — rather
    // than introducing a different ownership idiom for one member.
    std::unique_ptr<AudioRing> ring_;
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

    // --- input/output stage scratch and state (app-design.md §5.5, Track D) -----------
    //
    // inputStageBuf_: the trimmed-then-gated input signal for the current block. Written
    // once at the top of mixImpl(), then read TWICE -- once to feed the ring/analyser
    // (replacing the raw `in` pointer there) and once as the DRY signal for the output
    // mix at the bottom of mixImpl(). Sized in prepare(), never resized on the audio
    // thread, same pattern as fastBuf_/qualityBuf_ immediately above.
    std::vector<Sample> inputStageBuf_;

    // gateEnv_: the input gate's one-pole envelope-follower state, in LINEAR amplitude.
    // Carried across blocks so the gate's attack/release times are true wall-clock
    // times rather than resetting every 128 samples, and updated EVERY block regardless
    // of Tuning::inputGateOn (see mixImpl()) so that enabling the gate mid-performance
    // does not open or close against a value computed from audio the engine saw before
    // the gate was ever turned on.
    float gateEnv_ = 0.0f;

    // peakEnv_: the limiter's PEAK-HOLD detector, in linear amplitude. Rises instantly to
    // a new true peak (no lookahead -- it cannot know one is coming, so it must react the
    // instant it arrives) and decays at the RELEASE time constant otherwise. This is
    // deliberately NOT the raw instantaneous |sample| -- see limiterGain_'s comment for
    // why an undamped instantaneous value cannot drive a working limiter.
    float peakEnv_ = 0.0f;

    // limiterGain_: the limiter's smoothed gain-reduction state, 1.0 == no reduction.
    // Same "always track, only apply when enabled" reasoning as gateEnv_ above, for the
    // same reason: toggling Tuning::limiterOn on mid-performance must not multiply by a
    // reduction value computed from signal levels seen minutes ago while it was off.
    //
    // WHY TWO STAGES (peakEnv_ THEN limiterGain_), not one: an early version of this
    // computed the gain-reduction TARGET directly from the raw instantaneous |sample|
    // and smoothed only that. On real (periodic) audio the instantaneous sample sits at
    // its true crest for a vanishingly small fraction of each cycle and is well below
    // the ceiling the rest of the time -- so the target snapped back to "no reduction"
    // almost immediately after each peak, and a 1 ms-attack smoother chasing a target
    // that vanishes in under a millisecond never converges: measured on a three-voice
    // upward-shifted chord (tests/test_output_stage.cpp's VH-009 case), the achieved
    // "steady-state" peak sat at roughly half the required reduction, indefinitely, not
    // just during the note-on transient. peakEnv_'s slow release HOLDS the true crest
    // value across the quiet part of each cycle, giving limiterGain_ a stable target to
    // actually converge onto over several cycles instead of one that reappears and
    // vanishes faster than the smoother can track it.
    float limiterGain_ = 1.0f;

    // Per-voice delay line used to time-align the fast path against the quality path.
    // Flat storage with a stride rather than a vector-of-vectors: one allocation, and the
    // audio thread never touches a pointer it did not already have.
    std::vector<Sample> alignBuf_;
    FrameCount alignStride_ = 0;
    std::vector<FrameCount> alignWrite_;
    float alignAmount_ = 0.0f;

    // --- alignAmount_ snap-vs-ramp state (app-design.md §4.5 item 5) -------------------
    //
    // alignTarget_: what the last setBlendAlignment()/applyTuning() call actually asked
    // for. alignAmount_ (above) is what mixImpl() currently applies, and the two only
    // differ mid-ramp.
    float alignTarget_ = 0.0f;

    // everProcessed_: true once mixImpl() has run at least once since the most recent
    // prepare(). Reset to false BY prepare() itself. "The engine is not yet running" in
    // setBlendAlignment()'s doc comment means this is still false.
    bool everProcessed_ = false;

    // alignPrimed_: true once setAlignmentTarget() has been called at least once since the
    // most recent prepare(). Reset to false by prepare(). Ensures the FIRST call after a
    // (re-)prepare() always snaps even if, unusually, a block has already been processed —
    // "the first application after prepare()" in setBlendAlignment()'s doc comment.
    bool alignPrimed_ = false;

    // Per-voice decorrelation delay lines. See Humanization::staticDelayMs and BUGS.md
    // VH-005: N voices passing the same fricative through unshifted sum coherently, and a
    // few milliseconds of per-voice offset is what turns that back into an ensemble.
    std::vector<Sample> decorBuf_;
    std::vector<FrameCount> decorWrite_;
    FrameCount decorStride_ = 0;

    const IBlendPolicy* blend_ = nullptr;
    FastOnlyPolicy defaultBlend_{};   // used until someone sets a real one OR calls
                                       // applyTuning() — kept exactly as it was, since a
                                       // caller that never touches Tuning must see no change.

    // --- blend-policy ownership, app-design.md §4.4 ---------------------------------------
    //
    // ONE INSTANCE OF EACH concrete policy, owned here rather than by the caller. Selected
    // by Tuning::kind in applyTuning() via policyForKind(); its PARAMETERS (crossoverHz,
    // widthOctaves, handoverStartMs, handoverEndMs) are written into these instances by
    // applyTuning() too, so a slider bound to Tuning::rsCrossoverHz reaches this object
    // without any pointer indirection the audio thread would need to chase past what it
    // already had. This is the deviation engine.hpp's setBlendPolicy() comment (above)
    // documents: ownership moved off the caller for THESE FOUR, specifically so their
    // parameters can be Tuning-driven; setBlendPolicy() is unchanged for anything else.
    FastOnlyPolicy tuningFastOnly_{};
    QualityOnlyPolicy tuningQualityOnly_{};
    RegisterSplitPolicy tuningRegisterSplit_{};
    OnsetHandoverPolicy tuningOnsetHandover_{};

    // True once setBlendPolicy() has been called with a non-null pointer. While true,
    // applyTuning() leaves blend_ alone — a custom policy takes precedence over the enum,
    // per app-design.md §4.4 — until setBlendPolicy(nullptr) hands control back.
    bool customBlendPolicy_ = false;

    // hold_/freeze_ replace the old single `sustain_` bool (app-design.md §5.1). See
    // setHold()/setFreeze() above and Voice::holdDeferred/freezeLatched for the per-voice
    // half of the bookkeeping.
    bool hold_ = false;
    bool freeze_ = false;

    bool prepared_ = false;
};

} // namespace vh
