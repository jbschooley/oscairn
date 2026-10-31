// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "Engine.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace oscairn::standalone
{
    /// Edits which note each pad plays (the Performance's pad map). Choose a bank, tap a pad to
    /// select and hear it, then set its note, MIDI channel, velocity and pressure; or fill the
    /// bank (or every bank) chromatically, as a drum kit or in a key. Every change is heard at once.
    /// Drawn over the main window, so it works on a device's single screen too.
    class PadEditor : public juce::Component
    {
    public:
        explicit PadEditor(Engine& engine);
        std::function<void()> onDone;

        void paint(juce::Graphics&) override;
        void resized() override;
        void mouseDown(const juce::MouseEvent&) override;
        void mouseUp(const juce::MouseEvent&) override;

    private:
        juce::Rectangle<float> grid() const;
        juce::Rectangle<float> padBounds(int pad) const; // pad 0 at the bottom-left
        int padAt(juce::Point<float>) const;
        PadAssignment& selected() { return map_.assignments[map_.bank][size_t(selected_)]; }
        void apply();       // sends the map to the engine
        void showPad();     // the selected pad's settings into the controls
        void fill(bool everyBank);

        Engine& engine_;
        PadMap map_;
        int selected_ = 0, held_ = -1;

        juce::Label title_, padLabel_, noteLabel_, channelLabel_, velocityLabel_, pressureLabel_, fillLabel_;
        juce::TextButton done_{"Done"}, noteDown_{"-"}, noteUp_{"+"}, octaveDown_{"-12"}, octaveUp_{"+12"}, setAll_{"Set all pads in this bank"};
        std::array<juce::TextButton, PadMap::banks> banks_;
        juce::Label note_;
        juce::ComboBox channel_, pressure_, fillKind_, root_, scale_, first_;
        juce::ToggleButton fixedVelocity_{"Fixed velocity"}, inKey_{"In key"};
        juce::Slider velocity_;
        juce::TextButton fillBank_{"Fill this bank"}, fillAll_{"Fill all banks"};
    };
}
