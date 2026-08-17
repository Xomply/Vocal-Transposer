#include "ExpertContent.h"

#include "ParamBinding.h"

namespace vhapp {
namespace {
constexpr int kMargin = 8;
constexpr int kLabelW = 300;
constexpr int kRowH = 22;
constexpr int kRowGap = 5;
constexpr int kSectionH = 24;
constexpr int kSectionGapBefore = 12;
constexpr int kContentWidth = 720;
} // namespace

ExpertContent::ExpertContent(TuningStore& store) : store_(store) {
    vh::Tuning& t = store_.current();

    addSection("Voicing -- mu / tilt (preservation.voicing.*)");
    addFloatRow("kMu -- vocal-tract-length exponent (derived: 0.30)", t.preservation.voicing.kMu,
                0.0, 2.0, 0.01);
    addFloatRow("kTilt -- source-tilt exponent (derived: 1.00)", t.preservation.voicing.kTilt, 0.0,
                2.0, 0.01);
    addFloatRow("muMin -- lower clamp on mu", t.preservation.voicing.muMin, 0.0, 2.0, 0.01);
    addFloatRow("muMax -- upper clamp on mu", t.preservation.voicing.muMax, 0.0, 5.0, 0.01);
    addFloatRow("tiltGainMin -- shelf gain floor (linear)", t.preservation.voicing.tiltGainMin, 0.0,
                2.0, 0.01);
    addFloatRow("tiltGainMax -- shelf gain ceiling (linear)", t.preservation.voicing.tiltGainMax,
                0.0, 8.0, 0.01);
    addFloatRow("tiltCornerInF0 -- shelf corner as a multiple of target F0",
                t.preservation.voicing.tiltCornerInF0, 0.0, 10.0, 0.1);

    addSection("Granular engine (shifters.granular.*)");
    addIntRow("jumpPeriods -- jump distance in pitch periods", t.shifters.granular.jumpPeriods, 1, 8);
    addFloatRow("fadeFraction -- crossfade length, fraction of a period",
                t.shifters.granular.fadeFraction, 0.0, 1.0, 0.01);
    addFloatRow("unvoicedGrainMs -- fallback grain size with no F0",
                t.shifters.granular.unvoicedGrainMs, 0.0, 50.0, 0.1);
    addFloatRow("geometrySmoothMs -- one-pole lag on grain geometry",
                t.shifters.granular.geometrySmoothMs, 0.0, 200.0, 0.5);

    addSection("PSOLA engine (shifters.psola.*)");
    addFloatRow("handoverMs -- grain/passthrough crossfade time (tune first, per HANDOVER.md)",
                t.shifters.psola.handoverMs, 0.1, 100.0, 0.1);

    addSection("Analyzer -- YIN behavioural fields (analyzer.*)");
    addFloatRow("threshold -- YIN absolute-threshold", t.analyzer.threshold, 0.0, 1.0, 0.01);
    addFrameCountRow("hopSamples -- analysis hop (floored at 32; 0 hangs the audio thread)",
                      t.analyzer.hopSamples, 32.0, 2048.0);
    addToggleRow("holdThroughUnvoiced -- hold F0 through consonants", t.analyzer.holdThroughUnvoiced);
    addFloatRow("maxHoldMs -- longest a held F0 survives", t.analyzer.maxHoldMs, 0.0, 1000.0, 1.0);
    addIntRow("releaseHops -- hops before a lost lock releases", t.analyzer.releaseHops, 0, 20);
    addFloatRow("maxStepCents -- largest single-hop F0 jump accepted", t.analyzer.maxStepCents, 0.0,
                1200.0, 1.0);
    addIntRow("jumpConfirmHops -- hops to confirm a genuine jump", t.analyzer.jumpConfirmHops, 0, 10);
    addDisabledInfoRow("minHz -- RESTART-ONLY", "70 Hz (compiled default)",
                        "Sizes YIN's analysis window at prepare() (yin.cpp: minTau_/maxTau_ -> "
                        "windowSamples_ -> window_/decimated_/diff_/cmnd_). Changing it without "
                        "a restart is INERT, not wrong -- YinAnalyzer::process() never re-reads "
                        "it (app-design.md Sec 4.1). Not part of vh::Tuning; would need Engine "
                        "reconstruction to change.");
    addDisabledInfoRow("maxHz -- RESTART-ONLY", "1000 Hz (compiled default)",
                        "Same restart-only reasoning as minHz above.");

    addSection("Epoch tracker (analyzer.epoch* -- see epochCutoffRatio's note)");
    // app-design.md Sec 4.5 item 4: the ONE parameter in the whole struct that commits on
    // drag-release, not continuously -- EpochTracker::setCutoff() is gated by 5% hysteresis
    // specifically to rate-limit retuning and to avoid rewriting a running biquad's
    // coefficients while z1_/z2_ still reflect the OLD ones. A slider publishing every
    // intermediate value during a drag defeats that gate.
    addFloatRow("epochCutoffRatio -- COMMITS ON RELEASE ONLY, see tooltip",
                t.analyzer.epochCutoffRatio, 1.0, 4.0, 0.01, /*commitOnReleaseOnly=*/true);
    addFloatRow("epochLoudnessFactor", t.analyzer.epochLoudnessFactor, 0.0, 1.0, 0.01);
    addFloatRow("epochRefractoryFactor", t.analyzer.epochRefractoryFactor, 0.0, 2.0, 0.01);
    addFloatRow("epochEnvelopeRelease", t.analyzer.epochEnvelopeRelease, 0.0, 0.999999, 0.0001);
    addFloatRow("epochCutoffMinHz", t.analyzer.epochCutoffMinHz, 20.0, 2000.0, 1.0);
    addFloatRow("epochCutoffMaxHz", t.analyzer.epochCutoffMaxHz, 20.0, 4000.0, 1.0);

    addSection("Ensemble / freeze / voice cap");
    addFloatRow("decorMinMs -- decorrelation spread, low end", t.decorMinMs, 0.0, 50.0, 0.1);
    addFloatRow("decorMaxMs -- decorrelation spread, high end (bound: 50 ms, engine.cpp sizing)",
                t.decorMaxMs, 0.0, 50.0, 0.1);
    addFloatRow("freezeWindowMs -- captured loop length when Freeze engages", t.freezeWindowMs, 0.0,
                5000.0, 1.0);
    addIntRow("voiceCap -- runtime polyphony cap (<= kMaxVoices)", t.voiceCap, 1, 16);

    addSection("Velocity -> gain curve");
    addFloatRow("velocityToGainMin -- gain at velocity 0 (1.0 = velocity has no effect)",
                t.velocityToGainMin, 0.0, 1.0, 0.01);
    addFloatRow("velocityCurve -- exponent (1 = linear)", t.velocityCurve, 0.01, 8.0, 0.01);

    addSection("Signal path (Track D)");
    addFloatRow("inputTrimDb", t.inputTrimDb, -60.0, 24.0, 0.1);
    addToggleRow("inputGateOn", t.inputGateOn);
    addFloatRow("inputGateDb -- threshold when the gate is enabled", t.inputGateDb, -100.0, 0.0, 0.1);
    addFloatRow("dryGain -- linear", t.dryGain, 0.0, 4.0, 0.01);
    addFloatRow("wetGain -- linear", t.wetGain, 0.0, 4.0, 0.01);
    addFloatRow("outCeilingDb -- limiter ceiling when enabled", t.outCeilingDb, -60.0, 0.0, 0.1);
    addToggleRow("limiterOn", t.limiterOn);

    // A profile load or A/B recall replaces the WHOLE struct -- re-pull every widget.
    store_.addListener([this] { refreshAll(); });

    // Fixed content width; height grows with however many rows were just added. The
    // Viewport (ExpertPanel.h/.cpp) scrolls vertically, and horizontally too if the window
    // is narrower than kContentWidth.
    int y = kMargin;
    for (const auto& row : rows_) y += (row.isSection ? kSectionGapBefore + kSectionH : kRowH + kRowGap);
    setSize(kContentWidth, y + kMargin);
}

void ExpertContent::addSection(const juce::String& title) {
    auto lbl = std::make_unique<juce::Label>();
    lbl->setText(title, juce::dontSendNotification);
    lbl->setFont(juce::Font(14.5f, juce::Font::bold));
    addAndMakeVisible(*lbl);
    rows_.push_back(Row{std::move(lbl), nullptr, true});
}

void ExpertContent::addFloatRow(const juce::String& labelText, float& field, double lo, double hi,
                                 double interval, bool commitOnReleaseOnly) {
    auto lbl = std::make_unique<juce::Label>();
    lbl->setText(labelText, juce::dontSendNotification);
    addAndMakeVisible(*lbl);

    auto sl = std::make_unique<juce::Slider>();
    sl->setSliderStyle(juce::Slider::LinearHorizontal);
    sl->setTextBoxStyle(juce::Slider::TextBoxRight, false, 80, kRowH);
    if (commitOnReleaseOnly) sl->setChangeNotificationOnlyOnRelease(true);
    addAndMakeVisible(*sl);
    bindSlider(*sl, store_, field, lo, hi, interval);

    juce::Slider* rawSl = sl.get();
    refreshers_.push_back([rawSl, &field] { refresh(*rawSl, field); });
    rows_.push_back(Row{std::move(lbl), std::move(sl), false});
}

void ExpertContent::addIntRow(const juce::String& labelText, int& field, double lo, double hi) {
    auto lbl = std::make_unique<juce::Label>();
    lbl->setText(labelText, juce::dontSendNotification);
    addAndMakeVisible(*lbl);

    auto sl = std::make_unique<juce::Slider>();
    sl->setSliderStyle(juce::Slider::LinearHorizontal);
    sl->setTextBoxStyle(juce::Slider::TextBoxRight, false, 80, kRowH);
    addAndMakeVisible(*sl);
    bindIntSlider(*sl, store_, field, lo, hi, 1.0);

    juce::Slider* rawSl = sl.get();
    refreshers_.push_back([rawSl, &field] { refresh(*rawSl, field); });
    rows_.push_back(Row{std::move(lbl), std::move(sl), false});
}

void ExpertContent::addFrameCountRow(const juce::String& labelText, vh::FrameCount& field,
                                      double lo, double hi) {
    auto lbl = std::make_unique<juce::Label>();
    lbl->setText(labelText, juce::dontSendNotification);
    addAndMakeVisible(*lbl);

    auto sl = std::make_unique<juce::Slider>();
    sl->setSliderStyle(juce::Slider::LinearHorizontal);
    sl->setTextBoxStyle(juce::Slider::TextBoxRight, false, 80, kRowH);
    addAndMakeVisible(*sl);
    bindFrameCountSlider(*sl, store_, field, lo, hi, 1.0);

    juce::Slider* rawSl = sl.get();
    refreshers_.push_back([rawSl, &field] { refresh(*rawSl, field); });
    rows_.push_back(Row{std::move(lbl), std::move(sl), false});
}

void ExpertContent::addToggleRow(const juce::String& labelText, bool& field) {
    auto lbl = std::make_unique<juce::Label>();
    lbl->setText(labelText, juce::dontSendNotification);
    addAndMakeVisible(*lbl);

    auto btn = std::make_unique<juce::ToggleButton>();
    addAndMakeVisible(*btn);
    bindToggle(*btn, store_, field);

    juce::ToggleButton* rawBtn = btn.get();
    refreshers_.push_back([rawBtn, &field] { refresh(*rawBtn, field); });
    rows_.push_back(Row{std::move(lbl), std::move(btn), false});
}

void ExpertContent::addDisabledInfoRow(const juce::String& labelText, const juce::String& valueText,
                                        const juce::String& tooltip) {
    auto lbl = std::make_unique<juce::Label>();
    lbl->setText(labelText, juce::dontSendNotification);
    lbl->setTooltip(tooltip);
    addAndMakeVisible(*lbl);

    auto val = std::make_unique<juce::Label>();
    val->setText(valueText, juce::dontSendNotification);
    val->setJustificationType(juce::Justification::centredRight);
    val->setColour(juce::Label::textColourId, juce::Colours::grey);
    val->setTooltip(tooltip);
    val->setEnabled(false);
    addAndMakeVisible(*val);

    rows_.push_back(Row{std::move(lbl), std::move(val), false});
}

void ExpertContent::refreshAll() {
    for (auto& r : refreshers_)
        if (r) r();
}

void ExpertContent::resized() {
    int y = kMargin;
    for (auto& row : rows_) {
        if (row.isSection) {
            y += kSectionGapBefore;
            row.label->setBounds(kMargin, y, getWidth() - 2 * kMargin, kSectionH);
            y += kSectionH;
        } else {
            row.label->setBounds(kMargin, y, kLabelW, kRowH);
            if (row.control)
                row.control->setBounds(kMargin + kLabelW + 8, y, getWidth() - (kMargin + kLabelW + 8) - kMargin,
                                        kRowH);
            y += kRowH + kRowGap;
        }
    }
}

} // namespace vhapp
