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
            // A transparent button colour draws the glyph alone, for icons inside another control.
            if(const auto colour = findColour(juce::TextButton::buttonColourId); !colour.isTransparent()) getLookAndFeel().drawButtonBackground(g, *this, colour, hover, down);
            else if(hover || down) { g.setColour(ink(Ink::Text).withAlpha(down ? .16f : .08f)); g.fillRoundedRectangle(getLocalBounds().toFloat(), 4); }
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
            // Centred by what is drawn, so an arrow sits in the middle of its box.
            const auto drawn = p.getBounds();
            p.applyTransform(juce::AffineTransform::translation(box.getCentreX() - drawn.getCentreX(), box.getCentreY() - drawn.getCentreY()));
            g.setColour(ink(Ink::Text).withMultipliedAlpha(isEnabled() ? 1.f : .35f));
            g.strokePath(p, juce::PathStrokeType(std::max(1.5f, side * .12f), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

    private:
        Glyph glyph_;
    };

    /// Mute and solo, drawn the same everywhere: a small square with its letter, filled in its
    /// colour when on.
    inline void drawMark(juce::Graphics& g, juce::Rectangle<float> r, const juce::String& letter, bool on, Ink colour, float alpha = 1.f)
    {
        const float side = std::min(r.getWidth(), r.getHeight());
        r = r.withSizeKeepingCentre(side, side);
        // Square or round, by the theme's style.
        const float corner = themeData().style.marks == ThemeStyle::Marks::Round ? side / 2 : std::min(6.f, side * .28f);
        g.setColour((on ? ink(colour) : ink(Ink::Border).withAlpha(.5f)).withMultipliedAlpha(alpha)); g.fillRoundedRectangle(r, corner);
        g.setColour((on ? ink(Ink::Background) : ink(Ink::Muted)).withMultipliedAlpha(alpha));
        g.setFont(juce::FontOptions(side * .55f, juce::Font::bold));
        g.drawText(letter, r, juce::Justification::centred);
    }
    class MarkButton : public juce::Button
    {
    public:
        MarkButton(const juce::String& letter, Ink colour, const juce::String& name) : juce::Button(name), letter_(letter), colour_(colour) { setClickingTogglesState(true); setTooltip(name); }
        void paintButton(juce::Graphics& g, bool hover, bool) override
        {
            drawMark(g, getLocalBounds().toFloat().reduced(1), letter_, getToggleState(), colour_, isEnabled() ? 1.f : .5f);
            if(hover) { const float side = std::min(getWidth(), getHeight()) - 2.f; g.setColour(ink(Ink::Text).withAlpha(.08f)); g.fillRoundedRectangle(getLocalBounds().toFloat().withSizeKeepingCentre(side, side), themeData().style.marks == ThemeStyle::Marks::Round ? side / 2 : std::min(6.f, side * .28f)); }
        }
    private:
        juce::String letter_;
        Ink colour_;
    };

    /// A level meter from -60 to +6 dB in three zones: green, yellow from -12, red from -3. Each
    /// zone shows its colour as far as the level reaches. Vertical meters fill upwards.
    inline void drawMeter(juce::Graphics& g, juce::Rectangle<float> r, double peak, bool vertical)
    {
        const float round = std::min(r.getWidth(), r.getHeight()) / 2;
        g.setColour(ink(Ink::Border).withAlpha(.35f)); g.fillRoundedRectangle(r, round);
        const double db = peak > 0 ? 20 * std::log10(peak) : -100;
        const auto at = [](double level) { return float(std::clamp((level + 60) / 66, 0.0, 1.0)); };
        const float lit = at(db);
        if(lit <= 0) return;
        juce::Graphics::ScopedSaveState state(g);
        juce::Path clip; clip.addRoundedRectangle(r, round); g.reduceClipRegion(clip);
        const struct { float from, to; juce::Colour colour; } zones[] = {{0, at(-12), juce::Colour(0xff5ac46a)}, {at(-12), at(-3), juce::Colour(0xffe8c547)}, {at(-3), 1, ink(Ink::Danger)}};
        for(const auto& z : zones)
        {
            const float from = z.from, to = std::min(z.to, lit);
            if(to <= from) continue;
            g.setColour(z.colour);
            if(vertical) g.fillRect(r.getX(), r.getBottom() - r.getHeight() * to, r.getWidth(), r.getHeight() * (to - from));
            else g.fillRect(r.getX() + r.getWidth() * from, r.getY(), r.getWidth() * (to - from), r.getHeight());
        }
    }
}
