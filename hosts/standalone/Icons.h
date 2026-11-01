// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "Theme.h"
#include "IconData.h"
#include <map>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

namespace oscairn::standalone
{
    /// An icon from the Lucide set (icons/, ISC license, by Lucide Contributors and, for
    /// portions, Cole Bemis as part of Feather), drawn in a color within a box. Each is read once;
    /// filled draws its shapes filled as well as outlined (a chosen star).
    inline void drawIcon(juce::Graphics& g, const char* name, juce::Rectangle<float> box, juce::Colour color, bool filled = false)
    {
        static std::map<std::string, std::unique_ptr<juce::Drawable>> cache; // message thread only
        const auto key = std::string(name) + (filled ? "+" : "");
        auto& drawable = cache[key];
        if(!drawable)
        {
            int size = 0;
            const auto file = std::string(name) + ".svg";
            for(int i = 0; i < IconData::namedResourceListSize; ++i)
                if(file == IconData::getNamedResourceOriginalFilename(IconData::namedResourceList[i]))
                    if(const auto* data = IconData::getNamedResource(IconData::namedResourceList[i], size))
                    {
                        // Drawn black, then recolored: the files use currentColor.
                        auto text = juce::String::fromUTF8(data, size).replace("currentColor", "#000000");
                        if(filled) text = text.replace("fill=\"none\"", "fill=\"#000000\"");
                        drawable = juce::Drawable::createFromSVGString(text);
                    }
            if(!drawable) { jassertfalse; return; } // an icon the app does not carry
        }
        const auto copy = drawable->createCopy();
        copy->replaceColour(juce::Colours::black, color);
        copy->drawWithin(g, box, juce::RectanglePlacement::centred, 1.f);
    }

    /// A button that draws an icon rather than text, centered in its box, in the theme's text
    /// color. Give it a tooltip: the icon is its only label on screen.
    class IconButton : public juce::Button
    {
    public:
        enum class Glyph { Plus, Undo, Redo, Detach, List, Pills, Home, Back, Panic, More, Apps };
        IconButton(Glyph glyph, const juce::String& name) : juce::Button(name), glyph_(glyph) { setTitle(name); }

        void paintButton(juce::Graphics& g, bool hover, bool down) override
        {
            // A transparent button color draws the icon alone, for icons inside another control;
            // a toggle that is on is tinted with the accent, as a chosen tab is.
            const bool on = getClickingTogglesState() && getToggleState();
            if(on) { g.setColour(ink(Ink::Accent).withAlpha(.22f)); g.fillRoundedRectangle(getLocalBounds().toFloat(), buttonRadius(float(getHeight()))); }
            else if(const auto color = findColour(juce::TextButton::buttonColourId); !color.isTransparent()) getLookAndFeel().drawButtonBackground(g, *this, color, hover, down);
            else if(hover || down) { g.setColour(ink(Ink::Text).withAlpha(down ? .16f : .08f)); g.fillRoundedRectangle(getLocalBounds().toFloat(), 4); }
            static const char* const names[] = {"plus", "undo-2", "redo-2", "square-arrow-out-up-right", "list", "tags", "house", "arrow-left", "octagon-x", "ellipsis-vertical", "layers"};
            const float side = std::min(float(getWidth()), float(getHeight())) * .6f;
            drawIcon(g, names[int(glyph_)], getLocalBounds().toFloat().withSizeKeepingCentre(side, side),
                     (on ? ink(Ink::Accent) : getProperties()["danger"] ? ink(Ink::Danger) : ink(Ink::Text)).withMultipliedAlpha(isEnabled() ? 1.f : .35f));
        }

    private:
        Glyph glyph_;
    };

    /// Mute and solo, drawn the same everywhere: a small square with its letter, filled in its
    /// color when on.
    inline void drawMark(juce::Graphics& g, juce::Rectangle<float> r, const juce::String& letter, bool on, Ink color, float alpha = 1.f)
    {
        const float side = std::min(r.getWidth(), r.getHeight());
        r = r.withSizeKeepingCentre(side, side);
        // Square or round, by the theme's style.
        const float corner = themeData().style.marks == ThemeStyle::Marks::Round ? side / 2 : std::min({6.f, side * .28f, themeData().radius});
        g.setColour((on ? ink(color) : ink(Ink::Border).withAlpha(.5f)).withMultipliedAlpha(alpha)); g.fillRoundedRectangle(r, corner);
        g.setColour((on ? ink(Ink::Background) : ink(Ink::Muted)).withMultipliedAlpha(alpha));
        g.setFont(juce::FontOptions(side * .55f, juce::Font::bold));
        g.drawText(letter, r, juce::Justification::centred);
    }
    class MarkButton : public juce::Button
    {
    public:
        MarkButton(const juce::String& letter, Ink color, const juce::String& name) : juce::Button(name), letter_(letter), color_(color) { setClickingTogglesState(true); setTooltip(name); }
        void paintButton(juce::Graphics& g, bool hover, bool) override
        {
            drawMark(g, getLocalBounds().toFloat().reduced(1), letter_, getToggleState(), color_, isEnabled() ? 1.f : .5f);
            if(hover) { const float side = std::min(getWidth(), getHeight()) - 2.f; g.setColour(ink(Ink::Text).withAlpha(.08f)); g.fillRoundedRectangle(getLocalBounds().toFloat().withSizeKeepingCentre(side, side), themeData().style.marks == ThemeStyle::Marks::Round ? side / 2 : std::min({6.f, side * .28f, themeData().radius})); }
        }
    private:
        juce::String letter_;
        Ink color_;
    };

    /// The meters' zone a level is in: green, yellow from -12 dB, red from -3.
    inline const juce::Colour meterGreen{0xff5ac46a}, meterYellow{0xffe8c547};
    inline juce::Colour levelColor(double peak) { return peak >= .708 ? ink(Ink::Danger) : peak >= .251 ? meterYellow : meterGreen; }

    /// A level meter from -60 to +6 dB in three zones: green, yellow from -12, red from -3. Each
    /// zone shows its color as far as the level reaches. Vertical meters fill upwards.
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
        const struct { float from, to; juce::Colour color; } zones[] = {{0, at(-12), meterGreen}, {at(-12), at(-3), meterYellow}, {at(-3), 1, ink(Ink::Danger)}};
        for(const auto& z : zones)
        {
            const float from = z.from, to = std::min(z.to, lit);
            if(to <= from) continue;
            g.setColour(z.color);
            if(vertical) g.fillRect(r.getX(), r.getBottom() - r.getHeight() * to, r.getWidth(), r.getHeight() * (to - from));
            else g.fillRect(r.getX() + r.getWidth() * from, r.getY(), r.getWidth() * (to - from), r.getHeight());
        }
    }
}
