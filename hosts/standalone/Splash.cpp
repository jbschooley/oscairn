// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Splash.h"
#include "Icons.h"

namespace oscairn::standalone
{
    void Splash::paint(juce::Graphics& g)
    {
        const auto& splash = themeData().splash;
        const auto background = splash.background.value_or(ink(Ink::Background)), text = splash.ink.value_or(ink(Ink::Text));
        g.fillAll(background);
        // Sized by the shorter side, so a small window and a whole panel look alike.
        const float unit = float(std::min(getWidth(), getHeight()));
        const int markSide = juce::roundToInt(unit * .34f), nameSize = juce::roundToInt(unit * .12f);
        const int statusSize = juce::roundToInt(std::clamp(unit * .045f, 13.f, 24.f));
        const int nameHeight = nameSize * 3 / 2, statusHeight = statusSize * 2, gap = juce::roundToInt(unit * .04f);
        auto r = getLocalBounds().withSizeKeepingCentre(getWidth(), markSide + gap + nameHeight + gap / 2 + statusHeight);
        const auto box = r.removeFromTop(markSide).withSizeKeepingCentre(markSide, markSide).toFloat();
        if(const auto* image = splashImage()) image->drawWithin(g, box, juce::RectanglePlacement::centred, 1.f);
        else drawBrandMark(g, box, background.getPerceivedBrightness() < .5f);
        r.removeFromTop(gap);
        g.setColour(text);
        drawInked(g, "oscairn", r.removeFromTop(nameHeight), juce::Font(juce::FontOptions(float(nameSize), juce::Font::bold)), juce::Justification::horizontallyCentred);
        r.removeFromTop(gap / 2);
        g.setColour(text.withAlpha(.75f));
        drawInked(g, status_, r.removeFromTop(statusHeight).reduced(16, 0), juce::Font(juce::FontOptions(float(statusSize))), juce::Justification::horizontallyCentred);
    }
}
