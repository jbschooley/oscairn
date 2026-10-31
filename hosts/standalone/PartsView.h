// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "Engine.h"
#include "PartsModel.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <future>

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

    private:
        void timerCallback() override;
        void rebuild();           // the controls for the current page and state
        const PartRow* part(uint32_t id) const;
        juce::Rectangle<int> rowBounds(size_t row) const;
        void insertMenu(size_t index, juce::Component& at, ChainPlace place = {});
        void devicesMenu(uint32_t part, juce::Component& at);
        void sceneControlsMenu(NodeId node, juce::Component& at);
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
        std::vector<Scene> scenes_;
        std::vector<SceneParameter> sceneParameters_;
        uint32_t scene_ = 0;
        std::vector<Macro> macros_;
        static constexpr uint32_t controlsPage = 0xFFFFFFFFu, mixerPage = 0xFFFFFFFEu;
        std::string shape_; // what the controls were built for; rebuilt when it changes
        uint32_t page_ = 0; // 0: the list; otherwise the part shown
        NodeId selected_ = 0;
        uint32_t selectedSplit_ = 0; int selectedLane_ = -1; // a split (lane -1) or one of its lanes
        int chainHeight_ = 96;     // the Part page's chain row: taller for a split's lanes

        juce::Label title_, message_;
        juce::TextButton done_{"Done"}, addPart_{"+ Add part"}, addBus_{"+ Add bus"}, back_{"‹ Parts"}, removePart_{"Remove part"}, controlsButton_{"Controls"}, mixerButton_{"Mixer"}, addControl_{"+ Control"}, undo_{"Undo"}, redo_{"Redo"};
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
        std::map<uint32_t, double> peaks_; // the parts' meters, read at each tick while the Mixer shows
    };
}
