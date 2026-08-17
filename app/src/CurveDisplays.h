// app/src/CurveDisplays.h — the three curve plots from docs/app-design.md §6.3, which that
// section calls "the part of the UI that earns its keep": a slider alone hides that every
// voice parameter is a CURVE of the shift ratio (voicing.hpp), not a constant. Each plot
// recomputes from the CURRENT live parameters (so it updates as a slider moves) and shows
// a live dot for where the currently-playing voice sits on that curve right now.
//
// NO DSP HERE: every curve is computed by calling the SAME pure functions core already
// exposes for this purpose — VoicingProfile::muFor/tiltGainFor/tiltCornerHz
// (core/include/vh/voicing.hpp) and RegisterSplitPolicy::weightsFor/OnsetHandoverPolicy::
// weightsFor (core/include/vh/blend.hpp). These components never reimplement the curve
// shape; they sample the real function at plot resolution and draw the result, exactly the
// way a spreadsheet chart calls back into a formula rather than re-deriving it.
#pragma once

#include "vh/blend.hpp"
#include "vh/tuning.hpp"
#include "vh/voicing.hpp"

#include <juce_gui_basics/juce_gui_basics.h>

namespace vhapp {

// --- 1. mu vs. interval (-24..+24 st) -----------------------------------------------
class MuCurveComponent final : public juce::Component {
public:
    void setState(const vh::VoicingProfile& voicing, bool haveDot, double liveIntervalSemitones) {
        voicing_ = voicing;
        haveDot_ = haveDot;
        liveIntervalSemitones_ = liveIntervalSemitones;
        repaint();
    }

    void paint(juce::Graphics& g) override;

private:
    vh::VoicingProfile voicing_{};
    bool haveDot_ = false;
    double liveIntervalSemitones_ = 0.0;
};

// --- 2. blend weight vs. the active policy's axis ------------------------------------
class BlendCurveComponent final : public juce::Component {
public:
    void setState(vh::BlendKind kind, float rsCrossoverHz, float rsWidthOctaves, float ohStartMs,
                   float ohEndMs, bool haveDot, double liveTargetHz, double liveMsSinceNoteOn) {
        kind_ = kind;
        rsCrossoverHz_ = rsCrossoverHz;
        rsWidthOctaves_ = rsWidthOctaves;
        ohStartMs_ = ohStartMs;
        ohEndMs_ = ohEndMs;
        haveDot_ = haveDot;
        liveTargetHz_ = liveTargetHz;
        liveMsSinceNoteOn_ = liveMsSinceNoteOn;
        repaint();
    }

    void paint(juce::Graphics& g) override;

private:
    vh::BlendKind kind_ = vh::BlendKind::FastOnly;
    float rsCrossoverHz_ = 220.0f, rsWidthOctaves_ = 1.0f;
    float ohStartMs_ = 25.0f, ohEndMs_ = 70.0f;
    bool haveDot_ = false;
    double liveTargetHz_ = 0.0, liveMsSinceNoteOn_ = 0.0;
};

// --- 3. the tilt shelf: gain and corner, with a live dB readout ----------------------
class TiltCurveComponent final : public juce::Component {
public:
    void setState(const vh::VoicingProfile& voicing, bool haveDot, double sourceF0Hz, double ratio) {
        voicing_ = voicing;
        haveDot_ = haveDot;
        sourceF0Hz_ = sourceF0Hz;
        ratio_ = ratio;
        repaint();
    }

    void paint(juce::Graphics& g) override;

private:
    vh::VoicingProfile voicing_{};
    bool haveDot_ = false;
    double sourceF0Hz_ = 0.0;
    double ratio_ = 1.0;
};

} // namespace vhapp
