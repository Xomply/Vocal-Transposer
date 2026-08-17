// app/src/BlendPanel.h — the "Blend" panel from docs/app-design.md §6.2: policy picker,
// the policy-specific params (only meaningful for the ACTIVE kind -- see setVisible()
// calls in the .cpp), alignment, and the blend-weight curve display (§6.3).
#pragma once

#include "CurveDisplays.h"
#include "EngineAudioCallback.h"
#include "TuningStore.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace vhapp {

class BlendPanel final : public juce::Component, private juce::Timer {
public:
    BlendPanel(TuningStore& store, EngineAudioCallback& engine);
    ~BlendPanel() override;

    void resized() override;

private:
    void timerCallback() override;
    void refreshFromStore();
    void updatePolicySpecificVisibility();

    TuningStore& store_;
    EngineAudioCallback& engine_;

    juce::Label kindLabel_;
    juce::ComboBox kindCombo_;

    juce::Label rsCrossoverLabel_;
    juce::Slider rsCrossoverSlider_;
    juce::Label rsWidthLabel_;
    juce::Slider rsWidthSlider_;

    juce::Label ohStartLabel_;
    juce::Slider ohStartSlider_;
    juce::Label ohEndLabel_;
    juce::Slider ohEndSlider_;

    juce::Label alignmentLabel_;
    juce::Slider alignmentSlider_;

    juce::Label sectionCurve_;
    BlendCurveComponent blendCurve_;
};

} // namespace vhapp
