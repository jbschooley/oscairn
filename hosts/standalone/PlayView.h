// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "Engine.h"
#include "Launcher.h"
#include "PartsModel.h"
#include "PartsView.h"
#include "Profile.h"
#include "oscairn/physical_controls.h"

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
        // A strip's outline: a chosen part only. 0 selects none, so the master (strip 0) is
        // never drawn as selected and a tap on it cannot select or clear.
        bool stripSelected(uint32_t part) const { return part && selected_ == part; }
        void select(uint32_t part);           // a part or bus; 0 none
        void showPartControls(bool part);     // the knobs follow the selected part's controls, or the Performance's
        bool showsPartControls() const { return partControls_ && selected_; }
        // What the knobs play on the page shown, resolved through the physical controls' layers.
        const std::vector<ResolvedControl>& knobSlots() const { return knobSlots_; }
        int knobs() const { return int(knobGroup().count); }
        // The group the knob row shows: the device's screen knobs, or else its first assign group
        // of knobs, or else (the desktop, a panel layout) the layout's row knobs or four, paged.
        ControlGroup knobGroup() const;
        // Learn (RFC 0011): move a device's knob, then touch what it should play: a strip (its
        // level, or its M for its mute), a scene, or a knob's target. Default removes its override.
        void learn(bool on);
        bool learning() const { return learning_; }
        bool canLearn() const { return engine_.rowKnobs() > 0; } // a device with knobs
        void learnTarget(const Assignment&); // touched while learning, after a knob moved

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
        std::vector<Control::ParamInfo> parametersOf(NodeId node);
        std::string nodeName(NodeId node) const;
        const PartRow* partRow(uint32_t id) const;
        // A knob's target: its range, value, what moving it does, and its value as text.
        std::pair<double, double> rangeOf(const Assignment&) const;
        double valueOf(const Assignment&) const;
        void play(const Assignment&, double value);
        void gesture(const Assignment&, bool begin);
        juce::String describe(const Assignment&, double value) const;
        void learned(const Assignment&); // the knob moved to learn, to that (None: its default)

        Engine& engine_;
        Control::GraphState graph_;
        PartsState state_;
        std::map<uint32_t, std::vector<double>> meters_;
        AssignmentSource source_;
        std::vector<ResolvedControl> knobSlots_;
        std::map<NodeId, std::pair<std::string, std::vector<Control::ParamInfo>>> parameters_; // by node: its plugin's ID and parameters
        uint32_t selected_ = 0;
        bool partControls_ = false, learning_ = false;
        juce::String learnFrom_; // the knob moved while learning
        uint32_t touches_ = 0;
        juce::TextButton learnDefault_{"Default"}, learnDone_{"Done"};
        juce::Rectangle<int> banner_;
        int page_ = 0, ticks_ = 0;
        Control::LiveValues values_; // as last shown: a change shows at the next tick, a display frame
        void showValues();
        std::string shape_;
        juce::TextButton previous_{"Previous"}, next_{"Next"}, performanceControls_{"Performance"}, partControlsButton_, pageButton_;
        std::vector<juce::Rectangle<int>> scenes_; // eight tiles
        juce::Rectangle<int> knobRow_;
        std::vector<std::unique_ptr<Knob>> knobs_;
        juce::Viewport viewport_;
        juce::Component stripRow_;
        std::vector<std::unique_ptr<Strip>> strips_;
    };

    /// A profile's control groups, as physical controls resolve them.
    std::vector<ControlGroup> controlGroups(const Profile&);

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
