// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PartsView.h"
#include "Icons.h"
#include "ParameterGrid.h"
#include "ZoneEditor.h"
#include "Theme.h"

namespace oscairn::standalone
{
    namespace
    {
        juce::Colour roleColor(const std::string& role)
        {
            if(role == "Instrument") return juce::Colour(0xff8cc2ff);
            if(role == "Effect") return juce::Colour(0xff6fd6e3);
            return juce::Colour(0xffc9b2ff); // notes and sequencers
        }
        juce::String text(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }
        constexpr int header = 56, columnTitles = 28, rowHeight = 60;
        // Whether Routing shows parts collapsed: the user's, beside the theme selection.
        juce::File routingPreference()
        {
            auto* theme = dynamic_cast<Theme*>(&juce::LookAndFeel::getDefaultLookAndFeel());
            return theme ? theme->configuration().getChildFile("routing-view.txt") : juce::File();
        }
    }

    PartsView::PartsView(Engine& engine, const std::string& scanner) : engine_(engine)
    {
        for(auto* c : std::initializer_list<juce::Component*>{&title_, &message_, &done_, &addPart_, &addBus_, &back_, &removePart_, &name_, &controlsButton_, &mixerButton_, &routingButton_, &addControl_, &undo_, &redo_}) addChildComponent(c);
        routingButton_.setTooltip("Every plugin and wire: draw wires the parts cannot, and set each source's level");
        routingButton_.onClick = [this] { showRouting(0); };
        if(const auto f = routingPreference(); f.existsAsFile()) collapsedParts_ = f.loadFileAsString().trim() != "expanded";
        addPart_.setButtonText("+ Part"); addBus_.setButtonText("+ Bus");
        undo_.onClick = [this] { edit([](Control& c) { c.undo(); }); };
        redo_.onClick = [this] { edit([](Control& c) { c.redo(); }); };
        mixerButton_.setTooltip("Every part and bus side by side: level, mute, solo, sends and meters");
        mixerButton_.onClick = [this] { showPart(mixerPage); };
        controlsButton_.setTooltip("Controls: knobs that drive many parameters, each with its own range and curve, or other controls");
        controlsButton_.onClick = [this] { showPart(controlsPage); };
        addControl_.onClick = [this] { const auto n = macros_.size() + 1; edit([n](Control& c) { c.addMacro("Control " + std::to_string(n)); }); };
        title_.setFont(juce::FontOptions(22.0f * themeData().fontScale, juce::Font::bold));
        back_.setButtonText(juce::String::fromUTF8("‹ Parts"));
        name_.setFont(juce::FontOptions(18.0f * themeData().fontScale, juce::Font::bold));
        name_.setJustification(juce::Justification::centredLeft);
        name_.setTooltip("The part's name");
        message_.setColour(juce::Label::textColourId, ink(Ink::Danger));
        done_.onClick = [this] { if(onDone) onDone(); };
        save_.onClick = [this] { if(onSave) onSave(); };
        save_.setTooltip("Save the Performance to the library");
        addChildComponent(save_);
        addPart_.onClick = [this] { const auto n = state_.parts.size() + 1; edit([n](Control& c) { addPart(c, "Part " + std::to_string(n), false); }); };
        addBus_.onClick = [this] { const auto n = state_.buses.size(); edit([n](Control& c) { addPart(c, std::string("Bus ") + char('A' + n % 26), true); }); };
        back_.onClick = [this] { page_ = 0; selected_ = 0; selectedSplit_ = 0; selectedLane_ = -1; shape_.clear(); refresh(); };
        removePart_.onClick = [this] {
            const auto id = page_;
            edit([id](Control& c) { c.removePart(id); c.commit(); });
            page_ = 0; shape_.clear();
        };
        name_.onReturnKey = name_.onFocusLost = [this] {
            const auto id = page_; const auto name = name_.getText().toStdString();
            if(const auto* p = part(id); p && p->name != name) edit([id, name](Control& c) { c.setPartName(id, name); c.commit(); });
        };
        // The plugins a part can add, found by the isolated scanner.
        scan_ = std::async(std::launch::async, [dir = engine.pluginDir(), scanner] { try { return bundledPlugins(dir, scanner); } catch(const std::exception&) { return std::vector<PluginChoice>{}; } });
        refresh();
        startTimer(200);
    }

    PartsView::~PartsView() { stopTimer(); if(scan_.valid()) scan_.wait(); }

    void PartsView::timerCallback()
    {
        if(scan_.valid() && scan_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) plugins_ = scan_.get();
        // The Mixer's meters move at 20 frames a second; the other pages refresh more slowly.
        if(page_ == mixerPage) { peaks_.clear(); for(const auto& m : engine_.meters()) peaks_[m.part] = m.channels; }
        const bool metering = page_ == routingPage && wireMeters_;
        if(metering) { wirePeaks_.clear(); for(const auto& m : engine_.wireMeters()) wirePeaks_[m.wire] = m.peak; repaint(); }
        const int interval = page_ == mixerPage || metering ? 50 : 200;
        if(getTimerInterval() != interval) startTimer(interval);
        // The open Performance's name, for the parts list's title.
        if(const auto info = engine_.performanceInfo(); info && juce::String::fromUTF8(info->name.c_str()) != performance_)
        {
            performance_ = juce::String::fromUTF8(info->name.c_str());
            if(page_ == 0) title_.setText(performance_.isEmpty() ? juce::String("Parts") : performance_, juce::dontSendNotification);
        }
        refresh();
    }

    const PartRow* PartsView::part(uint32_t id) const
    {
        for(const auto* list : {&state_.parts, &state_.buses}) for(const auto& p : *list) if(p.id == id) return &p;
        return nullptr;
    }

    void PartsView::showPart(uint32_t id) { page_ = id; selected_ = 0; selectedSplit_ = 0; selectedLane_ = -1; shape_.clear(); refresh(); }
    void PartsView::showRouting(NodeId node, uint32_t port) { page_ = routingPage; selected_ = 0; routed_ = node; routedPort_ = port; routedPart_ = 0; shape_.clear(); refresh(); }
    void PartsView::selectBox(NodeId node) { selected_ = node; selectedSplit_ = 0; selectedLane_ = -1; shape_.clear(); refresh(); }
    void PartsView::showLane(uint32_t split, int lane) { selected_ = 0; selectedSplit_ = split; selectedLane_ = lane; shape_.clear(); refresh(); }

    void PartsView::refresh()
    {
        try
        {
            graph_ = engine_.graph();
            state_ = readParts(graph_); routing_ = readRouting(graph_);
            const auto status = engine_.status(); endpoints_ = status.midiInputs; failures_ = status.failures; scenes_ = graph_.scenes; untouched_ = graph_.untouched; scene_ = graph_.scene; macros_ = graph_.macros;
        }
        catch(const std::exception&) { return; }
        if(page_ && page_ != controlsPage && page_ != mixerPage && page_ != routingPage && !part(page_)) { page_ = 0; shape_.clear(); }
        if(page_ == routingPage && routed_ && !routing_.find(routed_, routedPort_)) routed_ = 0;
        if(page_ == routingPage && routedPart_ && !part(routedPart_)) routedPart_ = 0;
        for(auto& p : state_.parts) if(p.notes && !p.devices.empty() && !resolveDevices(p.devices, endpoints_)) p.input = "No device: " + p.input;
        // An edit that failed, or else a plugin that failed: outlined in red wherever it shows.
        std::string message = engine_.editError();
        if(message.empty() && !failures_.empty()) message = failures_.front().name + " is silent: " + failures_.front().reason;
        message_.setText(text(message), juce::dontSendNotification);
        // Undo and redo name what they would do.
        const auto history = engine_.history();
        undo_.setEnabled(!history.undo.empty()); redo_.setEnabled(!history.redo.empty());
        undo_.setTooltip(history.undo.empty() ? juce::String("Nothing to undo") : "Undo " + text(history.undo));
        redo_.setTooltip(history.redo.empty() ? juce::String("Nothing to redo") : "Redo " + text(history.redo));
        // Controls are rebuilt only when the structure changes, so a slider being dragged survives.
        std::string shape = std::to_string(page_) + "/" + std::to_string(selected_) + "/" + std::to_string(getWidth()) + "x" + std::to_string(getHeight());
        for(const auto* list : {&state_.parts, &state_.buses})
        {
            std::function<void(const std::vector<PartBox>&)> boxes = [&](const std::vector<PartBox>& chain) {
                for(const auto& b : chain)
                {
                    shape += "," + std::to_string(b.node) + "/" + std::to_string(b.split);
                    for(const auto& l : b.lanes) { shape += "["; boxes(l.boxes); shape += "]"; }
                }
            };
            for(const auto& p : *list) { shape += "|" + std::to_string(p.id) + ":" + p.name + ":" + std::to_string(p.through) + ":" + devicesName(p.devices); boxes(p.chain); }
        }
        shape += "|selected:" + std::to_string(selectedSplit_) + "/" + std::to_string(selectedLane_);
        if(page_ == routingPage)
        {
            shape += "|routing:" + std::to_string(routed_) + "/" + std::to_string(routedPort_) + "/" + std::to_string(routedPart_) + "/" + std::to_string(collapsedParts_);
            for(const auto p : expandedParts_) shape += "+" + std::to_string(p);
            for(const auto& n : routing_.nodes) shape += "," + std::to_string(n.node) + ":" + std::to_string(n.port) + ":" + n.name;
            for(const auto& w : routing_.wires) shape += ";" + std::to_string(w.id) + ":" + std::to_string(w.from) + ">" + std::to_string(w.to);
        }
        shape += "|off:";
        for(const auto n : graph_.disabled) shape += std::to_string(n) + ",";
        if(page_ == routingPage) for(const auto& w : graph_.wires) if(!w.enabled) shape += "w" + std::to_string(w.id) + ",";
        shape += "|scenes:" + std::to_string(scene_);
        for(const auto& s : scenes_) shape += "," + std::to_string(s.id) + ":" + s.name + ":" + std::to_string(s.scope) + ":" + std::to_string(s.glide);
        shape += "|controls";
        for(const auto& m : macros_) { shape += "," + std::to_string(m.id) + ":" + m.name + ":" + std::to_string(m.targets.size()); for(const auto& t : m.targets) { shape += ":" + describe(t) + std::to_string(int(t.curve)) + std::to_string(t.relative); for(const auto s : t.offIn) shape += "/" + std::to_string(s); } }
        if(shape != shape_) { shape_ = shape; rebuild(); }
        for(const auto& update : updaters_) (*update)();
        repaint();
    }

    juce::Rectangle<int> PartsView::rowBounds(size_t row) const
    {
        const int gap = row >= state_.parts.size() ? 36 : 0; // a heading above the buses
        return {16, header + columnTitles + int(row) * rowHeight + gap, getWidth() - 32, rowHeight - 6};
    }

    void PartsView::rebuild()
    {
        controls_.clear(); boxes_.clear(); splitAreas_.clear(); strips_.clear(); updaters_.clear(); tiles_.clear();
        auto grid = std::move(grid_); // kept below if the same plugin is still selected, so its page survives
        const bool list = page_ == 0, controlsShown = page_ == controlsPage, mixerShown = page_ == mixerPage, routingShown = page_ == routingPage;
        for(auto* c : std::initializer_list<juce::Component*>{&addPart_, &addBus_, &controlsButton_, &mixerButton_, &routingButton_}) c->setVisible(list);
        for(auto* c : std::initializer_list<juce::Component*>{&removePart_, &name_}) c->setVisible(!list && !controlsShown && !mixerShown && !routingShown);
        back_.setVisible(!list); addControl_.setVisible(controlsShown);
        title_.setVisible(list || controlsShown || mixerShown || routingShown); done_.setVisible(true); message_.setVisible(true);
        title_.setText(controlsShown ? "Controls" : mixerShown ? "Mixer" : routingShown ? "Routing" : performance_.isEmpty() ? juce::String("Parts") : performance_, juce::dontSendNotification);
        auto top = getLocalBounds().reduced(16, 8).removeFromTop(header - 8);
        // On the desktop with a mouse, rows are tighter and text follows the type scale (RFC 0019).
        const int mode = desktopMode(*this), lineHeight = mode == 1 ? 34 : 40;
        name_.setFont(juce::FontOptions(mode ? textSize(TextRole::Title, mode) : 18.0f * themeData().fontScale, juce::Font::bold));
        done_.setBounds(top.removeFromRight(80).reduced(2));
        redo_.setBounds(top.removeFromRight(60).reduced(2)); undo_.setBounds(top.removeFromRight(60).reduced(2));
        undo_.setVisible(true); redo_.setVisible(true);
        save_.setVisible(onSave != nullptr && !embedded_);
        if(save_.isVisible()) save_.setBounds(top.removeFromRight(70).reduced(2));
        // Inside the desktop's window, which has its own tabs, undo and parts list.
        if(embedded_) for(auto* c : std::initializer_list<juce::Component*>{&done_, &back_, &undo_, &redo_, &title_, &addPart_, &addBus_, &controlsButton_, &mixerButton_, &routingButton_}) c->setVisible(false);
        message_.setBounds(getLocalBounds().reduced(16).removeFromBottom(24));

        // A slider or toggle that follows the state unless it is being used.
        auto slider = [this](juce::Rectangle<int> at, double min, double max, std::function<double()> value, std::function<void(double)> set) {
            auto s = std::make_unique<juce::Slider>(juce::Slider::LinearHorizontal, juce::Slider::NoTextBox);
            s->setRange(min, max); s->setValue(value(), juce::dontSendNotification);
            s->onValueChange = [s = s.get(), set] { set(s->getValue()); };
            s->setBounds(at); addAndMakeVisible(*s);
            auto* raw = s.get();
            auto update = std::make_shared<std::function<void()>>([raw, value] { if(!raw->isMouseButtonDown()) raw->setValue(value(), juce::dontSendNotification); });
            controls_.push_back(std::move(s));
            return std::pair{raw, update};
        };
        std::vector<std::shared_ptr<std::function<void()>>> updates;
        auto toggle = [this, &updates](juce::Rectangle<int> at, const juce::String& label, std::function<bool()> value, std::function<void(bool)> set) {
            // Mute and solo are the same marks everywhere; other toggles are checkboxes.
            std::unique_ptr<juce::Button> t;
            if(label == "Mute" || label == "M") { t = std::make_unique<MarkButton>("M", Ink::Danger, "Mute"); at = at.withSizeKeepingCentre(std::min(at.getHeight(), 24), std::min(at.getHeight(), 24)); }
            else if(label == "Solo" || label == "S") { t = std::make_unique<MarkButton>("S", Ink::Accent, "Solo"); at = at.withSizeKeepingCentre(std::min(at.getHeight(), 24), std::min(at.getHeight(), 24)); }
            else t = std::make_unique<juce::ToggleButton>(label);
            t->setToggleState(value(), juce::dontSendNotification);
            t->onClick = [t = t.get(), set] { set(t->getToggleState()); };
            t->setBounds(at); addAndMakeVisible(*t);
            auto* raw = t.get();
            updates.push_back(std::make_shared<std::function<void()>>([raw, value] { raw->setToggleState(value(), juce::dontSendNotification); }));
            controls_.push_back(std::move(t));
        };
        auto button = [this](juce::Rectangle<int> at, const juce::String& label, std::function<void()> click) {
            auto b = std::make_unique<juce::TextButton>(label);
            b->onClick = std::move(click); b->setBounds(at); addAndMakeVisible(*b);
            auto* raw = b.get(); controls_.push_back(std::move(b)); return raw;
        };
        // Keys (and velocities): a keyboard to drag the range on, following the state.
        auto zoneEditor = [&](juce::Rectangle<int> at, std::function<std::array<int, 4>()> range, std::function<void(int, int)> keys, std::function<void(int, int)> velocities) {
            auto z = std::make_unique<ZoneEditor>();
            z->onKeys = std::move(keys); z->onVelocities = std::move(velocities);
            const auto r = range(); z->set(r[0], r[1], r[2], r[3]);
            z->setBounds(at); addAndMakeVisible(*z);
            auto* raw = z.get(); controls_.push_back(std::move(z));
            updates.push_back(std::make_shared<std::function<void()>>([raw, range] { const auto r = range(); raw->set(r[0], r[1], r[2], r[3]); }));
        };
        auto plusButton = [this](juce::Rectangle<int> at) {
            auto b = std::make_unique<IconButton>(IconButton::Glyph::Plus, "Insert");
            b->setBounds(at); addAndMakeVisible(*b);
            auto* raw = b.get(); controls_.push_back(std::move(b)); return raw;
        };
        auto levelOf = [this](uint32_t id) { return [this, id] { const auto* p = part(id); return p ? p->level : 0.0; }; };
        auto muteOf = [this](uint32_t id) { return [this, id] { const auto* p = part(id); return p && p->mute; }; };
        auto setLevel = [this](uint32_t id) { return [this, id](double v) { const auto* p = part(id); const bool m = p && p->mute; edit([id, v, m](Control& c) { c.setPartLevel(id, v, m); c.commit(); }); }; };
        auto setMute = [this](uint32_t id) { return [this, id](bool m) { const auto* p = part(id); const double v = p ? p->level : 1; edit([id, v, m](Control& c) { c.setPartLevel(id, v, m); c.commit(); }); }; };
        auto sendOf = [this](uint32_t id, uint32_t bus) { return [this, id, bus] { const auto* p = part(id); if(!p) return 0.0; const auto s = p->sends.find(bus); return s == p->sends.end() ? 0.0 : s->second; }; };
        auto setSend = [this](uint32_t id, uint32_t bus) { return [this, id, bus](double v) { edit([id, bus, v](Control& c) { setSendLevel(c, id, bus, v); }); }; };

        if(list)
        {
            addBus_.setBounds(top.removeFromRight(70).reduced(2));
            addPart_.setBounds(top.removeFromRight(70).reduced(2));
            routingButton_.setBounds(top.removeFromRight(76).reduced(2));
            controlsButton_.setBounds(top.removeFromRight(84).reduced(2));
            mixerButton_.setBounds(top.removeFromRight(66).reduced(2));
            title_.setBounds(top.removeFromLeft(90));
            // Scenes: tap one to switch to it; Store saves the parts' faders and the scene
            // parameters into the selected one.
            const auto sceneWidth = std::min(110, std::max(56, (top.getWidth() - 150) / std::max(1, int(scenes_.size()))));
            for(const auto& s : scenes_)
            {
                auto* b = button(top.removeFromLeft(sceneWidth).reduced(2), text(s.name), [this, id = s.id] { edit([id](Control& c) { c.selectScene(id); }); });
                b->setColour(juce::TextButton::buttonColourId, s.id == scene_ ? ink(Ink::Accent) : ink(Ink::Raised));
                b->setTooltip("Switch to this scene");
            }
            button(top.removeFromLeft(80).reduced(2), "+ Scene", [this] { const auto n = scenes_.size() + 1; edit([n](Control& c) { c.addScene("Scene " + std::to_string(n)); }); })
                ->setTooltip("A new scene with everything as it is now: plugins and wires on or off, mutes, the mix, parameters and controls");
            if(scene_)
            {
                button(top.removeFromLeft(66).reduced(2), "Store", [this] { const auto id = scene_; edit([id](Control& c) { c.storeScene(id); }); })
                    ->setTooltip("Save everything as it is now into the selected scene");
                auto* scope = button(top.removeFromLeft(80).reduced(2), "Scope...", {});
                scope->setTooltip("What switching to the selected scene changes; the rest stays as it is");
                scope->onClick = [this, scope] { sceneScopeMenu(*scope); };
                auto* transition = button(top.removeFromLeft(110).reduced(2), "Transition...", {});
                transition->setTooltip("How switching to the selected scene moves there: how long it glides, along which curve, and whether it waits for the beat or bar");
                transition->onClick = [this, transition] { sceneTransitionMenu(*transition); };
            }
            size_t row = 0;
            for(const auto* rows : {&state_.parts, &state_.buses})
                for(const auto& p : *rows)
                {
                    auto r = rowBounds(row++);
                    // The same columns on every row: level, mute, then a send per bus (blank on buses).
                    auto right = r.removeFromRight(160 + 92 * int(state_.buses.size()));
                    for(const auto& b : state_.buses)
                    {
                        auto cell = right.removeFromRight(92);
                        if(!p.bus) updates.push_back(slider(cell.reduced(6, 18), 0, 1, sendOf(p.id, b.id), setSend(p.id, b.id)).second);
                    }
                    toggle(right.removeFromRight(64).reduced(4, 14), "Mute", muteOf(p.id), setMute(p.id));
                    updates.push_back(slider(right.reduced(6, 18), 0, 1, levelOf(p.id), setLevel(p.id)).second);
                }
        }
        else if(routingShown)
        {
            back_.setBounds(top.removeFromLeft(110).reduced(2));
            title_.setBounds(top.removeFromLeft(110).reduced(8, 0));
            auto* add = button(top.removeFromLeft(100).reduced(2), "+ Plugin", {});
            add->setTooltip("A plugin outside the parts, to wire by hand");
            add->onClick = [this, add] { addLooseMenu(*add); };
            toggle(top.removeFromLeft(160).reduced(4, 8), "Parts collapsed", [this] { return collapsedParts_; }, [this](bool on) {
                collapsedParts_ = on; expandedParts_.clear();
                if(const auto f = routingPreference(); f != juce::File()) { f.getParentDirectory().createDirectory(); f.replaceWithText(on ? "collapsed\n" : "expanded\n"); }
                shape_.clear(); refresh(); });
            toggle(top.removeFromLeft(110).reduced(4, 8), "Meters", [this] { return wireMeters_; }, [this](bool on) {
                wireMeters_ = on; wirePeaks_.clear(); repaint(); });
            // The graph above, laid out by signal flow and fitted to the space; the chosen plugin's
            // or part's wiring below.
            auto area = getLocalBounds().reduced(16, 0).withTrimmedTop(header).withTrimmedBottom(28);
            auto panel = area.removeFromBottom(190);
            const auto canvas = area.reduced(8).toFloat();
            layout_ = layoutRouting(graph_, collapsedParts_, expandedParts_);
            routingScale_ = std::min({1.0f, canvas.getWidth() / float(std::max(1, layout_.width)), canvas.getHeight() / float(std::max(1, layout_.height))});
            routingOrigin_ = {canvas.getX(), canvas.getY() + (canvas.getHeight() - layout_.height * routingScale_) / 2};
            const auto* chosen = routed_ || routedPort_ ? routing_.find(routed_, routedPort_) : nullptr;
            if(routedPart_)
            {
                const auto* p = part(routedPart_);
                auto head = panel.removeFromTop(36);
                auto l = std::make_unique<juce::Label>(juce::String(), p ? text(p->name) : juce::String());
                l->setFont(juce::FontOptions(15.0f * themeData().fontScale, juce::Font::bold));
                l->setBounds(head.removeFromLeft(240)); addAndMakeVisible(*l); controls_.push_back(std::move(l));
                button(head.removeFromLeft(110).reduced(2), "Open part", [this, id = routedPart_] { showPart(id); });
                if(collapsedParts_)
                {
                    const bool open = expandedParts_.count(routedPart_) != 0;
                    button(head.removeFromLeft(110).reduced(2), open ? "Collapse" : "Expand", [this, id = routedPart_, open] {
                        if(open) expandedParts_.erase(id); else expandedParts_.insert(id);
                        shape_.clear(); refresh(); });
                }
            }
            else if(!chosen)
            {
                auto l = std::make_unique<juce::Label>(juce::String(), "Tap a plugin to wire it, or to set the level of each source into it.");
                l->setBounds(panel.removeFromTop(30)); addAndMakeVisible(*l); controls_.push_back(std::move(l));
            }
            else
            {
                const auto node = chosen->node;
                auto head = panel.removeFromTop(36);
                auto l = std::make_unique<juce::Label>(juce::String(), text(chosen->name) + (chosen->part ? juce::String::fromUTF8("  ·  ") + text(chosen->partName) : juce::String()));
                l->setFont(juce::FontOptions(15.0f * themeData().fontScale, juce::Font::bold));
                l->setBounds(head.removeFromLeft(240)); addAndMakeVisible(*l); controls_.push_back(std::move(l));
                const Graph::NodeInfo* info = nullptr;
                for(const auto& n : graph_.topology.nodes) if(n.id == node) info = &n;
                const bool sendsAudio = node == graphInput ? chosen->port != notesPort : info && !info->outputs.empty();
                const bool sendsNotes = node == graphInput ? chosen->port == notesPort : info && info->noteOutputs;
                if(sendsAudio) { auto* b = button(head.removeFromLeft(150).reduced(2), "Connect audio to...", {}); b->onClick = [this, b] { connectMenu(WireKind::Audio, *b); }; }
                if(sendsNotes) { auto* b = button(head.removeFromLeft(150).reduced(2), "Connect notes to...", {}); b->onClick = [this, b] { connectMenu(WireKind::Notes, *b); }; }
                if(node != graphInput && node != graphOutput)
                {
                    const bool off = graph_.disabled.count(node) != 0;
                    button(head.removeFromLeft(90).reduced(2), off ? "Turn on" : "Turn off", [this, node, off] { edit([node, off](Control& c) { c.setNodeEnabled(node, off); c.commit(); }); })
                        ->setTooltip(off ? "Switch this plugin on again" : "Switch this plugin off: its held notes and tail ring out");
                }
                if(chosen->part)
                    button(head.removeFromLeft(110).reduced(2), "Open part", [this, part = chosen->part] { showPart(part); });
                else if(node != graphInput && node != graphOutput)
                    button(head.removeFromLeft(90).reduced(2), "Remove", [this, node] { routed_ = 0; edit([node](Control& c) { removeLoose(c, node); }); })
                        ->setTooltip("Remove this plugin; its sources are wired to its destinations");
                if(node != graphInput && node != graphOutput && engine_.hasPluginWindow(node))
                {
                    const auto title = chosen->name + (chosen->part ? " · " + chosen->partName : std::string());
                    button(head.removeFromLeft(170).reduced(2), "Open plugin window", [this, node, title] { engine_.openPluginWindow(node, title); })
                        ->setTooltip("The plugin's own window, beside this one");
                }
                // Each input with sources: a Mix row per source.
                for(const auto& input : inputsOf(graph_, node == graphOutput ? graphOutput : node))
                {
                    if(node == graphOutput && input.port != chosen->port) continue;
                    if(panel.getHeight() < 30) break;
                    auto title = std::make_unique<juce::Label>(juce::String(), text(input.name) + (input.rows.size() > 1 ? juce::String::fromUTF8(" · mixed from ") + juce::String(int(input.rows.size())) + " sources" : juce::String()));
                    title->setFont(juce::FontOptions(12.0f * themeData().fontScale, juce::Font::bold));
                    title->setBounds(panel.removeFromTop(20)); addAndMakeVisible(*title); controls_.push_back(std::move(title));
                    for(const auto& row : input.rows)
                    {
                        if(panel.getHeight() < 30) break;
                        auto r = panel.removeFromTop(30);
                        auto source = std::make_unique<juce::Label>(juce::String(), text(row.source) + (row.user ? juce::String() : juce::String::fromUTF8(" · part")));
                        source->setBounds(r.removeFromLeft(240)); addAndMakeVisible(*source); controls_.push_back(std::move(source));
                        // The wire on or off (a scene stores which).
                        toggle(r.removeFromLeft(64).reduced(2, 4), "On", [this, wire = row.wire] { for(const auto& w : graph_.wires) if(w.id == wire) return w.enabled; return true; },
                               [this, wire = row.wire](bool on) { edit([wire, on](Control& c) { c.setWireEnabled(wire, on); c.commit(); }); });
                        if(input.port == notesPort) { if(row.user) button(r.removeFromLeft(100).reduced(2), "Disconnect", [this, wire = row.wire] { edit([wire](Control& c) { disconnect(c, wire); }); }); continue; }
                        const auto wire = row.wire;
                        auto rowOf = [this, node, wire]() -> MixRow { for(const auto& i : inputsOf(graph_, node)) for(const auto& x : i.rows) if(x.wire == wire) return x; return {}; };
                        auto mix = [this, wire, rowOf](auto change) { return [this, wire, rowOf, change](auto value) { auto x = rowOf(); change(x, value); edit([wire, x](Control& c) { setWireMix(c, wire, x.level, x.pan, x.mute, x.solo); }); }; };
                        auto [level, update] = slider(r.removeFromLeft(220).reduced(2, 8), 0, 1, [rowOf] { return rowOf().level; }, mix([](MixRow& x, double v) { x.level = v; }));
                        level->setEnabled(row.editable); level->setTooltip(row.editable ? "This source's level into the input" : "Set in its lane");
                        updates.push_back(update);
                        toggle(r.removeFromLeft(56).reduced(2, 4), "M", [rowOf] { return rowOf().mute; }, mix([](MixRow& x, bool v) { x.mute = v; }));
                        toggle(r.removeFromLeft(56).reduced(2, 4), "S", [rowOf] { return rowOf().solo; }, mix([](MixRow& x, bool v) { x.solo = v; }));
                        if(row.user) button(r.removeFromLeft(100).reduced(2), "Disconnect", [this, wire] { edit([wire](Control& c) { disconnect(c, wire); }); });
                    }
                }
            }
        }
        else if(mixerShown)
        {
            back_.setBounds(top.removeFromLeft(110).reduced(2));
            title_.setBounds(top.reduced(8, 0));
            auto area = getLocalBounds().reduced(16).withTrimmedTop(embedded_ ? 0 : header).withTrimmedBottom(28);
            std::vector<const PartRow*> rows;
            for(const auto* list : {&state_.parts, &state_.buses}) for(const auto& p : *list) rows.push_back(&p);
            const int gap = sectionGap(8), busGap = state_.buses.empty() ? 0 : sectionGap(16), n = std::max(1, int(rows.size()));
            const int width = std::clamp((area.getWidth() - busGap - gap * (n - 1)) / n, 72, 120);
            auto soloOf = [this](uint32_t id) { return [this, id] { const auto* p = part(id); return p && p->solo; }; };
            auto setSolo = [this](uint32_t id) { return [this, id](bool on) { edit([id, on](Control& c) { c.setPartSolo(id, on); c.commit(); }); }; };
            for(size_t i = 0; i < rows.size(); ++i)
            {
                const auto& p = *rows[i];
                if(p.bus && (i == 0 || !rows[i - 1]->bus)) area.removeFromLeft(busGap);
                auto strip = area.removeFromLeft(width); area.removeFromLeft(gap);
                const size_t first = controls_.size(); // the strip's controls, faded while it is muted or silenced
                auto inner = strip.reduced(6);
                auto name = std::make_unique<juce::Label>(juce::String(), text(p.name));
                name->setFont(juce::FontOptions(14.0f * themeData().fontScale, juce::Font::bold)); name->setJustificationType(juce::Justification::centred);
                name->setBounds(inner.removeFromTop(24)); addAndMakeVisible(*name); controls_.push_back(std::move(name));
                // Under the fader, top to bottom: its level, mute and solo, pan, then a send to each
                // bus (post-fader). Laid out from the bottom up.
                // Every strip keeps the send rows, so faders and buttons line up across them; a bus
                // leaves its own empty.
                std::vector<juce::Rectangle<int>> sendRows;
                for(size_t k = 0; k < state_.buses.size(); ++k) sendRows.insert(sendRows.begin(), inner.removeFromBottom(36));
                if(p.bus) sendRows.clear();
                auto panRow = inner.removeFromBottom(36);
                inner.removeFromBottom(6);
                auto buttons = inner.removeFromBottom(28);
                auto dbRow = inner.removeFromBottom(20);
                inner.removeFromBottom(4);
                auto small = [&](juce::Rectangle<int>& row, const juce::String& label) {
                    auto l = std::make_unique<juce::Label>(juce::String(), label);
                    l->setFont(juce::FontOptions(11.0f * themeData().fontScale)); l->setBounds(row.removeFromTop(14)); addAndMakeVisible(*l); controls_.push_back(std::move(l));
                };
                for(size_t k = 0; k < sendRows.size(); ++k)
                {
                    const auto& b = state_.buses[k];
                    small(sendRows[k], juce::String::fromUTF8("→ ") + text(b.name));
                    updates.push_back(slider(sendRows[k], 0, 1, sendOf(p.id, b.id), setSend(p.id, b.id)).second);
                }
                // Pan; a double-click centers it.
                small(panRow, "Pan");
                auto [pan, panUpdate] = slider(panRow.reduced(2, 0), -1, 1, [this, id = p.id] { const auto* q = part(id); return q ? q->pan : 0.0; },
                                               [this, id = p.id](double v) { edit([id, v](Control& c) { c.setPartPan(id, v); c.commit(); }); });
                pan->getProperties().set("bipolar", true);
                pan->setDoubleClickReturnValue(true, 0); pan->setTooltip("Pan: where it plays, left to right; its sends keep their own");
                updates.push_back(panUpdate);
                toggle(buttons.removeFromLeft(buttons.getWidth() / 2), "M", muteOf(p.id), setMute(p.id));
                toggle(buttons, "S", soloOf(p.id), setSolo(p.id));
                // The fader's level in dB.
                auto db = std::make_unique<juce::Label>(); db->setJustificationType(juce::Justification::centred);
                db->setFont(juce::FontOptions(12.0f * themeData().fontScale)); db->setBounds(dbRow); addAndMakeVisible(*db);
                auto* dbRaw = db.get(); controls_.push_back(std::move(db));
                auto showDb = std::make_shared<std::function<void()>>([this, dbRaw, id = p.id] {
                    const auto* q = part(id); const double level = q ? q->level : 0;
                    dbRaw->setText(level > 0 ? juce::String(20 * std::log10(level), 1) + " dB" : juce::String::fromUTF8("−∞ dB"), juce::dontSendNotification);
                });
                (*showDb)(); updates.push_back(showDb);
                // The fader and its meters as a pair, centred, 18 pixels apart.
                auto pair = inner.withSizeKeepingCentre(std::min(inner.getWidth(), 28 + 18 + 10), inner.getHeight());
                auto faderArea = pair.removeFromLeft(28); pair.removeFromLeft(18);
                const auto meter = pair.removeFromLeft(10);
                auto [fader, update] = slider(faderArea, 0, 1, levelOf(p.id), setLevel(p.id));
                fader->setSliderStyle(juce::Slider::LinearVertical);
                fader->setSkewFactorFromMidPoint(.25); // -12 dB halfway up
                fader->setTooltip("The part's level, before its sends");
                updates.push_back(update);
                strips_.push_back({p.id, strip, meter});
                std::vector<juce::Component*> faded;
                for(size_t i = first; i < controls_.size(); ++i) faded.push_back(controls_[i].get());
                auto fade = std::make_shared<std::function<void()>>([this, faded, id = p.id] {
                    const auto* q = part(id); const float alpha = q && (q->mute || q->silenced) ? .45f : 1.0f;
                    for(auto* c : faded) c->setAlpha(alpha);
                });
                (*fade)(); updates.push_back(fade);
            }
        }
        else if(controlsShown)
        {
            back_.setBounds(top.removeFromLeft(110).reduced(2));
            addControl_.setBounds(top.removeFromRight(120).reduced(2));
            // Every mapping against every scene, in one table.
            if(!scenes_.empty())
                toggle(top.removeFromRight(130).reduced(4, 8), "By scene", [this] { return byScene_; }, [this](bool on) { byScene_ = on; shape_.clear(); refresh(); });
            title_.setBounds(top.reduced(8, 0));
            auto area = getLocalBounds().reduced(16).withTrimmedTop(header).withTrimmedBottom(28);
            if(byScene_ && !scenes_.empty() && !macros_.empty())
            {
                // A row per mapping (control and target), a column per scene: ticked where it works.
                // Touch (and a device's panel) gets 44-pixel targets, as the rest of the view.
                const int tableMode = desktopMode(*this);
                const bool touch = tableMode != 1;
                const int nameWidth = std::min(320, area.getWidth() / 3);
                const int column = std::max(60, std::min(140, (area.getWidth() - nameWidth) / int(scenes_.size())));
                auto heading = area.removeFromTop(30);
                heading.removeFromLeft(nameWidth);
                for(const auto& s : scenes_)
                {
                    auto l = std::make_unique<juce::Label>(juce::String(), text(s.name));
                    l->setJustificationType(juce::Justification::centred); l->setFont(juce::FontOptions(tableMode ? textSize(TextRole::Small, tableMode) : 13.0f * themeData().fontScale, juce::Font::bold));
                    l->setBounds(heading.removeFromLeft(column)); addAndMakeVisible(*l); controls_.push_back(std::move(l));
                }
                for(const auto& m : macros_)
                    for(size_t i = 0; i < m.targets.size(); ++i)
                    {
                        if(area.getHeight() < (touch ? 48 : 30)) break;
                        auto r = area.removeFromTop(touch ? 48 : 32);
                        auto l = std::make_unique<juce::Label>(juce::String(), text(m.name + "  ·  " + describe(m.targets[i])));
                        l->setBounds(r.removeFromLeft(nameWidth)); addAndMakeVisible(*l); controls_.push_back(std::move(l));
                        for(const auto& s : scenes_)
                        {
                            const auto cell = r.removeFromLeft(column);
                            toggle(cell.withSizeKeepingCentre(std::min(cell.getWidth(), touch ? 56 : 40), touch ? 44 : 24), {},
                                   [this, id = m.id, i, scene = s.id] {
                                       for(const auto& x : macros_) if(x.id == id && i < x.targets.size()) return std::find(x.targets[i].offIn.begin(), x.targets[i].offIn.end(), scene) == x.targets[i].offIn.end();
                                       return true; },
                                   [this, id = m.id, i, scene = s.id](bool on) {
                                       setTargets(id, [i, scene, on](auto& ts) {
                                           if(i >= ts.size()) return;
                                           auto& off = ts[i].offIn;
                                           std::erase(off, scene);
                                           if(!on) off.push_back(scene);
                                       }); });
                        }
                    }
            }
            else if(macros_.empty())
            {
                auto l = std::make_unique<juce::Label>(juce::String(), "No controls yet. A control is a knob that drives any number of parameters, part levels or other controls, each with its own range and curve. Bind a panel knob to it with control:NAME.");
                l->setJustificationType(juce::Justification::centred); l->setBounds(area.withHeight(80)); addAndMakeVisible(*l); controls_.push_back(std::move(l));
            }
            const int controlsMode = desktopMode(*this);
            const bool bigTargets = controlsMode != 1; // touch, or a device's panel: 44-pixel targets
            const int buttonHeight = bigTargets ? 44 : 30;
            if(!byScene_ || scenes_.empty())
                for(const auto& m : macros_)
                {
                    const auto id = m.id;
                    auto row = area.removeFromTop(bigTargets ? 52 : 40);
                    auto name = std::make_unique<juce::Label>(juce::String(), text(m.name));
                    name->setFont(juce::FontOptions(controlsMode ? textSize(TextRole::Title, controlsMode) : 16.0f * themeData().fontScale, juce::Font::bold));
                    name->setEditable(false, true);
                    name->onTextChange = [this, id, raw = name.get()] { const auto n = raw->getText().toStdString(); edit([id, n](Control& c) { c.setMacroName(id, n); }); };
                    name->setTooltip("Double-click to rename");
                    name->setBounds(row.removeFromLeft(170)); addAndMakeVisible(*name); controls_.push_back(std::move(name));
                    button(row.removeFromRight(90).withSizeKeepingCentre(86, buttonHeight), "Remove", [this, id] { edit([id](Control& c) { c.removeMacro(id); }); });
                    auto* add = button(row.removeFromRight(110).withSizeKeepingCentre(106, buttonHeight), "+ Target", {});
                    add->onClick = [this, id, add] { targetMenu(id, *add); };
                    updates.push_back(slider(row.reduced(4, 10), 0, 1, [this, id] { for(const auto& x : macros_) if(x.id == id) return x.value; return 0.0; },
                                             [this, id](double v) { edit([id, v](Control& c) { c.setMacro(id, v); }); }).second);
                    for(size_t i = 0; i < m.targets.size(); ++i)
                    {
                        const auto& t = m.targets[i];
                        auto r = area.removeFromTop(bigTargets ? 50 : 34).withTrimmedLeft(24);
                        auto l = std::make_unique<juce::Label>(juce::String(), text(describe(t)));
                        l->setBounds(r.removeFromLeft(220)); addAndMakeVisible(*l); controls_.push_back(std::move(l));
                        button(r.removeFromRight(80).withSizeKeepingCentre(76, buttonHeight), "Remove", [this, id, i] { setTargets(id, [i](auto& ts) { ts.erase(ts.begin() + std::ptrdiff_t(i)); }); });
                        if(!scenes_.empty())
                        {
                            auto* in = button(r.removeFromRight(100).withSizeKeepingCentre(96, buttonHeight), t.offIn.empty() ? "Scenes: all" : "Scenes...", {});
                            in->setTooltip("The scenes this mapping works in");
                            in->onClick = [this, id, i, in] { mappingScenesMenu(id, i, *in); };
                        }
                        auto relative = std::make_unique<juce::ToggleButton>("Relative");
                        relative->setToggleState(t.relative, juce::dontSendNotification);
                        relative->setTooltip("Move the value around where it was, instead of setting it");
                        relative->onClick = [this, id, i, raw = relative.get()] { const bool on = raw->getToggleState(); setTargets(id, [i, on](auto& ts) { ts[i].relative = on; }); };
                        relative->setBounds(r.removeFromRight(110).withSizeKeepingCentre(106, buttonHeight)); addAndMakeVisible(*relative); controls_.push_back(std::move(relative));
                        auto curve = std::make_unique<juce::ComboBox>("Curve");
                        for(const auto& [n, c] : {std::pair{"Linear", 1}, {"Exponential", 2}, {"Logarithmic", 3}, {"S-curve", 4}}) curve->addItem(n, c);
                        curve->setSelectedId(int(t.curve) + 1, juce::dontSendNotification);
                        curve->onChange = [this, id, i, raw = curve.get()] { const auto c = MacroTarget::Curve(raw->getSelectedId() - 1); setTargets(id, [i, c](auto& ts) { ts[i].curve = c; }); };
                        curve->setBounds(r.removeFromRight(130).withSizeKeepingCentre(126, buttonHeight)); addAndMakeVisible(*curve); controls_.push_back(std::move(curve));
                        const int half = r.getWidth() / 2;
                        auto rangeEnd = [this, id, i](bool end) {
                            return [this, id, i, end] { for(const auto& x : macros_) if(x.id == id && i < x.targets.size()) return end ? x.targets[i].end : x.targets[i].start; return 0.0; };
                        };
                        auto setEnd = [this, id, i](bool end) { return [this, id, i, end](double v) { setTargets(id, [i, end, v](auto& ts) { (end ? ts[i].end : ts[i].start) = v; }); }; };
                        auto [from, u1] = slider(r.removeFromLeft(half).reduced(4, 8), 0, 1, rangeEnd(false), setEnd(false)); from->setTooltip("Where the target is when the control is at 0");
                        auto [to, u2] = slider(r.reduced(4, 8), 0, 1, rangeEnd(true), setEnd(true)); to->setTooltip("Where the target is when the control is at 100% (lower than the start reverses it)");
                        updates.push_back(u1); updates.push_back(u2);
                    }
                    area.removeFromTop(10);
                }
        }
        else if(const auto* p = part(page_))
        {
            if(!embedded_) back_.setBounds(top.removeFromLeft(110).reduced(2));
            removePart_.setBounds(top.removeFromRight(130).reduced(2));
            name_.setBounds(top.reduced(8, 4));
            if(!name_.hasKeyboardFocus(true)) name_.setText(text(p->name), juce::dontSendNotification);
            name_.applyFontToAllText(name_.getFont());
            name_.setIndents(10, 0); // the editor centers it vertically; this lines it up with the rows' text
            // The chain: IN, each box or split with a + before it, a last +, OUT. A split stacks its
            // lanes between a split pill and a join pill, each lane with its boxes and a + at its end.
            constexpr int laneHeight = 44, laneBox = 104;
            size_t mostLanes = 0;
            for(const auto& b : p->chain) mostLanes = std::max(mostLanes, b.lanes.size());
            chainHeight_ = std::max(96, int(mostLanes) * laneHeight + 8);
            auto chain = getLocalBounds().reduced(16, 0).withTrimmedTop(header + 12).withHeight(chainHeight_);
            auto splitWidth = [&](const PartBox& b) { size_t most = 0; for(const auto& l : b.lanes) most = std::max(most, l.boxes.size()); return 24 + int(most) * laneBox + 42 + 104; }; // room for the lane's keys and level
            const int n = int(p->chain.size());
            int fixed = 2 * 110 + (n + 1) * 34, normal = 0;
            for(const auto& b : p->chain) if(b.split) fixed += splitWidth(b); else ++normal;
            const int boxWidth = std::max(80, std::min(150, (chain.getWidth() - fixed) / std::max(1, normal)));
            chain.removeFromLeft(110);
            for(int i = 0; i <= n; ++i)
            {
                // Joined sections: one band, the boxes edge to edge and each + a column of its own.
                const bool joined = themeData().style.sections == ThemeStyle::Sections::Joined;
                auto* plus = plusButton(joined ? chain.removeFromLeft(34) : chain.removeFromLeft(34).withSizeKeepingCentre(28, 28));
                if(joined) plus->setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
                plus->setTooltip("Insert a plugin or a split here");
                plus->onClick = [this, i, plus] { insertMenu(size_t(i), *plus); };
                if(i == n) break;
                const auto& b = p->chain[size_t(i)];
                if(!b.split) { boxes_.push_back({b.node, chain.removeFromLeft(boxWidth).reduced(joined ? 0 : 3)}); continue; }
                auto area = chain.removeFromLeft(splitWidth(b)).reduced(0, 4);
                SplitArea s{b.split, area, area.removeFromLeft(12), area.removeFromRight(12), {}};
                for(size_t k = 0; k < b.lanes.size(); ++k)
                {
                    auto row = area.removeFromTop(laneHeight).reduced(4, 2);
                    s.lanes.push_back(row);
                    auto inner = row.reduced(4, 2);
                    // shortcut: a split inside a lane is shown as a gap; the views edit one level of splits, the protocol any.
                    for(const auto& box : b.lanes[k].boxes) { auto r = inner.removeFromLeft(laneBox - 4).reduced(2, 0); if(!box.split) boxes_.push_back({box.node, r}); }
                    auto* lanePlus = plusButton(inner.removeFromLeft(34).withSizeKeepingCentre(24, 24));
                    lanePlus->setTooltip("Add a plugin to this lane");
                    const auto at = b.lanes[k].boxes.size(); const ChainPlace place{b.split, uint32_t(k)};
                    lanePlus->onClick = [this, at, place, lanePlus] { insertMenu(at, *lanePlus, place); };
                }
                splitAreas_.push_back(std::move(s));
            }
            auto panel = getLocalBounds().reduced(16).withTrimmedTop(header + 12 + chainHeight_ + 12).withTrimmedBottom(28);
            // The selected box's actions.
            if(selected_)
            {
                auto actions = panel.removeFromTop(lineHeight);
                // Where the box is: the part's own chain or a lane, and its position there.
                std::optional<std::tuple<ChainPlace, size_t, size_t>> found;
                for(size_t i = 0; i < p->chain.size(); ++i)
                {
                    const auto& b = p->chain[i];
                    if(!b.split && b.node == selected_) found = {ChainPlace{}, i, p->chain.size()};
                    for(size_t k = 0; k < b.lanes.size(); ++k)
                        for(size_t j = 0; j < b.lanes[k].boxes.size(); ++j)
                            if(!b.lanes[k].boxes[j].split && b.lanes[k].boxes[j].node == selected_) found = {ChainPlace{b.split, uint32_t(k)}, j, b.lanes[k].boxes.size()};
                }
                if(found)
                {
                    const auto [place, index, size] = *found;
                    const auto id = p->id; const auto node = selected_;
                    button(actions.removeFromLeft(110).reduced(2), "Move left", [this, id, index, place] { if(index) edit([id, index, place](Control& c) { c.movePartNode(id, index, index - 1, place); c.commit(); }); })->setEnabled(index > 0);
                    button(actions.removeFromLeft(110).reduced(2), "Move right", [this, id, index, place] { edit([id, index, place](Control& c) { c.movePartNode(id, index, index + 1, place); c.commit(); }); })->setEnabled(index + 1 < size);
                    button(actions.removeFromLeft(110).reduced(2), "Remove", [this, id, node] { selected_ = 0; edit([id, node](Control& c) { removeBox(c, id, node); }); });
                    button(actions.removeFromLeft(100).reduced(2), "Routing...", [this, node] { showRouting(node); })
                        ->setTooltip("This plugin in Routing: wire it to plugins outside its part");
                    const bool off = graph_.disabled.count(node) != 0;
                    button(actions.removeFromLeft(90).reduced(2), off ? "Turn on" : "Turn off", [this, node, off] { edit([node, off](Control& c) { c.setNodeEnabled(node, off); c.commit(); }); })
                        ->setTooltip(off ? "Switch this plugin on again" : "Switch this plugin off: what reaches it passes around it, and its held notes and tail ring out. Store it in a scene to switch it there");
                    auto* controls = button(actions.removeFromLeft(150).reduced(2), "Scene controls...", {});
                    controls->setTooltip("Which of this plugin's parameters scenes set: all it lets be automated, but those you untick");
                    controls->onClick = [this, node, controls] { sceneControlsMenu(node, *controls); };
                    if(engine_.hasPluginWindow(node))
                    {
                        const auto* box = findBox(p->chain, node);
                        const auto title = (box && !box->plugin.empty() ? box->plugin : graph_.names.at(node)) + " · " + p->name;
                        button(actions.removeFromLeft(170).reduced(2), "Open plugin window", [this, node, title] { engine_.openPluginWindow(node, title); })
                            ->setTooltip("The plugin's own window, beside this one");
                    }
                }
                panel.removeFromTop(8);
                // A selected plugin's parameters take the panel, as pages of tiles.
                if(found && graph_.names.count(selected_))
                {
                    grid_ = grid && grid->node() == selected_ ? std::move(grid) : std::make_unique<ParameterGrid>(engine_, selected_, graph_.names.at(selected_));
                    addAndMakeVisible(*grid_); grid_->setBounds(panel);
                    updaters_ = std::move(updates);
                    return;
                }
            }
            const auto id = p->id;
            // The settings are a grid of tiles, each its name above its control (RFC 0019), as many
            // to a row as fit, on the desktop and a device's screen alike.
            const int columns = std::clamp(panel.getWidth() / 300, 2, 4), tileGap = sectionGap(8);
            const int tileWidth = (panel.getWidth() - tileGap * (columns - 1)) / columns, tileHeight = 16 + 18 + lineHeight + 6;
            int column = 0, tileTop = -1, rowHeight = tileHeight;
            auto labeled = [&](const juce::String& label, int span = 1, int height = 0) {
                span = std::min(span, columns);
                if(tileTop < 0) tileTop = panel.getY();
                if(column + span > columns)
                {
                    // The row's last tile takes the cells left over, so no row ends in a hole.
                    if(column < columns && !tiles_.empty()) tiles_.back().setRight(panel.getRight());
                    column = 0; tileTop += rowHeight + tileGap; rowHeight = tileHeight;
                }
                if(height) rowHeight = std::max(rowHeight, height);
                const juce::Rectangle<int> tile(panel.getX() + column * (tileWidth + tileGap), tileTop, tileWidth * span + tileGap * (span - 1), height ? height : tileHeight);
                column += span;
                tiles_.push_back(tile);
                auto inner = tile.reduced(12, 8);
                auto l = std::make_unique<juce::Label>(juce::String(), label);
                l->setFont(juce::FontOptions(textSize(TextRole::Small, mode), juce::Font::bold)); l->setColour(juce::Label::textColourId, ink(Ink::Muted));
                l->setBorderSize({}); l->setBounds(inner.removeFromTop(18)); addAndMakeVisible(*l); controls_.push_back(std::move(l));
                return inner;
            };
            // The selected split's actions, or the selected lane's keys and Mix.
            const PartBox* split = nullptr;
            for(const auto& b : p->chain) if(b.split && b.split == selectedSplit_) split = &b;
            if(split && !selected_)
            {
                const auto sp = split->split;
                auto actions = panel.removeFromTop(lineHeight);
                if(selectedLane_ < 0 || size_t(selectedLane_) >= split->lanes.size())
                {
                    const bool bands = split->kind == Split::Kind::Bands;
                    if(split->kind == Split::Kind::Copy || (bands && split->lanes.size() < 4))
                        button(actions.removeFromLeft(110).reduced(2), bands ? "Add band" : "Add lane", [this, id, sp] { edit([id, sp](Control& c) { c.addLane(id, sp); c.commit(); }); })
                            ->setTooltip(bands ? "Another frequency band, with its own crossover" : "Another layer: tap its + to add an instrument or effects");
                    button(actions.removeFromLeft(130).reduced(2), "Remove split", [this, id, sp] { selectedSplit_ = 0; edit([id, sp](Control& c) { removeSplitKeepingFirst(c, id, sp); }); })
                        ->setTooltip("Keep the first lane's plugins in the chain; the other lanes' plugins are removed");
                    // A band split's crossovers (the Split node's parameters 3 to 5), on a frequency scale.
                    if(bands && !split->splitter.empty())
                        for(size_t k = 0; k + 1 < split->lanes.size(); ++k)
                        {
                            const auto node = split->splitter; const auto param = uint32_t(3 + k);
                            auto r = labeled("Crossover " + juce::String(int(k + 1)));
                            auto [crossover, update] = slider(r.removeFromLeft(320).reduced(2, 8), 20, 20000, [this, node, param] { return engine_.param(node, param); },
                                                              [this, node, param](double v) { engine_.setParam(node, param, v); });
                            crossover->setSkewFactorFromMidPoint(1000);
                            crossover->setTextBoxStyle(juce::Slider::TextBoxRight, false, 72, 24);
                            crossover->setTextValueSuffix(" Hz"); crossover->setNumDecimalPlacesToDisplay(0);
                            updates.push_back(update);
                        }
                }
                else
                {
                    const auto lane = uint32_t(selectedLane_);
                    if(split->kind == Split::Kind::Copy || (split->kind == Split::Kind::Bands && split->lanes.size() > 2))
                        button(actions.removeFromLeft(130).reduced(2), "Remove lane", [this, id, sp, lane] { selectedLane_ = -1; edit([id, sp, lane](Control& c) { removeLaneAndBoxes(c, id, sp, lane); }); })
                            ->setTooltip("Remove this lane and its plugins");
                    panel.removeFromTop(8);
                    zoneEditor(labeled("Lane keys", columns, 120), [this, id, sp, lane] {
                        if(const auto* q = part(id)) for(const auto& b : q->chain) if(b.split == sp && lane < b.lanes.size()) return std::array<int, 4>{b.lanes[lane].lowKey, b.lanes[lane].highKey, 0, 127};
                        return std::array<int, 4>{0, 127, 0, 127};
                    }, [this, id, sp, lane](int low, int high) { edit([id, sp, lane, low, high](Control& c) { setLaneKeys(c, id, sp, lane, low, high); }); }, {});
                    auto laneOf = [this, id, sp, lane]() -> const PartLane* {
                        if(const auto* q = part(id)) for(const auto& b : q->chain) if(b.split == sp && lane < b.lanes.size()) return &b.lanes[lane];
                        return nullptr;
                    };
                    // Mute and solo at the tile's right, the level slider in the rest.
                    auto mix = labeled("Lane level");
                    toggle(mix.removeFromRight(34), "Solo", [laneOf] { const auto* x = laneOf(); return x && x->solo; },
                           [this, id, sp, lane](bool o) { edit([id, sp, lane, o](Control& c) { editLane(c, id, sp, lane, [o](Lane& x) { x.solo = o; }); }); });
                    toggle(mix.removeFromRight(34), "Mute", [laneOf] { const auto* x = laneOf(); return x && x->mute; },
                           [this, id, sp, lane](bool m) { edit([id, sp, lane, m](Control& c) { editLane(c, id, sp, lane, [m](Lane& x) { x.mute = m; }); }); });
                    updates.push_back(slider(mix.reduced(2, 10), 0, 1, [laneOf] { const auto* x = laneOf(); return x ? x->level : 1.0; },
                                             [this, id, sp, lane](double v) { edit([id, sp, lane, v](Control& c) { editLane(c, id, sp, lane, [v](Lane& x) { x.level = v; }); }); }).second);
                    auto panRow = labeled("Lane pan");
                    auto [lanePan, lanePanUpdate] = slider(panRow.removeFromLeft(320).reduced(2, 10), -1, 1, [laneOf] { const auto* x = laneOf(); return x ? x->pan : 0.0; },
                                                           [this, id, sp, lane](double v) { edit([id, sp, lane, v](Control& c) { editLane(c, id, sp, lane, [v](Lane& x) { x.pan = v; }); }); });
                    lanePan->getProperties().set("bipolar", true);
                    lanePan->setDoubleClickReturnValue(true, 0);
                    updates.push_back(lanePanUpdate);
                }
                panel.removeFromTop(8);
                // A selected split or lane has the panel to itself; tapping it again shows the part's settings.
                updaters_ = std::move(updates);
                return;
            }
            // The part's settings, as the model lists them (partFields), a tile each.
            for(const auto& field : partFields(*p, state_))
            {
                using K = PartField::Kind;
                auto r = labeled(text(field.label), field.span ? field.span : columns, field.kind == K::Zone ? 140 : 0);
                switch(field.kind)
                {
                case K::Devices:
                {
                    // Which devices play it (RFC 0012), picked from a menu.
                    auto* pick = button(r.removeFromLeft(340).reduced(2), text(devicesName(p->devices)), {});
                    pick->setTooltip("Which keyboards and controllers play this part, whatever channel they send on");
                    pick->onClick = [this, id, pick] { devicesMenu(id, *pick); };
                    break;
                }
                case K::NoteLimit:
                {
                    // Past it, the part's oldest released note fades out, then its oldest held one.
                    auto limit = std::make_unique<juce::ComboBox>("Notes at once");
                    limit->addItem("Any", 1);
                    for(const int n : {1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 16, 24, 32, 64}) limit->addItem(juce::String(n), n + 1);
                    limit->setSelectedId(p->maxNotes + 1, juce::dontSendNotification);
                    limit->setTooltip("How many notes the part plays at once; a note past it fades out the oldest released note, then the oldest held one (CLAP instruments)");
                    limit->onChange = [this, id, raw = limit.get()] { const int n = raw->getSelectedId() - 1; edit([id, n](Control& c) { setMaxNotes(c, id, n); }); };
                    limit->setBounds(r.removeFromLeft(160).reduced(2)); addAndMakeVisible(*limit); controls_.push_back(std::move(limit));
                    break;
                }
                case K::Zone:
                    zoneEditor(r, [this, id] {
                        const auto* q = part(id);
                        return q ? std::array<int, 4>{q->lowKey, q->highKey, q->lowVelocity, q->highVelocity} : std::array<int, 4>{0, 127, 0, 127};
                    }, [this, id](int low, int high) { const auto* q = part(id); const int t = q ? q->transpose : 0; edit([id, low, high, t](Control& c) { setZone(c, id, low, high, t); }); },
                       [this, id](int low, int high) { edit([id, low, high](Control& c) { setVelocities(c, id, low, high); }); });
                    break;
                case K::Transpose:
                {
                    auto [transpose, update] = slider(r.removeFromLeft(320).reduced(2, 8), -24, 24, [this, id] { const auto* q = part(id); return q ? double(q->transpose) : 0.0; },
                                                      [this, id](double v) { const auto* q = part(id); if(!q) return; const int low = q->lowKey, high = q->highKey, semis = int(std::lround(v)); edit([id, low, high, semis](Control& c) { setZone(c, id, low, high, semis); }); });
                    transpose->setRange(-24, 24, 1);
                    transpose->getProperties().set("bipolar", true);
                    transpose->setTextBoxStyle(juce::Slider::TextBoxRight, false, 48, 24);
                    updates.push_back(update);
                    break;
                }
                case K::Output:
                {
                    auto out = std::make_unique<juce::ComboBox>("Plays to");
                    out->addItem("Main", 1);
                    for(const auto& b : state_.buses) if(b.id != id) out->addItem(text(b.name), int(b.id) + 1);
                    out->setSelectedId(p->through ? int(p->through) + 1 : 1, juce::dontSendNotification);
                    out->onChange = [this, id, raw = out.get()] { const auto bus = uint32_t(raw->getSelectedId() > 1 ? raw->getSelectedId() - 1 : 0); edit([id, bus](Control& c) { setOutput(c, id, bus); }); };
                    out->setBounds(r.removeFromLeft(240).reduced(2)); addAndMakeVisible(*out); controls_.push_back(std::move(out));
                    break;
                }
                case K::Level:
                    toggle(r.removeFromRight(34), "Mute", muteOf(id), setMute(id));
                    updates.push_back(slider(r.reduced(2, 10), 0, 1, levelOf(id), setLevel(id)).second);
                    break;
                case K::Send:
                    updates.push_back(slider(r.removeFromLeft(320).reduced(2, 10), 0, 1, sendOf(id, field.bus), setSend(id, field.bus)).second);
                    break;
                }
            }
        }
        updaters_ = std::move(updates); // refresh() runs them
    }

    std::string PartsView::describe(const MacroTarget& t) const
    {
        switch(t.kind)
        {
        case MacroTarget::Kind::PartLevel: case MacroTarget::Kind::PartMute:
            for(const auto* list : {&state_.parts, &state_.buses}) for(const auto& p : *list) if(p.id == t.part) return p.name + (t.kind == MacroTarget::Kind::PartLevel ? " level" : " mute");
            return "a part";
        case MacroTarget::Kind::Macro:
            for(const auto& m : macros_) if(m.id == t.macro) return "→ " + m.name;
            return "a control";
        case MacroTarget::Kind::Parameter:
        {
            std::string node = graph_.names.count(t.node) ? graph_.names.at(t.node) : "a node", param = std::to_string(t.param);
            try { for(const auto& p : engine_.parameters(t.node)) if(p.id == t.param) param = p.name; } catch(const std::exception&) {}
            return node + " · " + param;
        }
        case MacroTarget::Kind::Morph:
        {
            auto name = [&](uint32_t id) { for(const auto& s : scenes_) if(s.id == id) return s.name; return std::string("a scene"); };
            return "Morph " + name(t.sceneFrom) + " → " + name(t.sceneTo);
        }
        }
        return {};
    }

    void PartsView::setTargets(uint32_t macro, std::function<void(std::vector<MacroTarget>&)> change)
    {
        for(const auto& m : macros_)
            if(m.id == macro)
            {
                auto targets = m.targets;
                change(targets);
                edit([macro, targets](Control& c) { c.setMacroTargets(macro, targets); });
                return;
            }
    }

    // What a control can drive: a part's level or mute, another control, or any node's parameter.
    void PartsView::targetMenu(uint32_t macro, juce::Component& at)
    {
        std::vector<MacroTarget> choices;
        juce::PopupMenu menu, levels, mutes, others, parameters;
        auto add = [&](juce::PopupMenu& to, const std::string& label, MacroTarget t) { choices.push_back(t); to.addItem(int(choices.size()), text(label)); };
        for(const auto* list : {&state_.parts, &state_.buses})
            for(const auto& p : *list)
            {
                MacroTarget level; level.kind = MacroTarget::Kind::PartLevel; level.part = p.id; add(levels, p.name, level);
                MacroTarget mute; mute.kind = MacroTarget::Kind::PartMute; mute.part = p.id; add(mutes, p.name, mute);
            }
        for(const auto& m : macros_) if(m.id != macro) { MacroTarget t; t.kind = MacroTarget::Kind::Macro; t.macro = m.id; add(others, m.name, t); }
        for(const auto& n : graph_.topology.nodes)
        {
            juce::PopupMenu node;
            try
            {
                for(const auto& p : engine_.parameters(n.id))
                    if(!(p.flags & CLAP_PARAM_IS_READONLY)) { MacroTarget t; t.node = n.id; t.param = p.id; add(node, p.name, t); }
            }
            catch(const std::exception&) {}
            parameters.addSubMenu(text(graph_.names.count(n.id) ? graph_.names.at(n.id) : std::to_string(n.id)), node);
        }
        menu.addSubMenu("Plugin parameter", parameters);
        menu.addSubMenu("Part level", levels);
        menu.addSubMenu("Part mute", mutes);
        menu.addSubMenu("Another control", others);
        // A morph: from one scene (at 0) to another (at 1).
        juce::PopupMenu morphs;
        for(const auto& a : scenes_)
        {
            juce::PopupMenu to;
            for(const auto& b : scenes_) if(b.id != a.id) { MacroTarget t; t.kind = MacroTarget::Kind::Morph; t.sceneFrom = a.id; t.sceneTo = b.id; add(to, b.name, t); }
            morphs.addSubMenu(text(a.name) + juce::String::fromUTF8(" →"), to);
        }
        menu.addSubMenu("Morph between scenes", morphs, scenes_.size() >= 2);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&at), [safe = juce::Component::SafePointer<PartsView>(this), macro, choices](int chosen) {
            if(!safe || chosen <= 0 || size_t(chosen) > choices.size()) return;
            const auto t = choices[size_t(chosen) - 1];
            safe->setTargets(macro, [t](auto& ts) { ts.push_back(t); });
        });
    }

    // The node's parameters, ticked where scenes touch them (every automatable one but those set
    // aside); choosing one sets it aside or takes it back.
    void PartsView::sceneControlsMenu(NodeId node, juce::Component& at)
    {
        std::vector<Control::ParamInfo> parameters;
        try { parameters = engine_.parameters(node); } catch(const std::exception&) { return; }
        juce::PopupMenu menu;
        for(size_t i = 0; i < parameters.size(); ++i)
        {
            const SceneParameter here{node, parameters[i].id};
            const bool can = (parameters[i].flags & CLAP_PARAM_IS_AUTOMATABLE) && !(parameters[i].flags & CLAP_PARAM_IS_READONLY);
            menu.addItem(int(i) + 1, text(parameters[i].name), can, can && std::find(untouched_.begin(), untouched_.end(), here) == untouched_.end());
        }
        if(parameters.empty()) menu.addItem(1, "No parameters", false);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&at), [safe = juce::Component::SafePointer<PartsView>(this), node, parameters](int chosen) {
            if(!safe || chosen <= 0 || size_t(chosen) > parameters.size()) return;
            auto list = safe->untouched_;
            const SceneParameter here{node, parameters[size_t(chosen) - 1].id};
            if(const auto it = std::find(list.begin(), list.end(), here); it != list.end()) list.erase(it); else list.push_back(here);
            safe->edit([list](Control& c) { c.setSceneUntouched(list); });
        });
    }

    // The selected scene's scope: the kinds switching to it changes, each ticked.
    void PartsView::sceneScopeMenu(juce::Component& at)
    {
        const auto it = std::find_if(scenes_.begin(), scenes_.end(), [&](const Scene& s) { return s.id == scene_; });
        if(it == scenes_.end()) return;
        const auto scope = it->scope;
        const std::array<const char*, sceneKinds.size()> names{"Plugins and wires on or off", "Mutes and solos", "Levels, pans and sends", "Parameters and controls", "Control mappings"};
        juce::PopupMenu menu;
        for(size_t k = 0; k < names.size(); ++k) menu.addItem(int(k) + 1, names[k], true, (scope & (1u << k)) != 0);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&at), [safe = juce::Component::SafePointer<PartsView>(this), id = scene_, scope](int chosen) {
            if(!safe || chosen <= 0) return;
            const auto next = scope ^ (1u << (chosen - 1));
            safe->edit([id, next](Control& c) { c.setSceneScope(id, next); });
        });
    }

    // The selected scene's transition: its glide time, curve and launch, each ticked.
    void PartsView::sceneTransitionMenu(juce::Component& at)
    {
        const auto it = std::find_if(scenes_.begin(), scenes_.end(), [&](const Scene& s) { return s.id == scene_; });
        if(it == scenes_.end()) return;
        const auto scene = *it;
        static constexpr std::array<uint32_t, 8> glides{0, 100, 250, 500, 1000, 2000, 4000, 8000};
        juce::PopupMenu menu, curves, launches;
        for(size_t i = 0; i < glides.size(); ++i)
            menu.addItem(int(i) + 1, glides[i] == 0 ? juce::String("At once") : glides[i] < 1000 ? juce::String(int(glides[i])) + " ms" : juce::String(glides[i] / 1000.0, 0) + " s", true, scene.glide == glides[i]);
        const std::array<const char*, 3> curveNames{"Linear", "Ease in and out", "Equal power (levels by power)"}, launchNames{"At once", "On the next beat", "On the next bar"};
        for(size_t i = 0; i < curveNames.size(); ++i) curves.addItem(100 + int(i), curveNames[i], scene.glide > 0, size_t(scene.curve) == i);
        for(size_t i = 0; i < launchNames.size(); ++i) launches.addItem(200 + int(i), launchNames[i], true, size_t(scene.launch) == i);
        menu.addSeparator();
        menu.addSubMenu("Curve", curves, scene.glide > 0);
        menu.addSubMenu("While playing", launches);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&at), [safe = juce::Component::SafePointer<PartsView>(this), scene](int chosen) {
            if(!safe || chosen <= 0) return;
            auto glide = scene.glide; auto curve = scene.curve; auto launch = scene.launch;
            if(chosen <= int(glides.size())) glide = glides[size_t(chosen - 1)];
            else if(chosen >= 200) launch = Scene::Launch(chosen - 200);
            else curve = Scene::Curve(chosen - 100);
            safe->edit([id = scene.id, glide, curve, launch](Control& c) { c.setSceneTransition(id, glide, curve, launch); });
        });
    }

    // The scenes a control's mapping works in, each ticked; choosing one switches it there.
    void PartsView::mappingScenesMenu(uint32_t macro, size_t target, juce::Component& at)
    {
        const auto m = std::find_if(macros_.begin(), macros_.end(), [&](const Macro& x) { return x.id == macro; });
        if(m == macros_.end() || target >= m->targets.size()) return;
        const auto offIn = m->targets[target].offIn;
        juce::PopupMenu menu;
        for(const auto& s : scenes_) menu.addItem(int(s.id), text(s.name), true, std::find(offIn.begin(), offIn.end(), s.id) == offIn.end());
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&at), [safe = juce::Component::SafePointer<PartsView>(this), macro, target](int chosen) {
            if(!safe || chosen <= 0) return;
            safe->setTargets(macro, [target, scene = uint32_t(chosen)](auto& ts) {
                if(target >= ts.size()) return;
                auto& off = ts[target].offIn;
                if(const auto it = std::find(off.begin(), off.end(), scene); it != off.end()) off.erase(it); else off.push_back(scene);
            });
        });
    }

    // Destinations for a wire from the selected node: every node that takes that kind, and the outputs.
    void PartsView::connectMenu(WireKind kind, juce::Component& at)
    {
        const auto from = routed_;
        std::vector<NodeId> choices;
        juce::PopupMenu menu;
        for(const auto& n : routing_.nodes)
        {
            if(n.node == from || n.node == graphInput) continue;
            if(n.node == graphOutput) { if(kind != WireKind::Audio || std::find(choices.begin(), choices.end(), graphOutput) != choices.end()) continue; }
            else
            {
                const Graph::NodeInfo* info = nullptr;
                for(const auto& x : graph_.topology.nodes) if(x.id == n.node) info = &x;
                if(!info || (kind == WireKind::Audio ? info->inputs.empty() : info->noteInputs == 0)) continue;
            }
            choices.push_back(n.node);
            menu.addItem(int(choices.size()), text(n.node == graphOutput ? "Main output" : n.name) + (n.part ? juce::String::fromUTF8(" · ") + text(n.partName) : juce::String()));
        }
        if(choices.empty()) menu.addItem(-1, "Nothing takes this", false);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&at), [safe = juce::Component::SafePointer<PartsView>(this), from, kind, choices](int chosen) {
            if(!safe || chosen <= 0 || size_t(chosen) > choices.size()) return;
            const auto to = choices[size_t(chosen) - 1];
            safe->edit([from, to, kind](Control& c) { connectNodes(c, from, to, kind); });
        });
    }

    void PartsView::addLooseMenu(juce::Component& at)
    {
        juce::PopupMenu menu;
        if(plugins_.empty()) menu.addItem(-1, scan_.valid() ? "Finding plugins..." : "No plugins found", false);
        for(size_t i = 0; i < plugins_.size(); ++i) if(plugins_[i].id != splitPlugin && plugins_[i].id != joinPlugin) menu.addItem(int(i) + 1, text(plugins_[i].name));
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&at), [safe = juce::Component::SafePointer<PartsView>(this)](int chosen) {
            if(!safe || chosen <= 0 || size_t(chosen) > safe->plugins_.size()) return;
            const auto plugin = safe->plugins_[size_t(chosen) - 1];
            safe->edit([plugin](Control& c) { addLoose(c, plugin); });
        });
    }

    // Kinds of device, then each connected one by name; each item adds or removes its rule.
    void PartsView::devicesMenu(uint32_t id, juce::Component& at)
    {
        const auto* p = part(id);
        if(!p) return;
        const std::vector<DeviceRule> kinds{{DeviceRule::Kind::All}, {DeviceRule::Kind::BuiltIn}, {DeviceRule::Kind::External}, {DeviceRule::Kind::Usb}, {DeviceRule::Kind::Network}};
        std::vector<DeviceRule> choices;
        juce::PopupMenu menu;
        auto add = [&](const DeviceRule& r, const std::string& label) {
            choices.push_back(r);
            const bool ticked = r.kind == DeviceRule::Kind::All ? p->devices.empty() : std::find(p->devices.begin(), p->devices.end(), r) != p->devices.end();
            menu.addItem(int(choices.size()), text(label), true, ticked);
        };
        for(const auto& r : kinds) add(r, devicesName({r}));
        menu.addSectionHeader("Connected");
        if(endpoints_.empty()) menu.addItem(-1, "No MIDI devices", false);
        for(const auto& e : endpoints_) add(ruleFor(e), e.kind == MidiEndpoint::Kind::Port ? devicesName({ruleFor(e)}) + " (" + e.name + ")" : e.name);
        // Other rules (patterns, devices not connected) stay listed, so they can be removed.
        for(const auto& r : p->devices)
            if(std::find(choices.begin(), choices.end(), r) == choices.end()) add(r, devicesName({r}) + (resolveDevices({r}, endpoints_) ? "" : " (not connected)"));
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&at), [safe = juce::Component::SafePointer<PartsView>(this), id, choices](int chosen) {
            if(!safe || chosen <= 0 || size_t(chosen) > choices.size()) return;
            const auto* q = safe->part(id); if(!q) return;
            const auto devices = toggled(q->devices, choices[size_t(chosen) - 1]);
            safe->edit([id, devices](Control& c) { setDevices(c, id, devices); });
        });
    }

    void PartsView::insertMenu(size_t index, juce::Component& at, ChainPlace place)
    {
        constexpr int splitItem = 100000; // + the split's kind
        juce::PopupMenu menu;
        const auto* p = part(page_);
        if(p && !place.split)
        {
            // The kinds that divide the sound need the Split and Join plugins.
            const bool splitters = std::any_of(plugins_.begin(), plugins_.end(), [](const PluginChoice& c) { return c.id == splitPlugin; });
            menu.addItem(splitItem + int(Split::Kind::Copy), "Split here: layers (each lane gets the notes and a copy of the sound)");
            menu.addItem(splitItem + int(Split::Kind::LeftRight), "Split here: left / right", splitters);
            menu.addItem(splitItem + int(Split::Kind::MidSide), "Split here: mid / side", splitters);
            menu.addItem(splitItem + int(Split::Kind::Bands), "Split here: frequency bands", splitters);
            menu.addSeparator();
        }
        if(plugins_.empty()) menu.addItem(1, scan_.valid() ? "Finding plugins..." : "No plugins found", false);
        // Split and Join belong to splits; they are not boxes of their own.
        for(size_t i = 0; i < plugins_.size(); ++i) if(plugins_[i].id != splitPlugin && plugins_[i].id != joinPlugin) menu.addItem(int(i) + 1, text(plugins_[i].name));
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&at), [safe = juce::Component::SafePointer<PartsView>(this), index, place](int chosen) {
            if(!safe) return;
            const auto id = safe->page_;
            if(chosen >= splitItem && chosen <= splitItem + int(Split::Kind::Bands))
            {
                const auto kind = Split::Kind(chosen - splitItem); const auto plugins = safe->plugins_;
                safe->edit([id, index, kind, plugins](Control& c) { insertSplit(c, id, index, kind, plugins); });
                return;
            }
            if(chosen <= 0 || size_t(chosen) > safe->plugins_.size()) return;
            const auto plugin = safe->plugins_[size_t(chosen) - 1];
            safe->edit([id, index, plugin, place](Control& c) { insertPlugin(c, id, index, plugin, place); });
        });
    }

    void PartsView::paint(juce::Graphics& g)
    {
        g.fillAll(ink(Ink::Background));
        for(const auto& t : tiles_) drawSection(g, t, ink(Ink::Surface));
        if(page_ == controlsPage) return;
        if(page_ == routingPage)
        {
            const auto scale = themeData().fontScale;
            using Kind = RoutingLayout::Box::Kind;
            auto at = [&](const RoutingRect& r) { return juce::Rectangle<float>(routingOrigin_.x + r.x * routingScale_, routingOrigin_.y + r.y * routingScale_, r.w * routingScale_, r.h * routingScale_); };
            auto point = [&](const RoutingPoint& p) { return juce::Point<float>(routingOrigin_.x + float(p.x) * routingScale_, routingOrigin_.y + float(p.y) * routingScale_); };
            auto partOfNode = [&](NodeId node) -> uint32_t { for(const auto& n : routing_.nodes) if(n.node == node && n.part) return n.part; return 0; };
            // Focus: with a plugin or part chosen, its wires and what they reach stay bright.
            const bool focus = routed_ || routedPart_;
            auto touches = [&](const Wire& w) {
                if(routedPart_) return partOfNode(w.from) == routedPart_ || partOfNode(w.to) == routedPart_;
                if(routed_ == graphInput) return w.from == graphInput && (w.kind == WireKind::Notes ? routedPort_ == notesPort : w.fromPort == routedPort_);
                if(routed_ == graphOutput) return w.to == graphOutput && w.toPort == routedPort_;
                return w.from == routed_ || w.to == routed_;
            };
            std::set<NodeId> reached; std::set<uint32_t> reachedParts;
            for(const auto& p : layout_.paths) if(touches(p.wire))
                for(const auto n : {p.wire.from, p.wire.to}) { reached.insert(n); if(const auto pp = partOfNode(n)) reachedParts.insert(pp); }
            auto bright = [&](const RoutingLayout::Box& b) {
                if(!focus) return true;
                if(b.kind == Kind::Node) return b.node == graphInput || b.node == graphOutput ? reached.count(b.node) != 0 : reached.count(b.node) || (routedPart_ && b.part == routedPart_);
                return b.part == routedPart_ || reachedParts.count(b.part) != 0;
            };
            auto chevron = [&](juce::Rectangle<float> r, bool down) {
                g.setColour(ink(Ink::Raised)); g.fillEllipse(r); g.setColour(ink(Ink::Border)); g.drawEllipse(r, 1);
                drawIcon(g, down ? "chevron-down" : "chevron-right", r.reduced(r.getWidth() * .22f), ink(Ink::Text));
            };
            const float chevronSize = 30 * routingScale_;
            // Expanded parts' frames, under everything.
            for(const auto& b : layout_.boxes)
            {
                if(b.kind != Kind::Frame) continue;
                const auto r = at(b.bounds);
                g.setColour(ink(Ink::Background).interpolatedWith(ink(Ink::Surface), .45f).withMultipliedAlpha(bright(b) ? 1.f : .5f));
                g.fillRoundedRectangle(r, themeData().radius + 2);
                g.setColour((b.part == routedPart_ ? ink(Ink::Accent) : ink(Ink::Border)).withMultipliedAlpha(bright(b) ? 1.f : .5f));
                g.drawRoundedRectangle(r, themeData().radius + 2, b.part == routedPart_ ? 2.f : 1.f);
                g.setColour(ink(Ink::Text)); g.setFont(juce::FontOptions(14.0f * scale * routingScale_, juce::Font::bold));
                g.drawText(text(b.name), r.withHeight(RoutingMetrics{}.frameHead * routingScale_).reduced(12 * routingScale_, 0), juce::Justification::centredLeft, true);
                if(collapsedParts_) chevron(juce::Rectangle<float>(chevronSize, chevronSize).withCentre({r.getRight() - 22 * routingScale_, r.getY() + RoutingMetrics{}.frameHead * routingScale_ / 2}), true);
            }
            // Wires, under the boxes, in the theme's wire colors: enabled audio and note wires
            // (user wires thicker), disabled and draining ones dashed, and those that no longer fit
            // their ports. An audio wire is a line per channel, side by side, and with Meters on
            // takes its level's color while it carries sound. With a plugin or part chosen, the
            // rest are faint.
            const auto& colors = themeData().wires;
            for(const auto& p : layout_.paths)
            {
                if(p.points.size() < 2) continue;
                const auto& w = p.wire;
                juce::Path path; path.startNewSubPath(point(p.points[0]));
                for(size_t i = 1; i < p.points.size(); ++i)
                {
                    const auto a = point(p.points[i - 1]), b = point(p.points[i]);
                    const float mid = (a.x + b.x) / 2;
                    path.cubicTo({mid, a.y}, {mid, b.y}, b);
                }
                const auto peak = wirePeaks_.find(w.id);
                const bool sounding = wireMeters_ && w.kind == WireKind::Audio && w.connected && p.state == WireState::Enabled && peak != wirePeaks_.end() && peak->second >= .001; // -60 dB
                auto color = !w.connected ? colors.unconnected : p.state == WireState::Draining ? colors.draining : p.state == WireState::Disabled ? colors.disabled
                            : sounding ? levelColor(peak->second) : w.kind == WireKind::Notes ? colors.notes : colors.audio;
                color = color.withAlpha(focus && !touches(w) ? .15f : w.owner == WireOwner::User || sounding ? 1.f : .75f);
                g.setColour(color);
                // The lines: nested strokes of the center line filled even-odd, so every other band
                // between them is empty. shortcut: eight lines at most, so a wider bus stays narrower than its port spacing.
                const float width = (w.owner == WireOwner::User || p.state == WireState::Draining ? 2.5f : 1.75f) * std::max(.6f, routingScale_);
                const auto lines = std::min(8u, wireChannels(w, graph_.topology));
                const float line = lines > 1 ? width * .7f : width, gap = width * .5f;
                const float across = lines * line + (lines - 1) * gap;
                juce::Path wire; wire.setUsingNonZeroWinding(false);
                float band = across;
                for(uint32_t k = 0; k < 2 * lines - 1 && band > 0; band -= 2 * (k++ % 2 ? gap : line))
                {
                    juce::Path outline; juce::PathStrokeType(band, juce::PathStrokeType::curved, juce::PathStrokeType::butt).createStrokedPath(outline, path);
                    wire.addPath(outline);
                }
                juce::Graphics::ScopedSaveState state(g);
                if(w.connected && p.state != WireState::Enabled)
                {
                    const float dashes[] = {6.f, 4.f};
                    juce::Path dashed; juce::PathStrokeType(across + 2).createDashedStroke(dashed, path, dashes, 2);
                    g.reduceClipRegion(dashed);
                }
                g.fillPath(wire);
            }
            // Plugins, collapsed parts and the graph's ends.
            for(const auto& b : layout_.boxes)
            {
                if(b.kind == Kind::Frame) continue;
                const auto r = at(b.bounds);
                const float alpha = bright(b) ? 1.f : .45f;
                if(b.kind == Kind::Part)
                {
                    const auto* row = part(b.part);
                    const bool chosenPart = b.part == routedPart_;
                    g.setColour(ink(Ink::Surface).withMultipliedAlpha(alpha)); g.fillRoundedRectangle(r, themeData().radius + 2);
                    // Muted or draining: the tint the parts lists give it.
                    if(row && (row->draining || row->mute)) { g.setColour((row->draining ? ink(Ink::Accent) : ink(Ink::Danger)).withAlpha(.14f * alpha)); g.fillRoundedRectangle(r, themeData().radius + 2); }
                    g.setColour((chosenPart ? ink(Ink::Accent) : ink(Ink::Border)).withMultipliedAlpha(alpha));
                    g.drawRoundedRectangle(r, themeData().radius + 2, chosenPart ? 2.f : 1.f);
                    auto inner = r.reduced(12 * routingScale_, 8 * routingScale_);
                    chevron(juce::Rectangle<float>(chevronSize, chevronSize).withCentre({r.getRight() - 22 * routingScale_, r.getCentreY()}), false);
                    inner.removeFromRight(chevronSize + 8 * routingScale_);
                    g.setColour(ink(Ink::Text).withMultipliedAlpha(alpha)); g.setFont(juce::FontOptions(14.0f * scale * routingScale_, juce::Font::bold));
                    g.drawText(text(b.name) + (b.detail.empty() ? juce::String() : "  " + text(b.detail).toUpperCase()), inner.removeFromTop(inner.getHeight() / 2), juce::Justification::bottomLeft, true);
                    // Its plugins as chips, in order, as many as fit.
                    g.setFont(juce::FontOptions(11.0f * scale * routingScale_));
                    auto chips = inner.withTrimmedTop(4 * routingScale_).withHeight(18 * routingScale_);
                    for(size_t i = 0; i < b.chips.size(); ++i)
                    {
                        const auto label = text(b.chips[i]);
                        const float width = juce::GlyphArrangement::getStringWidth(g.getCurrentFont(), label) + 14 * routingScale_;
                        if(width > chips.getWidth()) { g.setColour(ink(Ink::Muted).withMultipliedAlpha(alpha)); g.drawText(juce::String::fromUTF8("…"), chips, juce::Justification::centredLeft); break; }
                        const auto chip = chips.removeFromLeft(width);
                        g.setColour(ink(Ink::Raised).withMultipliedAlpha(alpha)); g.fillRoundedRectangle(chip, 4);
                        g.setColour(ink(Ink::Text).withMultipliedAlpha(alpha)); g.drawText(label, chip, juce::Justification::centred);
                        if(i + 1 < b.chips.size()) { const auto arrow = chips.removeFromLeft(14 * routingScale_); g.setColour(ink(Ink::Muted).withMultipliedAlpha(alpha)); g.drawText(juce::String::fromUTF8("›"), arrow, juce::Justification::centred); }
                    }
                    continue;
                }
                const auto* n = routing_.find(b.node, b.port);
                const bool chosenNode = b.node == routed_ && ((b.node != graphInput && b.node != graphOutput) || b.port == routedPort_);
                const bool off = graph_.disabled.count(b.node) != 0;
                if(off) g.beginTransparencyLayer(.55f);
                g.setColour((chosenNode ? ink(Ink::Accent).withAlpha(.25f) : ink(Ink::Surface)).withMultipliedAlpha(alpha));
                g.fillRoundedRectangle(r, themeData().radius);
                g.setColour((chosenNode ? ink(Ink::Accent) : failed(b.node) ? ink(Ink::Danger) : ink(Ink::Border)).withMultipliedAlpha(alpha));
                g.drawRoundedRectangle(r, themeData().radius, chosenNode ? 2.0f : 1.0f);
                auto inner = r.reduced(8 * routingScale_, 2);
                g.setColour(ink(Ink::Text).withMultipliedAlpha(alpha)); g.setFont(juce::FontOptions(13.0f * scale * routingScale_));
                g.drawText(text(b.name), inner.removeFromTop(inner.getHeight() * 3 / 5), juce::Justification::bottomLeft, true);
                g.setColour(ink(Ink::Muted).withMultipliedAlpha(alpha)); g.setFont(juce::FontOptions(10.0f * scale * routingScale_));
                g.drawText(off ? (b.detail.empty() ? juce::String("off") : text(b.detail) + juce::String::fromUTF8(" · off")) : text(b.detail), inner, juce::Justification::topLeft, true);
                if(n && n->userWires && n->part) { g.setColour(juce::Colour(0xffffa040)); g.fillEllipse(r.getRight() - 12, r.getY() + 5, 7, 7); }
                if(off) g.endTransparencyLayer();
            }
            return;
        }
        if(page_ == mixerPage)
        {
            // Strips, and their meters (Icons.h).
            for(const auto& s : strips_)
            {
                const auto* p = part(s.part);
                drawSection(g, s.bounds, p && (p->mute || p->silenced) ? ink(Ink::Surface).withAlpha(.5f) : ink(Ink::Surface));
                // A bar per channel of the part's sound (mono, stereo or more), side by side.
                const auto it = peaks_.find(s.part);
                const auto channels = it == peaks_.end() || it->second.empty() ? std::vector<double>{0.0} : it->second;
                auto bars = s.meter.toFloat();
                const float barWidth = (bars.getWidth() - float(channels.size() - 1)) / float(channels.size());
                for(const auto peak : channels) { drawMeter(g, bars.removeFromLeft(barWidth), peak, true); bars.removeFromLeft(1); }
            }
            return;
        }
        const auto scale = themeData().fontScale;
        auto chip = [&](juce::Rectangle<float> r, const PartBox& b, bool selected) {
            // Switched off: faint, and marked so.
            if(b.off) g.beginTransparencyLayer(.5f);
            g.setColour(selected ? ink(Ink::Accent).withAlpha(.25f) : ink(Ink::Raised));
            g.fillRoundedRectangle(r, themeData().radius);
            g.setColour(selected ? ink(Ink::Accent) : !b.split && failed(b.node) ? ink(Ink::Danger) : ink(Ink::Border));
            g.drawRoundedRectangle(r, themeData().radius, selected ? 2.0f : 1.0f);
            auto inner = r.reduced(8, 4);
            g.setColour(roleColor(b.role));
            g.setFont(juce::FontOptions(10.0f * scale, juce::Font::bold));
            g.drawText(text(b.role).toUpperCase() + (b.off ? juce::String::fromUTF8(" · OFF") : juce::String()), inner.removeFromTop(inner.getHeight() * .45f), juce::Justification::bottomLeft);
            g.setColour(ink(Ink::Text));
            g.setFont(juce::FontOptions(14.0f * scale));
            g.drawText(text(b.name), inner, juce::Justification::topLeft, true);
            // Wired in Routing too: a badge (Routing... opens it there).
            if(b.userWires) { g.setColour(juce::Colour(0xffffa040)); g.fillEllipse(r.getRight() - 12, r.getY() + 5, 7, 7); }
            if(b.off) g.endTransparencyLayer();
        };
        if(page_ == 0)
        {
            if(state_.parts.empty() && state_.buses.empty())
            {
                g.setColour(ink(Ink::Muted)); g.setFont(juce::FontOptions(16.0f * scale));
                g.drawText("No parts yet. Add a part: it listens to the keyboard and plays to Main.", getLocalBounds().withTrimmedTop(header), juce::Justification::centred);
                return;
            }
            const int partWidth = 150, fromWidth = 190, right = 160 + 92 * int(state_.buses.size());
            auto titles = juce::Rectangle<int>(16, header, getWidth() - 32, columnTitles);
            g.setColour(ink(Ink::Muted)); g.setFont(juce::FontOptions(12.0f * scale));
            titles.removeFromLeft(28);
            g.drawText("Part", titles.removeFromLeft(partWidth), juce::Justification::centredLeft);
            g.drawText("Plays from", titles.removeFromLeft(fromWidth), juce::Justification::centredLeft);
            auto sends = titles.removeFromRight(right);
            g.drawText("Chain", titles, juce::Justification::centredLeft);
            for(auto b = state_.buses.rbegin(); b != state_.buses.rend(); ++b) g.drawText("Send " + text(b->name), sends.removeFromRight(92), juce::Justification::centred);
            g.drawText("Mute", sends.removeFromRight(64), juce::Justification::centred);
            g.drawText("Level", sends, juce::Justification::centred);
            size_t row = 0;
            for(const auto* rows : {&state_.parts, &state_.buses})
            {
                if(rows == &state_.buses && !state_.buses.empty())
                {
                    g.setColour(ink(Ink::Muted)); g.setFont(juce::FontOptions(12.0f * scale, juce::Font::bold));
                    g.drawText("SHARED BUSES", rowBounds(row).withHeight(30).translated(0, -32), juce::Justification::bottomLeft);
                }
                for(const auto& p : *rows)
                {
                    auto r = rowBounds(row++);
                    g.setColour(p.mute || p.silenced ? ink(Ink::Surface).withAlpha(.5f) : ink(Ink::Surface));
                    g.fillRoundedRectangle(r.toFloat(), themeData().radius);
                    // Draining (being switched out) or muted: the row takes the accent's or the mute's tint.
                    if(p.draining || p.mute) { g.setColour((p.draining ? ink(Ink::Accent) : ink(Ink::Danger)).withAlpha(.14f)); g.fillRoundedRectangle(r.toFloat(), themeData().radius); }
                    auto rr = r.reduced(8, 6);
                    g.setColour(ink(Ink::Muted)); g.setFont(juce::FontOptions(14.0f * scale));
                    g.drawText(p.bus ? juce::String() : juce::String(row), rr.removeFromLeft(20), juce::Justification::centredLeft);
                    auto name = rr.removeFromLeft(partWidth);
                    g.setColour(ink(Ink::Text)); g.setFont(juce::FontOptions(16.0f * scale, juce::Font::bold));
                    g.drawText(text(p.name), name.removeFromTop(name.getHeight() / 2), juce::Justification::bottomLeft, true);
                    g.setColour(ink(Ink::Muted)); g.setFont(juce::FontOptions(12.0f * scale));
                    g.drawText(p.bus ? juce::String("Shared bus") : text(p.sound.empty() ? "No instrument yet" : p.sound), name, juce::Justification::topLeft, true);
                    auto from = rr.removeFromLeft(fromWidth);
                    g.setColour(ink(Ink::Text)); g.setFont(juce::FontOptions(13.0f * scale));
                    g.drawText(text(p.input), from.removeFromTop(from.getHeight() / 2 + 4), juce::Justification::bottomLeft, true);
                    if(p.notes)
                    {
                        // The zone over the whole keyboard.
                        auto bar = from.reduced(0, 6).withWidth(from.getWidth() - 20).toFloat();
                        g.setColour(ink(Ink::Raised)); g.fillRoundedRectangle(bar, 3);
                        g.setColour(juce::Colour(0xff8cc2ff));
                        g.fillRoundedRectangle(bar.withX(bar.getX() + bar.getWidth() * p.lowKey / 128.0f).withWidth(bar.getWidth() * (p.highKey - p.lowKey + 1) / 128.0f), 3);
                    }
                    rr.removeFromRight(right);
                    auto chain = rr.reduced(0, 2);
                    if(p.chain.empty()) { g.setColour(ink(Ink::Muted)); g.drawText("Empty: tap to add plugins", chain, juce::Justification::centredLeft); }
                    for(const auto& b : p.chain)
                    {
                        const int w = std::min(150, std::max(60, chain.getWidth() / int(p.chain.size())));
                        chip(chain.removeFromLeft(w).reduced(2).toFloat(), b, false);
                    }
                }
            }
            // Plugins several parts are wired into in Routing: buses in all but name.
            for(const auto& shared : state_.shared)
            {
                auto r = rowBounds(row++);
                g.setColour(ink(Ink::Surface).withAlpha(.7f)); g.fillRoundedRectangle(r.toFloat(), themeData().radius);
                auto rr = r.reduced(28, 6);
                g.setColour(ink(Ink::Text)); g.setFont(juce::FontOptions(16.0f * scale, juce::Font::bold));
                g.drawText(juce::String::fromUTF8("Shared · ") + text(shared.name), rr.removeFromLeft(partWidth + fromWidth), juce::Justification::centredLeft, true);
                std::string fed;
                for(const auto& f : shared.fedBy) fed += (fed.empty() ? "" : ", ") + f;
                g.setColour(ink(Ink::Muted)); g.setFont(juce::FontOptions(13.0f * scale));
                g.drawText("Fed by " + text(fed) + juce::String::fromUTF8(" in Routing · tap to open it"), rr, juce::Justification::centredLeft, true);
            }
            return;
        }
        const auto* p = part(page_);
        if(!p) return;
        auto ends = getLocalBounds().reduced(16, 0).withTrimmedTop(header + 12).withHeight(chainHeight_);
        // Joined sections: the chain is one band, its ends divided from the boxes by a line.
        const bool joined = themeData().style.sections == ThemeStyle::Sections::Joined;
        if(joined) drawSection(g, ends, ink(Ink::Surface));
        auto end = [&](juce::Rectangle<int> r, const juce::String& top, const juce::String& bottom) {
            if(joined) { g.setColour(ink(Ink::Border)); g.fillRect(r.getX() == ends.getX() ? r.getRight() : r.getX() - 1, r.getY(), 1, r.getHeight()); }
            else { g.setColour(ink(Ink::Surface)); g.fillRoundedRectangle(r.toFloat(), themeData().radius); }
            auto inner = r.reduced(8, 10);
            g.setColour(ink(Ink::Muted)); g.setFont(juce::FontOptions(11.0f * scale, juce::Font::bold));
            g.drawText(top, inner.removeFromTop(16), juce::Justification::topLeft);
            g.setColour(ink(Ink::Text)); g.setFont(juce::FontOptions(13.0f * scale));
            g.drawFittedText(bottom, inner, juce::Justification::topLeft, 3);
        };
        end(ends.removeFromLeft(joined ? 110 : 104), "IN", text(p->input)); // joined: the first + starts at 110
        juce::String out = p->through ? text(part(p->through) ? part(p->through)->name : "a bus") : juce::String("Main");
        for(const auto& [bus, level] : p->sends) if(const auto* b = part(bus)) out << "\nSend " << text(b->name) << " " << juce::String(juce::roundToInt(level * 100)) << "%";
        for(const auto& exit : p->exits) out << juce::String::fromUTF8("\n→ ") << text(exit);
        end(ends.removeFromRight(joined ? 110 : 104), "OUT", out);
        // Splits: their pills, and a row per lane (dimmed when it cannot be heard).
        for(const auto& s : splitAreas_)
        {
            const PartBox* b = nullptr;
            for(const auto& x : p->chain) if(x.split == s.split) b = &x;
            if(!b) continue;
            const bool splitSelected = s.split == selectedSplit_ && selectedLane_ < 0 && !selected_;
            const bool solo = std::any_of(b->lanes.begin(), b->lanes.end(), [](const PartLane& l) { return l.solo; });
            for(const auto* pill : {&s.splitPill, &s.joinPill})
            {
                g.setColour(splitSelected ? ink(Ink::Accent) : ink(Ink::Raised));
                g.fillRoundedRectangle(pill->toFloat(), 5);
            }
            for(size_t k = 0; k < s.lanes.size() && k < b->lanes.size(); ++k)
            {
                const auto& l = b->lanes[k];
                const bool laneSelected = s.split == selectedSplit_ && selectedLane_ == int(k) && !selected_;
                const bool silent = l.mute || (solo && !l.solo);
                g.setColour((laneSelected ? ink(Ink::Accent).withAlpha(.18f) : ink(Ink::Surface)).withMultipliedAlpha(silent ? .5f : 1.0f));
                g.fillRoundedRectangle(s.lanes[k].toFloat(), themeData().radius);
                g.setColour(ink(Ink::Border));
                g.drawLine(float(s.splitPill.getRight()), float(s.lanes[k].getCentreY()), float(s.lanes[k].getX()), float(s.lanes[k].getCentreY()));
                g.drawLine(float(s.lanes[k].getRight()), float(s.lanes[k].getCentreY()), float(s.joinPill.getX()), float(s.lanes[k].getCentreY()));
                // Its keys and level, at the lane's right; "Dry" for a lane without plugins.
                juce::String about = !l.name.empty() ? text(l.name) + " · " : l.boxes.empty() ? juce::String("Dry · ") : juce::String();
                if(l.lowKey != 0 || l.highKey != 127) about << text(noteName(l.lowKey)) << "–" << text(noteName(l.highKey)) << " · ";
                about << juce::roundToInt(l.level * 100) << "%" << (l.mute ? " · muted" : l.solo ? " · solo" : "");
                g.setColour(ink(Ink::Muted)); g.setFont(juce::FontOptions(11.0f * scale));
                g.drawText(about, s.lanes[k].reduced(8, 2), juce::Justification::centredRight, true);
            }
        }
        for(const auto& box : boxes_)
            if(const auto* b = findBox(p->chain, box.node)) chip(box.bounds.toFloat(), *b, b->node == selected_);
        if(p->chain.empty())
        {
            g.setColour(ink(Ink::Muted)); g.setFont(juce::FontOptions(14.0f * scale));
            g.drawText("Press + to add an instrument or effect", ends.reduced(50, 0), juce::Justification::centred);
        }
    }

    void PartsView::resized() { shape_.clear(); refresh(); }

    void PartsView::mouseUp(const juce::MouseEvent& e)
    {
        if(page_ == controlsPage || page_ == mixerPage) return;
        if(page_ == routingPage)
        {
            // In the layout's own pixels: the topmost box under the click.
            const double x = (e.position.x - routingOrigin_.x) / routingScale_, y = (e.position.y - routingOrigin_.y) / routingScale_;
            using Kind = RoutingLayout::Box::Kind;
            const RoutingMetrics m;
            for(auto b = layout_.boxes.rbegin(); b != layout_.boxes.rend(); ++b)
            {
                const auto& r = b->bounds;
                if(!(x >= r.x && x <= r.x + r.w && y >= r.y && y <= r.y + r.h)) continue;
                if(b->kind == Kind::Frame && y > r.y + m.frameHead) continue; // inside the frame: its plugins or nothing
                const bool control = x > r.x + r.w - 44; // the expand or collapse button
                if(b->kind != Kind::Node && control && collapsedParts_)
                {
                    if(b->kind == Kind::Part) expandedParts_.insert(b->part); else expandedParts_.erase(b->part);
                    shape_.clear(); refresh(); return;
                }
                if(b->kind != Kind::Node) { routedPart_ = routedPart_ == b->part ? 0 : b->part; routed_ = 0; routedPort_ = 0; shape_.clear(); refresh(); return; }
                const bool again = routed_ == b->node && routedPort_ == b->port;
                routed_ = again ? 0 : b->node; routedPort_ = again ? 0 : b->port; routedPart_ = 0;
                shape_.clear(); refresh(); return;
            }
            return;
        }
        if(page_ == 0)
        {
            const size_t parts = state_.parts.size(), buses = state_.buses.size();
            for(size_t row = 0; row < parts + buses + state_.shared.size(); ++row)
                if(rowBounds(row).contains(e.getPosition()))
                {
                    if(row >= parts + buses) return showRouting(state_.shared[row - parts - buses].node);
                    return showPart(row < parts ? state_.parts[row].id : state_.buses[row - parts].id);
                }
            return;
        }
        for(const auto& box : boxes_)
            if(box.bounds.contains(e.getPosition())) { selected_ = selected_ == box.node ? 0 : box.node; selectedSplit_ = 0; selectedLane_ = -1; shape_.clear(); refresh(); return; }
        for(const auto& s : splitAreas_)
        {
            int lane = -2; // -1: the split itself
            if(s.splitPill.contains(e.getPosition()) || s.joinPill.contains(e.getPosition())) lane = -1;
            for(size_t k = 0; k < s.lanes.size(); ++k) if(s.lanes[k].contains(e.getPosition())) lane = int(k);
            if(lane == -2) continue;
            const bool again = !selected_ && selectedSplit_ == s.split && selectedLane_ == lane;
            selected_ = 0; selectedSplit_ = again ? 0 : s.split; selectedLane_ = again ? -1 : lane;
            shape_.clear(); refresh(); return;
        }
    }
}
