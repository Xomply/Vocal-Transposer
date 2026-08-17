#include "ExpertPanel.h"

namespace vhapp {

ExpertPanel::ExpertPanel(TuningStore& store) : content_(store) {
    addAndMakeVisible(viewport_);
    viewport_.setViewedComponent(&content_, false);
}

void ExpertPanel::resized() { viewport_.setBounds(getLocalBounds()); }

} // namespace vhapp
