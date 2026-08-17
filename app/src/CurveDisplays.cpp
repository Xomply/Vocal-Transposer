#include "CurveDisplays.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace vhapp {
namespace {

// Shared plot mechanics: every curve here is drawn as a set of pre-mapped screen-space
// points, so the three components can each choose their own x-axis semantics (linear
// semitones, log-Hz, linear ms) without duplicating the stroke/dash logic three times.
void strokePolyline(juce::Graphics& g, const std::vector<juce::Point<float>>& pts,
                     juce::Colour colour, float width, bool dashed) {
    if (pts.size() < 2) return;
    juce::Path p;
    p.startNewSubPath(pts.front());
    for (std::size_t i = 1; i < pts.size(); ++i) p.lineTo(pts[i]);

    g.setColour(colour);
    if (dashed) {
        // createDashedStroke already returns the STROKE OUTLINE (a filled region), not a
        // centreline to stroke again -- fillPath is correct here, strokePath would double
        // up the width.
        juce::Path dashedPath;
        const float dashLengths[] = {5.0f, 4.0f};
        juce::PathStrokeType(width).createDashedStroke(dashedPath, p, dashLengths, 2);
        g.fillPath(dashedPath);
    } else {
        g.strokePath(p, juce::PathStrokeType(width));
    }
}

void paintBackground(juce::Graphics& g, juce::Rectangle<float> bounds) {
    g.setColour(juce::Colours::black);
    g.fillRect(bounds);
    g.setColour(juce::Colours::darkgrey);
    g.drawRect(bounds, 1.0f);
}

void drawDot(juce::Graphics& g, juce::Point<float> p) {
    g.setColour(juce::Colours::yellow);
    g.fillEllipse(juce::Rectangle<float>(9.0f, 9.0f).withCentre(p));
    g.setColour(juce::Colours::black);
    g.drawEllipse(juce::Rectangle<float>(9.0f, 9.0f).withCentre(p), 1.0f);
}

void drawNotApplicable(juce::Graphics& g, juce::Rectangle<float> area, const juce::String& text) {
    g.setColour(juce::Colours::grey);
    g.setFont(14.0f);
    g.drawFittedText(text, area.toNearestInt(), juce::Justification::centred, 3);
}

} // namespace

// =====================================================================================
// 1. mu vs. interval
// =====================================================================================
void MuCurveComponent::paint(juce::Graphics& g) {
    auto bounds = getLocalBounds().toFloat();
    paintBackground(g, bounds);

    auto plotArea = bounds.reduced(10.0f).withTrimmedTop(18.0f).withTrimmedBottom(18.0f);

    constexpr int kSamples = 97; // every 0.5 semitone across -24..+24
    std::vector<float> semis(kSamples), muFlat(kSamples), muFull(kSamples), muActual(kSamples);
    for (int i = 0; i < kSamples; ++i) {
        const float s = -24.0f + 48.0f * static_cast<float>(i) / (kSamples - 1);
        const double ratio = std::pow(2.0, static_cast<double>(s) / 12.0);
        semis[i] = s;
        muFlat[i] = 1.0f;                                      // k=0: PSOLA, formants held
        muFull[i] = static_cast<float>(ratio);                 // k=1: granular, formants track pitch
        muActual[i] = static_cast<float>(voicing_.muFor(ratio)); // the CURRENT dial
    }

    float yMin = std::min({*std::min_element(muActual.begin(), muActual.end()),
                            *std::min_element(muFlat.begin(), muFlat.end()),
                            *std::min_element(muFull.begin(), muFull.end()), 0.0f});
    float yMax = std::max({*std::max_element(muActual.begin(), muActual.end()),
                            *std::max_element(muFlat.begin(), muFlat.end()),
                            *std::max_element(muFull.begin(), muFull.end())});
    const float pad = std::max(0.05f, (yMax - yMin) * 0.08f);
    yMin -= pad;
    yMax += pad;

    auto toScreen = [&](float s, float mu) {
        const float xFrac = (s + 24.0f) / 48.0f;
        const float yFrac = (mu - yMin) / std::max(1e-6f, yMax - yMin);
        return juce::Point<float>(plotArea.getX() + xFrac * plotArea.getWidth(),
                                   plotArea.getBottom() - yFrac * plotArea.getHeight());
    };

    // Zero-interval reference line.
    g.setColour(juce::Colours::darkgrey);
    g.drawVerticalLine(juce::roundToInt(toScreen(0.0f, 0.0f).x), plotArea.getY(), plotArea.getBottom());

    std::vector<juce::Point<float>> ptsFlat, ptsFull, ptsActual;
    for (int i = 0; i < kSamples; ++i) {
        ptsFlat.push_back(toScreen(semis[i], muFlat[i]));
        ptsFull.push_back(toScreen(semis[i], muFull[i]));
        ptsActual.push_back(toScreen(semis[i], muActual[i]));
    }
    strokePolyline(g, ptsFlat, juce::Colours::grey, 1.4f, true);
    strokePolyline(g, ptsFull, juce::Colours::grey, 1.4f, true);
    strokePolyline(g, ptsActual, juce::Colours::cyan, 2.2f, false);

    double liveMu = 0.0;
    if (haveDot_) {
        const double clampedSemis = juce::jlimit(-24.0, 24.0, liveIntervalSemitones_);
        const double ratio = std::pow(2.0, clampedSemis / 12.0);
        liveMu = voicing_.muFor(ratio);
        drawDot(g, toScreen(static_cast<float>(clampedSemis), static_cast<float>(liveMu)));
    }

    g.setColour(juce::Colours::white);
    g.setFont(12.5f);
    g.drawText("mu vs. interval (st) -- dashed: k=0 PSOLA (formants held) / k=1 granular "
               "(formants track pitch)",
               bounds.removeFromTop(16.0f).toNearestInt(), juce::Justification::centredLeft);

    const juce::String caption = haveDot_
        ? ("achieved mu: " + juce::String(liveMu, 3) + "  at " +
           juce::String(juce::jlimit(-24.0, 24.0, liveIntervalSemitones_), 1) + " st")
        : juce::String("no active voice");
    g.drawText(caption, getLocalBounds().removeFromBottom(16), juce::Justification::centredLeft);
}

// =====================================================================================
// 2. blend weight vs. the active policy's axis
// =====================================================================================
void BlendCurveComponent::paint(juce::Graphics& g) {
    auto bounds = getLocalBounds().toFloat();
    paintBackground(g, bounds);
    auto plotArea = bounds.reduced(10.0f).withTrimmedTop(18.0f).withTrimmedBottom(16.0f);

    if (kind_ == vh::BlendKind::FastOnly || kind_ == vh::BlendKind::QualityOnly) {
        // Degenerate by construction (BlendWeights is a constant {1,0} or {0,1} for every
        // context) -- a flat line here would look like a bug rather than a fact, so this
        // says what is actually true instead of plotting it.
        drawNotApplicable(g, plotArea,
                           kind_ == vh::BlendKind::FastOnly
                               ? "Fast-only: always 100% fast engine.\nNo crossfade to show."
                               : "Quality-only: always 100% quality engine.\nNo crossfade to show.");
        g.setColour(juce::Colours::white);
        g.setFont(12.5f);
        g.drawText("blend weight vs. policy axis -- not applicable to this policy",
                   bounds.removeFromTop(16.0f).toNearestInt(), juce::Justification::centredLeft);
        return;
    }

    constexpr int kSamples = 129;
    std::vector<float> xFrac(kSamples), wFast(kSamples);
    bool dotInDomain = false;
    float dotXFrac = 0.0f;
    float dotWFast = 0.0f;

    if (kind_ == vh::BlendKind::RegisterSplit) {
        // Real core policy object, driven by the CURRENT live parameters -- computes
        // exactly what Engine::applyTuning() would write into its owned instance.
        vh::RegisterSplitPolicy policy;
        policy.crossoverHz = rsCrossoverHz_;
        policy.widthOctaves = rsWidthOctaves_;

        // Log-Hz domain: 3 octaves either side of the crossover, which comfortably shows
        // the full transition for the typical ~1-octave widthOctaves default.
        const float lo = rsCrossoverHz_ * std::pow(2.0f, -3.0f);
        const float hi = rsCrossoverHz_ * std::pow(2.0f, 3.0f);
        const float logLo = std::log2(std::max(lo, 1.0f));
        const float logHi = std::log2(std::max(hi, logLo + 1.0f));

        for (int i = 0; i < kSamples; ++i) {
            const float frac = static_cast<float>(i) / (kSamples - 1);
            const float hz = std::pow(2.0f, logLo + frac * (logHi - logLo));
            vh::BlendContext ctx;
            ctx.targetF0Hz = hz;
            xFrac[i] = frac;
            wFast[i] = policy.weightsFor(ctx).fast;
        }

        if (haveDot_ && liveTargetHz_ > 1.0) {
            const float logHz = std::log2(static_cast<float>(liveTargetHz_));
            if (logHz >= logLo && logHz <= logHi) {
                dotInDomain = true;
                dotXFrac = (logHz - logLo) / (logHi - logLo);
                vh::BlendContext ctx;
                ctx.targetF0Hz = static_cast<float>(liveTargetHz_);
                dotWFast = policy.weightsFor(ctx).fast;
            }
        }

        g.setColour(juce::Colours::white);
        g.setFont(12.5f);
        g.drawText("blend weight vs. target pitch (register-split, "
                       + juce::String(lo, 0) + "-" + juce::String(hi, 0) + " Hz shown)",
                   bounds.removeFromTop(16.0f).toNearestInt(), juce::Justification::centredLeft);
    } else { // OnsetHandover
        vh::OnsetHandoverPolicy policy;
        policy.handoverStartMs = ohStartMs_;
        policy.handoverEndMs = ohEndMs_;

        constexpr double kPlotSampleRate = 48000.0; // arbitrary but consistent -- ms is
                                                      // recovered by the same division the
                                                      // policy itself uses, so the choice
                                                      // of sample rate here cancels out.
        const float domainMs = std::max(ohEndMs_ * 1.3f, ohEndMs_ + 20.0f);

        for (int i = 0; i < kSamples; ++i) {
            const float frac = static_cast<float>(i) / (kSamples - 1);
            const float ms = frac * domainMs;
            vh::BlendContext ctx;
            ctx.samplesSinceNoteOn = static_cast<float>(ms / 1000.0 * kPlotSampleRate);
            ctx.sampleRate = kPlotSampleRate;
            xFrac[i] = frac;
            wFast[i] = policy.weightsFor(ctx).fast;
        }

        if (haveDot_ && liveMsSinceNoteOn_ >= 0.0 && liveMsSinceNoteOn_ <= domainMs) {
            dotInDomain = true;
            dotXFrac = static_cast<float>(liveMsSinceNoteOn_ / domainMs);
            vh::BlendContext ctx;
            ctx.samplesSinceNoteOn = static_cast<float>(liveMsSinceNoteOn_ / 1000.0 * kPlotSampleRate);
            ctx.sampleRate = kPlotSampleRate;
            dotWFast = policy.weightsFor(ctx).fast;
        }

        g.setColour(juce::Colours::white);
        g.setFont(12.5f);
        g.drawText("blend weight vs. time since note-on (onset-handover, 0-"
                       + juce::String(domainMs, 0) + " ms shown)",
                   bounds.removeFromTop(16.0f).toNearestInt(), juce::Justification::centredLeft);
    }

    auto toScreen = [&](float xf, float w) {
        return juce::Point<float>(plotArea.getX() + xf * plotArea.getWidth(),
                                   plotArea.getBottom() - w * plotArea.getHeight());
    };

    std::vector<juce::Point<float>> ptsFast, ptsQuality;
    for (int i = 0; i < kSamples; ++i) {
        ptsFast.push_back(toScreen(xFrac[i], wFast[i]));
        ptsQuality.push_back(toScreen(xFrac[i], 1.0f - wFast[i]));
    }
    strokePolyline(g, ptsQuality, juce::Colours::grey, 1.6f, true);   // quality weight, dashed
    strokePolyline(g, ptsFast, juce::Colours::cyan, 2.2f, false);     // fast weight, solid

    if (dotInDomain) drawDot(g, toScreen(dotXFrac, dotWFast));

    const juce::String caption = dotInDomain
        ? ("fast weight here: " + juce::String(dotWFast, 2))
        : juce::String(haveDot_ ? "current position is off this plot's window"
                                 : "no active voice");
    g.setColour(juce::Colours::white);
    g.setFont(12.5f);
    g.drawText(caption, getLocalBounds().removeFromBottom(16), juce::Justification::centredLeft);
}

// =====================================================================================
// 3. the tilt shelf
// =====================================================================================
void TiltCurveComponent::paint(juce::Graphics& g) {
    auto bounds = getLocalBounds().toFloat();
    paintBackground(g, bounds);
    auto plotArea = bounds.reduced(10.0f).withTrimmedTop(18.0f).withTrimmedBottom(16.0f);

    // tiltCornerHz() returns 0 as "bypass" when there is no source F0 to anchor to
    // (voicing.hpp's own contract) -- an unvoiced frame has no open quotient to correct.
    const double cornerHz = haveDot_ ? voicing_.tiltCornerHz(sourceF0Hz_, ratio_) : 0.0;
    if (!haveDot_ || cornerHz <= 0.0) {
        drawNotApplicable(g, plotArea,
                           "No active pitch to anchor the shelf to.\ntiltCornerHz() == 0 (bypass).");
        g.setColour(juce::Colours::white);
        g.setFont(12.5f);
        g.drawText("tilt shelf -- gain and corner",
                   bounds.removeFromTop(16.0f).toNearestInt(), juce::Justification::centredLeft);
        return;
    }

    const double gain = voicing_.tiltGainFor(ratio_);
    const double gainDb = 20.0 * std::log10(std::max(gain, 1e-6));

    // SCHEMATIC sketch, not the true filter response: VoicingProfile gives a gain and a
    // corner scalar, not the biquad's actual transfer function, so this draws a plausible
    // first-order-shelf SHAPE (flat, one-octave transition, flat) anchored at the real
    // gain/corner -- honest about being an illustration, per this file's own header.
    const float logCorner = std::log2(static_cast<float>(cornerHz));
    const float logLo = logCorner - 4.0f; // 4 octaves either side
    const float logHi = logCorner + 4.0f;

    constexpr int kSamples = 129;
    std::vector<juce::Point<float>> pts;
    const float gMin = std::min(0.0f, static_cast<float>(gainDb)) - 3.0f;
    const float gMax = std::max(0.0f, static_cast<float>(gainDb)) + 3.0f;

    auto toScreen = [&](float logHzX, float db) {
        const float xFrac = (logHzX - logLo) / (logHi - logLo);
        const float yFrac = (db - gMin) / std::max(1e-6f, gMax - gMin);
        return juce::Point<float>(plotArea.getX() + xFrac * plotArea.getWidth(),
                                   plotArea.getBottom() - yFrac * plotArea.getHeight());
    };

    for (int i = 0; i < kSamples; ++i) {
        const float frac = static_cast<float>(i) / (kSamples - 1);
        const float logHzX = logLo + frac * (logHi - logLo);
        // One-octave smoothstep transition centred on the corner -- schematic, see above.
        const float x = juce::jlimit(0.0f, 1.0f, (logHzX - logCorner) + 0.5f);
        const float shape = x * x * (3.0f - 2.0f * x);
        const float db = shape * static_cast<float>(gainDb);
        pts.push_back(toScreen(logHzX, db));
    }
    strokePolyline(g, pts, juce::Colours::cyan, 2.2f, false);

    // Corner marker.
    const float cornerX = toScreen(logCorner, 0.0f).x;
    g.setColour(juce::Colours::grey);
    g.drawVerticalLine(juce::roundToInt(cornerX), plotArea.getY(), plotArea.getBottom());

    g.setColour(juce::Colours::white);
    g.setFont(12.5f);
    g.drawText("tilt shelf (schematic) -- gain vs. log frequency",
               bounds.removeFromTop(16.0f).toNearestInt(), juce::Justification::centredLeft);

    const juce::String caption = "gain: " + juce::String(gainDb, 1) + " dB   corner: "
                                    + juce::String(cornerHz, 0) + " Hz";
    g.drawText(caption, getLocalBounds().removeFromBottom(16), juce::Justification::centredLeft);
}

} // namespace vhapp
