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
        for(auto* c : std::initializer_list<juce::Component*>{&title_, &message_, &done_, &addPart_, &addBus_, &back_, &removePart_, &name_, &controlsButton_, &addControl_}) addChildComponent(c);
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
        back_.onClick = [this] { page_ = 0; selected_ = 0; shape_.clear(); refresh(); };
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
        refresh();
    }

    const PartRow* PartsView::part(uint32_t id) const
    {
        for(const auto* list : {&state_.parts, &state_.buses}) for(const auto& p : *list) if(p.id == id) return &p;
        return nullptr;
    }

    void PartsView::showPart(uint32_t id) { page_ = id; selected_ = 0; shape_.clear(); refresh(); }

    void PartsView::refresh()
    {
        try
        {
            graph_ = engine_.graph();
            state_ = readParts(graph_); scenes_ = graph_.scenes; sceneParameters_ = graph_.sceneParameters; scene_ = graph_.scene; macros_ = graph_.macros;
        }
        catch(const std::exception&) { return; }
        if(page_ && page_ != controlsPage && !part(page_)) { page_ = 0; shape_.clear(); }
        message_.setText(text(engine_.editError()), juce::dontSendNotification);
        // Controls are rebuilt only when the structure changes, so a slider being dragged survives.
        std::string shape = std::to_string(page_) + "/" + std::to_string(selected_) + "/" + std::to_string(getWidth()) + "x" + std::to_string(getHeight());
        for(const auto* list : {&state_.parts, &state_.buses})
            for(const auto& p : *list) { shape += "|" + std::to_string(p.id) + ":" + p.name + ":" + std::to_string(p.through); for(const auto& b : p.chain) shape += "," + std::to_string(b.node); }
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
        controls_.clear(); boxes_.clear(); updaters_.clear();
        const bool list = page_ == 0, controlsShown = page_ == controlsPage;
        for(auto* c : std::initializer_list<juce::Component*>{&addPart_, &addBus_, &controlsButton_}) c->setVisible(list);
        for(auto* c : std::initializer_list<juce::Component*>{&removePart_, &name_}) c->setVisible(!list && !controlsShown);
        back_.setVisible(!list); addControl_.setVisible(controlsShown);
        title_.setVisible(list || controlsShown); done_.setVisible(true); message_.setVisible(true);
        title_.setText(controlsShown ? "Controls" : "Parts", juce::dontSendNotification);
        auto top = getLocalBounds().reduced(16, 8).removeFromTop(header - 8);
        done_.setBounds(top.removeFromRight(100).reduced(2));
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
            addBus_.setBounds(top.removeFromRight(120).reduced(2));
            addPart_.setBounds(top.removeFromRight(120).reduced(2));
            controlsButton_.setBounds(top.removeFromRight(100).reduced(2));
            title_.setBounds(top.removeFromLeft(90));
            // Scenes: tap one to switch to it; Store saves the parts' faders and the scene
            // parameters into the selected one.
            const auto sceneWidth = std::min(110, std::max(56, (top.getWidth() - 200) / std::max(1, int(scenes_.size()))));
            for(const auto& s : scenes_)
            {
                auto* b = button(top.removeFromLeft(sceneWidth).reduced(2), text(s.name), [this, id = s.id] { edit([id](Control& c) { c.selectScene(id); }); });
                b->setColour(juce::TextButton::buttonColourId, s.id == scene_ ? ink(Ink::Accent) : ink(Ink::Raised));
                b->setTooltip("Switch to this scene");
            }
            button(top.removeFromLeft(90).reduced(2), "+ Scene", [this] { const auto n = scenes_.size() + 1; edit([n](Control& c) { c.addScene("Scene " + std::to_string(n)); }); })
                ->setTooltip("A new scene with the parts' levels and mutes, and the scene controls, as they are now");
            if(scene_)
                button(top.removeFromLeft(80).reduced(2), "Store", [this] { const auto id = scene_; edit([id](Control& c) { c.storeScene(id); }); })
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
            // The chain: IN, each box with a + before it, a last +, OUT.
            auto chain = getLocalBounds().reduced(16, 0).withTrimmedTop(header + 12).withHeight(96);
            const int n = int(p->chain.size());
            const int boxWidth = std::max(80, std::min(150, (chain.getWidth() - 2 * 110 - (n + 1) * 34) / std::max(1, n)));
            chain.removeFromLeft(110);
            for(int i = 0; i <= n; ++i)
            {
                auto* plus = button(chain.removeFromLeft(34).withSizeKeepingCentre(30, 30), "+", {});
                plus->setTooltip("Insert a plugin here");
                plus->onClick = [this, i, plus] { insertMenu(size_t(i), *plus); };
                if(i < n) boxes_.push_back({p->chain[size_t(i)].node, chain.removeFromLeft(boxWidth).reduced(3)});
            }
            auto panel = getLocalBounds().reduced(16).withTrimmedTop(header + 12 + 96 + 12).withTrimmedBottom(28);
            // The selected box's actions.
            if(selected_)
            {
                auto actions = panel.removeFromTop(40);
                const auto at = std::find_if(p->chain.begin(), p->chain.end(), [&](const PartBox& b) { return b.node == selected_; });
                if(at != p->chain.end())
                {
                    const auto index = size_t(at - p->chain.begin());
                    const auto id = p->id; const auto node = selected_;
                    button(actions.removeFromLeft(110).reduced(2), "Move left", [this, id, index] { if(index) edit([id, index](Control& c) { c.movePartNode(id, index, index - 1); c.commit(); }); })->setEnabled(index > 0);
                    button(actions.removeFromLeft(110).reduced(2), "Move right", [this, id, index] { edit([id, index](Control& c) { c.movePartNode(id, index, index + 1); c.commit(); }); })->setEnabled(index + 1 < p->chain.size());
                    button(actions.removeFromLeft(110).reduced(2), "Remove", [this, id, node] { selected_ = 0; edit([id, node](Control& c) { removeBox(c, id, node); }); });
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
            if(!p->bus && p->input.rfind("Keyboard", 0) == 0)
            {
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

    void PartsView::insertMenu(size_t index, juce::Component& at)
    {
        juce::PopupMenu menu;
        if(plugins_.empty()) menu.addItem(1, scan_.valid() ? "Finding plugins..." : "No plugins found", false);
        for(size_t i = 0; i < plugins_.size(); ++i) menu.addItem(int(i) + 1, text(plugins_[i].name));
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&at), [safe = juce::Component::SafePointer<PartsView>(this), index](int chosen) {
            if(!safe || chosen <= 0 || size_t(chosen) > safe->plugins_.size()) return;
            const auto plugin = safe->plugins_[size_t(chosen) - 1]; const auto id = safe->page_;
            safe->edit([id, index, plugin](Control& c) { insertPlugin(c, id, index, plugin); });
        });
    }

    void PartsView::paint(juce::Graphics& g)
    {
        g.fillAll(ink(Ink::Background));
        if(page_ == controlsPage) return;
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
                    g.setColour(p.mute ? ink(Ink::Surface).withAlpha(.5f) : ink(Ink::Surface));
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
                    if(!p.bus && p.input.rfind("Keyboard", 0) == 0)
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
            return;
        }
        const auto* p = part(page_);
        if(!p) return;
        auto ends = getLocalBounds().reduced(16, 0).withTrimmedTop(header + 12).withHeight(96);
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
        end(ends.removeFromRight(104), "OUT", out);
        for(const auto& box : boxes_)
            for(const auto& b : p->chain) if(b.node == box.node) chip(box.bounds.toFloat(), b, b.node == selected_);
        if(p->chain.empty())
        {
            g.setColour(ink(Ink::Muted)); g.setFont(juce::FontOptions(14.0f * scale));
            g.drawText("Press + to add an instrument or effect", ends.reduced(50, 0), juce::Justification::centred);
        }
    }

    void PartsView::resized() { shape_.clear(); refresh(); }

    void PartsView::mouseUp(const juce::MouseEvent& e)
    {
        if(page_ == controlsPage) return;
        if(page_ == 0)
        {
            for(size_t row = 0; row < state_.parts.size() + state_.buses.size(); ++row)
                if(rowBounds(row).contains(e.getPosition()))
                    return showPart(row < state_.parts.size() ? state_.parts[row].id : state_.buses[row - state_.parts.size()].id);
            return;
        }
        for(const auto& box : boxes_)
            if(box.bounds.contains(e.getPosition())) { selected_ = selected_ == box.node ? 0 : box.node; shape_.clear(); refresh(); return; }
    }
}
