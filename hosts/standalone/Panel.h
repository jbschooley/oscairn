// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "Engine.h"
#include "Layout.h"
#include "Screen.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace oscairn::standalone
{
    /// A device's front panel built from a Layout: its screen and controls, bound to the
    /// engine, drawn at the layout's design size and scaled to fit the component.
    class Panel : public juce::Component, private juce::Timer
    {
    public:
        /// Throws std::runtime_error if a control is bound to something the engine lacks.
        Panel( Engine& engine, const Layout& layout );
        ~Panel() override;

        const Layout& layout() const { return layout_; }

        void paint( juce::Graphics& ) override;
        void resized() override;

    private:
        class Board;
        class Knob;
        class Pads;
        class Keys;

        void timerCallback() override;

        Engine&                engine_;
        Layout                 layout_;
        std::unique_ptr<Board> board_;
    };
}
