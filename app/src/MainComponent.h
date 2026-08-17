// app/src/MainComponent.h — top-level window content. Owns the audio/MIDI machine state
// (AudioDeviceManager, MidiRouter, EngineAudioCallback), the ONE authoritative TuningStore
// (Track G — see TuningStore.h's own header for why there is exactly one), and every panel
// from docs/app-design.md §6.2 (Devices, Perform, Voice, Blend, Expert, Bench). No DSP or
// voice logic here, per app/README.md: this class is wiring only.
#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "BenchPanel.h"
#include "BlendPanel.h"
#include "DevicesPanel.h"
#include "EngineAudioCallback.h"
#include "ExpertPanel.h"
#include "MidiRouter.h"
#include "PerformPanel.h"
#include "TuningStore.h"
#include "VoicePanel.h"

namespace vhapp {

class MainComponent final : public juce::Component, private juce::Timer {
public:
    MainComponent();
    ~MainComponent() override;

    void resized() override;

private:
    void timerCallback() override;

    // Declaration order is construction order: deviceManager_ must exist before
    // midiRouter_ (which takes a reference to it); tuningStore_ must exist before
    // engineCallback_ (which takes a reference to its bus()) and before every panel below,
    // all of which take a TuningStore& (Track G — see TuningStore.h: it is the app's ONE
    // authoritative vh::Tuning, and every panel reaches the audio thread only through it).
    juce::AudioDeviceManager deviceManager_;
    MidiRouter midiRouter_;
    TuningStore tuningStore_;
    EngineAudioCallback engineCallback_;

    DevicesPanel devicesPanel_;
    PerformPanel performPanel_;
    VoicePanel voicePanel_;
    BlendPanel blendPanel_;
    ExpertPanel expertPanel_;
    BenchPanel benchPanel_;

    juce::TabbedComponent tabs_;

    // App-wide tooltip popup -- needed for the Expert panel's restart-only minHz/maxHz
    // rows (ExpertContent.cpp), which are marked disabled with an explanatory tooltip
    // rather than omitted (app-design.md §4.1 / this track's own instructions). Without a
    // TooltipWindow instantiated somewhere, juce::Component::setTooltip() has nothing to
    // display it.
    juce::TooltipWindow tooltipWindow_;
};

} // namespace vhapp
