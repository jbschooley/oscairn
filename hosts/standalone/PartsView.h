// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "Engine.h"
#include "PartsModel.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <future>
#include <set>

namespace oscairn::standalone
{
    /// The Parts and Part views (RFC 0011). Parts lists every part (where it plays from, its
    /// chain, level, mute and sends) and the shared buses under them; tapping a part opens its
    /// chain, zone, output and sends. Every edit is heard at once; nothing needs wiring. Drawn
    /// over the main window, so it works on a device's single screen too.
    class PartsView : public juce::Component, private juce::Timer
    {
    public:
        explicit PartsView(Engine& engine, const std::string& scanner);
        ~PartsView() override;
        std::function<void()> onDone;

        void paint(juce::Graphics&) override;
        void resized() override;
        void mouseUp(const juce::MouseEvent&) override;
        void refresh(); // reads the engine's graph state
        void showPart(uint32_t id); // 0: the list
        void showLane(uint32_t split, int lane); // selects a split (lane -1) or one of its lanes on the part shown
        void selectBox(NodeId node);             // selects a plugin on the part shown: its actions, and on the desktop its parameters
        void showRouting(NodeId node, uint32_t port = 0); // Routing, with that node (0: none) selected
        // Inside the desktop's window (RFC 0019): the window has the tabs, undo and the parts
        // list, so this view leaves out its own.
        void setEmbedded(bool embedded) { embedded_ = embedded; shape_.clear(); refresh(); }
        uint32_t page() const { return page_; }
        static constexpr uint32_t controlsPage = 0xFFFFFFFFu, mixerPage = 0xFFFFFFFEu, routingPage = 0xFFFFFFFDu;

    private:
        void timerCallback() override;
        void rebuild();           // the controls for the current page and state
        const PartRow* part(uint32_t id) const;
        juce::Rectangle<int> rowBounds(size_t row) const;
        void insertMenu(size_t index, juce::Component& at, ChainPlace place = {});
        void devicesMenu(uint32_t part, juce::Component& at);
        void sceneControlsMenu(NodeId node, juce::Component& at);
        void sceneScopeMenu(juce::Component& at);
        void sceneTransitionMenu(juce::Component& at);
        void mappingScenesMenu(uint32_t macro, size_t target, juce::Component& at);
        void targetMenu(uint32_t macro, juce::Component& at);
        std::string describe(const MacroTarget&) const;
        void setTargets(uint32_t macro, std::function<void(std::vector<MacroTarget>&)> change);
        Control::GraphState graph_;
        template<class F> void edit(F&& change) { engine_.edit(std::forward<F>(change)); }

        Engine& engine_;
        std::future<std::vector<PluginChoice>> scan_;
        std::vector<PluginChoice> plugins_;
        PartsState state_;
        std::vector<MidiEndpoint> endpoints_; // the host's MIDI inputs
        std::vector<Control::SessionStatus::PluginFailure> failures_; // plugins that failed and are silent
        bool failed(NodeId node) const { return std::any_of(failures_.begin(), failures_.end(), [&](const auto& f) { return f.node == node; }); }
        std::vector<Scene> scenes_;
        std::vector<SceneParameter> untouched_; // parameters scenes leave alone
        uint32_t scene_ = 0;
        std::vector<Macro> macros_;
        bool embedded_ = false;
        std::string shape_; // what the controls were built for; rebuilt when it changes
        uint32_t page_ = 0; // 0: the list; otherwise the part shown
        NodeId selected_ = 0;
        uint32_t selectedSplit_ = 0; int selectedLane_ = -1; // a split (lane -1) or one of its lanes
        int chainHeight_ = 96;     // the Part page's chain row: taller for a split's lanes

        juce::Label title_, message_;
        juce::TextButton done_{"Done"}, addPart_{"+ Add part"}, addBus_{"+ Add bus"}, back_{"‹ Parts"}, removePart_{"Remove part"}, controlsButton_{"Controls"}, mixerButton_{"Mixer"}, routingButton_{"Routing"}, addControl_{"+ Control"}, undo_{"Undo"}, redo_{"Redo"};
        juce::TextEditor name_;
        std::vector<std::unique_ptr<juce::Component>> controls_; // rebuilt per page and shape
        std::vector<std::shared_ptr<std::function<void()>>> updaters_; // set controls from the state
        struct Box { NodeId node; juce::Rectangle<int> bounds; };
        std::vector<Box> boxes_; // the Part page's chain boxes, for tapping
        struct SplitArea { uint32_t split; juce::Rectangle<int> bounds, splitPill, joinPill; std::vector<juce::Rectangle<int>> lanes; };
        std::vector<SplitArea> splitAreas_;
        // The Mixer: a strip per part and bus, with its meter beside the fader.
        struct Strip { uint32_t part; juce::Rectangle<int> bounds, meter; };
        std::vector<Strip> strips_;
        std::vector<juce::Rectangle<int>> tiles_; // the Part page's settings on the desktop, a tile each
        std::unique_ptr<class ParameterGrid> grid_; // the selected plugin's parameters, on the desktop
        std::map<uint32_t, std::vector<double>> peaks_; // the parts' meters per channel, read at each tick while the Mixer shows
        // Routing: the graph laid out by signal flow (layoutRouting), drawn at routingOrigin_ and
        // routingScale_; the plugin or part chosen; whether parts are collapsed (the user's
        // preference, kept beside the theme's) and which are expanded meanwhile.
        RoutingState routing_;
        RoutingLayout layout_;
        juce::Point<float> routingOrigin_; float routingScale_ = 1;
        NodeId routed_ = 0; uint32_t routedPort_ = 0, routedPart_ = 0;
        bool collapsedParts_ = true;
        std::set<uint32_t> expandedParts_;
        bool wireMeters_ = false; std::map<uint32_t, double> wirePeaks_; // wires colored by level, read at each tick while on
        void connectMenu(WireKind kind, juce::Component& at);
        void addLooseMenu(juce::Component& at);
    };
}
