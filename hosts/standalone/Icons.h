// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace oscairn::standalone
{
    /// A button that draws a symbol rather than text, centred in its box, in the theme's text
    /// colour. Give it a tooltip: the symbol is its only label on screen.
    class IconButton : public juce::Button
    {
    public:
        enum class Glyph { Plus, Undo, Redo, Detach };
        IconButton(Glyph glyph, const juce::String& name) : juce::Button(name), glyph_(glyph) { setTitle(name); }

        void paintButton(juce::Graphics& g, bool hover, bool down) override
        {
            getLookAndFeel().drawButtonBackground(g, *this, findColour(juce::TextButton::buttonColourId), hover, down);
            const float side = std::min(float(getWidth()), float(getHeight())) * .5f;
            const auto box = getLocalBounds().toFloat().withSizeKeepingCentre(side, side);
            juce::Path p;
            switch(glyph_)
            {
            case Glyph::Plus:
                p.startNewSubPath(box.getCentreX(), box.getY()); p.lineTo(box.getCentreX(), box.getBottom());
                p.startNewSubPath(box.getX(), box.getCentreY()); p.lineTo(box.getRight(), box.getCentreY());
                break;
            case Glyph::Undo: case Glyph::Redo:
            {
                // An arrow curving back over the top: left for undo, mirrored for redo.
                const auto r = box.reduced(side * .05f);
                p.startNewSubPath(r.getX() + r.getWidth() * .1f, r.getY() + r.getHeight() * .38f);
                p.lineTo(r.getX() + r.getWidth() * .62f, r.getY() + r.getHeight() * .38f);
                p.quadraticTo(r.getRight(), r.getY() + r.getHeight() * .38f, r.getRight(), r.getY() + r.getHeight() * .66f);
                p.quadraticTo(r.getRight(), r.getBottom(), r.getX() + r.getWidth() * .55f, r.getBottom());
                p.startNewSubPath(r.getX() + r.getWidth() * .34f, r.getY() + r.getHeight() * .12f);
                p.lineTo(r.getX() + r.getWidth() * .1f, r.getY() + r.getHeight() * .38f);
                p.lineTo(r.getX() + r.getWidth() * .34f, r.getY() + r.getHeight() * .64f);
                if(glyph_ == Glyph::Redo) p.applyTransform(juce::AffineTransform::scale(-1, 1, box.getCentreX(), box.getCentreY()));
                break;
            }
            case Glyph::Detach:
                // A window with an arrow leaving it.
                p.startNewSubPath(box.getX() + side * .45f, box.getY() + side * .1f);
                p.lineTo(box.getX(), box.getY() + side * .1f); p.lineTo(box.getX(), box.getBottom());
                p.lineTo(box.getRight() - side * .1f, box.getBottom()); p.lineTo(box.getRight() - side * .1f, box.getY() + side * .55f);
                p.startNewSubPath(box.getX() + side * .45f, box.getY() + side * .55f); p.lineTo(box.getRight(), box.getY());
                p.startNewSubPath(box.getRight() - side * .4f, box.getY()); p.lineTo(box.getRight(), box.getY()); p.lineTo(box.getRight(), box.getY() + side * .4f);
                break;
            }
            g.setColour(ink(Ink::Text).withMultipliedAlpha(isEnabled() ? 1.f : .35f));
            g.strokePath(p, juce::PathStrokeType(std::max(1.5f, side * .12f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

    private:
        Glyph glyph_;
    };
}
