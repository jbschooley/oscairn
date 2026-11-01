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

    /// A knob of the row: the control's name, what it drives and its value beside a rotary in
    /// the theme's style; blank where the bank has no control.
    class PlayView::Knob : public juce::Component
    {
    public:
        Knob(PlayView& view, int index) : view_(view), index_(index)
        {
            slider_.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
            slider_.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
            slider_.setRange(0, 1);
            slider_.onValueChange = [this] { const auto id = id_; const double v = slider_.getValue(); if(id) view_.engine_.setControl(id, v); repaint(); };
            addChildComponent(slider_);
        }
        void set(const Macro* control, const std::string& detail)
        {
            id_ = control ? control->id : 0;
            name_ = control ? text(control->name) : juce::String();
            detail_ = text(detail);
            slider_.setVisible(control != nullptr);
            if(control && !slider_.isMouseButtonDown()) slider_.setValue(control->value, juce::dontSendNotification);
            repaint();
        }
        void paint(juce::Graphics& g) override
        {
            drawSection(g, getLocalBounds(), ink(Ink::Surface));
            const int m = view_.mode();
            auto r = getLocalBounds().reduced(12, 8).withTrimmedLeft(slider_.getRight() - 2);
            const juce::Font small{juce::FontOptions(textSize(TextRole::Small, m))}, body{juce::FontOptions(textSize(TextRole::Body, m), juce::Font::bold)};
            const int line = int(textSize(TextRole::Body, m)) + 6;
            if(!id_)
            {
                g.setColour(ink(Ink::Muted)); drawInked(g, "No control", r, small);
                return;
            }
            auto block = r.withSizeKeepingCentre(r.getWidth(), line * 3);
            // The physical knob it follows on a device, as its panel numbers them.
            if(view_.engine_.rowKnobs() > 0) { g.setColour(ink(Ink::Accent)); drawInked(g, juce::String(index_ + 1), block.removeFromTop(line), small.boldened()); }
            else block.translate(0, line / 2);
            g.setColour(ink(Ink::Text)); drawInked(g, name_, block.removeFromTop(line), body);
            g.setColour(ink(Ink::Muted)); drawInked(g, detail_ + juce::String::fromUTF8(" · ") + juce::String(juce::roundToInt(slider_.getValue() * 100)) + "%", block.removeFromTop(line), small);
        }
        void resized() override
        {
            const int side = getHeight() - 16;
            slider_.setBounds(10, 8, side, side);
        }
    private:
        PlayView& view_;
        int index_;
        uint32_t id_ = 0;
        juce::String name_, detail_;
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
            mute_.onClick = [this] { const auto id = part_; const double v = fader_.getValue(); const bool m = mute_.getToggleState(); view_.engine_.edit([id, v, m](Control& c) { c.setPartLevel(id, v, m); c.commit(); }); };
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
            if(!e.mouseWasDraggedSinceMouseDown() && e.y < detailArea().getBottom() + 4) view_.select(view_.selected_ == part_ ? 0 : part_);
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
        for(auto* c : std::initializer_list<juce::Component*>{&previous_, &next_, &performanceControls_, &partControlsButton_, &pageButton_}) addChildComponent(c);
        viewport_.setViewedComponent(&stripRow_, false);
        viewport_.setScrollBarsShown(false, true);
        addAndMakeVisible(viewport_);
        refresh();
        startTimer(50);
    }

    PlayView::~PlayView() { stopTimer(); }

    int PlayView::knobs() const { return engine_.rowKnobs() >= 8 ? 8 : 4; } // banks in fours; an eight-knob device shows two

    void PlayView::select(uint32_t part) { selected_ = part; if(!part) partControls_ = false; page_ = 0; shape_.clear(); refresh(); repaint(); }
    void PlayView::showPartControls(bool part) { partControls_ = part; page_ = 0; shape_.clear(); refresh(); }

    void PlayView::timerCallback()
    {
        if(!isVisible()) return; // Edit's views are showing
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
        // The knobs' controls: the Performance's, or the selected part's own.
        const uint32_t owner = partControls_ && selected_ ? selected_ : 0;
        knobControls_.clear();
        for(const auto& m : graph_.macros) if(m.part == owner) knobControls_.push_back(m);
        const int banks = std::max(1, (int(knobControls_.size()) + knobs() - 1) / knobs());
        page_ %= banks;
        // Rebuilt when what shows changes, so a fader being moved survives the rest.
        std::string shape = std::to_string(getWidth()) + "x" + std::to_string(getHeight()) + "/" + std::to_string(owner) + "/" + std::to_string(page_) + "/" + std::to_string(selected_);
        for(const auto* list : {&state_.parts, &state_.buses}) for(const auto& p : *list) shape += "," + std::to_string(p.id);
        if(shape != shape_) { shape_ = shape; rebuild(); }
        for(auto& s : strips_) for(const auto* list : {&state_.parts, &state_.buses}) for(const auto& p : *list) if(p.id == s->part()) s->set(p);
        for(size_t i = 0; i < knobs_.size(); ++i)
        {
            const size_t at = size_t(page_ * knobs()) + i;
            knobs_[i]->set(at < knobControls_.size() ? &knobControls_[at] : nullptr, at < knobControls_.size() ? targetsOf(knobControls_[at], graph_, state_) : std::string());
        }
        const bool setlist = inSetlist && inSetlist();
        if(setlist != previous_.isVisible()) { previous_.setVisible(setlist); next_.setVisible(setlist); resized(); }
        if(part) partControlsButton_.setButtonText(text(part->name));
        performanceControls_.setToggleState(!owner, juce::dontSendNotification);
        partControlsButton_.setToggleState(owner != 0, juce::dontSendNotification);
        pageButton_.setButtonText("Bank " + juce::String(page_ + 1) + "/" + juce::String(banks));
        pageButton_.setVisible(banks > 1);
        publishRow();
        repaint();
    }

    // The device's knobs bound "row:N" play the bank shown.
    void PlayView::publishRow()
    {
        if(!isShowing() || engine_.rowKnobs() <= 0) return;
        std::vector<Engine::RowParam> row;
        for(int i = 0; i < knobs(); ++i)
        {
            const size_t at = size_t(page_ * knobs() + i);
            if(at >= knobControls_.size()) break;
            row.push_back({{}, 0, knobControls_[at].name, knobControls_[at].id});
        }
        engine_.setRow(std::move(row));
    }

    void PlayView::rebuild()
    {
        knobs_.clear();
        for(int i = 0; i < knobs(); ++i) { knobs_.push_back(std::make_unique<Knob>(*this, i)); addAndMakeVisible(*knobs_.back()); }
        strips_.clear();
        for(const auto* list : {&state_.parts, &state_.buses})
            for(const auto& p : *list) { strips_.push_back(std::make_unique<Strip>(*this, p.id)); stripRow_.addAndMakeVisible(*strips_.back()); }
        const bool any = selected_ != 0;
        performanceControls_.setVisible(any); partControlsButton_.setVisible(any);
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
        knobRow_ = area.removeFromTop(touch ? 84 : 72);
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
        // The strips: at least six across; more scroll sideways.
        viewport_.setBounds(area);
        const int count = std::max(1, int(strips_.size())), minWidth = touch ? 150 : 130;
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
            if(i < graph_.scenes.size()) { const auto id = graph_.scenes[i].id; engine_.edit([id](Control& c) { c.selectScene(id); }); }
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
        bar_->setActions(std::move(list));
    }

    void PerformanceApp::resized() { play_.setBounds(getLocalBounds()); parts_.setBounds(getLocalBounds()); }
}
