// app/src/ParamBinding.h — the one place a widget is wired to a Tuning field, shared by
// every panel Track G adds. No DSP: this only copies a widget's value into a field on
// TuningStore's live Tuning and calls publish() (the WHOLE-struct republish, per
// TuningStore.h's own header on why a partial update is never an option).
//
// WHY A SHARED HELPER RATHER THAN EACH PANEL WIRING onValueChange BY HAND: with ~60
// fields spread across four panels, the risk is not any one binding being wrong, it is one
// of them being SUBTLY wrong in a way that only breaks on republish — e.g. a panel that
// captures `field` by value instead of by reference, silently freezing its slider's writes
// to a stale copy that never reaches TuningStore. Writing the binding once and reusing it
// everywhere means that class of mistake can only exist once, not four times.
#pragma once

#include "TuningStore.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>

namespace vhapp {

// `field` must be a reference into store.current() (i.e. it lives inside TuningStore, not
// a local copy) — see TuningStore.h: current_ is a plain member, never reallocated, so a
// reference into it is valid for the store's whole lifetime, which is at least as long as
// any panel holding a reference to the store.
inline void bindSlider(juce::Slider& slider, TuningStore& store, float& field, double lo,
                        double hi, double interval = 0.0) {
    slider.setRange(lo, hi, interval);
    slider.setValue(static_cast<double>(field), juce::dontSendNotification);
    slider.onValueChange = [&slider, &field, &store] {
        field = static_cast<float>(slider.getValue());
        store.publish();
    };
}

// Same, for a field TuningStore represents as int (voiceCap, releaseHops, ...).
inline void bindIntSlider(juce::Slider& slider, TuningStore& store, int& field, double lo,
                           double hi, double interval = 1.0) {
    slider.setRange(lo, hi, interval);
    slider.setValue(static_cast<double>(field), juce::dontSendNotification);
    slider.onValueChange = [&slider, &field, &store] {
        field = static_cast<int>(std::lround(slider.getValue()));
        store.publish();
    };
}

// FrameCount (std::size_t) fields, e.g. AnalyzerTuning::hopSamples.
inline void bindFrameCountSlider(juce::Slider& slider, TuningStore& store, vh::FrameCount& field,
                                  double lo, double hi, double interval = 1.0) {
    slider.setRange(lo, hi, interval);
    slider.setValue(static_cast<double>(field), juce::dontSendNotification);
    slider.onValueChange = [&slider, &field, &store] {
        field = static_cast<vh::FrameCount>(std::max(0.0, slider.getValue()));
        store.publish();
    };
}

inline void bindToggle(juce::ToggleButton& button, TuningStore& store, bool& field) {
    button.setToggleState(field, juce::dontSendNotification);
    button.onClick = [&button, &field, &store] {
        field = button.getToggleState();
        store.publish();
    };
}

// Re-pulls a widget's displayed value from the field WITHOUT triggering its own
// onValueChange/onClick — used by every panel's listener callback (TuningStore::
// addListener) after an external change (profile load, A/B recall) replaces the whole
// struct out from under whatever the widgets were last showing.
inline void refresh(juce::Slider& slider, float field) {
    slider.setValue(static_cast<double>(field), juce::dontSendNotification);
}
inline void refresh(juce::Slider& slider, int field) {
    slider.setValue(static_cast<double>(field), juce::dontSendNotification);
}
inline void refresh(juce::Slider& slider, vh::FrameCount field) {
    slider.setValue(static_cast<double>(field), juce::dontSendNotification);
}
inline void refresh(juce::ToggleButton& button, bool field) {
    button.setToggleState(field, juce::dontSendNotification);
}

} // namespace vhapp
