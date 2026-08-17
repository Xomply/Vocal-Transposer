#include "EngineAudioCallback.h"

#include "MidiEventFifo.h"
#include "MidiRouter.h"

#include "vh/granular_shifter.hpp"
#include "vh/psola_shifter.hpp"
#include "vh/yin.hpp"

#include <juce_events/juce_events.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace vhapp {
namespace {

// RT-safety guarantee, checked at compile time rather than trusted from documentation —
// matching the spirit of core/include/vh/rt.hpp's "make the rule testable" philosophy.
// If any of these ever fail on a target platform, the meter/control atomics below would
// silently start taking a lock inside the audio callback, which is exactly what this whole
// file exists to prevent.
static_assert(std::atomic<float>::is_always_lock_free, "meter atomics must be lock-free");
static_assert(std::atomic<int>::is_always_lock_free, "meter atomics must be lock-free");
static_assert(std::atomic<bool>::is_always_lock_free, "meter atomics must be lock-free");

// CC64 (Damper/Sustain) -> Engine::setHold. Not configurable: this is the one CC number
// with a fixed, universal meaning in the MIDI spec, and every keyboard's own pedal jack
// sends it, so remapping it would only create a way to accidentally break the pedal.
constexpr int kSustainCC = 64;

// CC66 (Sostenuto) -> Engine::setFreeze, as a DEFAULT rather than a fixed mapping — see the
// comment at the call site for why 66 was chosen and why this is expected to become
// reassignable once a MIDI-learn surface exists (docs/app-design.md §8).
constexpr int kFreezeCC = 66;

} // namespace

EngineAudioCallback::EngineAudioCallback(MidiRouter& midi, vh::TuningBus& tuningBus)
    : midi_(midi), tuningBus_(tuningBus) {
    // Explicit rather than relying on std::atomic<bool>'s default constructor to
    // value-initialise (that only became guaranteed behaviour in C++20 — this project does
    // build as C++20, but there is no reason to lean on the subtlety here).
    for (auto& b : noteActive_) b.store(false, std::memory_order_relaxed);
}

void EngineAudioCallback::audioDeviceAboutToStart(juce::AudioIODevice* device) {
    // NOT the per-block callback. This is JUCE's "about to start" call — the audio
    // equivalent of vh::Engine's own prepare(), and allocation/reconfiguration belongs
    // here, never in audioDeviceIOCallbackWithContext below.
    const double sr = device->getCurrentSampleRate();

    vh::EngineConfig cfg{};
    cfg.sampleRate = sr;
    cfg.maxBlock = kEngineMaxBlock;
    // RatioSource (Mode A vs Mode B) and the Mode B root note are live-tunable parameters
    // that belong to Track G's parameter surface (Tuning/TuningBus, docs/app-design.md
    // §4.2-4.3), not to this shell. Left at EngineConfig's own defaults
    // (RatioSource::AbsoluteTarget, rootMidiNote 60).

    auto makeFast = []() -> std::unique_ptr<vh::IPitchShifter> {
        return std::make_unique<vh::GranularShifter>();
    };
    auto makeQuality = []() -> std::unique_ptr<vh::IPitchShifter> {
        return std::make_unique<vh::PsolaShifter>();
    };

    // Exactly tools/render.cpp's construction pattern: YIN analyser, granular as the fast
    // engine, PSOLA as the quality engine. The blend policy is left at Engine's own default
    // (FastOnlyPolicy, wired up in the Engine constructor) — this track changes no DSP
    // behaviour, only how the Engine is driven from real hardware.
    engine_.prepare(cfg, std::make_unique<vh::YinAnalyzer>(), makeFast, makeQuality);
    prepared_ = true;

    monoScratch_.assign(kEngineMaxBlock, vh::Sample{0});
    silenceIn_.assign(kEngineMaxBlock, vh::Sample{0});

    meterSampleRate_ = sr;
    peakHoldValue_ = 0.0f;
    peakHoldSamplesSincePeak_ = 0;

    sampleRate_.store(sr, std::memory_order_relaxed);

    // Device I/O latency, per docs/app-design.md §6.2: display these AS REPORTED, labelled
    // as what they are (JUCE's own figures, not a loopback measurement — that is Track H's
    // bench feature, §9 item 4). The recon report backing this design is explicit that
    // these numbers are trustworthy under ASIO/WASAPI-exclusive and closer to an averaged
    // guess under DirectSound (straight from JUCE's original author, forum-cited in the
    // recon). We surface them unmodified either way and let the label say what they are.
    deviceInputLatencySamples_.store(device->getInputLatencyInSamples(), std::memory_order_relaxed);
    deviceOutputLatencySamples_.store(device->getOutputLatencyInSamples(), std::memory_order_relaxed);

    // Logical input-channel index -> device channel name, in delivery order. Index N here
    // is exactly inputChannelData[N] in the callback below, because both are built by
    // walking the same getActiveInputChannels() bit set in the same order. This is how
    // "which physical input is the mic" (docs/app-design.md's Scarlett-2i4 example) gets
    // answered without this shell needing to second-guess JUCE's channel delivery order.
    juce::StringArray names;
    const auto active = device->getActiveInputChannels();
    const auto allNames = device->getInputChannelNames();
    for (int ch = 0; ch <= active.getHighestBit(); ++ch)
        if (active[ch] && ch < allNames.size())
            names.add(allNames[ch]);

    const int clamped = names.isEmpty() ? 0
                                        : juce::jlimit(0, names.size() - 1,
                                                       selectedInputChannel_.load(std::memory_order_relaxed));
    selectedInputChannel_.store(clamped, std::memory_order_relaxed);

    if (onInputChannelsChanged) {
        auto callback = onInputChannelsChanged;
        // audioDeviceAboutToStart is not guaranteed to run on the message thread, and
        // touching UI components off it is not safe — hand the result to the UI via the
        // ordinary JUCE mechanism for that.
        juce::MessageManager::callAsync([callback, names]() { callback(names); });
    }

    running_.store(true, std::memory_order_relaxed);
}

void EngineAudioCallback::audioDeviceStopped() {
    running_.store(false, std::memory_order_relaxed);
}

void EngineAudioCallback::audioDeviceIOCallbackWithContext(
    const float* const* inputChannelData, int numInputChannels,
    float* const* outputChannelData, int numOutputChannels,
    int numSamples, const juce::AudioIODeviceCallbackContext&) {
    // THE AUDIO CALLBACK. Per app/README.md this does exactly three things — pump MIDI,
    // call Engine::process, nothing else — plus the meter bookkeeping docs/app-design.md
    // §6.1 assigns to this same thread. No allocation, no locks, no I/O below this line.

    // (1) FTZ/DAZ for the duration of this block. docs/app-design.md §6.1 and the root
    // CMakeLists.txt (see its comment on why -ffast-math is deliberately NOT set) both call
    // this out: denormal handling must be explicit here rather than assumed from a compiler
    // flag.
    juce::ScopedNoDenormals noDenormals;

    if (!prepared_) {
        for (int ch = 0; ch < numOutputChannels; ++ch)
            if (outputChannelData[ch] != nullptr)
                juce::FloatVectorOperations::clear(outputChannelData[ch], numSamples);
        return;
    }

    // (2) Pump the MIDI queue into noteOn/noteOff/setHold/setFreeze. See MidiRouter.h for
    // why this is several independent SPSC fifos rather than one shared queue, and
    // MidiEventFifo.h for why that beats juce::MidiMessageCollector here.
    midi_.drainAll([this](const MidiEvent& e) {
        switch (e.kind) {
            case MidiEvent::Kind::NoteOn:
                engine_.noteOn(e.number, static_cast<float>(e.value) / 127.0f);
                noteActive_[e.number].store(true, std::memory_order_relaxed);
                // Live-dot bookkeeping for the curve displays (app-design.md §6.3) — see
                // LiveVoiceEstimate.h for why this is the best approximation available
                // without per-voice introspection on Engine. Block-resolution, not
                // sample-accurate: reset to 0 here, accumulated by numSamples once per
                // callback below.
                lastNoteOn_.store(e.number, std::memory_order_relaxed);
                samplesSinceLastNoteOn_.store(0, std::memory_order_relaxed);
                break;
            case MidiEvent::Kind::NoteOff:
                engine_.noteOff(e.number);
                noteActive_[e.number].store(false, std::memory_order_relaxed);
                break;
            case MidiEvent::Kind::ControlChange:
                // HOLD and FREEZE are two controls now, not one — see
                // docs/app-design.md §5.1 and Engine::setHold/setFreeze. CC64 gets
                // CONVENTIONAL SUSTAIN, which is what a keyboard player's hands expect
                // from that pedal and what this app previously got wrong by routing CC64
                // to the old setSustain (which froze rather than sustained).
                if (e.number == kSustainCC) {
                    engine_.setHold(e.value >= 64);
                } else if (e.number == kFreezeCC) {
                    // PROVISIONAL default, and the reason for this particular number:
                    // CC66 is Sostenuto in the MIDI CC assignment, i.e. "hold what is
                    // already sounding" — semantically the closest standard controller to
                    // freeze, which latches sounding voices and stalls their cursors into
                    // a captured loop. §8 specifies this as ASSIGNABLE, so this is only a
                    // default; a MIDI-learn surface belongs to the parameter-UI track.
                    engine_.setFreeze(e.value >= 64);
                }
                break;
        }
    });

    // Panic. Checked after the drain above so a note-on/off pair that arrived in the same
    // block is not stepped on.
    //
    // Uses Engine::allNotesOff() rather than walking noteActive_ and sending 128 note-offs.
    // WHY THAT MATTERS BEYOND TIDINESS: the Engine is the only thing that knows what is
    // actually sounding. A voice stolen under polyphony pressure, or one still in Releasing
    // after its key came up, is invisible to this shell's bitset — so a shell-side panic
    // could leave exactly the voice you were panicking about still running. Deciding what
    // is sounding is voice logic, and app/README.md is explicit that voice logic does not
    // live in the shell.
    if (panicRequested_.exchange(false, std::memory_order_acq_rel)) {
        engine_.allNotesOff();
        for (auto& bit : noteActive_) bit.store(false, std::memory_order_relaxed);
        lastNoteOn_.store(-1, std::memory_order_relaxed);   // hide the curve displays' dot
    }

    // Track G: the parameter surface. tuningBus_ is UI-thread-written (TuningStore::
    // publish(), on every widget edit) and audio-thread-read here, exactly once per block,
    // per docs/app-design.md §6.1/§4.3. poll() returns nullptr on the steady-state path
    // (most blocks — nobody is turning a knob right now) at the cost of one acquire-load;
    // applyTuning() is only called when something actually changed, and it clamps its OWN
    // copy of what it receives (app-design.md §4.5's closing rule) rather than trusting
    // whatever TuningStore last published.
    if (const vh::Tuning* t = tuningBus_.poll()) {
        engine_.applyTuning(*t);
    }

    const int inIdx = selectedInputChannel_.load(std::memory_order_relaxed);
    const float* inChan =
        (inIdx >= 0 && inIdx < numInputChannels && inputChannelData[inIdx] != nullptr)
            ? inputChannelData[inIdx]
            : nullptr;

    // (3) Engine::process(), chunked. core/src/engine.cpp:230: Engine::process() returns
    // SILENCE (writes zeros, does no processing at all) if numFrames > maxBlock. This is a
    // real trap here specifically: ASIO always hands back a fixed buffer matching what was
    // negotiated, but WASAPI shared mode does not guarantee that, and the device's block
    // can legitimately be larger than the kEngineMaxBlock the Engine was prepared with. So
    // the device's block is walked in <= kEngineMaxBlock pieces rather than trusted to fit
    // in one call.
    int offset = 0;
    float chunkPeak = 0.0f;
    while (offset < numSamples) {
        const int chunk = std::min<int>(static_cast<int>(kEngineMaxBlock), numSamples - offset);
        const float* inPtr = inChan ? (inChan + offset) : silenceIn_.data();

        engine_.process(inPtr, monoScratch_.data(), static_cast<vh::FrameCount>(chunk));

        for (int i = 0; i < chunk; ++i)
            chunkPeak = std::max(chunkPeak, std::fabs(inPtr[i]));

        for (int ch = 0; ch < numOutputChannels; ++ch) {
            if (outputChannelData[ch] == nullptr) continue;
            if (ch < 2) {
                // Mono in, mono out "for now" (docs/app-design.md §2b — types.hpp already
                // predicts stereo output arriving for per-voice panning; it has not yet).
                // This shell duplicates the single wet signal to both output channels
                // rather than deciding anything about panning, which is core's job
                // (§5.3), not this file's.
                std::memcpy(outputChannelData[ch] + offset, monoScratch_.data(),
                            sizeof(float) * static_cast<std::size_t>(chunk));
            } else {
                juce::FloatVectorOperations::clear(outputChannelData[ch] + offset, chunk);
            }
        }

        offset += chunk;
    }

    // (4) Meters: atomics only, written here, read by a UI timer. No allocation, no locks,
    // nothing that could block — docs/app-design.md §6.1's "meters cross threads" rule.
    if (chunkPeak >= peakHoldValue_) {
        peakHoldValue_ = chunkPeak;
        peakHoldSamplesSincePeak_ = 0;
    } else {
        peakHoldSamplesSincePeak_ += numSamples;
        // ~1.5 s hold, then a slow decay. PROVISIONAL timing — a UI nicety, not something
        // any measurement in RESULTS.md depends on.
        if (peakHoldSamplesSincePeak_ > static_cast<int>(meterSampleRate_ * 1.5))
            peakHoldValue_ *= 0.98f;
    }
    inputPeakHold_.store(peakHoldValue_, std::memory_order_relaxed);
    if (chunkPeak >= 0.999f) inputClipped_.store(true, std::memory_order_relaxed);

    const vh::AnalysisFrame& a = engine_.analysis();
    f0Hz_.store(a.f0Hz, std::memory_order_relaxed);
    voiced_.store(a.voicing == vh::Voicing::Voiced, std::memory_order_relaxed);
    activeVoices_.store(engine_.activeVoiceCount(), std::memory_order_relaxed);

    // VH-012 (BUGS.md), now fixed core-side (docs/app-design.md §13 item 2): the old single
    // latencySamples() summed YIN's analysis window into the shifters' transport delay for
    // Mode A, conflating an estimator's convergence cost with an actual hold-up. Engine now
    // exposes the two real quantities separately; report both, sum neither. See offline/
    // main.cpp for the same two figures printed by the offline tool, for consistency.
    engineSteadyStateLatencySamples_.store(static_cast<int>(engine_.steadyStateLatencySamples()),
                                            std::memory_order_relaxed);
    engineAcquisitionLatencySamples_.store(static_cast<int>(engine_.acquisitionLatencySamples()),
                                            std::memory_order_relaxed);

    // Live-dot bookkeeping (see the NoteOn case above): block-resolution wall-clock time
    // since the most recent noteOn was drained. Advanced by the FULL device block
    // (numSamples), not per-chunk, since the dot only needs a few-ms resolution.
    samplesSinceLastNoteOn_.fetch_add(numSamples, std::memory_order_relaxed);
}

} // namespace vhapp
