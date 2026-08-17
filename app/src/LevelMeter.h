// app/src/LevelMeter.h — draws a float the audio thread already computed and published to
// an atomic (see EngineAudioCallback::inputPeakHold/consumeClipFlag). No DSP: this file
// does not touch a single audio sample, only a peak value handed to it once per UI frame.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>

namespace vhapp {

class LevelMeter final : public juce::Component {
public:
    // level: linear peak, 0..~1+ (a hot signal can briefly exceed 1.0; drawing clamps it,
    // the clip latch is the thing that actually reports it). Called from the UI thread's
    // meter-refresh timer only.
    void setLevel(float linearPeak, bool clipped) noexcept {
        level_ = linearPeak;
        if (clipped) clipLatch_ = true;
        repaint();
    }

    void paint(juce::Graphics& g) override {
        auto r = getLocalBounds().toFloat();
        g.setColour(juce::Colours::black);
        g.fillRect(r);

        const float db = level_ > 1.0e-6f ? 20.0f * std::log10(level_) : -100.0f;
        const float frac = std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f); // -60..0 dB shown
        auto fill = r.withWidth(r.getWidth() * frac);
        g.setColour(frac > 0.95f ? juce::Colours::red
                                  : (frac > 0.75f ? juce::Colours::yellow : juce::Colours::limegreen));
        g.fillRect(fill);

        g.setColour(juce::Colours::grey);
        g.drawRect(r, 1.0f);

        if (clipLatch_) {
            g.setColour(juce::Colours::red);
            g.fillRect(r.removeFromRight(10.0f));
        }
    }

    // The clip indicator latches until clicked, rather than flickering off every time
    // consumeClipFlag() happens to read false between two genuinely clipped blocks.
    void mouseDown(const juce::MouseEvent&) override {
        clipLatch_ = false;
        repaint();
    }

private:
    float level_ = 0.0f;
    bool clipLatch_ = false;
};

} // namespace vhapp
