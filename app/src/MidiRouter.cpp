#include "MidiRouter.h"

namespace vhapp {

void MidiRouter::DeviceCallback::handleIncomingMidiMessage(juce::MidiInput*,
                                                            const juce::MidiMessage& m) {
    // Runs on that device's own high-priority MIDI thread. Pushing into a fixed-size
    // lock-free fifo is the only thing this does, per docs/app-design.md §6.1's "MIDI
    // callback: ... push to a lock-free SPSC FIFO. No allocation."
    //
    // isNoteOn()/isNoteOff() are called with JUCE's own default arguments, which already
    // encode the MIDI convention correctly: isNoteOn(returnTrueForVelocity0 = false) and
    // isNoteOff(returnTrueForNoteOnVelocity0 = true) mean a velocity-0 "note on" is
    // classified as a note-off by both predicates without this code special-casing it.
    if (m.isNoteOn()) {
        fifo_.push({MidiEvent::Kind::NoteOn,
                    static_cast<std::uint8_t>(m.getNoteNumber()),
                    m.getVelocity()});
    } else if (m.isNoteOff()) {
        fifo_.push({MidiEvent::Kind::NoteOff,
                    static_cast<std::uint8_t>(m.getNoteNumber()),
                    0});
    } else if (m.isController()) {
        fifo_.push({MidiEvent::Kind::ControlChange,
                    static_cast<std::uint8_t>(m.getControllerNumber()),
                    static_cast<std::uint8_t>(m.getControllerValue())});
    }
    // Anything else (program change, pitch bend, sysex, clock...) is out of scope for
    // this track per docs/app-design.md §8 and is silently ignored, not queued.
}

MidiRouter::MidiRouter(juce::AudioDeviceManager& deviceManager) : deviceManager_(deviceManager) {
    // The on-screen keyboard is always available — it is testing convenience #3 from
    // docs/app-design.md §1 ("the laptop keyboard as a MIDI source... so the thing is
    // usable without a full rig"), so unlike hardware slots it is never gated on a device
    // being enabled.
    slots_[kKeyboardSlot].active.store(true, std::memory_order_relaxed);
    keyboardState_.addListener(this);
}

MidiRouter::~MidiRouter() {
    keyboardState_.removeListener(this);
    for (int i = 0; i < kMaxHardwareSources; ++i) {
        auto& slot = slots_[static_cast<std::size_t>(i)];
        if (slot.callback)
            deviceManager_.removeMidiInputDeviceCallback(slot.deviceIdentifier, slot.callback.get());
    }
}

void MidiRouter::refresh() {
    // Disable slots whose device is no longer enabled.
    for (int i = 0; i < kMaxHardwareSources; ++i) {
        auto& slot = slots_[static_cast<std::size_t>(i)];
        if (slot.callback && !deviceManager_.isMidiInputDeviceEnabled(slot.deviceIdentifier)) {
            deviceManager_.removeMidiInputDeviceCallback(slot.deviceIdentifier, slot.callback.get());
            slot.callback.reset();
            slot.deviceIdentifier = {};
            // KNOWN LIMITATION: any note-on this slot already delivered but whose
            // matching note-off has not yet been drained by the audio thread will now
            // never be turned off by this path (the fifo simply stops being drained once
            // active is cleared). Acceptable for a Track F skeleton — the Perform panel's
            // panic button is the mitigation — but worth fixing properly (e.g. draining
            // once more before deactivating) if hot-unplugging turns out to be routine.
            slot.active.store(false, std::memory_order_release);
        }
    }

    // Assign a free slot to every enabled device not already claimed.
    for (const auto& info : juce::MidiInput::getAvailableDevices()) {
        if (!deviceManager_.isMidiInputDeviceEnabled(info.identifier)) continue;

        bool already = false;
        for (int i = 0; i < kMaxHardwareSources; ++i) {
            if (slots_[static_cast<std::size_t>(i)].deviceIdentifier == info.identifier) {
                already = true;
                break;
            }
        }
        if (already) continue;

        for (int i = 0; i < kMaxHardwareSources; ++i) {
            auto& slot = slots_[static_cast<std::size_t>(i)];
            if (slot.callback != nullptr) continue;

            slot.callback = std::make_unique<DeviceCallback>(slot.fifo);
            slot.deviceIdentifier = info.identifier;
            deviceManager_.addMidiInputDeviceCallback(info.identifier, slot.callback.get());
            slot.active.store(true, std::memory_order_release);
            break;
            // If no free slot exists, kMaxHardwareSources has been exceeded and this
            // device silently does not get merged in. A UI surfacing "N of 8 slots in
            // use" would be a reasonable addition; not built for this track.
        }
    }
}

void MidiRouter::handleNoteOn(juce::MidiKeyboardState*, int /*midiChannel*/, int midiNoteNumber,
                              float velocity) {
    // Fires on the message thread (MidiKeyboardComponent polls key state on a timer, and
    // mouse-driven key presses arrive via the message thread too) — the sole writer for
    // this slot, so this stays genuinely SPSC.
    slots_[kKeyboardSlot].fifo.push(
        {MidiEvent::Kind::NoteOn,
         static_cast<std::uint8_t>(midiNoteNumber),
         static_cast<std::uint8_t>(juce::jlimit(0.0f, 127.0f, velocity * 127.0f))});
}

void MidiRouter::handleNoteOff(juce::MidiKeyboardState*, int /*midiChannel*/, int midiNoteNumber,
                               float /*velocity*/) {
    slots_[kKeyboardSlot].fifo.push(
        {MidiEvent::Kind::NoteOff, static_cast<std::uint8_t>(midiNoteNumber), 0});
}

} // namespace vhapp
