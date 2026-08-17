#include "BlendPanel.h"

#include "LiveVoiceEstimate.h"
#include "ParamBinding.h"

namespace vhapp {
namespace {
void setupSlider(juce::Slider& s) {
    s.setSliderStyle(juce::Slider::LinearHorizontal);
    s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 20);
}
} // namespace

BlendPanel::BlendPanel(TuningStore& store, EngineAudioCallback& engine)
    : store_(store), engine_(engine) {
    vh::Tuning& t = store_.current();

    addAndMakeVisible(kindLabel_);
    kindLabel_.setText("Policy", juce::dontSendNotification);
    addAndMakeVisible(kindCombo_);
    // IDs are enum value + 1 (BlendKind, tuning.hpp) -- 1-based JUCE combo IDs.
    kindCombo_.addItem("Fast only", 1);
    kindCombo_.addItem("Quality only", 2);
    kindCombo_.addItem("Register split", 3);
    kindCombo_.addItem("Onset handover", 4);
    kindCombo_.setSelectedId(static_cast<int>(t.kind) + 1, juce::dontSendNotification);
    kindCombo_.onChange = [this] {
        store_.current().kind = static_cast<vh::BlendKind>(kindCombo_.getSelectedId() - 1);
        store_.publish();
        updatePolicySpecificVisibility();
    };

    addAndMakeVisible(rsCrossoverLabel_);
    rsCrossoverLabel_.setText("Crossover (Hz) -- register-split only", juce::dontSendNotification);
    setupSlider(rsCrossoverSlider_);
    addAndMakeVisible(rsCrossoverSlider_);
    bindSlider(rsCrossoverSlider_, store_, t.rsCrossoverHz, 40.0, 2000.0, 1.0);

    addAndMakeVisible(rsWidthLabel_);
    rsWidthLabel_.setText("Crossover width (octaves) -- register-split only",
                           juce::dontSendNotification);
    setupSlider(rsWidthSlider_);
    addAndMakeVisible(rsWidthSlider_);
    bindSlider(rsWidthSlider_, store_, t.rsWidthOctaves, 0.05, 4.0, 0.01);

    addAndMakeVisible(ohStartLabel_);
    ohStartLabel_.setText("Handover start (ms) -- onset-handover only", juce::dontSendNotification);
    setupSlider(ohStartSlider_);
    addAndMakeVisible(ohStartSlider_);
    bindSlider(ohStartSlider_, store_, t.ohStartMs, 0.0, 300.0, 1.0);

    addAndMakeVisible(ohEndLabel_);
    ohEndLabel_.setText("Handover end (ms) -- onset-handover only", juce::dontSendNotification);
    setupSlider(ohEndSlider_);
    addAndMakeVisible(ohEndSlider_);
    bindSlider(ohEndSlider_, store_, t.ohEndMs, 0.0, 400.0, 1.0);

    addAndMakeVisible(alignmentLabel_);
    alignmentLabel_.setText(
        "Alignment: 0 = fast path arrives early (playable, smeared) -- 1 = time-aligned "
        "(coherent, latency thrown away). No correct value -- app-design.md Sec 5.5/blend.hpp.",
        juce::dontSendNotification);
    setupSlider(alignmentSlider_);
    addAndMakeVisible(alignmentSlider_);
    bindSlider(alignmentSlider_, store_, t.alignment, 0.0, 1.0, 0.01);

    addAndMakeVisible(sectionCurve_);
    sectionCurve_.setText("Live blend-weight curve (app-design.md Sec 6.3)",
                           juce::dontSendNotification);
    sectionCurve_.setFont(juce::Font(15.0f, juce::Font::bold));
    addAndMakeVisible(blendCurve_);

    updatePolicySpecificVisibility();

    store_.addListener([this] {
        refreshFromStore();
        updatePolicySpecificVisibility();
    });

    startTimerHz(20);
}

BlendPanel::~BlendPanel() { stopTimer(); }

void BlendPanel::updatePolicySpecificVisibility() {
    const bool rs = store_.current().kind == vh::BlendKind::RegisterSplit;
    const bool oh = store_.current().kind == vh::BlendKind::OnsetHandover;

    rsCrossoverLabel_.setVisible(rs);
    rsCrossoverSlider_.setVisible(rs);
    rsWidthLabel_.setVisible(rs);
    rsWidthSlider_.setVisible(rs);

    ohStartLabel_.setVisible(oh);
    ohStartSlider_.setVisible(oh);
    ohEndLabel_.setVisible(oh);
    ohEndSlider_.setVisible(oh);

    resized();
}

void BlendPanel::refreshFromStore() {
    const vh::Tuning& t = store_.current();
    kindCombo_.setSelectedId(static_cast<int>(t.kind) + 1, juce::dontSendNotification);
    refresh(rsCrossoverSlider_, t.rsCrossoverHz);
    refresh(rsWidthSlider_, t.rsWidthOctaves);
    refresh(ohStartSlider_, t.ohStartMs);
    refresh(ohEndSlider_, t.ohEndMs);
    refresh(alignmentSlider_, t.alignment);
}

void BlendPanel::timerCallback() {
    const vh::Tuning& t = store_.current();
    const LiveVoiceEstimate est = estimateLiveVoice(engine_, t.mode, t.rootMidiNote);
    blendCurve_.setState(t.kind, t.rsCrossoverHz, t.rsWidthOctaves, t.ohStartMs, t.ohEndMs,
                          est.available, est.targetHz, est.msSinceNoteOn);
}

void BlendPanel::resized() {
    auto r = getLocalBounds().reduced(8);
    auto left = r.removeFromLeft(460);
    r.removeFromLeft(10);
    auto right = r;

    auto row = [&left](int h) { return left.removeFromTop(h); };
    auto gap = [&left](int h = 4) { left.removeFromTop(h); };

    auto kindRow = row(24);
    kindLabel_.setBounds(kindRow.removeFromLeft(60));
    kindCombo_.setBounds(kindRow);
    gap(10);

    // Rows for hidden controls still get skipped visually since setVisible(false)
    // components simply are not painted -- but their row still needs to be reserved only
    // when visible, so the layout does not leave large gaps. Since at most one of the two
    // policy-specific pairs is visible at a time, laying out BOTH unconditionally (visible
    // ones show, hidden ones occupy no visible space but DO consume layout row -- accepted
    // as the simplest correct behaviour here) keeps this function simple; the alternative
    // (conditionally skipping rows) would shift the alignment slider up and down every time
    // the policy changes, which reads as more surprising, not less.
    rsCrossoverLabel_.setBounds(row(16));
    rsCrossoverSlider_.setBounds(row(22));
    gap();
    rsWidthLabel_.setBounds(row(16));
    rsWidthSlider_.setBounds(row(22));
    gap();
    ohStartLabel_.setBounds(row(16));
    ohStartSlider_.setBounds(row(22));
    gap();
    ohEndLabel_.setBounds(row(16));
    ohEndSlider_.setBounds(row(22));
    gap(10);

    alignmentLabel_.setBounds(row(32));
    alignmentSlider_.setBounds(row(22));

    sectionCurve_.setBounds(right.removeFromTop(20));
    right.removeFromTop(4);
    blendCurve_.setBounds(right);
}

} // namespace vhapp
