// app/src/MidiEventFifo.h — one lock-free SPSC channel per MIDI source.
//
// WHY A HAND-ROLLED FIFO AND NOT juce::MidiMessageCollector, per docs/app-design.md §6.1
// ("MIDI callback: ... push to a lock-free SPSC FIFO. No allocation") and the recon report
// this track was built from:
//
// MidiMessageCollector's own header calls it "fully thread-safe", but its internals are a
// juce::CriticalSection guarding one shared juce::MidiBuffer — a mutex, not a lock-free
// structure. This app opens SEVERAL MIDI inputs simultaneously and merges them
// (docs/app-design.md §8), so a single shared collector would mean up to N independent
// driver callback threads contending for the same critical section, any one of which could
// in principle be preempted while holding it. The sections are short and this is a
// standard, accepted JUCE idiom — but "accepted" is not "cannot ever block the audio
// thread", and that is the one guarantee this file exists to make absolute.
//
// The alternative used here: juce::AbstractFifo (a genuine lock-free SPSC index allocator —
// confirmed directly from its own header, modules/juce_core/containers/juce_AbstractFifo.h)
// wraps a fixed array. Critically, this class is instantiated ONCE PER SOURCE, not once
// shared across sources (see MidiRouter.h) — so each instance is honestly single-producer/
// single-consumer: exactly one thread ever calls push() on a given instance (that device's
// own MIDI driver thread, or the message thread for the on-screen keyboard), and exactly
// one thread ever calls drain() (the audio thread). Merging N devices becomes "the audio
// thread drains N independent fifos every block" rather than "N threads contend for one
// fifo" — which is what keeps this genuinely lock-free rather than merely low-contention.
#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace vhapp {

// Minimal fixed-size event: note on/off, or a controller message (only CC64 is acted on
// today — see EngineAudioCallback.cpp — but any controller is queued so a later track can
// read others, e.g. CC1, without touching this file). No sysex, no NRPN, no program
// change: docs/app-design.md §8 does not ask for them in this track.
struct MidiEvent {
    enum class Kind : std::uint8_t { NoteOn, NoteOff, ControlChange };
    Kind kind = Kind::NoteOn;
    std::uint8_t number = 0;   // note number, or controller number (both 0-127 by the MIDI spec)
    std::uint8_t value  = 0;   // velocity (0-127), or controller value (0-127)
};

class MidiEventFifo {
public:
    // 256 is generous headroom: this fifo is drained every audio callback (hundreds of
    // times a second at any real buffer size), so in practice it almost never holds more
    // than a handful of events between drains. PROVISIONAL — raise if a very fast chord
    // dump (e.g. a DAW sending a whole pattern at once) is ever seen to overflow it; there
    // is no signal today that it needs to be larger.
    static constexpr int kCapacity = 256;

    MidiEventFifo() : fifo_(kCapacity) {}

    // Producer side: exactly one thread per instance — see the file header. Never the
    // audio thread.
    void push(const MidiEvent& e) noexcept {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo_.prepareToWrite(1, start1, size1, start2, size2);
        // Full fifo: both sizes are 0 and the event is dropped rather than blocking or
        // growing the buffer. Silent data loss is an acceptable failure mode for a
        // performance instrument's input; a stall on the audio thread's producer side
        // is not — and at this capacity, for note/CC traffic, it should not happen.
        if (size1 > 0)      buf_[static_cast<std::size_t>(start1)] = e;
        else if (size2 > 0) buf_[static_cast<std::size_t>(start2)] = e;
        fifo_.finishedWrite(size1 + size2);
    }

    // Consumer side: the audio thread only. Calls fn(const MidiEvent&) for each event
    // currently queued, oldest first. Must not allocate — fn is expected to be a small
    // stateless lambda (see EngineAudioCallback.cpp), so this stays header-only and inline.
    template <typename Fn>
    void drain(Fn&& fn) noexcept {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo_.prepareToRead(fifo_.getNumReady(), start1, size1, start2, size2);
        for (int i = 0; i < size1; ++i) fn(buf_[static_cast<std::size_t>(start1 + i)]);
        for (int i = 0; i < size2; ++i) fn(buf_[static_cast<std::size_t>(start2 + i)]);
        fifo_.finishedRead(size1 + size2);
    }

private:
    juce::AbstractFifo fifo_;
    std::array<MidiEvent, kCapacity> buf_{};
};

} // namespace vhapp
