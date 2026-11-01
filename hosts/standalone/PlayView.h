// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "Engine.h"
#include "Launcher.h"
#include "PartsModel.h"
#include "PartsView.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace oscairn::standalone
{
    /// Play (RFC 0011), the Performance app's first view, for the stage: the scenes along the
    /// top, a row of knobs following the Performance's controls (or, switched, the selected part's
    /// own), and a strip per part and bus with its level, meters, mute and solo. Everything is
    /// one touch. Drawn in the theme's styles for sections, sliders, knobs and marks.
    class PlayView : public juce::Component, private juce::Timer
    {
    public:
        explicit PlayView(Engine& engine);
        ~PlayView() override;
        // The open Performance came from the setlist: Previous and Next show, and step it.
        std::function<bool()> inSetlist;
        std::function<void(int by)> step;

        uint32_t selectedPart() const { return selected_; }
        void select(uint32_t part);           // a part or bus; 0 none
        void showPartControls(bool part);     // the knobs follow the selected part's controls, or the Performance's
        bool showsPartControls() const { return partControls_ && selected_; }
        // The controls the knobs show, in order, a bank of `knobs()` at a time.
        const std::vector<Macro>& knobControls() const { return knobControls_; }
        int knobs() const;

        void paint(juce::Graphics&) override;
        void resized() override;
        void mouseUp(const juce::MouseEvent&) override;

    private:
        class Knob;
        class Strip;
        void timerCallback() override;
        void refresh();
        void rebuild();
        void publishRow();
        int mode() const { return std::max(1, desktopMode(*this)); }

        Engine& engine_;
        Control::GraphState graph_;
        PartsState state_;
        std::map<uint32_t, std::vector<double>> meters_;
        std::vector<Macro> knobControls_;
        uint32_t selected_ = 0;
        bool partControls_ = false;
        int page_ = 0, ticks_ = 0;
        std::string shape_;
        juce::TextButton previous_{"Previous"}, next_{"Next"}, performanceControls_{"Performance"}, partControlsButton_, pageButton_;
        std::vector<juce::Rectangle<int>> scenes_; // eight tiles
        juce::Rectangle<int> knobRow_;
        std::vector<std::unique_ptr<Knob>> knobs_;
        juce::Viewport viewport_;
        juce::Component stripRow_;
        std::vector<std::unique_ptr<Strip>> strips_;
    };

    /// The Performance app on a device's screen (RFC 0030): Play, and Edit's parts views, with
    /// the top bar's actions for each.
    class PerformanceApp : public juce::Component
    {
    public:
        PerformanceApp(Engine& engine, const std::string& scanner);
        PlayView& play() { return play_; }
        // Puts its actions in the bar: Edit or Play, and Save.
        void attach(TopBar& bar, std::function<void()> save);
        void edit(bool editing);
        bool editing() const { return parts_.isVisible(); }
        void resized() override;

    private:
        void actions();
        PlayView play_;
        PartsView parts_;
        TopBar* bar_ = nullptr;
        std::function<void()> save_;
    };
}
