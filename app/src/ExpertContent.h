// app/src/ExpertContent.h — the scrollable body of the Expert panel (docs/app-design.md
// §6.2): every remaining live field in vh::Tuning not already covered by Voice/Blend, plus
// the restart-only YIN minHz/maxHz shown DISABLED with a tooltip rather than omitted (see
// app-design.md §4.1 and this track's own instructions: a knob that silently does not
// exist is worse than one visibly marked "not live").
//
// WHY A SEPARATE Content CLASS FROM ExpertPanel: matches DevicesPanel's own selector_ +
// selectorViewport_ pattern (DevicesPanel.h) — the actual widget-heavy component is sized
// to its natural (larger-than-visible) height and hosted inside a juce::Viewport owned by
// the thin wrapper, so this class knows nothing about being inside a tab.
#pragma once

#include "TuningStore.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <vector>

namespace vhapp {

class ExpertContent final : public juce::Component {
public:
    explicit ExpertContent(TuningStore& store);

    void resized() override;

private:
    struct Row {
        std::unique_ptr<juce::Component> label;
        std::unique_ptr<juce::Component> control;   // nullptr for a section header
        bool isSection = false;
    };

    void addSection(const juce::String& title);
    void addFloatRow(const juce::String& labelText, float& field, double lo, double hi,
                      double interval, bool commitOnReleaseOnly = false);
    void addIntRow(const juce::String& labelText, int& field, double lo, double hi);
    void addFrameCountRow(const juce::String& labelText, vh::FrameCount& field, double lo, double hi);
    void addToggleRow(const juce::String& labelText, bool& field);
    void addDisabledInfoRow(const juce::String& labelText, const juce::String& valueText,
                             const juce::String& tooltip);
    void refreshAll();

    TuningStore& store_;
    std::vector<Row> rows_;
    std::vector<std::function<void()>> refreshers_;
};

} // namespace vhapp
