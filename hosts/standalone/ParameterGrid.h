// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "Engine.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace oscairn::standalone
{
    /// An envelope recognized from its parameters' names: the level it starts at, then its
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
    // shortcut: recognized by name only (ADSR; DX7 rates and levels; FM-X times and levels), case-insensitive; replace with the plugins' own descriptions once oscairn.ui exists (RFC 0019).
    EnvelopeShape envelopeOf(const std::vector<std::string>& names);

    /// An LFO recognized from its parameters' names: its wave and its speed, and a delay, fade-in,
    /// phase and user steps when it has them. Indices as for EnvelopeShape.
    struct LfoShape { int wave = -1, speed = -1, delay = -1, fade = -1, phase = -1; std::vector<int> steps; bool empty() const { return wave < 0 || speed < 0; } };
    // shortcut: by name only (Wave, Type or Shape with Speed, Rate or Frequency), as envelopeOf.
    LfoShape lfoOf(const std::vector<std::string>& names);
    // The waveform a wave's value text names: sine, triangle, square, saw up or down, random (sample and hold, noise), steps (user), or none.
    enum class Wave { None, Sine, Triangle, Square, SawUp, SawDown, Random, Steps };
    Wave waveNamed(const juce::String& text);

    /// A filter recognized from its parameters' names: Cutoff and Resonance, and its Type when it
    /// has one, whose value text gives the response (FilterResponse).
    struct FilterShape { int cutoff = -1, resonance = -1, type = -1; bool empty() const { return cutoff < 0 || resonance < 0; } };
    // The parameters a recognized envelope, LFO or filter uses, in the order their tiles go when
    // linked (Attack, Decay, Sustain, Release; a DX7's rates, then its levels).
    std::vector<int> linkedOrder(const EnvelopeShape&);
    std::vector<int> linkedOrder(const struct LfoShape&);
    std::vector<int> linkedOrder(const FilterShape&);
    // shortcut: by name only, as envelopeOf.
    FilterShape filterOf(const std::vector<std::string>& names);
    // A filter type's name as a response: "LP 24 dB" or "LPF24D" a 4-pole low-pass; "Off" or "Thru" none.
    struct FilterResponse { enum class Kind { None, LowPass, HighPass, BandPass, Notch, Unknown } kind = Kind::LowPass; int poles = 2; };
    FilterResponse filterNamed(const juce::String& text);

    /// A plugin's generated editor description (RFC 0019), which every surface draws: a page per
    /// top-level CLAP module path ("Main" for parameters without one), in the plugin's order except
    /// that numbered pages of one kind sit together in number order (Operator 1 to 8). `params`
    /// are indices into the parameters given.
    struct EditorPage { std::string name; std::vector<size_t> params; };
    std::vector<EditorPage> editorPages(const std::vector<Engine::ParamInfo>& params);
    // Parameters in the plugin's priority order (oscairn.ui): numbered first, then the rest in its order.
    void byPriority(const std::vector<Engine::ParamInfo>& params, std::vector<size_t>& indices);

    /// Every parameter of one plugin, as pages of tiles (RFC 0019's generated editor), drawn with
    /// the controls and in the priority order the plugin's oscairn.ui hints ask for: a page per
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
        std::string pageName(size_t page) const { return page < pages_.size() ? pages_[page].name : std::string(); }
        void showPage(size_t page);
        void showTab(size_t tab);
        size_t tabs() const { return tabNames_.size(); }
        size_t tilesShown() const { return tiles_.size(); }
        std::vector<std::string> namesShown() const { std::vector<std::string> n; for(const auto& t : tiles_) n.push_back(params_[t.param].name); return n; }
        size_t panelsShown() const { return panels_.size(); } // envelopes and LFOs
        // Any page's parameters whose name or module has the text; empty: the page again.
        void search(const juce::String& text) { search_.setText(text, false); search_.onTextChange(); }

        void paint(juce::Graphics&) override;
        void resized() override;
        void mouseDown(const juce::MouseEvent&) override; // a tap on a tile selects its row or column
        // The line of tiles the device's knobs play (RFC 0019): a row, left to right, when the
        // knobs sit above or below the screen; a column, top to bottom, when they sit beside it.
        void selectLine(const juce::Rectangle<int>& tile);
        std::vector<std::string> rowShown() const { std::vector<std::string> n; for(const auto& r : engine_.row()) n.push_back(r.name); return n; }

    private:
        class Choices;
        void publishRow();               // the selected line to the engine, for "row:N" knobs
        bool columns() const;            // the knobs sit beside the screen: they play a column
        bool inLine(const juce::Rectangle<int>& tile) const { return (columns() ? tile.getX() : tile.getY()) == line_; }
        juce::Rectangle<int> strip_;     // names what each of the device's row knobs plays
        int line_ = -1;                  // the selected row's top, or column's left, in the content
        struct Tile
        {
            size_t param;
            juce::Rectangle<int> bounds;
            juce::Label* value = nullptr;
            juce::Slider* slider = nullptr;
            juce::ComboBox* list = nullptr;
            juce::Button* toggle = nullptr;
            Choices* choices = nullptr;
            juce::Slider* range = nullptr;   // a two-sided range: this parameter its low end
            size_t high = 0;                 // and this one (an index into params_) its high
            double shownHigh = std::numeric_limits<double>::quiet_NaN();
            bool linked = false; // drawn inside its group's card, a line to its right if one follows
            double shown = std::numeric_limits<double>::quiet_NaN();
        };
        struct Heading { juce::String text; juce::Rectangle<int> bounds; };
        class Content;
        class Envelope;
        class Lfo;
        class Filter;
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
        std::vector<EditorPage> pages_;
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
        std::vector<juce::Rectangle<int>> links_;        // each row of linked tiles: one card
        size_t linkedRuns_ = 0;
    public:
        size_t linkedShown() const { return linkedRuns_; } // groups of tiles drawn joined
    private:
    };
}
