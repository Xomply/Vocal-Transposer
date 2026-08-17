// app/src/BenchPanel.h — the "Bench" panel from docs/app-design.md §6.2/§9: profile
// load/save and two A/B slots. Deliberately simple per §9 item 2 / §3's explicit
// "out of scope" list: no automation, no undo, no interpolation between slots.
#pragma once

#include "TuningStore.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace vhapp {

class BenchPanel final : public juce::Component, private juce::Timer {
public:
    explicit BenchPanel(TuningStore& store);
    ~BenchPanel() override;

    void resized() override;

private:
    void timerCallback() override;
    void updateSlotDisplay();

    TuningStore& store_;

    juce::Label profileLabel_;

    juce::Label nameLabel_;
    juce::TextEditor nameEditor_;
    juce::Label notesLabel_;
    juce::TextEditor notesEditor_;

    juce::TextButton loadButton_;
    juce::TextButton saveButton_;

    juce::Label abLabel_;
    juce::TextButton copyToAButton_;
    juce::TextButton copyToBButton_;
    juce::TextButton recallAButton_;
    juce::TextButton recallBButton_;
    juce::Label liveSlotLabel_;

    juce::Label statusLabel_;

    std::unique_ptr<juce::FileChooser> chooser_;   // kept alive for the async dialog
};

} // namespace vhapp
