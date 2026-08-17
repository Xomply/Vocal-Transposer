#include "TuningStore.h"

#include "vh/profile.hpp"

namespace vhapp {
namespace {

// Walks upward from `start`, looking for a `profiles/factory.vhprofile` sibling. Needed
// because build-app puts the executable at build-app/app/vh_app_artefacts/<config>/*.exe,
// several directories below the repo root where profiles/ lives, and this project has no
// install/packaging step that copies profiles/ next to the binary (deliberately out of
// scope for a personal, non-distributed app — app-design.md §10). Depth 10 is generous
// for any plausible build-directory nesting; failing the search is not an error, just a
// signal to fall back to Tuning{}'s own bit-exact defaults.
juce::File searchUpwardsForProfiles(juce::File start, int maxDepth) {
    juce::File dir = start;
    for (int i = 0; i < maxDepth; ++i) {
        const juce::File candidate = dir.getChildFile("profiles");
        if (candidate.getChildFile("factory.vhprofile").existsAsFile()) return candidate;
        if (dir.isRoot()) break;
        dir = dir.getParentDirectory();
    }
    return {};
}

} // namespace

TuningStore::TuningStore() {
    const juce::File exeDir =
        juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    const juce::File profilesDir = searchUpwardsForProfiles(exeDir, 10);

    if (profilesDir.exists()) {
        juce::String err;
        if (loadProfile(profilesDir.getChildFile("factory.vhprofile"), err)) return;
        // Fall through to the Tuning{} default below — a factory profile that exists but
        // fails to parse (a hand-edit gone wrong) should not stop the app from starting;
        // it should start exactly as if the file were absent, per app-design.md §11's
        // "defaults are the null hypothesis" framing.
    }

    current_ = vh::Tuning{};
    profileName_ = "(default -- factory.vhprofile not found)";
    // Publish even the fallback default: it happens to be numerically identical to
    // EngineConfig's own prepare()-time defaults, but publishing unconditionally means the
    // audio thread's very first poll() sees exactly what current_ holds rather than relying
    // on that coincidence — see the loadProfile() success path below, which does the same.
    publish();
}

bool TuningStore::loadProfile(const juce::File& file, juce::String& errorOut) {
    vh::Tuning loaded{};
    vh::profile::ProfileMeta meta;
    vh::profile::LoadReport report;

    if (!vh::profile::load(file.getFullPathName().toStdString(), loaded, meta, report)) {
        errorOut = juce::String(report.error);
        return false;
    }

    // WHOLE-STRUCT replace, matching publish()'s own rule (file header): current_ is never
    // updated field-by-field from untrusted input, only assigned wholesale. load() already
    // ran Tuning::clamp() on `loaded` (profile.hpp's own contract) so a hand-edited
    // hopSamples=0 or similar hazard from app-design.md §4.5 cannot reach the audio thread
    // through this path either.
    current_ = loaded;
    profileName_ = meta.name.empty() ? file.getFileNameWithoutExtension() : juce::String(meta.name);
    liveSlot_ = -1;   // a freshly loaded profile matches neither A/B slot's contents

    publish();
    notifyListeners();
    return true;
}

bool TuningStore::saveProfile(const juce::File& file, const juce::String& name,
                               const juce::String& notes, juce::String& errorOut) {
    vh::profile::ProfileMeta meta;
    meta.name = name.toStdString();
    meta.notes = notes.toStdString();

    std::string err;
    if (!vh::profile::save(current_, meta, file.getFullPathName().toStdString(), err)) {
        errorOut = juce::String(err);
        return false;
    }

    profileName_ = name.isEmpty() ? file.getFileNameWithoutExtension() : name;
    return true;
}

juce::File TuningStore::profilesDirectory() const {
    const juce::File exeDir =
        juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    const juce::File dir = searchUpwardsForProfiles(exeDir, 10);
    return dir.exists() ? dir : juce::File::getCurrentWorkingDirectory();
}

void TuningStore::copyCurrentToSlot(int slot) {
    jassert(slot == kSlotA || slot == kSlotB);
    slots_[static_cast<std::size_t>(slot)] = current_;
    slotFilled_[static_cast<std::size_t>(slot)] = true;
    liveSlot_ = slot;   // current_ now equals exactly this slot's contents
}

void TuningStore::recallSlot(int slot) {
    jassert(slot == kSlotA || slot == kSlotB);
    if (!slotFilled_[static_cast<std::size_t>(slot)]) return;

    current_ = slots_[static_cast<std::size_t>(slot)];   // whole-struct replace, as above
    liveSlot_ = slot;

    publish();
    notifyListeners();
}

} // namespace vhapp
