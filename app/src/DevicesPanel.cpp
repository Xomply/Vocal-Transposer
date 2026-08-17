#include "DevicesPanel.h"

namespace vhapp {

DevicesPanel::DevicesPanel(juce::AudioDeviceManager& deviceManager, EngineAudioCallback& engine)
    : engine_(engine),
      selector_(deviceManager,
                0, 8,   // input channels min/max: up to 8, so a multi-in interface (the
                        // Scarlett 2i4 named in docs/app-design.md's example, or bigger)
                        // can have every physical input made available. The mic combo box
                        // below is what actually picks ONE of them for the mono engine.
                0, 2,   // output channels min/max: this shell only ever writes a stereo
                        // pair — see EngineAudioCallback::audioDeviceIOCallbackWithContext.
                true,   // showMidiInputOptions — this IS the "MIDI input device list with
                        // checkboxes" docs/app-design.md asks for. It calls
                        // AudioDeviceManager::setMidiInputDeviceEnabled() directly; MidiRouter
                        // polls that same state (see MidiRouter::refresh) to open/merge
                        // whatever the user has checked here.
                false,  // showMidiOutputSelector — MIDI output is explicitly Out of scope
                        // (docs/app-design.md §3).
                false,  // showChannelsAsStereoPairs — off, so input channels list
                        // individually and the mic combo's indices line up with single
                        // physical inputs rather than L/R pairs.
                true)   // hideAdvancedOptionsWithButton
{
    addAndMakeVisible(selectorViewport_);
    selectorViewport_.setViewedComponent(&selector_, false);
    selector_.setSize(600, 560);

    addAndMakeVisible(micLabel_);
    micLabel_.setText("Mic input channel (which physical input is the singer):",
                       juce::dontSendNotification);

    addAndMakeVisible(micChannelCombo_);
    micChannelCombo_.setTextWhenNoChoicesAvailable("(no input channels active)");
    micChannelCombo_.onChange = [this] {
        engine_.setInputChannel(micChannelCombo_.getSelectedItemIndex());
    };

    addAndMakeVisible(backendNote_);
    backendNote_.setText(
        "The backend dropdown above lists WASAPI three times (Windows Audio / Exclusive "
        "Mode / Low Latency Mode), plus DirectSound and ASIO, as separate entries in one "
        "list -- that is not a bug (docs/app-design.md Sec 6.2). Try \"Windows Audio (Low "
        "Latency Mode)\" first on built-in laptop audio; use ASIO for an external interface. "
        "Wet output is silent with no MIDI notes held -- the engine has no dry/wet mix yet.",
        juce::dontSendNotification);
    backendNote_.setJustificationType(juce::Justification::topLeft);
}

void DevicesPanel::resized() {
    auto r = getLocalBounds().reduced(8);

    auto top = r.removeFromTop(28);
    micLabel_.setBounds(top.removeFromLeft(360));
    micChannelCombo_.setBounds(top.removeFromLeft(220));

    r.removeFromTop(4);
    backendNote_.setBounds(r.removeFromTop(56));
    r.removeFromTop(4);

    selectorViewport_.setBounds(r);
}

void DevicesPanel::setInputChannelNames(const juce::StringArray& names) {
    const int previous = micChannelCombo_.getSelectedItemIndex();
    micChannelCombo_.clear(juce::dontSendNotification);
    for (int i = 0; i < names.size(); ++i)
        micChannelCombo_.addItem(names[i], i + 1); // JUCE combo-box item IDs are 1-based
    if (names.isEmpty()) return;

    const int restore = juce::jlimit(0, names.size() - 1, previous < 0 ? 0 : previous);
    micChannelCombo_.setSelectedItemIndex(restore, juce::dontSendNotification);
    engine_.setInputChannel(restore);
}

} // namespace vhapp
