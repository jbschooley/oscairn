// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "Engine.h"
#include "../../plugins/step-sequencer/Parameters.h"

#include <juce_gui_basics/juce_gui_basics.h>

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
        explicit Screen( Engine& engine );

        /// Redraws from the engine's current state; the panel calls it from its timer.
        void refresh();

        void paint( juce::Graphics& ) override;
        void resized() override;
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
        void refreshPicker();
        SoundAction selectedAction(SoundAction::Kind) const;

        juce::ComboBox sounds_, tracks_, channels_, lengths_, patterns_;
        juce::ToggleButton mute_{"Mute track"};
        juce::TextButton launch_{"Launch pattern"};
        juce::Label patternStatus_;
        uint32_t track_=0, pattern_=0;
        int length_=16;
        uint32_t stepParameter(sequencer::StepKind kind,int step) const;
        juce::TextButton plugins_{"Plugins..."}, pads_{"Pads..."};
        juce::TextButton useSound_{"Use sound"}, prepareSound_{"Prepare..."}, removeSound_{"Remove"};
        std::unique_ptr<juce::FileChooser> chooser_;
        juce::Label playingSound_, soundNotice_;
        juce::ToggleButton brightnessLocks_{"Brightness locks"};
        std::vector<uint32_t> soundIds_;
        juce::StringArray soundLabels_;

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
        Drag drag_      = Drag::None;
        int  dragStep_  = -1;
        int  dragNote_  = 0; // the note when the drag began
        bool dragMoved_ = false;
    };
}
