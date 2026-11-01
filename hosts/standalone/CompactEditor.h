// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "ParameterGrid.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace oscairn::standalone
{
    /// RFC 0019's Compact surface, for testing: a small screen (about 320 × 240) with four
    /// encoders and no touch, drawing the same editor descriptions as Desktop and Panel, one
    /// list at a time: the Performance's plugins, a plugin's pages, then a page's parameters by
    /// priority, four at a time, one on each encoder. In a list, turning an encoder moves the
    /// choice and pressing encoder 4 opens it; on a page, each encoder turns its parameter and
    /// pressing encoder 4 shows the next four. Pressing encoder 1 goes back.
    class CompactEditor : public juce::Component, private juce::Timer
    {
    public:
        static constexpr int encoders = 4;
        enum class Level { Plugins, Pages, Parameters };

        explicit CompactEditor(Engine& engine);
        ~CompactEditor() override;
        void edit(NodeId node, const std::string& name); // a plugin's pages (its parameters when it has one page)
        void turn(int encoder, int steps);                // encoder 0 to 3, a step per detent
        void press(int encoder);

        Level level() const { return level_; }
        std::vector<std::string> listShown() const;      // a list's items
        size_t chosen() const { return chosen_; }
        std::vector<std::string> encoderNames() const;   // on a page: the parameter each encoder turns
        std::string encoderParam(int encoder, uint32_t& id) const; // its plugin's node name, or "" for none

        void paint(juce::Graphics&) override;

    private:
        struct Plugin { NodeId node = 0; std::string name; };
        void timerCallback() override;
        std::vector<Plugin> plugins() const;  // in the parts' chains, in order
        void open(size_t page);
        const Engine::ParamInfo* onEncoder(int encoder) const;

        Engine& engine_;
        Level level_ = Level::Plugins;
        std::vector<Plugin> plugins_;
        Plugin plugin_;
        std::vector<Engine::ParamInfo> params_;
        std::vector<EditorPage> pages_;
        size_t chosen_ = 0, page_ = 0, first_ = 0; // the list's choice; the page shown and its first parameter on encoder 1
        std::vector<size_t> shown_;                // the page's parameters by priority
        unsigned ticks_ = 0;
    };
}
