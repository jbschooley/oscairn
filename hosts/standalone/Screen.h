// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "Engine.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <map>

namespace oscairn::standalone
{
    /// The instrument's display: the step sequencer's 16 steps with the playhead, and
    /// the transport. It draws at whatever size the layout gives it.
    ///
    /// On a step: click the upper part to switch it on or off, drag up or down (or use
    /// the mouse wheel) to change its note, and drag in the bar at the bottom to set its
    /// lock (the synth's brightness on that step).
    class Screen : public juce::Component
    {
    public:
        explicit Screen( Engine& engine ) : engine_( engine ) {}

        /// Redraws from the engine's current state; the panel calls it from its timer.
        void refresh();

        void paint( juce::Graphics& ) override;
        void mouseDown( const juce::MouseEvent& ) override;
        void mouseDrag( const juce::MouseEvent& ) override;
        void mouseUp( const juce::MouseEvent& ) override;
        void mouseWheelMove( const juce::MouseEvent&, const juce::MouseWheelDetails& ) override;

    private:
        static constexpr int steps = 16;

        juce::Rectangle<float> header() const;
        juce::Rectangle<float> cell( int step ) const;
        juce::Rectangle<float> lockBar( int step ) const;
        int                    stepAt( juce::Point<float> ) const;
        void                   setLock( int step, float y );

        Engine& engine_;
        Engine::Status status_;
        std::array<bool, steps>   on_{};
        std::array<int, steps>    note_{};
        std::array<double, steps> lock_{};

        enum class Drag
        {
            None,
            Toggle,
            Note,
            Lock
        };
        struct Touch
        {
            Drag drag = Drag::None;
            int  step = -1;
            int  note = 0; // the step's note when the touch began
        };
        std::map<int, Touch> touches_; // by finger, so several steps can be edited at once
    };
}
