// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PartsView.h"
#include "Theme.h"

namespace oscairn::standalone
{
    namespace
    {
        juce::Colour roleColour(const std::string& role)
        {
            if(role == "Instrument") return juce::Colour(0xff8cc2ff);
            if(role == "Effect") return juce::Colour(0xff6fd6e3);
            return juce::Colour(0xffc9b2ff); // notes and sequencers
        }
        juce::String text(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }
        constexpr int header = 56, columnTitles = 28, rowHeight = 60;
    }

    PartsView::PartsView(Engine& engine, const std::string& scanner) : engine_(engine)
    {
        for(auto* c : std::initializer_list<juce::Component*>{&title_, &message_, &done_, &addPart_, &addBus_, &back_, &removePart_, &name_, &controlsButton_, &mixerButton_, &routingButton_, &addControl_, &undo_, &redo_}) addChildComponent(c);
        routingButton_.setTooltip("Every plugin and wire: draw wires the parts cannot, and set each source's level");
        routingButton_.onClick = [this] { showRouting(0); };
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
        if(page_ == mixerPage) { peaks_.clear(); for(const auto& m : engine_.meters()) peaks_[m.part] = m.peak; }
        const int interval = page_ == mixerPage ? 50 : 200;
        if(getTimerInterval() != interval) startTimer(interval);
        refresh();
    }

    const PartRow* PartsView::part(uint32_t id) const
    {
        for(const auto* list : {&state_.parts, &state_.buses}) for(const auto& p : *list) if(p.id == id) return &p;
        return nullptr;
    }

    void PartsView::showPart(uint32_t id) { page_ = id; selected_ = 0; selectedSplit_ = 0; selectedLane_ = -1; shape_.clear(); refresh(); }
    void PartsView::showRouting(NodeId node, uint32_t port) { page_ = routingPage; selected_ = 0; routed_ = node; routedPort_ = port; shape_.clear(); refresh(); }
    void PartsView::showLane(uint32_t split, int lane) { selected_ = 0; selectedSplit_ = split; selectedLane_ = lane; shape_.clear(); refresh(); }

    void PartsView::refresh()
    {
        try
        {
            graph_ = engine_.graph();
            state_ = readParts(graph_); routing_ = readRouting(graph_); endpoints_ = engine_.status().midiInputs; scenes_ = graph_.scenes; sceneParameters_ = graph_.sceneParameters; scene_ = graph_.scene; macros_ = graph_.macros;
        }
        catch(const std::exception&) { return; }
        if(page_ && page_ != controlsPage && page_ != mixerPage && page_ != routingPage && !part(page_)) { page_ = 0; shape_.clear(); }
        if(page_ == routingPage && routed_ && !routing_.find(routed_, routedPort_)) routed_ = 0;
        for(auto& p : state_.parts) if(p.notes && !p.devices.empty() && !resolveDevices(p.devices, endpoints_)) p.input = "No device: " + p.input;
        message_.setText(text(engine_.editError()), juce::dontSendNotification);
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
            shape += "|routing:" + std::to_string(routed_) + "/" + std::to_string(routedPort_) + "/" + std::to_string(partWires_);
            for(const auto& n : routing_.nodes) shape += "," + std::to_string(n.node) + ":" + std::to_string(n.port) + ":" + n.name;
            for(const auto& w : routing_.wires) shape += ";" + std::to_string(w.id) + ":" + std::to_string(w.from) + ">" + std::to_string(w.to);
        }
        shape += "|scenes:" + std::to_string(scene_);
        for(const auto& s : scenes_) shape += "," + std::to_string(s.id) + ":" + s.name;
        shape += "|controls";
        for(const auto& m : macros_) { shape += "," + std::to_string(m.id) + ":" + m.name + ":" + std::to_string(m.targets.size()); for(const auto& t : m.targets) shape += ":" + describe(t) + std::to_string(int(t.curve)) + std::to_string(t.relative); }
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
        controls_.clear(); boxes_.clear(); splitAreas_.clear(); strips_.clear(); updaters_.clear();
        const bool list = page_ == 0, controlsShown = page_ == controlsPage, mixerShown = page_ == mixerPage, routingShown = page_ == routingPage;
        for(auto* c : std::initializer_list<juce::Component*>{&addPart_, &addBus_, &controlsButton_, &mixerButton_, &routingButton_}) c->setVisible(list);
        for(auto* c : std::initializer_list<juce::Component*>{&removePart_, &name_}) c->setVisible(!list && !controlsShown && !mixerShown && !routingShown);
        back_.setVisible(!list); addControl_.setVisible(controlsShown);
        title_.setVisible(list || controlsShown || mixerShown || routingShown); done_.setVisible(true); message_.setVisible(true);
        title_.setText(controlsShown ? "Controls" : mixerShown ? "Mixer" : routingShown ? "Routing" : "Parts", juce::dontSendNotification);
        routingBoxes_.clear();
        auto top = getLocalBounds().reduced(16, 8).removeFromTop(header - 8);
        done_.setBounds(top.removeFromRight(80).reduced(2));
        redo_.setBounds(top.removeFromRight(60).reduced(2)); undo_.setBounds(top.removeFromRight(60).reduced(2));
        undo_.setVisible(true); redo_.setVisible(true);
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
            auto t = std::make_unique<juce::ToggleButton>(label);
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
                ->setTooltip("A new scene with the parts' levels and mutes, and the scene controls, as they are now");
            if(scene_)
                button(top.removeFromLeft(66).reduced(2), "Store", [this] { const auto id = scene_; edit([id](Control& c) { c.storeScene(id); }); })
                    ->setTooltip("Save the parts' levels and mutes, and the scene controls, into the selected scene");
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
            toggle(top.removeFromLeft(130).reduced(4, 8), "Part wires", [this] { return partWires_; }, [this](bool on) { partWires_ = on; shape_.clear(); refresh(); });
            // Columns of nodes above; the selected node's wiring and Mix below.
            auto area = getLocalBounds().reduced(16, 0).withTrimmedTop(header).withTrimmedBottom(28);
            auto panel = area.removeFromBottom(190);
            auto canvas = area.withTrimmedTop(22);
            const int columnWidth = canvas.getWidth() / 5;
            for(int column = 0; column < 5; ++column)
            {
                std::vector<const RoutingNode*> in;
                for(const auto& n : routing_.nodes) if(n.column == column) in.push_back(&n);
                const int height = std::clamp(in.empty() ? 40 : (canvas.getHeight() - 8) / int(in.size()), 22, 44);
                auto col = canvas.withX(canvas.getX() + column * columnWidth).withWidth(columnWidth).reduced(14, 0);
                for(const auto* n : in) routingBoxes_.push_back({n->node, n->port, col.removeFromTop(height).reduced(0, 3)});
            }
            const auto* chosen = routed_ || routedPort_ ? routing_.find(routed_, routedPort_) : nullptr;
            if(!chosen)
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
                if(chosen->part)
                    button(head.removeFromLeft(110).reduced(2), "Open part", [this, part = chosen->part] { showPart(part); });
                else if(node != graphInput && node != graphOutput)
                    button(head.removeFromLeft(90).reduced(2), "Remove", [this, node] { routed_ = 0; edit([node](Control& c) { removeLoose(c, node); }); })
                        ->setTooltip("Remove this plugin; its sources are wired to its destinations");
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
            auto area = getLocalBounds().reduced(16).withTrimmedTop(header).withTrimmedBottom(28);
            std::vector<const PartRow*> rows;
            for(const auto* list : {&state_.parts, &state_.buses}) for(const auto& p : *list) rows.push_back(&p);
            const int gap = 8, busGap = state_.buses.empty() ? 0 : 16, n = std::max(1, int(rows.size()));
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
                // A send to each bus, post-fader, like the Parts list's.
                for(const auto& b : state_.buses)
                {
                    if(p.bus) break;
                    auto r = inner.removeFromTop(36);
                    auto l = std::make_unique<juce::Label>(juce::String(), juce::String::fromUTF8("→ ") + text(b.name));
                    l->setFont(juce::FontOptions(11.0f * themeData().fontScale)); l->setBounds(r.removeFromTop(14)); addAndMakeVisible(*l); controls_.push_back(std::move(l));
                    updates.push_back(slider(r, 0, 1, sendOf(p.id, b.id), setSend(p.id, b.id)).second);
                }
                // Pan, above the fader; a double-click centres it.
                auto panLabel = std::make_unique<juce::Label>(juce::String(), "Pan");
                panLabel->setFont(juce::FontOptions(11.0f * themeData().fontScale)); panLabel->setBounds(inner.removeFromTop(14)); addAndMakeVisible(*panLabel); controls_.push_back(std::move(panLabel));
                auto [pan, panUpdate] = slider(inner.removeFromTop(22).reduced(2, 2), -1, 1, [this, id = p.id] { const auto* q = part(id); return q ? q->pan : 0.0; },
                                               [this, id = p.id](double v) { edit([id, v](Control& c) { c.setPartPan(id, v); c.commit(); }); });
                pan->setDoubleClickReturnValue(true, 0); pan->setTooltip("Pan: where it plays, left to right; its sends keep their own");
                updates.push_back(panUpdate);
                auto buttons = inner.removeFromBottom(28);
                toggle(buttons.removeFromLeft(buttons.getWidth() / 2), "M", muteOf(p.id), setMute(p.id));
                toggle(buttons, "S", soloOf(p.id), setSolo(p.id));
                // The fader's level in dB, under it.
                auto db = std::make_unique<juce::Label>(); db->setJustificationType(juce::Justification::centred);
                db->setFont(juce::FontOptions(12.0f * themeData().fontScale)); db->setBounds(inner.removeFromBottom(20)); addAndMakeVisible(*db);
                auto* dbRaw = db.get(); controls_.push_back(std::move(db));
                auto showDb = std::make_shared<std::function<void()>>([this, dbRaw, id = p.id] {
                    const auto* q = part(id); const double level = q ? q->level : 0;
                    dbRaw->setText(level > 0 ? juce::String(20 * std::log10(level), 1) + " dB" : juce::String::fromUTF8("−∞ dB"), juce::dontSendNotification);
                });
                (*showDb)(); updates.push_back(showDb);
                inner.removeFromBottom(4);
                const auto meter = inner.removeFromRight(10);
                auto [fader, update] = slider(inner.reduced(4, 0), 0, 1, levelOf(p.id), setLevel(p.id));
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
            title_.setBounds(top.reduced(8, 0));
            auto area = getLocalBounds().reduced(16).withTrimmedTop(header).withTrimmedBottom(28);
            if(macros_.empty())
            {
                auto l = std::make_unique<juce::Label>(juce::String(), "No controls yet. A control is a knob that drives any number of parameters, part levels or other controls, each with its own range and curve. Bind a panel knob to it with control:NAME.");
                l->setJustificationType(juce::Justification::centred); l->setBounds(area.withHeight(80)); addAndMakeVisible(*l); controls_.push_back(std::move(l));
            }
            for(const auto& m : macros_)
            {
                const auto id = m.id;
                auto row = area.removeFromTop(40);
                auto name = std::make_unique<juce::Label>(juce::String(), text(m.name));
                name->setFont(juce::FontOptions(16.0f * themeData().fontScale, juce::Font::bold));
                name->setEditable(false, true);
                name->onTextChange = [this, id, raw = name.get()] { const auto n = raw->getText().toStdString(); edit([id, n](Control& c) { c.setMacroName(id, n); }); };
                name->setTooltip("Double-click to rename");
                name->setBounds(row.removeFromLeft(170)); addAndMakeVisible(*name); controls_.push_back(std::move(name));
                button(row.removeFromRight(90).reduced(2), "Remove", [this, id] { edit([id](Control& c) { c.removeMacro(id); }); });
                auto* add = button(row.removeFromRight(110).reduced(2), "+ Target", {});
                add->onClick = [this, id, add] { targetMenu(id, *add); };
                updates.push_back(slider(row.reduced(4, 10), 0, 1, [this, id] { for(const auto& x : macros_) if(x.id == id) return x.value; return 0.0; },
                                         [this, id](double v) { edit([id, v](Control& c) { c.setMacro(id, v); }); }).second);
                for(size_t i = 0; i < m.targets.size(); ++i)
                {
                    const auto& t = m.targets[i];
                    auto r = area.removeFromTop(34).withTrimmedLeft(24);
                    auto l = std::make_unique<juce::Label>(juce::String(), text(describe(t)));
                    l->setBounds(r.removeFromLeft(220)); addAndMakeVisible(*l); controls_.push_back(std::move(l));
                    button(r.removeFromRight(80).reduced(2), "Remove", [this, id, i] { setTargets(id, [i](auto& ts) { ts.erase(ts.begin() + std::ptrdiff_t(i)); }); });
                    auto relative = std::make_unique<juce::ToggleButton>("Relative");
                    relative->setToggleState(t.relative, juce::dontSendNotification);
                    relative->setTooltip("Move the value around where it was, instead of setting it");
                    relative->onClick = [this, id, i, raw = relative.get()] { const bool on = raw->getToggleState(); setTargets(id, [i, on](auto& ts) { ts[i].relative = on; }); };
                    relative->setBounds(r.removeFromRight(100)); addAndMakeVisible(*relative); controls_.push_back(std::move(relative));
                    auto curve = std::make_unique<juce::ComboBox>("Curve");
                    for(const auto& [n, c] : {std::pair{"Linear", 1}, {"Exponential", 2}, {"Logarithmic", 3}, {"S-curve", 4}}) curve->addItem(n, c);
                    curve->setSelectedId(int(t.curve) + 1, juce::dontSendNotification);
                    curve->onChange = [this, id, i, raw = curve.get()] { const auto c = MacroTarget::Curve(raw->getSelectedId() - 1); setTargets(id, [i, c](auto& ts) { ts[i].curve = c; }); };
                    curve->setBounds(r.removeFromRight(130).reduced(2)); addAndMakeVisible(*curve); controls_.push_back(std::move(curve));
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
            back_.setBounds(top.removeFromLeft(110).reduced(2));
            removePart_.setBounds(top.removeFromRight(130).reduced(2));
            name_.setBounds(top.reduced(8, 4));
            if(!name_.hasKeyboardFocus(true)) name_.setText(text(p->name), juce::dontSendNotification);
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
                auto* plus = button(chain.removeFromLeft(34).withSizeKeepingCentre(30, 30), "+", {});
                plus->setTooltip("Insert a plugin or a split here");
                plus->onClick = [this, i, plus] { insertMenu(size_t(i), *plus); };
                if(i == n) break;
                const auto& b = p->chain[size_t(i)];
                if(!b.split) { boxes_.push_back({b.node, chain.removeFromLeft(boxWidth).reduced(3)}); continue; }
                auto area = chain.removeFromLeft(splitWidth(b)).reduced(0, 4);
                SplitArea s{b.split, area, area.removeFromLeft(12), area.removeFromRight(12), {}};
                for(size_t k = 0; k < b.lanes.size(); ++k)
                {
                    auto row = area.removeFromTop(laneHeight).reduced(4, 2);
                    s.lanes.push_back(row);
                    auto inner = row.reduced(4, 2);
                    // shortcut: a split inside a lane is shown as a gap; the views edit one level of splits, the protocol any.
                    for(const auto& box : b.lanes[k].boxes) { auto r = inner.removeFromLeft(laneBox - 4).reduced(2, 0); if(!box.split) boxes_.push_back({box.node, r}); }
                    auto* lanePlus = button(inner.removeFromLeft(34).withSizeKeepingCentre(28, 28), "+", {});
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
                auto actions = panel.removeFromTop(40);
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
                    auto* controls = button(actions.removeFromLeft(150).reduced(2), "Scene controls...", {});
                    controls->setTooltip("Choose which of this plugin's parameters scenes set");
                    controls->onClick = [this, node, controls] { sceneControlsMenu(node, *controls); };
                }
                panel.removeFromTop(8);
            }
            const auto id = p->id;
            auto labelled = [&](const juce::String& label) {
                auto r = panel.removeFromTop(40);
                auto l = std::make_unique<juce::Label>(juce::String(), label);
                l->setFont(juce::FontOptions(15.0f * themeData().fontScale));
                l->setBounds(r.removeFromLeft(130)); addAndMakeVisible(*l); controls_.push_back(std::move(l));
                return r;
            };
            // The selected split's actions, or the selected lane's keys and Mix.
            const PartBox* split = nullptr;
            for(const auto& b : p->chain) if(b.split && b.split == selectedSplit_) split = &b;
            if(split && !selected_)
            {
                const auto sp = split->split;
                auto actions = panel.removeFromTop(40);
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
                            auto r = labelled("Crossover " + juce::String(int(k + 1)));
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
                    const auto& l = split->lanes[lane];
                    if(split->kind == Split::Kind::Copy || (split->kind == Split::Kind::Bands && split->lanes.size() > 2))
                        button(actions.removeFromLeft(130).reduced(2), "Remove lane", [this, id, sp, lane] { selectedLane_ = -1; edit([id, sp, lane](Control& c) { removeLaneAndBoxes(c, id, sp, lane); }); })
                            ->setTooltip("Remove this lane and its plugins");
                    panel.removeFromTop(8);
                    auto r = labelled("Lane keys");
                    auto keyBox = [&](juce::Rectangle<int> at, bool high) {
                        auto box = std::make_unique<juce::ComboBox>(high ? "Highest key" : "Lowest key");
                        for(int k = 0; k < 128; ++k) box->addItem(text(noteName(k)), k + 1);
                        box->setSelectedId((high ? l.highKey : l.lowKey) + 1, juce::dontSendNotification);
                        const int other = high ? l.lowKey : l.highKey;
                        box->onChange = [this, id, sp, lane, high, other, raw = box.get()] {
                            const int key = raw->getSelectedId() - 1, low = high ? other : key, hi = high ? key : other;
                            edit([id, sp, lane, low, hi](Control& c) { setLaneKeys(c, id, sp, lane, low, std::max(low, hi)); });
                        };
                        box->setBounds(at); addAndMakeVisible(*box); controls_.push_back(std::move(box));
                    };
                    keyBox(r.removeFromLeft(110).reduced(2), false);
                    keyBox(r.removeFromLeft(110).reduced(2), true);
                    button(r.removeFromLeft(100).reduced(2), "All keys", [this, id, sp, lane] { edit([id, sp, lane](Control& c) { setLaneKeys(c, id, sp, lane, 0, 127); }); });
                    auto laneOf = [this, id, sp, lane]() -> const PartLane* {
                        if(const auto* q = part(id)) for(const auto& b : q->chain) if(b.split == sp && lane < b.lanes.size()) return &b.lanes[lane];
                        return nullptr;
                    };
                    auto mix = labelled("Lane level");
                    updates.push_back(slider(mix.removeFromLeft(320).reduced(2, 10), 0, 1, [laneOf] { const auto* x = laneOf(); return x ? x->level : 1.0; },
                                             [this, id, sp, lane](double v) { edit([id, sp, lane, v](Control& c) { editLane(c, id, sp, lane, [v](Lane& x) { x.level = v; }); }); }).second);
                    toggle(mix.removeFromLeft(90).reduced(4, 6), "Mute", [laneOf] { const auto* x = laneOf(); return x && x->mute; },
                           [this, id, sp, lane](bool m) { edit([id, sp, lane, m](Control& c) { editLane(c, id, sp, lane, [m](Lane& x) { x.mute = m; }); }); });
                    toggle(mix.removeFromLeft(90).reduced(4, 6), "Solo", [laneOf] { const auto* x = laneOf(); return x && x->solo; },
                           [this, id, sp, lane](bool o) { edit([id, sp, lane, o](Control& c) { editLane(c, id, sp, lane, [o](Lane& x) { x.solo = o; }); }); });
                    auto panRow = labelled("Lane pan");
                    auto [lanePan, lanePanUpdate] = slider(panRow.removeFromLeft(320).reduced(2, 10), -1, 1, [laneOf] { const auto* x = laneOf(); return x ? x->pan : 0.0; },
                                                           [this, id, sp, lane](double v) { edit([id, sp, lane, v](Control& c) { editLane(c, id, sp, lane, [v](Lane& x) { x.pan = v; }); }); });
                    lanePan->setDoubleClickReturnValue(true, 0);
                    updates.push_back(lanePanUpdate);
                }
                panel.removeFromTop(8);
                // A selected split or lane has the panel to itself; tapping it again shows the part's settings.
                updaters_ = std::move(updates);
                return;
            }
            if(p->notes)
            {
                // Which devices play it (RFC 0012), picked from a menu.
                auto from = labelled("Plays from");
                auto* pick = button(from.removeFromLeft(340).reduced(2), text(devicesName(p->devices)), {});
                pick->setTooltip("Which keyboards and controllers play this part, whatever channel they send on");
                pick->onClick = [this, id, pick] { devicesMenu(id, *pick); };
                // The zone: lowest and highest key, and a transpose.
                auto r = labelled("Keys");
                auto keyBox = [&](juce::Rectangle<int> at, bool high) {
                    auto box = std::make_unique<juce::ComboBox>(high ? "Highest key" : "Lowest key");
                    for(int k = 0; k < 128; ++k) box->addItem(text(noteName(k)), k + 1);
                    box->setSelectedId((high ? p->highKey : p->lowKey) + 1, juce::dontSendNotification);
                    box->onChange = [this, id, high, raw = box.get()] {
                        const auto* q = part(id); if(!q) return;
                        const int key = raw->getSelectedId() - 1;
                        const int low = high ? q->lowKey : key, hi = high ? key : q->highKey, t = q->transpose;
                        edit([id, low, hi, t](Control& c) { setZone(c, id, low, std::max(low, hi), t); });
                    };
                    box->setBounds(at); addAndMakeVisible(*box); controls_.push_back(std::move(box));
                };
                keyBox(r.removeFromLeft(110).reduced(2), false);
                auto to = std::make_unique<juce::Label>(juce::String(), "to"); to->setFont(juce::FontOptions(15.0f * themeData().fontScale)); to->setBounds(r.removeFromLeft(30)); addAndMakeVisible(*to); controls_.push_back(std::move(to));
                keyBox(r.removeFromLeft(110).reduced(2), true);
                button(r.removeFromLeft(100).reduced(2), "All keys", [this, id] { const auto* q = part(id); const int t = q ? q->transpose : 0; edit([id, t](Control& c) { setZone(c, id, 0, 127, t); }); });
                auto t = labelled("Transpose");
                auto [transpose, update] = slider(t.removeFromLeft(320).reduced(2, 8), -24, 24, [this, id] { const auto* q = part(id); return q ? double(q->transpose) : 0.0; },
                                                  [this, id](double v) { const auto* q = part(id); if(!q) return; const int low = q->lowKey, high = q->highKey, semis = int(std::lround(v)); edit([id, low, high, semis](Control& c) { setZone(c, id, low, high, semis); }); });
                transpose->setRange(-24, 24, 1);
                transpose->setTextBoxStyle(juce::Slider::TextBoxRight, false, 48, 24);
                updates.push_back(update);
            }
            {
                auto r = labelled("Plays to");
                auto out = std::make_unique<juce::ComboBox>("Plays to");
                out->addItem("Main", 1);
                for(const auto& b : state_.buses) if(b.id != id) out->addItem(text(b.name), int(b.id) + 1);
                out->setSelectedId(p->through ? int(p->through) + 1 : 1, juce::dontSendNotification);
                out->onChange = [this, id, raw = out.get()] { const auto bus = uint32_t(raw->getSelectedId() > 1 ? raw->getSelectedId() - 1 : 0); edit([id, bus](Control& c) { setOutput(c, id, bus); }); };
                out->setBounds(r.removeFromLeft(240).reduced(2)); addAndMakeVisible(*out); controls_.push_back(std::move(out));
            }
            {
                auto r = labelled("Level");
                updates.push_back(slider(r.removeFromLeft(320).reduced(2, 10), 0, 1, levelOf(id), setLevel(id)).second);
                toggle(r.removeFromLeft(90).reduced(4, 6), "Mute", muteOf(id), setMute(id));
            }
            for(const auto& b : state_.buses)
                if(b.id != id) updates.push_back(slider(labelled("Send to " + text(b.name)).removeFromLeft(320).reduced(2, 10), 0, 1, sendOf(id, b.id), setSend(id, b.id)).second);
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
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&at), [safe = juce::Component::SafePointer<PartsView>(this), macro, choices](int chosen) {
            if(!safe || chosen <= 0 || size_t(chosen) > choices.size()) return;
            const auto t = choices[size_t(chosen) - 1];
            safe->setTargets(macro, [t](auto& ts) { ts.push_back(t); });
        });
    }

    // The node's parameters, ticked where scenes control them; choosing one toggles it.
    void PartsView::sceneControlsMenu(NodeId node, juce::Component& at)
    {
        std::vector<Control::ParamInfo> parameters;
        try { parameters = engine_.parameters(node); } catch(const std::exception&) { return; }
        juce::PopupMenu menu;
        for(size_t i = 0; i < parameters.size(); ++i)
        {
            const SceneParameter here{node, parameters[i].id};
            const bool on = std::find(sceneParameters_.begin(), sceneParameters_.end(), here) != sceneParameters_.end();
            menu.addItem(int(i) + 1, text(parameters[i].name), !(parameters[i].flags & CLAP_PARAM_IS_READONLY), on);
        }
        if(parameters.empty()) menu.addItem(1, "No parameters", false);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&at), [safe = juce::Component::SafePointer<PartsView>(this), node, parameters](int chosen) {
            if(!safe || chosen <= 0 || size_t(chosen) > parameters.size()) return;
            auto list = safe->sceneParameters_;
            const SceneParameter here{node, parameters[size_t(chosen) - 1].id};
            if(const auto it = std::find(list.begin(), list.end(), here); it != list.end()) list.erase(it); else list.push_back(here);
            safe->edit([list](Control& c) { c.setSceneParameters(list); });
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
        if(page_ == controlsPage) return;
        if(page_ == routingPage)
        {
            const auto scale = themeData().fontScale;
            auto titles = getLocalBounds().reduced(16, 0).withTrimmedTop(header).withHeight(20);
            g.setColour(ink(Ink::Muted)); g.setFont(juce::FontOptions(11.0f * scale, juce::Font::bold));
            const int columnWidth = titles.getWidth() / 5;
            for(const auto* name : {"INPUTS", "NOTES", "INSTRUMENTS", "EFFECTS AND BUSES", "OUTPUTS"}) g.drawText(name, titles.removeFromLeft(columnWidth).reduced(14, 0), juce::Justification::centredLeft);
            auto boxOf = [&](NodeId node, uint32_t port) -> const RoutingBox* {
                for(const auto& b : routingBoxes_) if(b.node == node && ((node != graphInput && node != graphOutput) || b.port == port)) return &b;
                return nullptr;
            };
            // Wires first, under the boxes: audio blue, notes violet; part wires faint, user wires
            // bright, unconnected ones red.
            for(const auto& w : routing_.wires)
            {
                if(w.owner == WireOwner::Part && !partWires_) continue;
                const auto* a = boxOf(w.from, w.from == graphInput ? (w.kind == WireKind::Notes ? notesPort : w.fromPort) : 0);
                const auto* b = boxOf(w.to, w.toPort);
                if(!a || !b) continue;
                const juce::Point<float> p0(float(a->bounds.getRight()), float(a->bounds.getCentreY())), p1(float(b->bounds.getX()), float(b->bounds.getCentreY()));
                juce::Path path; path.startNewSubPath(p0);
                const float bend = std::max(30.0f, (p1.x - p0.x) / 2);
                path.cubicTo(p0.translated(bend, 0), p1.translated(-bend, 0), p1);
                auto colour = !w.connected ? ink(Ink::Danger) : w.kind == WireKind::Notes ? juce::Colour(0xffc29cff) : juce::Colour(0xff8cc2ff);
                if(w.owner == WireOwner::Part) colour = colour.withAlpha(.35f);
                if(w.mute) colour = colour.withMultipliedAlpha(.4f);
                g.setColour(colour);
                g.strokePath(path, juce::PathStrokeType(w.owner == WireOwner::User ? 2.5f : 1.5f));
            }
            for(const auto& b : routingBoxes_)
            {
                const auto* n = routing_.find(b.node, b.port);
                if(!n) continue;
                const bool chosen = b.node == routed_ && ((b.node != graphInput && b.node != graphOutput) || b.port == routedPort_);
                g.setColour(chosen ? ink(Ink::Accent).withAlpha(.25f) : ink(Ink::Surface));
                g.fillRoundedRectangle(b.bounds.toFloat(), themeData().radius);
                g.setColour(chosen ? ink(Ink::Accent) : ink(Ink::Border));
                g.drawRoundedRectangle(b.bounds.toFloat(), themeData().radius, chosen ? 2.0f : 1.0f);
                auto inner = b.bounds.reduced(8, 2);
                g.setColour(ink(Ink::Text)); g.setFont(juce::FontOptions(13.0f * scale));
                g.drawText(text(n->name), inner.removeFromTop(inner.getHeight() * 3 / 5), juce::Justification::bottomLeft, true);
                g.setColour(ink(Ink::Muted)); g.setFont(juce::FontOptions(10.0f * scale));
                g.drawText(n->part ? text(n->partName) : n->node == graphInput || n->node == graphOutput ? juce::String() : juce::String("no part"), inner, juce::Justification::topLeft, true);
                if(n->userWires && n->part) { g.setColour(juce::Colour(0xffffa040)); g.fillEllipse(float(b.bounds.getRight() - 12), float(b.bounds.getY() + 5), 7, 7); }
            }
            return;
        }
        if(page_ == mixerPage)
        {
            // Strips, and meters from -60 to +6 dB: green, yellow above -12, red above -3.
            for(const auto& s : strips_)
            {
                const auto* p = part(s.part);
                g.setColour(p && (p->mute || p->silenced) ? ink(Ink::Surface).withAlpha(.5f) : ink(Ink::Surface));
                g.fillRoundedRectangle(s.bounds.toFloat(), themeData().radius);
                g.setColour(ink(Ink::Raised)); g.fillRoundedRectangle(s.meter.toFloat(), 3);
                const auto peak = peaks_.count(s.part) ? peaks_.at(s.part) : 0.0;
                const double db = peak > 0 ? 20 * std::log10(peak) : -100;
                const float fraction = float(std::clamp((db + 60) / 66, 0.0, 1.0));
                g.setColour(db > -3 ? ink(Ink::Danger) : db > -12 ? juce::Colour(0xffe8c547) : juce::Colour(0xff5ac46a));
                g.fillRoundedRectangle(s.meter.toFloat().withTrimmedTop(s.meter.getHeight() * (1 - fraction)), 3);
            }
            return;
        }
        const auto scale = themeData().fontScale;
        auto chip = [&](juce::Rectangle<float> r, const PartBox& b, bool selected) {
            g.setColour(selected ? ink(Ink::Accent).withAlpha(.25f) : ink(Ink::Raised));
            g.fillRoundedRectangle(r, themeData().radius);
            g.setColour(selected ? ink(Ink::Accent) : ink(Ink::Border));
            g.drawRoundedRectangle(r, themeData().radius, selected ? 2.0f : 1.0f);
            auto inner = r.reduced(8, 4);
            g.setColour(roleColour(b.role));
            g.setFont(juce::FontOptions(10.0f * scale, juce::Font::bold));
            g.drawText(text(b.role).toUpperCase(), inner.removeFromTop(inner.getHeight() * .45f), juce::Justification::bottomLeft);
            g.setColour(ink(Ink::Text));
            g.setFont(juce::FontOptions(14.0f * scale));
            g.drawText(text(b.name), inner, juce::Justification::topLeft, true);
            // Wired in Routing too: a badge (Routing... opens it there).
            if(b.userWires) { g.setColour(juce::Colour(0xffffa040)); g.fillEllipse(r.getRight() - 12, r.getY() + 5, 7, 7); }
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
        auto end = [&](juce::Rectangle<int> r, const juce::String& top, const juce::String& bottom) {
            g.setColour(ink(Ink::Surface)); g.fillRoundedRectangle(r.toFloat(), themeData().radius);
            auto inner = r.reduced(8, 10);
            g.setColour(ink(Ink::Muted)); g.setFont(juce::FontOptions(11.0f * scale, juce::Font::bold));
            g.drawText(top, inner.removeFromTop(16), juce::Justification::topLeft);
            g.setColour(ink(Ink::Text)); g.setFont(juce::FontOptions(13.0f * scale));
            g.drawFittedText(bottom, inner, juce::Justification::topLeft, 3);
        };
        end(ends.removeFromLeft(104), "IN", text(p->input));
        juce::String out = p->through ? text(part(p->through) ? part(p->through)->name : "a bus") : juce::String("Main");
        for(const auto& [bus, level] : p->sends) if(const auto* b = part(bus)) out << "\nSend " << text(b->name) << " " << juce::String(juce::roundToInt(level * 100)) << "%";
        for(const auto& exit : p->exits) out << juce::String::fromUTF8("\n→ ") << text(exit);
        end(ends.removeFromRight(104), "OUT", out);
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
            for(const auto& b : routingBoxes_)
                if(b.bounds.contains(e.getPosition()))
                {
                    const bool again = routed_ == b.node && routedPort_ == b.port;
                    routed_ = again ? 0 : b.node; routedPort_ = again ? 0 : b.port;
                    if(!routed_) routedPort_ = 0;
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
