#include "VoicePanel.h"

#include "LiveVoiceEstimate.h"
#include "ParamBinding.h"

namespace vhapp {
namespace {
void setupSlider(juce::Slider& s) {
    s.setSliderStyle(juce::Slider::LinearHorizontal);
    s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 70, 20);
}
} // namespace

VoicePanel::VoicePanel(TuningStore& store, EngineAudioCallback& engine)
    : store_(store), engine_(engine) {
    vh::Tuning& t = store_.current();

    addAndMakeVisible(sectionTimbre_);
    sectionTimbre_.setText("Timbre", juce::dontSendNotification);
    sectionTimbre_.setFont(juce::Font(15.0f, juce::Font::bold));

    addAndMakeVisible(muLabel_);
    muLabel_.setText(
        "Timbre Follow (muStrength): 0 = same body, new pitch  |  1 = derived  |  3.33 = "
        "body follows pitch",
        juce::dontSendNotification);
    setupSlider(muSlider_);
    addAndMakeVisible(muSlider_);
    // Range 0..1/kMu at kMu's DEFAULT (0.30 -> 3.33), per app-design.md §6.2's own wording.
    // kMu itself is Expert-panel-editable, which would technically move the "granular"
    // endpoint -- the slider's range is a fixed, sensible UI bound (this track's own
    // constraint: "the shell does not need to reimplement clamp()'s exact bounds"), not a
    // recomputed one; the curve display below shows the TRUE current relationship
    // regardless of what kMu is set to.
    bindSlider(muSlider_, store_, t.preservation.voicing.muStrength, 0.0, 3.34, 0.01);

    addAndMakeVisible(tiltLabel_);
    tiltLabel_.setText("Source Brightness (tiltStrength)", juce::dontSendNotification);
    setupSlider(tiltSlider_);
    addAndMakeVisible(tiltSlider_);
    bindSlider(tiltSlider_, store_, t.preservation.voicing.tiltStrength, 0.0, 3.0, 0.01);

    addAndMakeVisible(unvoicedLabel_);
    unvoicedLabel_.setText("Unvoiced policy", juce::dontSendNotification);
    addAndMakeVisible(unvoicedCombo_);
    // Item IDs are enum value + 1 (JUCE combo IDs are 1-based); see the onChange handler.
    unvoicedCombo_.addItem("Pass through (unshifted) -- A/B control", 1);
    unvoicedCombo_.addItem("Pass through, decorrelated (default)", 2);
    unvoicedCombo_.addItem("Shift -- A/B, likely wrong", 3);
    unvoicedCombo_.setSelectedId(static_cast<int>(t.preservation.unvoiced) + 1,
                                  juce::dontSendNotification);
    unvoicedCombo_.onChange = [this] {
        store_.current().preservation.unvoiced =
            static_cast<vh::UnvoicedPolicy>(unvoicedCombo_.getSelectedId() - 1);
        store_.publish();
    };

    addAndMakeVisible(attackLabel_);
    attackLabel_.setText("Attack (ms)", juce::dontSendNotification);
    setupSlider(attackSlider_);
    addAndMakeVisible(attackSlider_);
    bindSlider(attackSlider_, store_, t.attackMs, 0.0, 200.0, 0.5);

    addAndMakeVisible(releaseLabel_);
    releaseLabel_.setText("Release (ms)", juce::dontSendNotification);
    setupSlider(releaseSlider_);
    addAndMakeVisible(releaseSlider_);
    bindSlider(releaseSlider_, store_, t.releaseMs, 0.0, 500.0, 0.5);

    addAndMakeVisible(sectionEnsemble_);
    sectionEnsemble_.setText("Ensemble spread (humanize.* -- width across voices, not "
                              "per-voice values; app-design.md Sec 5.2)",
                              juce::dontSendNotification);
    sectionEnsemble_.setFont(juce::Font(15.0f, juce::Font::bold));

    addAndMakeVisible(detuneLabel_);
    detuneLabel_.setText("Detune width (cents)", juce::dontSendNotification);
    setupSlider(detuneSlider_);
    addAndMakeVisible(detuneSlider_);
    bindSlider(detuneSlider_, store_, t.humanize.detuneCents, 0.0, 50.0, 0.1);

    addAndMakeVisible(detuneDriftLabel_);
    detuneDriftLabel_.setText("Detune drift amplitude (cents)", juce::dontSendNotification);
    setupSlider(detuneDriftSlider_);
    addAndMakeVisible(detuneDriftSlider_);
    bindSlider(detuneDriftSlider_, store_, t.humanize.detuneDriftCents, 0.0, 30.0, 0.1);

    addAndMakeVisible(jitterLabel_);
    jitterLabel_.setText("Onset jitter (ms)", juce::dontSendNotification);
    setupSlider(jitterSlider_);
    addAndMakeVisible(jitterSlider_);
    bindSlider(jitterSlider_, store_, t.humanize.onsetJitterMs, 0.0, 50.0, 0.5);

    addAndMakeVisible(vibratoRateLabel_);
    vibratoRateLabel_.setText("Vibrato rate (Hz)", juce::dontSendNotification);
    setupSlider(vibratoRateSlider_);
    addAndMakeVisible(vibratoRateSlider_);
    bindSlider(vibratoRateSlider_, store_, t.humanize.vibratoRateHz, 0.0, 12.0, 0.05);

    addAndMakeVisible(vibratoDepthLabel_);
    vibratoDepthLabel_.setText("Vibrato depth (cents)", juce::dontSendNotification);
    setupSlider(vibratoDepthSlider_);
    addAndMakeVisible(vibratoDepthSlider_);
    bindSlider(vibratoDepthSlider_, store_, t.humanize.vibratoDepthCents, 0.0, 50.0, 0.5);

    addAndMakeVisible(warpSpreadLabel_);
    warpSpreadLabel_.setText("Tract-difference spread (envelopeWarpSpread)",
                              juce::dontSendNotification);
    setupSlider(warpSpreadSlider_);
    addAndMakeVisible(warpSpreadSlider_);
    bindSlider(warpSpreadSlider_, store_, t.humanize.envelopeWarpSpread, 0.0, 1.0, 0.01);

    addAndMakeVisible(panSpreadLabel_);
    panSpreadLabel_.setText("Pan spread (0 = every voice centred, mono-identical)",
                             juce::dontSendNotification);
    setupSlider(panSpreadSlider_);
    addAndMakeVisible(panSpreadSlider_);
    bindSlider(panSpreadSlider_, store_, t.humanize.panSpread, 0.0, 1.0, 0.01);

    addAndMakeVisible(sectionCurves_);
    sectionCurves_.setText("Live curves (app-design.md Sec 6.3)", juce::dontSendNotification);
    sectionCurves_.setFont(juce::Font(15.0f, juce::Font::bold));
    addAndMakeVisible(muCurve_);
    addAndMakeVisible(tiltCurve_);

    // A profile load or A/B recall replaces the WHOLE struct out from under these widgets
    // -- refreshFromStore() re-pulls every one of them (dontSendNotification, so it does
    // not itself trigger another publish()).
    store_.addListener([this] { refreshFromStore(); });

    startTimerHz(20);
}

VoicePanel::~VoicePanel() { stopTimer(); }

void VoicePanel::refreshFromStore() {
    const vh::Tuning& t = store_.current();
    refresh(muSlider_, t.preservation.voicing.muStrength);
    refresh(tiltSlider_, t.preservation.voicing.tiltStrength);
    unvoicedCombo_.setSelectedId(static_cast<int>(t.preservation.unvoiced) + 1,
                                  juce::dontSendNotification);
    refresh(attackSlider_, t.attackMs);
    refresh(releaseSlider_, t.releaseMs);
    refresh(detuneSlider_, t.humanize.detuneCents);
    refresh(detuneDriftSlider_, t.humanize.detuneDriftCents);
    refresh(jitterSlider_, t.humanize.onsetJitterMs);
    refresh(vibratoRateSlider_, t.humanize.vibratoRateHz);
    refresh(vibratoDepthSlider_, t.humanize.vibratoDepthCents);
    refresh(warpSpreadSlider_, t.humanize.envelopeWarpSpread);
    refresh(panSpreadSlider_, t.humanize.panSpread);
}

void VoicePanel::timerCallback() {
    const vh::Tuning& t = store_.current();
    const LiveVoiceEstimate est = estimateLiveVoice(engine_, t.mode, t.rootMidiNote);
    muCurve_.setState(t.preservation.voicing, est.available, est.intervalSemitones);
    tiltCurve_.setState(t.preservation.voicing, est.available, est.sourceF0Hz, est.ratio);
}

void VoicePanel::resized() {
    auto r = getLocalBounds().reduced(8);
    auto left = r.removeFromLeft(460);
    r.removeFromLeft(10);
    auto right = r;

    auto row = [&left](int h) { return left.removeFromTop(h); };
    auto gap = [&left](int h = 4) { left.removeFromTop(h); };

    sectionTimbre_.setBounds(row(20));
    gap();
    muLabel_.setBounds(row(16));
    muSlider_.setBounds(row(22));
    gap();
    tiltLabel_.setBounds(row(16));
    tiltSlider_.setBounds(row(22));
    gap();
    auto unvoicedRow = row(24);
    unvoicedLabel_.setBounds(unvoicedRow.removeFromLeft(120));
    unvoicedCombo_.setBounds(unvoicedRow);
    gap();
    attackLabel_.setBounds(row(16));
    attackSlider_.setBounds(row(22));
    gap();
    releaseLabel_.setBounds(row(16));
    releaseSlider_.setBounds(row(22));
    gap(10);

    sectionEnsemble_.setBounds(row(32));
    gap();
    detuneLabel_.setBounds(row(16));
    detuneSlider_.setBounds(row(22));
    gap();
    detuneDriftLabel_.setBounds(row(16));
    detuneDriftSlider_.setBounds(row(22));
    gap();
    jitterLabel_.setBounds(row(16));
    jitterSlider_.setBounds(row(22));
    gap();
    vibratoRateLabel_.setBounds(row(16));
    vibratoRateSlider_.setBounds(row(22));
    gap();
    vibratoDepthLabel_.setBounds(row(16));
    vibratoDepthSlider_.setBounds(row(22));
    gap();
    warpSpreadLabel_.setBounds(row(16));
    warpSpreadSlider_.setBounds(row(22));
    gap();
    panSpreadLabel_.setBounds(row(16));
    panSpreadSlider_.setBounds(row(22));

    sectionCurves_.setBounds(right.removeFromTop(20));
    right.removeFromTop(4);
    const int curveH = (right.getHeight() - 8) / 2;
    muCurve_.setBounds(right.removeFromTop(curveH));
    right.removeFromTop(8);
    tiltCurve_.setBounds(right);
}

} // namespace vhapp
