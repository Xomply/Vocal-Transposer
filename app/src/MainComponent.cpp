#include "MainComponent.h"

namespace vhapp {

MainComponent::MainComponent()
    : midiRouter_(deviceManager_),
      engineCallback_(midiRouter_, tuningStore_.bus()),
      devicesPanel_(deviceManager_, engineCallback_),
      performPanel_(engineCallback_, midiRouter_, tuningStore_),
      voicePanel_(tuningStore_, engineCallback_),
      blendPanel_(tuningStore_, engineCallback_),
      expertPanel_(tuningStore_),
      benchPanel_(tuningStore_),
      tabs_(juce::TabbedButtonBar::TabsAtTop) {
    // 2 in / 2 out is only the DEFAULT channel-count hint AudioDeviceManager uses to build
    // an initial AudioDeviceSetup when there is no saved state; it does not cap how many
    // channels the user can later activate through DevicesPanel's selector (constructed
    // with up to 8 input channels — see DevicesPanel.cpp).
    const juce::String initError = deviceManager_.initialise(2, 2, nullptr, true);
    juce::ignoreUnused(initError); // surfaced through AudioDeviceSelectorComponent's own UI

    engineCallback_.onInputChannelsChanged = [this](juce::StringArray names) {
        devicesPanel_.setInputChannelNames(names);
    };

    deviceManager_.addAudioCallback(&engineCallback_);

    addAndMakeVisible(tabs_);
    const auto tabColour = getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId);
    tabs_.addTab("Devices", tabColour, &devicesPanel_, false);
    tabs_.addTab("Perform", tabColour, &performPanel_, false);
    tabs_.addTab("Voice", tabColour, &voicePanel_, false);
    tabs_.addTab("Blend", tabColour, &blendPanel_, false);
    // Expert is "collapsed by default" (docs/app-design.md §6.2) -- read here as "not
    // shown at startup", the same property every non-Perform tab already has. See
    // ExpertPanel.h's own header for the fuller reasoning.
    tabs_.addTab("Expert", tabColour, &expertPanel_, false);
    tabs_.addTab("Bench", tabColour, &benchPanel_, false);
    tabs_.setCurrentTabIndex(1); // Perform is "always visible" per docs/app-design.md
                                 // §6.2 — default to it so the app opens ready to play.

    setSize(1100, 800);

    // Polls AudioDeviceManager's enabled-MIDI-device set and (re)wires MidiRouter's fixed
    // slots to match — see MidiRouter::refresh() for why polling rather than a
    // ChangeListener callback.
    startTimerHz(10);
}

MainComponent::~MainComponent() {
    stopTimer();
    deviceManager_.removeAudioCallback(&engineCallback_);
    deviceManager_.closeAudioDevice();
}

void MainComponent::resized() { tabs_.setBounds(getLocalBounds()); }

void MainComponent::timerCallback() { midiRouter_.refresh(); }

} // namespace vhapp
