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
        /// Where an element goes when a stretching layout gets extra space: along each
        /// axis it keeps its distance from the start (left, top) or the end (right,
        /// bottom), stays centred, or stretches with the space.
        struct Anchor
        {
            enum Edge
            {
                Start,
                End,
                Centre,
                Stretch
            };
            Edge x = Start, y = Start;

            /// `rect` from the design, moved or sized for `dx`, `dy` extra design units.
            juce::Rectangle<int> place( juce::Rectangle<int> rect, int dx, int dy ) const;
        };

        struct Panel
        {
            juce::Rectangle<int> rect;
            std::string          label;
            Anchor               anchor;
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
            bool        endless = false;    // knobs: an endless encoder rather than a pot with end stops
            juce::Justification align = juce::Justification::centredLeft; // labels
            Anchor      anchor;
        };

        std::string          name;
        int                  width = 0, height = 0;
        /// How the panel fits a resized window. Scale: the whole panel scales, keeping its
        /// proportions. Stretch: it scales to fit the tighter dimension and the space left
        /// over in the other goes to elements by their anchors, so any window shape works.
        enum class Resize
        {
            Fixed,
            Scale,
            Stretch
        };
        Resize               resize = Resize::Scale;
        std::vector<Panel>   panels;
        std::vector<Control> controls;

        /// Parses a layout; throws std::runtime_error with the reason if it is not valid.
        static Layout parse( const juce::String& json );
    };
}
