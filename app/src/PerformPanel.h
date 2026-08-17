// app/src/PerformPanel.h — the "Perform" panel from docs/app-design.md §6.2: always
// visible, meaningful even with no other panel built yet (Voice/Blend/Expert/Bench are
// Track G work, now built alongside it). Input meter, F0/note/voicing, active voice count,
// latency split into two labelled numbers (never summed — see VH-012, now fixed core-side:
// Engine exposes steadyStateLatencySamples()/acquisitionLatencySamples() separately),
// current profile / A-B slot (read-only here; BenchPanel owns load/save/copy/recall),
// panic, and an on-screen keyboard so the whole app is playable with no hardware attached
// at all (docs/app-design.md §1, testing convenience 3).
#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "EngineAudioCallback.h"
#include "LevelMeter.h"
#include "MidiRouter.h"
#include "TuningStore.h"

namespace vhapp {

class PerformPanel final : public juce::Component, private juce::Timer {
public:
    PerformPanel(EngineAudioCallback& engine, MidiRouter& midi, TuningStore& tuningStore);
    ~PerformPanel() override;

    void resized() override;

private:
    void timerCallback() override;

    EngineAudioCallback& engine_;
    TuningStore& tuningStore_;

    juce::Label wetOnlyNotice_;

    juce::Label inputMeterLabel_;
    LevelMeter inputMeter_;

    juce::Label f0Label_;
    juce::Label voicingLabel_;
    juce::Label voiceCountLabel_;

    juce::Label deviceLatencyLabel_;
    // Two real figures (VH-012), never summed -- see the file header.
    juce::Label engineSteadyStateLatencyLabel_;
    juce::Label engineAcquisitionLatencyLabel_;

    juce::Label profileLabel_;

    juce::TextButton panicButton_;

    juce::MidiKeyboardComponent keyboard_;
};

} // namespace vhapp
