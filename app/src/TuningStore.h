// app/src/TuningStore.h — the app's ONE authoritative vh::Tuning, and the seam every
// parameter widget in Track G goes through to reach the audio thread.
//
// WHY THIS CLASS EXISTS, and why it is not just a bare vh::Tuning member on MainComponent:
// docs/app-design.md §4.3's hazard (added after a real bug during Track D) is that
// Engine::applyTuning() is a COMPLETE SNAPSHOT, never a merge — publishing a freshly
// constructed vh::Tuning{} to change one field silently resets every other live parameter
// to its struct default. The only way to make that mistake structurally hard to make is to
// give the whole app exactly one place that owns the live Tuning and exactly one function
// that reaches the audio thread (publish()), so every panel mutates a field on the SAME
// struct and republishes the WHOLE thing, every time, with no code path that could
// construct a partial one. Concentrating that in one small class is cheaper than trusting
// four different panels to each remember the rule.
//
// THREADING: current_ is UI-thread-owned and UI-thread-mutated only (every panel's widget
// callback runs on the JUCE message thread). The only cross-thread traffic is through
// bus_ (vh::TuningBus — triple-buffered, lock-free, single-writer/single-reader, see
// tuning_bus.hpp), which is exactly the channel app-design.md §6.1 specifies. TuningStore
// itself does no locking because it does not need to: there is only ever one writer.
//
// WHAT LIVES HERE VS. IN A PANEL: the authoritative Tuning, the bus, profile load/save
// (thin wrappers over vh_profile, itself just file I/O — no DSP), and the two A/B slots
// (docs/app-design.md §9 item 2, deliberately simple: two in-memory snapshots and a
// recall, no automation, no undo, per §3's explicit "out of scope" list). Everything about
// HOW a value is chosen (a slider's range, a combo box's items) stays in the panel that
// owns that widget — this class knows nothing about JUCE widgets.
#pragma once

#include "vh/tuning.hpp"
#include "vh/tuning_bus.hpp"

#include <juce_core/juce_core.h>

#include <functional>
#include <vector>

namespace vhapp {

class TuningStore {
public:
    // Bootstraps current_ from profiles/factory.vhprofile if it can be found relative to
    // the running executable, else falls back to vh::Tuning{} (bit-exact reproduction of
    // today's engine — see tuning.hpp's own header comment on why that fallback is always
    // safe, never a degraded start). See app-design.md §5.2/§7/§11: the factory profile's
    // ONLY departure from Tuning{} is humanize.* — everything else is struct defaults
    // written out explicitly, so loading it is "the same engine, plus an ensemble spread"
    // rather than a different configuration entirely.
    TuningStore();

    // The authoritative struct. Panels mutate a field through this reference, then MUST
    // call publish() — see the file header. Returning a mutable reference rather than
    // exposing per-field setters is deliberate: with ~60 fields, a setter per field is
    // more surface area than the one rule ("mutate, then publish") it would exist to
    // enforce, and the struct is not accessible from any other thread for this to race
    // against.
    vh::Tuning& current() noexcept { return current_; }
    const vh::Tuning& current() const noexcept { return current_; }

    // For EngineAudioCallback to hold a reference to and poll() once per audio block. The
    // Engine itself does not own a TuningBus (engine.hpp's own comment on applyTuning()) —
    // the app does, and this is where.
    vh::TuningBus& bus() noexcept { return bus_; }

    // Republishes the WHOLE current struct to the audio thread. Cheap (one struct copy,
    // one atomic RMW — see tuning_bus.hpp) and safe to call on every keystroke of a slider
    // drag; TripleBuffer is built specifically to drop stale intermediate values rather
    // than queue them (tuning_bus.hpp's own "why not a queue" section) so a drag that
    // publishes 50 times before the audio thread next polls costs nothing extra.
    //
    // Deliberately does NOT call current_.clamp() first — app-design.md's own instruction
    // for this track is that the shell does not reimplement clamp()'s bounds; it publishes
    // what the user set and lets Engine::applyTuning() clamp its own copy on the way in
    // (app-design.md §4.5's closing rule). Calling clamp() here would also make a widget's
    // displayed value silently diverge from what the user typed whenever it was out of
    // range, which is a worse UI than "the engine ignores an out-of-range value safely".
    void publish() noexcept { bus_.publish(current_); }

    // A panel registers a refresh callback here once, at construction. Called after any
    // change to current_ that did NOT originate from that panel's own widgets — a profile
    // load or an A/B recall replaces every field at once, and every panel showing any of
    // those fields needs to re-pull its widgets from current_ (with dontSendNotification,
    // so re-pulling does not itself trigger another publish()). Ordinary slider-driven
    // edits do not need this: the widget that moved already shows the value the user set.
    void addListener(std::function<void()> cb) { listeners_.push_back(std::move(cb)); }
    void notifyListeners() {
        for (auto& cb : listeners_)
            if (cb) cb();
    }

    // Thin wrappers over vh_profile (profile/include/vh/profile.hpp). REPLACE current_
    // wholesale on success, exactly like the constructor's factory-profile bootstrap —
    // never a field-by-field merge, for the same reason publish() never partial-updates
    // the audio thread. Calls publish() + notifyListeners() on success so every panel and
    // the audio thread see the new struct in the same call. Returns false and fills
    // errorOut (from vh::profile::LoadReport::error / the save() error string) on failure,
    // leaving current_ untouched — vh_profile's own contract (profile.hpp) guarantees a
    // failed load never half-writes the caller's Tuning, so this class does not need to
    // guard against that itself.
    bool loadProfile(const juce::File& file, juce::String& errorOut);
    bool saveProfile(const juce::File& file, const juce::String& name, const juce::String& notes,
                      juce::String& errorOut);

    juce::String currentProfileName() const noexcept { return profileName_; }

    // Best-effort directory to default a file chooser to. Searches upward from the running
    // executable for a `profiles/` sibling (see the .cpp for why: build-app puts the .exe
    // several directories below the repo root, and there is no install step that copies
    // profiles/ next to it). Falls back to the current working directory if not found —
    // never returns a non-existent File, so a caller can hand this straight to
    // juce::FileChooser without checking.
    juce::File profilesDirectory() const;

    // --- A/B slots, app-design.md §9 item 2 ------------------------------------------
    // Deliberately this simple: two snapshots, copy-in, recall. No automation, no undo,
    // no interpolation between them (§12 explicitly declines a morph — "schema complexity
    // bought before anyone has asked for it").
    static constexpr int kSlotA = 0;
    static constexpr int kSlotB = 1;

    void copyCurrentToSlot(int slot);
    void recallSlot(int slot);   // no-op if the slot has never been filled
    bool slotHasData(int slot) const noexcept { return slotFilled_[static_cast<std::size_t>(slot)]; }

    // -1 = current_ has diverged from both slots (an edit was made since the last
    // copy/recall) or neither slot has ever been filled. Purely a display convenience for
    // the Bench panel / Perform strip; nothing downstream reads it.
    int liveSlot() const noexcept { return liveSlot_; }

private:
    vh::Tuning current_{};
    vh::TuningBus bus_;
    juce::String profileName_ = "(default)";

    vh::Tuning slots_[2]{};
    bool slotFilled_[2] = {false, false};
    int liveSlot_ = -1;

    std::vector<std::function<void()>> listeners_;
};

} // namespace vhapp
