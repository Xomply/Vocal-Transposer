#include "PerformPanel.h"

#include <cmath>

namespace vhapp {
namespace {

// Display-only: which MIDI note the analyser's current F0 is closest to. This is NOT the
// note-guessing machinery docs/app-design.md §3 forbids ("no note-guessing machinery...
// the keyboard is the note source") — it feeds nothing back into the engine and influences
// no voice; it exists purely to put a human-readable label on a number that is already on
// screen as Hz.
int hzToNearestMidiNote(float hz) noexcept {
    if (hz <= 1.0f) return -1;
    return static_cast<int>(std::lround(69.0 + 12.0 * std::log2(static_cast<double>(hz) / 440.0)));
}

} // namespace

PerformPanel::PerformPanel(EngineAudioCallback& engine, MidiRouter& midi, TuningStore& tuningStore)
    : engine_(engine),
      tuningStore_(tuningStore),
      keyboard_(midi.keyboardState(), juce::MidiKeyboardComponent::horizontalKeyboard) {
    addAndMakeVisible(wetOnlyNotice_);
    wetOnlyNotice_.setText(
        "Output is wet only -- core has no dry/wet mix yet. Silence with no MIDI notes "
        "held is correct, not a fault.",
        juce::dontSendNotification);
    wetOnlyNotice_.setJustificationType(juce::Justification::topLeft);

    addAndMakeVisible(inputMeterLabel_);
    inputMeterLabel_.setText("Input", juce::dontSendNotification);

    addAndMakeVisible(inputMeter_);

    addAndMakeVisible(f0Label_);
    addAndMakeVisible(voicingLabel_);
    addAndMakeVisible(voiceCountLabel_);
    addAndMakeVisible(deviceLatencyLabel_);
    addAndMakeVisible(engineSteadyStateLatencyLabel_);
    addAndMakeVisible(engineAcquisitionLatencyLabel_);
    addAndMakeVisible(profileLabel_);

    addAndMakeVisible(panicButton_);
    panicButton_.setButtonText("Panic (all notes off)");
    panicButton_.onClick = [this] { engine_.requestPanic(); };

    addAndMakeVisible(keyboard_);
    keyboard_.setAvailableRange(36, 96);
    keyboard_.setKeyWidth(18.0f);

    startTimerHz(30);
}

PerformPanel::~PerformPanel() { stopTimer(); }

void PerformPanel::resized() {
    auto r = getLocalBounds().reduced(8);

    wetOnlyNotice_.setBounds(r.removeFromTop(36));
    r.removeFromTop(6);

    auto meterRow = r.removeFromTop(80);
    inputMeterLabel_.setBounds(meterRow.removeFromLeft(60));
    inputMeter_.setBounds(meterRow.removeFromLeft(220));
    r.removeFromTop(6);

    f0Label_.setBounds(r.removeFromTop(24));
    voicingLabel_.setBounds(r.removeFromTop(24));
    voiceCountLabel_.setBounds(r.removeFromTop(24));
    r.removeFromTop(4);
    deviceLatencyLabel_.setBounds(r.removeFromTop(24));
    engineSteadyStateLatencyLabel_.setBounds(r.removeFromTop(24));
    engineAcquisitionLatencyLabel_.setBounds(r.removeFromTop(24));
    r.removeFromTop(4);
    profileLabel_.setBounds(r.removeFromTop(24));
    r.removeFromTop(8);

    panicButton_.setBounds(r.removeFromTop(32).removeFromLeft(200));
    r.removeFromTop(8);

    keyboard_.setBounds(r.removeFromTop(90));
}

void PerformPanel::timerCallback() {
    inputMeter_.setLevel(engine_.inputPeakHold(), engine_.consumeClipFlag());

    const float f0 = engine_.f0Hz();
    if (f0 > 1.0f) {
        const int note = hzToNearestMidiNote(f0);
        f0Label_.setText("F0: " + juce::String(f0, 1) + " Hz  ("
                              + juce::MidiMessage::getMidiNoteName(note, true, true, 4) + ")",
                          juce::dontSendNotification);
    } else {
        f0Label_.setText("F0: --", juce::dontSendNotification);
    }

    voicingLabel_.setText(engine_.voiced() ? "Voicing: voiced" : "Voicing: unvoiced",
                          juce::dontSendNotification);
    voiceCountLabel_.setText("Active voices: " + juce::String(engine_.activeVoices()),
                             juce::dontSendNotification);

    const double sr = engine_.sampleRate();
    const double inMs = sr > 0.0 ? 1000.0 * static_cast<double>(engine_.deviceInputLatencySamples()) / sr : 0.0;
    const double outMs = sr > 0.0 ? 1000.0 * static_cast<double>(engine_.deviceOutputLatencySamples()) / sr : 0.0;
    deviceLatencyLabel_.setText(
        "Device I/O latency (JUCE-reported): in " + juce::String(inMs, 1) + " ms / out "
            + juce::String(outMs, 1) + " ms",
        juce::dontSendNotification);

    // VH-012 (BUGS.md), now fixed core-side: Engine exposes the two REAL quantities
    // separately rather than one over-reporting sum. steadyState is "how far behind the
    // live edge is a locked, running voice" (correct for both modes); acquisition adds
    // Mode A's F0-lock convergence cost on top (zero extra for Mode B). Shown as two
    // labelled numbers, per docs/app-design.md §13 item 2 -- never summed here either.
    const double steadyMs =
        sr > 0.0 ? 1000.0 * static_cast<double>(engine_.engineSteadyStateLatencySamples()) / sr : 0.0;
    const double acquireMs =
        sr > 0.0 ? 1000.0 * static_cast<double>(engine_.engineAcquisitionLatencySamples()) / sr : 0.0;
    engineSteadyStateLatencyLabel_.setText(
        "Engine steady-state latency: " + juce::String(steadyMs, 1) + " ms  (how far behind "
        "the live edge a locked, running voice sits)",
        juce::dontSendNotification);
    engineAcquisitionLatencyLabel_.setText(
        "Engine acquisition latency: " + juce::String(acquireMs, 1) + " ms  (Mode A onset "
        "only -- adds YIN's F0-lock convergence cost; zero extra for Mode B)",
        juce::dontSendNotification);

    profileLabel_.setText(
        "Profile: " + tuningStore_.currentProfileName()
            + "  |  live A/B slot: "
            + (tuningStore_.liveSlot() == TuningStore::kSlotA
                   ? juce::String("A")
                   : (tuningStore_.liveSlot() == TuningStore::kSlotB ? juce::String("B")
                                                                      : juce::String("--"))),
        juce::dontSendNotification);
}

} // namespace vhapp
