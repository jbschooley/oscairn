// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "Engine.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace oscairn::standalone
{
    /// An envelope recognised from its parameters' names: the level it starts at, then its
    /// breakpoints in order, each reached after a time (or a rate, faster when higher) at a level.
    /// Indices are positions in the names given; -1 is none, with a fixed level instead. The
    /// sustain point holds until release.
    struct EnvelopeShape
    {
        struct Point { int time = -1, level = -1; double fixed = 0; bool rate = false, sustain = false; };
        int start = -1; double fixedStart = 0;
        std::vector<Point> points;
        bool empty() const { return points.empty(); }
    };
    // shortcut: recognised by name only (ADSR; DX7 rates and levels; FM-X times and levels), case-insensitive; replace with the plugins' own descriptions once oscairn.ui exists (RFC 0019).
    EnvelopeShape envelopeOf(const std::vector<std::string>& names);

    /// Every parameter of one plugin, as pages of tiles (RFC 0019's generated editor): a page per
    /// top-level CLAP module path; on a page of more than 24, tabs for the second level; a heading
    /// per module below that; and a tile per parameter with
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
        void showTab(size_t tab);
        size_t tabs() const { return tabNames_.size(); }
        size_t tilesShown() const { return tiles_.size(); }
        size_t envelopesShown() const { return panels_.size(); }
        // Any page's parameters whose name or module has the text; empty: the page again.
        void search(const juce::String& text) { search_.setText(text, false); search_.onTextChange(); }

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
        class Envelope;
        double normal(size_t param) const;               // its value, 0 to 1 over its range
        void setNormal(size_t param, double value);      // from 0 to 1, rounded when stepped
        void timerCallback() override;
        std::vector<std::string> tabsFor(size_t page) const; // second-level modules, when a page needs them
        void refresh();                 // tabs, layout and tiles for the page, tab and search shown
        void build();                   // the tiles shown
        void show(Tile&, double value); // a value into a tile's control and text

        Engine& engine_;
        NodeId node_;
        std::string name_;
        std::vector<Engine::ParamInfo> params_;
        std::vector<Page> pages_;
        size_t page_ = 0, tab_ = 0;
        bool stale_ = false; // built while the engine was busy: value texts were placeholders
        std::vector<std::string> tabNames_;
        std::vector<std::unique_ptr<juce::Button>> pageButtons_, tabButtons_;
        juce::TextEditor search_;
        juce::Viewport viewport_;
        std::unique_ptr<Content> content_;
        std::vector<std::unique_ptr<juce::Component>> controls_;
        std::vector<Tile> tiles_;
        std::vector<Heading> headings_;
        std::vector<juce::Rectangle<int>> panels_;       // wide tiles: envelopes
    };
}
