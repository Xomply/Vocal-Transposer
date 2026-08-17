// app/src/ExpertPanel.h — the "Expert" panel from docs/app-design.md §6.2, "collapsed by
// default". INTERPRETATION, recorded because the spec does not say HOW to collapse it: this
// app's top-level panels are already a juce::TabbedComponent (MainComponent.h), and Perform
// is what the app opens onto (MainComponent.cpp: "Perform is 'always visible'... default to
// it so the app opens ready to play"). Expert is simply another tab, not shown until
// clicked -- "collapsed" is read as "not competing for screen space at startup", the same
// property Devices/Voice/Blend/Bench already have relative to Perform. An accordion-style
// in-panel collapse was considered and rejected: it would duplicate a mechanism the tab bar
// already provides, for a panel already the correct size at "off by default".
#pragma once

#include "ExpertContent.h"
#include "TuningStore.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace vhapp {

class ExpertPanel final : public juce::Component {
public:
    explicit ExpertPanel(TuningStore& store);

    void resized() override;

private:
    ExpertContent content_;
    juce::Viewport viewport_;
};

} // namespace vhapp
