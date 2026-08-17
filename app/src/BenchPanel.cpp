#include "BenchPanel.h"

namespace vhapp {

BenchPanel::BenchPanel(TuningStore& store) : store_(store) {
    addAndMakeVisible(profileLabel_);
    profileLabel_.setFont(juce::Font(15.0f, juce::Font::bold));

    addAndMakeVisible(nameLabel_);
    nameLabel_.setText("Name (for Save)", juce::dontSendNotification);
    addAndMakeVisible(nameEditor_);
    nameEditor_.setText(store_.currentProfileName(), juce::dontSendNotification);

    addAndMakeVisible(notesLabel_);
    notesLabel_.setText("Notes (for Save)", juce::dontSendNotification);
    addAndMakeVisible(notesEditor_);
    notesEditor_.setMultiLine(true);
    notesEditor_.setReturnKeyStartsNewLine(true);

    addAndMakeVisible(loadButton_);
    loadButton_.setButtonText("Load profile...");
    loadButton_.onClick = [this] {
        chooser_ = std::make_unique<juce::FileChooser>("Load a .vhprofile", store_.profilesDirectory(),
                                                         "*.vhprofile");
        chooser_->launchAsync(
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
            [this](const juce::FileChooser& fc) {
                const juce::File file = fc.getResult();
                if (file == juce::File{}) return;
                juce::String err;
                if (store_.loadProfile(file, err)) {
                    nameEditor_.setText(store_.currentProfileName(), juce::dontSendNotification);
                    statusLabel_.setText("Loaded '" + store_.currentProfileName() + "' from "
                                              + file.getFileName(),
                                          juce::dontSendNotification);
                } else {
                    statusLabel_.setText("Load failed: " + err, juce::dontSendNotification);
                }
                updateSlotDisplay();
            });
    };

    addAndMakeVisible(saveButton_);
    saveButton_.setButtonText("Save profile...");
    saveButton_.onClick = [this] {
        const juce::String suggested = nameEditor_.getText().isNotEmpty() ? nameEditor_.getText()
                                                                           : juce::String("untitled");
        chooser_ = std::make_unique<juce::FileChooser>(
            "Save a .vhprofile", store_.profilesDirectory().getChildFile(suggested + ".vhprofile"),
            "*.vhprofile");
        chooser_->launchAsync(
            juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                | juce::FileBrowserComponent::warnAboutOverwriting,
            [this](const juce::FileChooser& fc) {
                const juce::File file = fc.getResult();
                if (file == juce::File{}) return;
                juce::String err;
                if (store_.saveProfile(file, nameEditor_.getText(), notesEditor_.getText(), err)) {
                    statusLabel_.setText("Saved to " + file.getFullPathName(), juce::dontSendNotification);
                } else {
                    statusLabel_.setText("Save failed: " + err, juce::dontSendNotification);
                }
            });
    };

    addAndMakeVisible(abLabel_);
    abLabel_.setText("A/B slots (in memory only -- app-design.md Sec 9 item 2)",
                      juce::dontSendNotification);
    abLabel_.setFont(juce::Font(15.0f, juce::Font::bold));

    addAndMakeVisible(copyToAButton_);
    copyToAButton_.setButtonText("Copy current -> A");
    copyToAButton_.onClick = [this] {
        store_.copyCurrentToSlot(TuningStore::kSlotA);
        updateSlotDisplay();
    };

    addAndMakeVisible(copyToBButton_);
    copyToBButton_.setButtonText("Copy current -> B");
    copyToBButton_.onClick = [this] {
        store_.copyCurrentToSlot(TuningStore::kSlotB);
        updateSlotDisplay();
    };

    addAndMakeVisible(recallAButton_);
    recallAButton_.setButtonText("Recall A (make it live)");
    recallAButton_.onClick = [this] {
        store_.recallSlot(TuningStore::kSlotA);
        updateSlotDisplay();
    };

    addAndMakeVisible(recallBButton_);
    recallBButton_.setButtonText("Recall B (make it live)");
    recallBButton_.onClick = [this] {
        store_.recallSlot(TuningStore::kSlotB);
        updateSlotDisplay();
    };

    addAndMakeVisible(liveSlotLabel_);

    addAndMakeVisible(statusLabel_);
    statusLabel_.setText("Profile: " + store_.currentProfileName(), juce::dontSendNotification);
    statusLabel_.setJustificationType(juce::Justification::topLeft);

    // A load/recall triggered from elsewhere (there is nowhere else today, but this keeps
    // the panel honest if that changes) also needs the name box and slot display refreshed.
    store_.addListener([this] {
        nameEditor_.setText(store_.currentProfileName(), juce::dontSendNotification);
        updateSlotDisplay();
    });

    updateSlotDisplay();
    startTimerHz(4);   // cheap poll for the live-slot indicator; no meters to chase here
}

BenchPanel::~BenchPanel() { stopTimer(); }

void BenchPanel::timerCallback() {
    profileLabel_.setText("Current profile: " + store_.currentProfileName(), juce::dontSendNotification);
}

void BenchPanel::updateSlotDisplay() {
    recallAButton_.setEnabled(store_.slotHasData(TuningStore::kSlotA));
    recallBButton_.setEnabled(store_.slotHasData(TuningStore::kSlotB));

    juce::String live = "Live slot: ";
    switch (store_.liveSlot()) {
        case TuningStore::kSlotA: live += "A"; break;
        case TuningStore::kSlotB: live += "B"; break;
        default:                  live += "neither (diverged, or nothing recalled yet)"; break;
    }
    liveSlotLabel_.setText(live, juce::dontSendNotification);
}

void BenchPanel::resized() {
    auto r = getLocalBounds().reduced(10);

    profileLabel_.setBounds(r.removeFromTop(22));
    r.removeFromTop(8);

    auto nameRow = r.removeFromTop(24);
    nameLabel_.setBounds(nameRow.removeFromLeft(150));
    nameEditor_.setBounds(nameRow);
    r.removeFromTop(4);

    notesLabel_.setBounds(r.removeFromTop(18));
    notesEditor_.setBounds(r.removeFromTop(70));
    r.removeFromTop(8);

    auto btnRow = r.removeFromTop(28);
    loadButton_.setBounds(btnRow.removeFromLeft(160));
    btnRow.removeFromLeft(8);
    saveButton_.setBounds(btnRow.removeFromLeft(160));
    r.removeFromTop(16);

    abLabel_.setBounds(r.removeFromTop(22));
    r.removeFromTop(6);

    auto abRow1 = r.removeFromTop(28);
    copyToAButton_.setBounds(abRow1.removeFromLeft(180));
    abRow1.removeFromLeft(8);
    copyToBButton_.setBounds(abRow1.removeFromLeft(180));
    r.removeFromTop(6);

    auto abRow2 = r.removeFromTop(28);
    recallAButton_.setBounds(abRow2.removeFromLeft(200));
    abRow2.removeFromLeft(8);
    recallBButton_.setBounds(abRow2.removeFromLeft(200));
    r.removeFromTop(6);

    liveSlotLabel_.setBounds(r.removeFromTop(22));
    r.removeFromTop(12);

    statusLabel_.setBounds(r);
}

} // namespace vhapp
