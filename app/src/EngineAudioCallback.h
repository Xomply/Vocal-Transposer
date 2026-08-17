// app/src/EngineAudioCallback.h — the ENTIRE seam between real audio hardware and
// vh::Engine. Per app/README.md: "the audio callback does exactly three things: pump the
// MIDI queue into Engine::noteOn/noteOff/setSustain, call Engine::process, and nothing
// else." Everything in this class exists to make that literally true — no DSP, no voice
// logic, no parameter smoothing lives here; every one of those belongs in core.
#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include "vh/engine.hpp"
#include "vh/tuning_bus.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <vector>

namespace vhapp {

class MidiRouter; // full definition only needed in the .cpp, where drainAll() is called.

class EngineAudioCallback final : public juce::AudioIODeviceCallback {
public:
    // tuningBus: owned by TuningStore (app-design.md §4.3 -- the Engine itself does not
    // own a TuningBus, see engine.hpp's applyTuning() comment). Polled once per block, at
    // the exact spot the old skeleton left a comment marking for this — see the .cpp.
    EngineAudioCallback(MidiRouter& midi, vh::TuningBus& tuningBus);

    // ---- JUCE calls these on the audio thread ----
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                          int numInputChannels,
                                          float* const* outputChannelData,
                                          int numOutputChannels,
                                          int numSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override;

    // ---- UI thread: control (lock-free writes, read by the audio thread) ----
    void requestPanic() noexcept { panicRequested_.store(true, std::memory_order_release); }

    // `logicalIndex` indexes into the list handed to onInputChannelsChanged below — i.e.
    // position within the device's ACTIVE input channels, not a raw device channel number.
    void setInputChannel(int logicalIndex) noexcept {
        selectedInputChannel_.store(logicalIndex, std::memory_order_release);
    }
    int inputChannel() const noexcept { return selectedInputChannel_.load(std::memory_order_acquire); }

    // Fired (via juce::MessageManager::callAsync — see the .cpp) whenever the device
    // (re)starts with a possibly different set of active input channels. Index N in the
    // given list is exactly what inputChannelData[N] will be in the callback, because both
    // are built by walking AudioIODevice::getActiveInputChannels() the same way.
    std::function<void(juce::StringArray)> onInputChannelsChanged;

    // ---- UI thread: meters. Every one of these is a relaxed load of a value the audio
    // thread published; none of them touch vh::Engine directly (the UI thread must never
    // call into the Engine — it is not thread-safe against the audio thread doing so). ----
    float  inputPeakHold() const noexcept { return inputPeakHold_.load(std::memory_order_relaxed); }
    bool   consumeClipFlag() noexcept { return inputClipped_.exchange(false, std::memory_order_relaxed); }
    float  f0Hz() const noexcept { return f0Hz_.load(std::memory_order_relaxed); }
    bool   voiced() const noexcept { return voiced_.load(std::memory_order_relaxed); }
    int    activeVoices() const noexcept { return activeVoices_.load(std::memory_order_relaxed); }

    // VH-012 (BUGS.md, now fixed core-side per docs/app-design.md §13 item 2): Engine
    // exposes TWO real latency figures, never one sum -- steadyStateLatencySamples() ("how
    // far behind the live edge is a locked, running voice") and acquisitionLatencySamples()
    // ("how long before Mode A can be trusted at all", zero extra for Mode B). Reported here
    // as two separate atomics for the same reason; PerformPanel.cpp displays them as two
    // labelled numbers and must never add them together.
    int    engineSteadyStateLatencySamples() const noexcept { return engineSteadyStateLatencySamples_.load(std::memory_order_relaxed); }
    int    engineAcquisitionLatencySamples() const noexcept { return engineAcquisitionLatencySamples_.load(std::memory_order_relaxed); }

    double sampleRate() const noexcept { return sampleRate_.load(std::memory_order_relaxed); }
    int    deviceInputLatencySamples() const noexcept { return deviceInputLatencySamples_.load(std::memory_order_relaxed); }
    int    deviceOutputLatencySamples() const noexcept { return deviceOutputLatencySamples_.load(std::memory_order_relaxed); }
    bool   isRunning() const noexcept { return running_.load(std::memory_order_relaxed); }

    // ---- UI thread: a best-effort position for the curve displays' live dot -----------
    // (docs/app-design.md §6.3). Engine has no per-voice introspection to query "which
    // voice is loudest right now" -- see LiveVoiceEstimate.h for the honest caveat this
    // carries. lastNoteOn() is the most recently triggered MIDI note (-1 if none / after a
    // panic); msSinceLastNoteOn() is wall-clock time since that noteOn was drained, at
    // roughly one audio-block's resolution (a few ms), which is more than adequate for a
    // UI plot's dot position.
    int    lastNoteOn() const noexcept { return lastNoteOn_.load(std::memory_order_relaxed); }
    double msSinceLastNoteOn() const noexcept {
        const double sr = sampleRate();
        const auto samples = samplesSinceLastNoteOn_.load(std::memory_order_relaxed);
        return sr > 0.0 ? 1000.0 * static_cast<double>(samples) / sr : 0.0;
    }

private:
    // Decoupled from the device's own buffer size on purpose — see the .cpp for the trap
    // this avoids (Engine::process silently returns silence above maxBlock).
    static constexpr vh::FrameCount kEngineMaxBlock = 512;

    MidiRouter& midi_;
    vh::TuningBus& tuningBus_;

    vh::Engine engine_;
    bool prepared_ = false;

    // Scratch, sized once in audioDeviceAboutToStart (the "prepare" call — allocation is
    // allowed there, exactly as core's own prepare()/process() split requires). Never
    // resized in the callback.
    std::vector<vh::Sample> monoScratch_;
    std::vector<vh::Sample> silenceIn_;

    // Audio-thread-only, set once per device start, read every block for the meter's
    // hold-time calculation — kept separate from the UI-facing sampleRate_ atomic below so
    // the hot path never touches an atomic it doesn't need to.
    double meterSampleRate_ = 48000.0;
    float  peakHoldValue_ = 0.0f;
    int    peakHoldSamplesSincePeak_ = 0;

    // One bit per MIDI note (0-127), so the panic button can turn off exactly the notes
    // THIS instance turned on. vh::Engine exposes activeVoiceCount() but not a "list active
    // notes" query, so the app tracks this itself from the same MIDI events it is about to
    // feed the engine anyway. Written and read only by the audio thread.
    std::array<std::atomic<bool>, 128> noteActive_{};
    std::atomic<bool> panicRequested_{false};

    std::atomic<int> selectedInputChannel_{0};

    // Meters. Only atomics are written here — see docs/app-design.md §6.1 ("meters cross
    // threads... the audio callback may only write to std::atomics") and app/README.md.
    std::atomic<float>  inputPeakHold_{0.0f};
    std::atomic<bool>   inputClipped_{false};
    std::atomic<float>  f0Hz_{0.0f};
    std::atomic<bool>   voiced_{false};
    std::atomic<int>    activeVoices_{0};
    std::atomic<int>    engineSteadyStateLatencySamples_{0};
    std::atomic<int>    engineAcquisitionLatencySamples_{0};
    std::atomic<double> sampleRate_{48000.0};
    std::atomic<int>    deviceInputLatencySamples_{0};
    std::atomic<int>    deviceOutputLatencySamples_{0};
    std::atomic<bool>   running_{false};

    // Live-dot position estimate (see accessors above / LiveVoiceEstimate.h).
    std::atomic<int>         lastNoteOn_{-1};
    std::atomic<std::int64_t> samplesSinceLastNoteOn_{0};
};

} // namespace vhapp
