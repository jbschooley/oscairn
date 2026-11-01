// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include <string>

namespace oscairn::standalone
{
    /// The colors offered for Performances and setlist entries. Files keep any "#rrggbb", so
    /// other apps may use others; these are the ones the menus give.
    struct NamedColor { const char* name; const char* hex; };
    inline constexpr NamedColor performanceColors[] = {
        {"Red", "#e5484d"}, {"Orange", "#f76b15"}, {"Yellow", "#ffc53d"}, {"Green", "#46a758"}, {"Teal", "#12a594"},
        {"Blue", "#3e63dd"}, {"Purple", "#8e4ec6"}, {"Pink", "#d6409f"}, {"Gray", "#8b8d98"}};

    // Transparent for none.
    inline juce::Colour colorOf(const std::string& hex)
    {
        return hex.size() == 7 ? juce::Colour::fromString("ff" + juce::String(hex.substr(1))) : juce::Colours::transparentBlack;
    }

    // The colors as menu items, with IDs from base + 1, and base itself for `none`.
    inline void addColors(juce::PopupMenu& menu, const std::string& current, int base, const juce::String& none)
    {
        menu.addItem(base, none, true, current.empty());
        for(size_t i = 0; i < std::size(performanceColors); ++i)
            menu.addColouredItem(base + 1 + int(i), performanceColors[i].name, colorOf(performanceColors[i].hex), true, current == performanceColors[i].hex);
    }
    // What a menu result from addColors chose: true with the color (empty for none), or false.
    inline bool colorChosen(int result, int base, std::string& color)
    {
        if(result == base) { color.clear(); return true; }
        if(result > base && result <= base + int(std::size(performanceColors))) { color = performanceColors[size_t(result - base - 1)].hex; return true; }
        return false;
    }
}
