// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "Engine.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace oscairn::standalone
{
    /// Every parameter of one plugin, as pages of tiles (RFC 0019's generated editor): a page per
    /// top-level CLAP module path, a heading per module below that, and a tile per parameter with
    /// its name, its value as the plugin writes it, and a control. Stepped parameters with a few
    /// values get a list of them; two values, a switch; the rest a slider. Hidden parameters are
    /// left out; read-only ones show their value only. Values follow the engine as they change.
    class ParameterGrid : public juce::Component, private juce::Timer
    {
    public:
        ParameterGrid(Engine& engine, NodeId node, std::string name);
        ~ParameterGrid() override;
        NodeId node() const { return node_; }
        size_t pages() const { return pages_.size(); }
        void showPage(size_t page);

        void paint(juce::Graphics&) override;
        void resized() override;

    private:
        struct Page { std::string name; std::vector<size_t> params; };
        struct Tile
        {
            size_t param;
            juce::Rectangle<int> bounds;
            juce::Label* value = nullptr;
            juce::Slider* slider = nullptr;
            juce::ComboBox* list = nullptr;
            juce::Button* toggle = nullptr;
            double shown = std::numeric_limits<double>::quiet_NaN();
        };
        struct Heading { juce::String text; juce::Rectangle<int> bounds; };
        class Content;
        void timerCallback() override;
        void build();                   // the tiles of the page shown
        void show(Tile&, double value); // a value into a tile's control and text

        Engine& engine_;
        NodeId node_;
        std::string name_;
        std::vector<Engine::ParamInfo> params_;
        std::vector<Page> pages_;
        size_t page_ = 0;
        std::vector<std::unique_ptr<juce::Button>> pageButtons_;
        juce::Viewport viewport_;
        std::unique_ptr<Content> content_;
        std::vector<std::unique_ptr<juce::Component>> controls_;
        std::vector<Tile> tiles_;
        std::vector<Heading> headings_;
    };
}
