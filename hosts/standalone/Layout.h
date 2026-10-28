// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <juce_graphics/juce_graphics.h>

#include <string>
#include <vector>

namespace oscairn::standalone
{
    /// A device's front panel, as data: its size in design units and where each control
    /// sits. The same engine runs behind every layout, so a layout can describe a device
    /// that does not exist yet (a keyboard, a larger groovebox) and be tried on the desktop.
    /// See layouts/README.md for the format.
    struct Layout
    {
        struct Panel
        {
            juce::Rectangle<int> rect;
            std::string          label;
        };

        struct Control
        {
            enum class Type
            {
                Screen,
                Knob,
                Button,
                Pads,
                Keys,
                Label
            };
            Type                 type = Type::Label;
            juce::Rectangle<int> rect;
            std::string          label;
            /// Knobs: "node:param" (for example "synth:1") or "tempo". Buttons: "play",
            /// "stop", "play-stop" or "sync". Empty: drawn, but does nothing yet.
            std::string bind;
            int         rows = 4, cols = 4; // pads
            int         note  = 36;         // pads: the first pad's note; keys: the lowest key
            int         count = 25;         // keys
            bool        steps = false;      // pads light with the sequencer's playhead
        };

        std::string          name;
        int                  width = 0, height = 0;
        bool                 resizable = true; // scaled to the window, keeping its proportions
        juce::Colour         background{ 0xff1b1d21 };
        std::vector<Panel>   panels;
        std::vector<Control> controls;

        /// Parses a layout; throws std::runtime_error with the reason if it is not valid.
        static Layout parse( const juce::String& json );
    };
}
