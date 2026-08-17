// app/src/MidiRouter.h — merges N hardware MIDI inputs plus the on-screen keyboard into a
// fixed set of lock-free channels the audio thread can drain without ever blocking.
//
// docs/app-design.md §8: "Multiple MIDI devices open simultaneously and merged, so the
// laptop keyboard and a hardware keyboard are both live without a mode switch." This class
// is that merge.
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_devices/juce_audio_devices.h>

#include "MidiEventFifo.h"

#include <array>
#include <atomic>
#include <memory>

namespace vhapp {

class MidiRouter final : private juce::MidiKeyboardState::Listener {
public:
    // PROVISIONAL cap: a fixed-size array of MidiEventFifo, sized generously for a
    // performance rig (a hardware keyboard, maybe a pedal/controller, maybe a second
    // keyboard) rather than for a MIDI patchbay. Raising it only grows a fixed array — none
    // of this reallocates at runtime, which is the point.
    static constexpr int kMaxHardwareSources = 8;
    static constexpr int kTotalSources = kMaxHardwareSources + 1; // +1 = on-screen keyboard
    static constexpr int kKeyboardSlot = kMaxHardwareSources;

    explicit MidiRouter(juce::AudioDeviceManager& deviceManager);
    ~MidiRouter() override;

    MidiRouter(const MidiRouter&) = delete;
    MidiRouter& operator=(const MidiRouter&) = delete;

    // UI/message thread only. Re-syncs the fixed hardware slots against whichever MIDI
    // input devices AudioDeviceManager currently has enabled (via setMidiInputDeviceEnabled,
    // e.g. from AudioDeviceSelectorComponent's own MIDI checkbox list in DevicesPanel).
    //
    // WHY POLLED FROM A UI TIMER RATHER THAN A juce::ChangeListener CALLBACK: it makes
    // correctness independent of exactly when, or whether, AudioDeviceManager broadcasts a
    // change for MIDI enablement specifically (not verified against the 9.0.1 source for
    // this exact call path). The cost is a worst-case few-hundred-ms delay picking up a
    // newly (un)checked device, which is irrelevant for a settings panel.
    void refresh();

    juce::MidiKeyboardState& keyboardState() noexcept { return keyboardState_; }

    // Audio thread only. Calls fn(const MidiEvent&) once per queued event, across every
    // currently-active source, hardware slots first then the on-screen keyboard. Order is
    // NOT a single merged timeline across devices — see MidiEventFifo.h — which is fine at
    // the granularity this app needs (note on/off, CC64) but would matter for anything
    // sample-accurate.
    template <typename Fn>
    void drainAll(Fn&& fn) noexcept {
        for (auto& slot : slots_)
            if (slot.active.load(std::memory_order_acquire))
                slot.fifo.drain(fn);
    }

private:
    // One juce::MidiInputCallback per hardware slot, each closed over just that slot's own
    // fifo so push() is genuinely single-producer (that device's own driver thread — see
    // MidiInputCallback::handleIncomingMidiMessage's doc comment: "a high-priority system
    // thread").
    class DeviceCallback final : public juce::MidiInputCallback {
    public:
        explicit DeviceCallback(MidiEventFifo& fifo) noexcept : fifo_(fifo) {}
        void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& m) override;

    private:
        MidiEventFifo& fifo_;
    };

    struct Slot {
        MidiEventFifo fifo;
        std::atomic<bool> active{false};
        juce::String deviceIdentifier;             // message thread only
        std::unique_ptr<DeviceCallback> callback;   // message thread only
    };

    void handleNoteOn(juce::MidiKeyboardState*, int midiChannel, int midiNoteNumber, float velocity) override;
    void handleNoteOff(juce::MidiKeyboardState*, int midiChannel, int midiNoteNumber, float velocity) override;

    juce::AudioDeviceManager& deviceManager_;
    std::array<Slot, kTotalSources> slots_;
    juce::MidiKeyboardState keyboardState_;
};

} // namespace vhapp
