// app/src/VoicePanel.h — the "Voice" panel from docs/app-design.md §6.2: Timbre Follow,
// Source Brightness, unvoiced policy, attack/release, ensemble spread, plus the mu and
// tilt curve displays (§6.3). glideSemisPerSec is DELIBERATELY ABSENT here — see
// tuning.hpp's own comment and Engine::applyTuning()'s closing note: nothing reads
// Voice::glideRateSemitonesPerSec, so a control for it would be exactly the "knob that
// does nothing" §5.8 warns against.
#pragma once

#include "CurveDisplays.h"
#include "EngineAudioCallback.h"
#include "TuningStore.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace vhapp {

class VoicePanel final : public juce::Component, private juce::Timer {
public:
    VoicePanel(TuningStore& store, EngineAudioCallback& engine);
    ~VoicePanel() override;

    void resized() override;

private:
    void timerCallback() override;
    void refreshFromStore();

    TuningStore& store_;
    EngineAudioCallback& engine_;

    juce::Label sectionTimbre_;
    juce::Label muLabel_;
    juce::Slider muSlider_;
    juce::Label tiltLabel_;
    juce::Slider tiltSlider_;
    juce::Label unvoicedLabel_;
    juce::ComboBox unvoicedCombo_;
    juce::Label attackLabel_;
    juce::Slider attackSlider_;
    juce::Label releaseLabel_;
    juce::Slider releaseSlider_;

    juce::Label sectionEnsemble_;
    juce::Label detuneLabel_;
    juce::Slider detuneSlider_;
    juce::Label detuneDriftLabel_;
    juce::Slider detuneDriftSlider_;
    juce::Label jitterLabel_;
    juce::Slider jitterSlider_;
    juce::Label vibratoRateLabel_;
    juce::Slider vibratoRateSlider_;
    juce::Label vibratoDepthLabel_;
    juce::Slider vibratoDepthSlider_;
    juce::Label warpSpreadLabel_;
    juce::Slider warpSpreadSlider_;
    juce::Label panSpreadLabel_;
    juce::Slider panSpreadSlider_;

    juce::Label sectionCurves_;
    MuCurveComponent muCurve_;
    TiltCurveComponent tiltCurve_;
};

} // namespace vhapp
