// app/src/DevicesPanel.h — the "Devices" panel from docs/app-design.md §6.2: backend,
// device, sample rate, buffer size, MIDI input checkboxes, and which single input channel
// is the mic. Everything here runs on the message thread; the only thing it touches on
// EngineAudioCallback is its thread-safe control surface (setInputChannel /
// onInputChannelsChanged), never the Engine or the audio thread directly.
#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "EngineAudioCallback.h"

namespace vhapp {

class DevicesPanel final : public juce::Component {
public:
    DevicesPanel(juce::AudioDeviceManager& deviceManager, EngineAudioCallback& engine);

    void resized() override;

    // Called (via juce::MessageManager::callAsync, from EngineAudioCallback) whenever the
    // device (re)starts with a possibly different set of active input channels.
    void setInputChannelNames(const juce::StringArray& names);

private:
    EngineAudioCallback& engine_;

    // Declaration order matches constructor-initialiser-list order (selector_ is
    // constructed with real arguments there; selectorViewport_ merely hosts it and is
    // wired up in the constructor body) to avoid an MSVC /W4 reorder warning.
    juce::AudioDeviceSelectorComponent selector_;
    juce::Viewport selectorViewport_;

    juce::Label micLabel_;
    juce::ComboBox micChannelCombo_;

    juce::Label backendNote_;
};

} // namespace vhapp
