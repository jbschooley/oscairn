// SPDX-License-Identifier: AGPL-3.0-or-later
#include "PlayView.h"
#include "Icons.h"

namespace oscairn::standalone
{
    namespace
    {
        juce::String text(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }
        constexpr int sceneTiles = 8;
        // What a control drives, by name: "Rhodes, Pad level".
        std::string targetsOf(const Macro& m, const Control::GraphState& graph, const PartsState& parts)
        {
            auto partName = [&](uint32_t id) { for(const auto* list : {&parts.parts, &parts.buses}) for(const auto& p : *list) if(p.id == id) return p.name; return std::string("Part"); };
            std::vector<std::string> names;
            for(const auto& t : m.targets)
            {
                std::string name;
                switch(t.kind)
                {
                case MacroTarget::Kind::Parameter: { const auto n = graph.names.find(t.node); name = n == graph.names.end() ? "Plugin" : n->second; break; }
                case MacroTarget::Kind::PartLevel: name = partName(t.part) + " level"; break;
                case MacroTarget::Kind::PartMute: name = partName(t.part) + " mute"; break;
                case MacroTarget::Kind::Macro: for(const auto& other : graph.macros) if(other.id == t.macro) name = other.name; break;
                case MacroTarget::Kind::Morph: name = "Scene morph"; break;
                }
                if(!name.empty() && std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
            }
            std::string out;
            for(const auto& n : names) out += (out.empty() ? "" : ", ") + n;
            return out.empty() ? "Not assigned" : out;
        }
    }

    /// A knob of the row: what it plays (from the physical controls' layers) and its value
    /// beside a rotary in the theme's style, with its bank; blank where the bank has none.
    class PlayView::Knob : public juce::Component
    {
    public:
        Knob(PlayView& view, int index) : view_(view), index_(index)
        {
            slider_.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            slider_.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
            slider_.setRange(0, 1);
            slider_.onValueChange = [this] { view_.play(slot_.target, slider_.getValue()); repaint(); };
            slider_.onDragStart = [this] { view_.gesture(slot_.target, true); };
            slider_.onDragEnd = [this] { view_.gesture(slot_.target, false); };
            addChildComponent(slider_);
        }
        const ResolvedControl& slot() const { return slot_; }
        void set(const ResolvedControl& slot)
        {
            if(!(slot.target == slot_.target))
            {
                const auto [low, high] = view_.rangeOf(slot.target);
                slider_.setRange(low, high);
            }
            slot_ = slot;
            const bool any = slot.target.kind != Assignment::Kind::None;
            slider_.setVisible(any);
            slider_.setEnabled(slot.target.kind != Assignment::Kind::Scene);
            if(any && !slider_.isMouseButtonDown()) slider_.setValue(view_.valueOf(slot.target), juce::dontSendNotification);
            repaint();
        }
        void paint(juce::Graphics& g) override
        {
            drawSection(g, getLocalBounds(), ink(Ink::Surface));
            if(view_.learning_ && view_.learnFrom_.isNotEmpty()) drawEdge(g, getLocalBounds().toFloat().reduced(1), themeData().radius + 2, true); // a target to touch
            const int m = view_.mode();
            // Beside the dial, or under it when the tile is narrow (six or more across).
            auto r = stacked() ? getLocalBounds().reduced(8, 6).withTrimmedTop(slider_.getBottom() - 2) : getLocalBounds().reduced(12, 8).withTrimmedLeft(slider_.getRight() - 2);
            const auto just = stacked() ? juce::Justification::horizontallyCentred : juce::Justification::left;
            const juce::Font small{juce::FontOptions(textSize(TextRole::Small, m))}, body{juce::FontOptions(textSize(TextRole::Body, m), juce::Font::bold)};
            const int line = int(textSize(TextRole::Body, m)) + 6;
            if(slot_.target.kind == Assignment::Kind::None)
            {
                g.setColour(ink(Ink::Muted)); drawInked(g, "No control", stacked() ? getLocalBounds() : r, small, just);
                return;
            }
            const bool value = r.getHeight() >= line * 3; // a short tile leaves out the value
            auto block = r.withSizeKeepingCentre(r.getWidth(), line * (value ? 3 : 2));
            // Its bank, or Override, after the physical knob it follows on a device.
            const auto where = slot_.layer == AssignmentLayer::Override ? juce::String("Override") : text(slot_.bank);
            g.setColour(ink(Ink::Accent)); drawInked(g, view_.engine_.rowKnobs() > 0 ? juce::String(index_ + 1) + juce::String::fromUTF8(" · ") + where : where, block.removeFromTop(line), small.boldened(), just);
            g.setColour(ink(Ink::Text)); drawInked(g, stacked() ? text(slot_.label).fromLastOccurrenceOf(": ", false, false) : text(slot_.label), block.removeFromTop(line), body, just); // narrow: the parameter, not its plugin
            if(value) { g.setColour(ink(Ink::Muted)); drawInked(g, view_.describe(slot_.target, slider_.getValue()), block.removeFromTop(line), small, just); }
        }
        void resized() override
        {
            if(stacked()) { const int side = std::min(getWidth() - 24, 52); slider_.setBounds((getWidth() - side) / 2, 8, side, side); return; }
            const int side = getHeight() - 16;
            slider_.setBounds(10, 8, side, side);
        }
        static constexpr int narrow = 200; // narrower tiles put the text under the dial
        bool stacked() const { return getWidth() < narrow; }
        void mouseUp(const juce::MouseEvent& e) override { if(!e.mouseWasDraggedSinceMouseDown()) view_.learnTarget(slot_.target); }
    private:
        PlayView& view_;
        int index_;
        ResolvedControl slot_;
        juce::Slider slider_;
    };

    /// A part's or bus's strip: its name and sound, its fader with a meter per channel, mute and
    /// solo; dimmed while muted or silenced by a solo. A tap on its name selects it.
    class PlayView::Strip : public juce::Component
    {
    public:
        Strip(PlayView& view, uint32_t part) : view_(view), part_(part)
        {
            fader_.setSliderStyle(juce::Slider::LinearVertical);
            fader_.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
            fader_.setRange(0, 1);
            fader_.setSkewFactorFromMidPoint(.25); // -12 dB halfway up, as the Mixer's
            fader_.setTooltip("The part's level, before its sends");
            fader_.onValueChange = [this] { const auto id = part_; const double v = fader_.getValue(); const bool m = mute_.getToggleState(); view_.engine_.edit([id, v, m](Control& c) { c.setPartLevel(id, v, m); c.commit(); }); };
            mute_.onClick = [this] {
                if(view_.learning_ && view_.learnFrom_.isNotEmpty()) { mute_.setToggleState(!mute_.getToggleState(), juce::dontSendNotification); Assignment t; t.kind = Assignment::Kind::PartMute; t.id = part_; view_.learnTarget(t); return; }
                const auto id = part_; const double v = fader_.getValue(); const bool m = mute_.getToggleState(); view_.engine_.edit([id, v, m](Control& c) { c.setPartLevel(id, v, m); c.commit(); }); };
            solo_.onClick = [this] { const auto id = part_; const bool s = solo_.getToggleState(); view_.engine_.edit([id, s](Control& c) { c.setPartSolo(id, s); c.commit(); }); };
            for(auto* c : std::initializer_list<juce::Component*>{&fader_, &mute_, &solo_}) addAndMakeVisible(c);
        }
        uint32_t part() const { return part_; }
        void set(const PartRow& p)
        {
            name_ = text(p.name);
            detail_ = p.bus ? "Bus" + (p.chain.empty() ? juce::String() : juce::String::fromUTF8(" · ") + text(p.chain.front().name))
                            : (p.sound.empty() ? juce::String("No instrument") : text(p.sound)) + (p.input.empty() ? juce::String() : juce::String::fromUTF8(" · ") + text(p.input));
            if(!fader_.isMouseButtonDown()) fader_.setValue(p.level, juce::dontSendNotification);
            mute_.setToggleState(p.mute, juce::dontSendNotification);
            solo_.setToggleState(p.solo, juce::dontSendNotification);
            const float alpha = p.mute || p.silenced ? .45f : 1.f;
            for(auto* c : std::initializer_list<juce::Component*>{&fader_, &mute_, &solo_}) c->setAlpha(alpha);
            alpha_ = alpha;
            repaint();
        }
        void paint(juce::Graphics& g) override
        {
            const int m = view_.mode();
            const bool selected = view_.selected_ == part_;
            drawSection(g, getLocalBounds(), ink(Ink::Surface));
            if(selected) drawEdge(g, getLocalBounds().toFloat().reduced(1), themeData().radius + 2, true);
            g.setColour(ink(Ink::Text).withAlpha(alpha_)); drawInked(g, name_, nameArea(), juce::Font(juce::FontOptions(textSize(TextRole::Body, m), juce::Font::bold)));
            g.setColour(ink(Ink::Muted).withAlpha(alpha_)); drawInked(g, detail_, detailArea(), juce::Font(juce::FontOptions(textSize(TextRole::Small, m))));
            // A bar per channel of its sound, beside the fader.
            const auto it = view_.meters_.find(part_);
            const auto channels = it == view_.meters_.end() || it->second.empty() ? std::vector<double>{0.0} : it->second;
            auto bars = meter_.toFloat();
            const float width = (bars.getWidth() - 3.f * float(channels.size() - 1)) / float(channels.size());
            for(const auto peak : channels) { drawMeter(g, bars.removeFromLeft(width), peak * alpha_, true); bars.removeFromLeft(3); }
        }
        void resized() override
        {
            auto r = inner();
            r.removeFromTop(nameArea().getHeight() + detailArea().getHeight() + 6);
            auto marks = r.removeFromBottom(view_.mode() == 2 ? 40 : 30);
            const int side = marks.getHeight();
            mute_.setBounds(marks.withSizeKeepingCentre(side * 2 + 8, side).removeFromLeft(side));
            solo_.setBounds(marks.withSizeKeepingCentre(side * 2 + 8, side).removeFromRight(side));
            r.removeFromBottom(8);
            // The fader and its meters as a pair, centered.
            auto pair = r.withSizeKeepingCentre(std::min(r.getWidth(), 28 + 12 + 14), r.getHeight());
            fader_.setBounds(pair.removeFromLeft(28)); pair.removeFromLeft(12);
            meter_ = pair.removeFromLeft(14).reduced(0, 8);
        }
        void mouseUp(const juce::MouseEvent& e) override
        {
            if(e.mouseWasDraggedSinceMouseDown() || e.y >= detailArea().getBottom() + 4) return;
            if(view_.learning_ && view_.learnFrom_.isNotEmpty()) { Assignment t; t.kind = Assignment::Kind::PartLevel; t.id = part_; view_.learnTarget(t); return; }
            view_.select(view_.selected_ == part_ ? 0 : part_);
        }
    private:
        juce::Rectangle<int> inner() const { return getLocalBounds().reduced(10, 10); }
        juce::Rectangle<int> nameArea() const { return inner().removeFromTop(int(textSize(TextRole::Body, view_.mode())) + 8); }
        juce::Rectangle<int> detailArea() const { return inner().withTrimmedTop(nameArea().getHeight()).removeFromTop(int(textSize(TextRole::Small, view_.mode())) + 6); }
        PlayView& view_;
        uint32_t part_;
        juce::String name_, detail_;
        float alpha_ = 1;
        juce::Slider fader_;
        MarkButton mute_{"M", Ink::Danger, "Mute"}, solo_{"S", Ink::Accent, "Solo"};
        juce::Rectangle<int> meter_;
    };

    PlayView::PlayView(Engine& engine) : engine_(engine)
    {
        previous_.setTooltip("The setlist's previous Performance");
        next_.setTooltip("The setlist's next Performance");
        previous_.onClick = [this] { if(step) step(-1); };
        next_.onClick = [this] { if(step) step(1); };
        performanceControls_.setTooltip("The knobs follow the Performance's controls");
        partControlsButton_.setTooltip("The knobs follow the selected part's own controls");
        for(auto* b : {&performanceControls_, &partControlsButton_}) { b->setClickingTogglesState(true); b->setRadioGroupId(1); }
        performanceControls_.onClick = [this] { showPartControls(false); };
        partControlsButton_.onClick = [this] { showPartControls(true); };
        pageButton_.setTooltip("The next bank of controls");
        pageButton_.onClick = [this] { ++page_; shape_.clear(); refresh(); };
        learnDefault_.setTooltip("Back to what the knob plays by default");
        learnDefault_.onClick = [this] { learned({}); };
        learnDone_.onClick = [this] { learn(false); };
        for(auto* c : std::initializer_list<juce::Component*>{&previous_, &next_, &performanceControls_, &partControlsButton_, &pageButton_, &learnDefault_, &learnDone_}) addChildComponent(c);
        viewport_.setViewedComponent(&stripRow_, false);
        viewport_.setScrollBarsShown(false, true);
        addAndMakeVisible(viewport_);
        refresh();
        startTimer(50);
    }

    PlayView::~PlayView() { stopTimer(); }

    namespace
    {
        // The group the knob row shows, by its place among the device's groups; none: past the end.
        size_t knobGroupIndex(const std::vector<ControlGroup>& groups)
        {
            for(const auto role : {ControlGroup::Role::Screen, ControlGroup::Role::Assign})
                for(size_t i = 0; i < groups.size(); ++i)
                    if(groups[i].role == role && (groups[i].kind == "knobs" || groups[i].kind == "encoders")) return i;
            return groups.size();
        }
    }

    ControlGroup PlayView::knobGroup() const
    {
        const auto& groups = engine_.controlGroups();
        if(const auto i = knobGroupIndex(groups); i < groups.size()) return groups[i];
        ControlGroup g; g.kind = "knobs"; g.count = uint32_t(engine_.rowKnobs() > 0 ? engine_.rowKnobs() : 4);
        return g;
    }

    std::vector<ControlGroup> controlGroups(const Profile& profile)
    {
        std::vector<ControlGroup> out;
        for(const auto& c : profile.controls)
        {
            ControlGroup g;
            g.role = c.role == "notes" ? ControlGroup::Role::Notes : c.role == "screen" ? ControlGroup::Role::Screen : c.role == "fixed" ? ControlGroup::Role::Fixed : ControlGroup::Role::Assign;
            g.kind = c.kind; g.count = uint32_t(c.count); g.modes = c.modes;
            for(const auto& r : c.rows)
                for(size_t k = 0; k < bankKinds.size(); ++k) if(r.bank == bankKinds[k]) g.rows.push_back({r.name, BankKind(k)});
            // Its default target, and a controller's MIDI (RFC 0024).
            g.scenes = c.target == "scenes"; g.paged = !c.paging.empty();
            if(c.role != "fixed") g.functions = c.functions;
            if(profile.controller() && c.midi.channel)
            {
                using Message = ControlGroup::Midi::Message;
                g.midi.device = profile.id; g.midi.port = uint32_t(c.midi.port); g.midi.channel = uint32_t(c.midi.channel);
                g.midi.channels.assign(c.midi.channels.begin(), c.midi.channels.end());
                g.midi.message = c.midi.pitchBend ? Message::PitchBend : c.midi.cc.empty() ? Message::Note : Message::CC;
                for(const auto n : c.midi.cc.empty() ? c.midi.notes : c.midi.cc) g.midi.numbers.push_back(uint8_t(n));
                for(const auto& e : c.midi.encodings)
                {
                    g.midi.encodings.push_back(Encoding::Steps); // "steps", unless it is one of the names
                    for(size_t k = 0; k < encodings.size(); ++k) if(e == encodings[k]) g.midi.encodings.back() = Encoding(k);
                }
                for(const auto& [up, down] : c.midi.steps) g.midi.steps.push_back({uint8_t(up), uint8_t(down)});
                g.midi.latch = c.midi.button == "latch";
            }
            out.push_back(std::move(g));
        }
        return out;
    }

    void PlayView::select(uint32_t part) { selected_ = part; if(!part) partControls_ = false; page_ = 0; refresh(); repaint(); }
    void PlayView::showPartControls(bool part) { partControls_ = part; page_ = 0; shape_.clear(); refresh(); }

    void PlayView::timerCallback()
    {
        if(!isShowing()) return; // Edit's views are showing, or the app is in the background
        if(learning_ && engine_.touches() != touches_)
        {
            touches_ = engine_.touches(); learnFrom_ = text(engine_.lastTouched());
            learnDefault_.setVisible(true); for(auto& k : knobs_) k->repaint(); repaint();
        }
        meters_.clear();
        for(const auto& m : engine_.meters()) meters_[m.part] = m.channels;
        for(auto& s : strips_) s->repaint();
        if(++ticks_ % 4 == 0) refresh();
    }

    void PlayView::refresh()
    {
        try { graph_ = engine_.graph(); } catch(const std::exception&) { return; }
        state_ = readParts(graph_);
        const auto* part = [&]() -> const PartRow* { for(const auto* list : {&state_.parts, &state_.buses}) for(const auto& p : *list) if(p.id == selected_) return &p; return nullptr; }();
        if(selected_ && !part) { selected_ = 0; partControls_ = false; }
        // The knobs: the Performance's controls, or the selected part's banks, through the
        // physical controls' layers (overrides first).
        const uint32_t owner = partControls_ && selected_ ? selected_ : 0;
        source_ = assignmentSource(graph_, engine_.assignments(), [this](NodeId node) { return parametersOf(node); });
        const auto group = knobGroup();
        const int banks = int(groupPositions(source_, group, owner));
        page_ %= banks;
        knobSlots_ = resolveControls(source_, {group}, owner, {uint32_t(page_)});
        // A controller's knobs play the page and part shown.
        if(const auto i = knobGroupIndex(engine_.controlGroups()); i < engine_.controlGroups().size())
        {
            auto& positions = engine_.positions();
            if(positions.size() <= i) positions.resize(i + 1);
            positions[i] = uint32_t(page_);
        }
        engine_.setKnobPart(owner);
        pageButton_.setVisible(banks > 1);
        // The switch between the Performance's controls and the part's shows while a part is selected.
        if((selected_ != 0) != performanceControls_.isVisible()) { performanceControls_.setVisible(selected_ != 0); partControlsButton_.setVisible(selected_ != 0); resized(); }
        // Rebuilt when what shows changes, so a fader being moved survives the rest.
        std::string shape = std::to_string(getWidth()) + "x" + std::to_string(getHeight()) + "/" + std::to_string(owner) + "/" + std::to_string(banks) + "/" + std::to_string(group.count);
        for(const auto* list : {&state_.parts, &state_.buses}) for(const auto& p : *list) shape += "," + std::to_string(p.id);
        if(shape != shape_) { shape_ = shape; rebuild(); }
        for(auto& s : strips_) for(const auto* list : {&state_.parts, &state_.buses}) for(const auto& p : *list) if(p.id == s->part()) s->set(p);
        for(size_t i = 0; i < knobs_.size() && i < knobSlots_.size(); ++i) knobs_[i]->set(knobSlots_[i]);
        const bool setlist = inSetlist && inSetlist();
        if(setlist != previous_.isVisible()) { previous_.setVisible(setlist); next_.setVisible(setlist); resized(); }
        if(part) partControlsButton_.setButtonText(text(part->name));
        performanceControls_.setToggleState(!owner, juce::dontSendNotification);
        partControlsButton_.setToggleState(owner != 0, juce::dontSendNotification);
        pageButton_.setButtonText(group.rows.empty() ? "Bank " + juce::String(page_ + 1) + "/" + juce::String(banks) : text(group.rows[size_t(page_)].name)); // a row by its name
        publishRow();
        repaint();
    }

    // The device's knobs bound "row:N" play the bank shown.
    void PlayView::publishRow()
    {
        if(!isShowing() || engine_.rowKnobs() <= 0) return;
        std::vector<Engine::RowParam> row;
        for(const auto& slot : knobSlots_)
        {
            const auto& t = slot.target;
            // shortcut: a device's knob plays a parameter or a control; levels, mutes and scenes on a knob show here only.
            if(t.kind == Assignment::Kind::Parameter) row.push_back({nodeName(t.node), t.param, slot.label, 0});
            else if(t.kind == Assignment::Kind::Control) row.push_back({{}, 0, slot.label, t.id});
            else row.push_back({});
        }
        engine_.setRow(std::move(row));
    }

    std::vector<Control::ParamInfo> PlayView::parametersOf(NodeId node)
    {
        // Read once per plugin: a node replaced by another plugin is read again.
        std::string plugin;
        for(const auto& n : graph_.topology.nodes) if(n.id == node) plugin = n.plugin.id;
        auto& cached = parameters_[node];
        if(cached.first != plugin || plugin.empty()) { try { cached = {plugin, engine_.parameters(node)}; } catch(const std::exception&) { cached = {plugin, {}}; } }
        return cached.second;
    }

    std::string PlayView::nodeName(NodeId node) const
    {
        const auto it = graph_.names.find(node);
        return it == graph_.names.end() ? std::string() : it->second;
    }

    const PartRow* PlayView::partRow(uint32_t id) const
    {
        for(const auto* list : {&state_.parts, &state_.buses}) for(const auto& p : *list) if(p.id == id) return &p;
        return nullptr;
    }

    std::pair<double, double> PlayView::rangeOf(const Assignment& t) const
    {
        if(t.kind == Assignment::Kind::Parameter && engine_.hasParam(nodeName(t.node), t.param)) { const auto info = engine_.paramInfo(nodeName(t.node), t.param); return {info.minimum, info.maximum}; }
        return {0.0, 1.0};
    }

    double PlayView::valueOf(const Assignment& t) const
    {
        switch(t.kind)
        {
        case Assignment::Kind::Parameter: return engine_.hasParam(nodeName(t.node), t.param) ? engine_.param(nodeName(t.node), t.param) : 0.0;
        case Assignment::Kind::Control: for(const auto& m : graph_.macros) if(m.id == t.id) return m.value; return 0.0;
        case Assignment::Kind::PartLevel: if(const auto* p = partRow(t.id)) return p->level; return 0.0;
        case Assignment::Kind::PartMute: if(const auto* p = partRow(t.id)) return p->mute ? 1.0 : 0.0; return 0.0;
        case Assignment::Kind::Scene: return graph_.scene == t.id ? 1.0 : 0.0;
        case Assignment::Kind::None: break;
        }
        return 0.0;
    }

    void PlayView::play(const Assignment& t, double v)
    {
        switch(t.kind)
        {
        case Assignment::Kind::Parameter: engine_.setParam(nodeName(t.node), t.param, v); break;
        case Assignment::Kind::Control: engine_.setControl(t.id, v); break;
        case Assignment::Kind::PartLevel: { const auto* p = partRow(t.id); const bool m = p && p->mute; const auto id = t.id; engine_.edit([id, v, m](Control& c) { c.setPartLevel(id, v, m); c.commit(); }); break; }
        case Assignment::Kind::PartMute: { const auto* p = partRow(t.id); const double level = p ? p->level : 1; const auto id = t.id; const bool m = v >= .5; engine_.edit([id, level, m](Control& c) { c.setPartLevel(id, level, m); c.commit(); }); break; }
        case Assignment::Kind::Scene: case Assignment::Kind::None: break;
        }
    }

    void PlayView::gesture(const Assignment& t, bool begin)
    {
        if(t.kind == Assignment::Kind::Parameter) engine_.paramGesture(nodeName(t.node), t.param, begin);
    }

    juce::String PlayView::describe(const Assignment& t, double v) const
    {
        switch(t.kind)
        {
        case Assignment::Kind::Parameter: return text(engine_.paramText(nodeName(t.node), t.param, v));
        case Assignment::Kind::Control: for(const auto& m : graph_.macros) if(m.id == t.id) return text(targetsOf(m, graph_, state_)) + juce::String::fromUTF8(" · ") + juce::String(juce::roundToInt(v * 100)) + "%"; return {};
        case Assignment::Kind::PartLevel: return v > 0 ? juce::String(20 * std::log10(v), 1) + " dB" : juce::String::fromUTF8("−∞ dB");
        case Assignment::Kind::PartMute: return v >= .5 ? "Muted" : "On";
        case Assignment::Kind::Scene: return graph_.scene == t.id ? "Playing" : "Scene";
        case Assignment::Kind::None: break;
        }
        return {};
    }

    void PlayView::learn(bool on)
    {
        learning_ = on; learnFrom_.clear(); touches_ = engine_.touches();
        learnDone_.setVisible(on); learnDefault_.setVisible(false);
        resized(); repaint();
        for(auto& k : knobs_) k->repaint();
    }

    void PlayView::learnTarget(const Assignment& t) { if(learning_ && learnFrom_.isNotEmpty() && t.kind != Assignment::Kind::None) learned(t); }

    void PlayView::learned(const Assignment& t)
    {
        const auto control = learnFrom_.toStdString();
        if(control.empty()) return;
        engine_.edit([control, t](Control& c) { c.assign(control, t); });
        learn(false);
    }

    void PlayView::rebuild()
    {
        knobs_.clear();
        for(int i = 0; i < knobs(); ++i) { knobs_.push_back(std::make_unique<Knob>(*this, i)); addAndMakeVisible(*knobs_.back()); }
        strips_.clear();
        for(const auto* list : {&state_.parts, &state_.buses})
            for(const auto& p : *list) { strips_.push_back(std::make_unique<Strip>(*this, p.id)); stripRow_.addAndMakeVisible(*strips_.back()); }
        resized();
    }

    void PlayView::resized()
    {
        const bool touch = mode() == 2;
        const int gap = sectionGap(touch ? 10 : 8), pad = touch ? 14 : 12;
        auto area = getLocalBounds().reduced(pad);
        // The scenes, with Previous and Next at the ends when the Performance is from the setlist.
        auto row = area.removeFromTop(touch ? 56 : 46);
        if(previous_.isVisible())
        {
            previous_.setBounds(row.removeFromLeft(touch ? 120 : 100)); row.removeFromLeft(touch ? 10 : 8);
            next_.setBounds(row.removeFromRight(touch ? 120 : 100)); row.removeFromRight(touch ? 10 : 8);
        }
        scenes_.clear();
        const int sceneGap = touch ? 8 : 6, sceneWidth = (row.getWidth() - sceneGap * (sceneTiles - 1)) / sceneTiles;
        for(int i = 0; i < sceneTiles; ++i) scenes_.push_back(row.removeFromLeft(sceneWidth)), row.removeFromLeft(sceneGap);
        area.removeFromTop(touch ? 12 : 10);
        // The knobs, and their switch and bank at the right.
        // Taller when the tiles are too narrow for text beside the dial (six screen knobs and more),
        // but at most a quarter of the view, leaving the strips room on a short screen.
        const int sideWidth = (performanceControls_.isVisible() || pageButton_.isVisible() ? (touch ? 150 : 130) + 8 : 0) + (performanceControls_.isVisible() && pageButton_.isVisible() ? (touch ? 110 : 96) + 8 : 0);
        const int tiles = std::max(1, int(knobs_.size()));
        const bool tall = (area.getWidth() - sideWidth - gap * (tiles - 1)) / tiles < Knob::narrow;
        knobRow_ = area.removeFromTop(tall ? std::min(touch ? 140 : 128, getHeight() / 4) : touch ? 84 : 72);
        auto knobs = knobRow_;
        const bool side = performanceControls_.isVisible() || pageButton_.isVisible();
        if(side)
        {
            auto column = knobs.removeFromRight(touch ? 150 : 130); knobs.removeFromRight(gap ? gap : 8);
            const int h = touch ? 40 : 30;
            if(performanceControls_.isVisible()) { performanceControls_.setBounds(column.removeFromTop(knobRow_.getHeight() / 2).withSizeKeepingCentre(column.getWidth(), h)); partControlsButton_.setBounds(column.withSizeKeepingCentre(column.getWidth(), h)); }
            if(pageButton_.isVisible() && !performanceControls_.isVisible()) pageButton_.setBounds(column.withSizeKeepingCentre(column.getWidth(), h));
            else if(pageButton_.isVisible()) { pageButton_.setBounds(knobs.removeFromRight(touch ? 110 : 96).withSizeKeepingCentre(touch ? 110 : 96, h)); knobs.removeFromRight(8); }
        }
        const int n = std::max(1, int(knobs_.size())), w = (knobs.getWidth() - gap * (n - 1)) / n;
        for(auto& k : knobs_) { k->setBounds(knobs.removeFromLeft(w)); knobs.removeFromLeft(gap); }
        area.removeFromTop(touch ? 12 : 10);
        // Learning: what to do next, over the strips.
        banner_ = {};
        if(learning_)
        {
            banner_ = area.removeFromTop(touch ? 48 : 40);
            auto buttons = banner_.reduced(6);
            learnDone_.setBounds(buttons.removeFromRight(touch ? 90 : 76)); buttons.removeFromRight(6);
            learnDefault_.setBounds(buttons.removeFromRight(touch ? 100 : 86));
            area.removeFromTop(touch ? 10 : 8);
        }
        // The strips: at least six across, six fitting an 800-wide screen; more scroll sideways.
        viewport_.setBounds(area);
        const int count = std::max(1, int(strips_.size())), minWidth = touch ? 120 : 110;
        const int width = std::max(minWidth, (area.getWidth() - gap * (count - 1)) / std::max(count, 6));
        const int height = area.getHeight() - (count * (width + gap) - gap > area.getWidth() ? viewport_.getScrollBarThickness() : 0);
        stripRow_.setSize(std::max(area.getWidth(), count * (width + gap) - gap), height);
        int x = 0;
        for(auto& s : strips_) { s->setBounds(x, 0, width, height); x += width + gap; }
    }

    void PlayView::paint(juce::Graphics& g)
    {
        g.fillAll(ink(Ink::Background));
        const int m = mode();
        const juce::Font small{juce::FontOptions(textSize(TextRole::Small, m))}, name{juce::FontOptions(textSize(TextRole::Button, m), juce::Font::bold)};
        for(size_t i = 0; i < scenes_.size(); ++i)
        {
            const auto r = scenes_[i];
            const auto rf = r.toFloat().reduced(.5f);
            const float radius = themeData().style.sections == ThemeStyle::Sections::Joined ? 0.f : themeData().radius + 2; // as drawSection's
            const bool filled = i < graph_.scenes.size(), on = filled && graph_.scenes[i].id == graph_.scene;
            if(on) { g.setColour(ink(Ink::Accent).withAlpha(.17f)); g.fillRoundedRectangle(rf, radius); g.setColour(ink(Ink::Accent)); g.drawRoundedRectangle(rf.reduced(.5f), radius, 2.f); }
            else if(filled) drawSection(g, r, ink(Ink::Surface));
            else { g.setColour(ink(Ink::Border).withAlpha(.35f)); g.drawRoundedRectangle(rf, radius, 1.f); }
            g.setColour(ink(Ink::Muted).withAlpha(filled ? 1.f : .6f));
            drawInked(g, juce::String(int(i) + 1), r.reduced(9, 0).withHeight(int(textSize(TextRole::Small, m)) + 10).translated(0, 2), small);
            const auto label = filled ? text(graph_.scenes[i].name) : i == graph_.scenes.size() ? juce::String("+ Scene") : juce::String();
            g.setColour(on ? ink(Ink::Accent) : filled ? ink(Ink::Text) : ink(Ink::Muted));
            drawInked(g, label, r.reduced(8, 0), filled ? name : small, juce::Justification::horizontallyCentred);
        }
        if(!banner_.isEmpty())
        {
            g.setColour(ink(Ink::Accent).withAlpha(.16f)); g.fillRoundedRectangle(banner_.toFloat(), themeData().radius + 2);
            const auto knob = learnFrom_.replaceCharacter(':', ' ').replaceSection(0, 1, learnFrom_.substring(0, 1).toUpperCase());
            g.setColour(ink(Ink::Text));
            drawInked(g, learnFrom_.isEmpty() ? juce::String("Learn: move a knob on the panel") : knob + ": touch what it should play: a strip, its M, a scene or a knob",
                      banner_.reduced(14, 0).withRight(learnDefault_.getX() - 8), juce::Font(juce::FontOptions(textSize(TextRole::Body, m))));
        }
        if(strips_.empty())
        {
            g.setColour(ink(Ink::Muted));
            drawInked(g, "No parts yet. Edit adds them.", viewport_.getBounds(), juce::Font(juce::FontOptions(textSize(TextRole::Body, m))), juce::Justification::horizontallyCentred);
        }
    }

    void PlayView::mouseUp(const juce::MouseEvent& e)
    {
        if(e.mouseWasDraggedSinceMouseDown()) return;
        for(size_t i = 0; i < scenes_.size(); ++i)
        {
            if(!scenes_[i].contains(e.getPosition())) continue;
            // shortcut: the first eight scenes only; page them when a Performance needs more.
            if(i < graph_.scenes.size() && learning_ && learnFrom_.isNotEmpty()) { Assignment t; t.kind = Assignment::Kind::Scene; t.id = graph_.scenes[i].id; learnTarget(t); }
            else if(i < graph_.scenes.size()) { const auto id = graph_.scenes[i].id; engine_.edit([id](Control& c) { c.selectScene(id); }); }
            else if(i == graph_.scenes.size()) { const auto n = graph_.scenes.size() + 1; engine_.edit([n](Control& c) { c.addScene("Scene " + std::to_string(n)); }); }
            return;
        }
    }

    PerformanceApp::PerformanceApp(Engine& engine, const std::string& scanner) : play_(engine), parts_(engine, scanner)
    {
        parts_.onDone = [this] { juce::MessageManager::callAsync([safe = juce::Component::SafePointer<PerformanceApp>(this)] { if(safe) safe->edit(false); }); };
        addAndMakeVisible(play_);
        addChildComponent(parts_);
    }

    void PerformanceApp::attach(TopBar& bar, std::function<void()> save) { bar_ = &bar; save_ = std::move(save); actions(); }

    void PerformanceApp::edit(bool editing)
    {
        // Edit opens on the part selected in Play, or the parts list.
        if(editing && !parts_.isVisible()) parts_.showPart(play_.selectedPart());
        parts_.setVisible(editing); play_.setVisible(!editing);
        actions();
    }

    void PerformanceApp::actions()
    {
        if(!bar_) return;
        std::vector<TopBar::Action> list;
        const bool editing = parts_.isVisible();
        list.push_back({editing ? "Play" : "Edit", [safe = juce::Component::SafePointer<PerformanceApp>(this), editing] { if(safe) safe->edit(!editing); }, !editing});
        if(save_) list.push_back({"Save", save_});
        if(!editing && play_.canLearn()) list.push_back({"Learn", [safe = juce::Component::SafePointer<PerformanceApp>(this)] { if(safe) safe->play_.learn(!safe->play_.learning()); }});
        bar_->setActions(std::move(list));
    }

    void PerformanceApp::resized() { play_.setBounds(getLocalBounds()); parts_.setBounds(getLocalBounds()); }
}
