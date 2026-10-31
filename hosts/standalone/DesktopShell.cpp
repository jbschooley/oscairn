// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DesktopShell.h"

namespace oscairn::standalone
{
    namespace
    {
        juce::String text(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }
        const char* const tabNames[] = {"Part", "Mixer", "Routing", "Controls"};
        juce::File modeFile(const juce::File& configuration) { return configuration.getChildFile("input-mode.txt"); }
        int modeNumber(InputMode mode) { return touchSized(mode) ? 2 : 1; }
    }

    InputMode readInputMode(const juce::File& configuration)
    {
        const auto name = modeFile(configuration).loadFileAsString().trim();
        return name == "touch" ? InputMode::Touch : name == "mouse" ? InputMode::Mouse : InputMode::Automatic;
    }
    void writeInputMode(const juce::File& configuration, InputMode mode)
    {
        configuration.createDirectory();
        modeFile(configuration).replaceWithText(mode == InputMode::Touch ? "touch\n" : mode == InputMode::Mouse ? "mouse\n" : "automatic\n");
    }
    // shortcut: Automatic means keyboard and mouse; detect a touch-only computer when one is tested.
    bool touchSized(InputMode mode) { return mode == InputMode::Touch; }
    juce::String inputModeName(InputMode mode) { return mode == InputMode::Touch ? "Touch" : mode == InputMode::Mouse ? "Keyboard and mouse" : "Automatic"; }

    SequencerPane::SequencerPane(Engine& engine) : engine_(engine), screen_(engine)
    {
        play_.onClick = [this] { engine_.setPlaying(true); };
        stop_.onClick = [this] { engine_.setPlaying(false); };
        sync_.setTooltip("Start, stop and follow tempo from an external MIDI clock");
        sync_.onClick = [this] { engine_.setFollowMidiClock(sync_.getToggleState()); };
        for(auto* c : std::initializer_list<juce::Component*>{&play_, &stop_, &sync_, &screen_}) addAndMakeVisible(c);
        refresh();
        startTimer(200);
    }
    void SequencerPane::refresh()
    {
        const bool playing = engine_.status().playing;
        play_.setColour(juce::TextButton::buttonColourId, playing ? ink(Ink::Positive) : ink(Ink::Raised));
        play_.setColour(juce::TextButton::textColourOffId, playing ? ink(Ink::Background) : ink(Ink::Text));
        sync_.setToggleState(engine_.followsMidiClock(), juce::dontSendNotification);
        screen_.refresh();
    }
    void SequencerPane::resized()
    {
        const int control = desktopMode(*this) == 2 ? 44 : 30;
        auto area = getLocalBounds().reduced(16, 12);
        auto bar = area.removeFromTop(control);
        play_.setBounds(bar.removeFromLeft(88)); bar.removeFromLeft(6);
        stop_.setBounds(bar.removeFromLeft(88)); bar.removeFromLeft(16);
        sync_.setBounds(bar.removeFromLeft(180));
        area.removeFromTop(12);
        screen_.setBounds(area);
    }

    /// The parts and buses down the left: name, what plays and from where, mute and solo, a
    /// meter per channel of its sound and where its fader sits. Clicking one shows it.
    class DesktopShell::PartList : public juce::Component
    {
    public:
        explicit PartList(DesktopShell& shell) : shell_(shell) {}
        void paint(juce::Graphics& g) override
        {
            const auto m = shell_.metrics();
            const int mode = modeNumber(shell_.mode_);
            const float body = textSize(TextRole::Body, mode), small = textSize(TextRole::Small, mode);
            const int pad = m.gap + 2;
            g.fillAll(ink(Ink::Surface));
            rows_.clear();
            auto area = getLocalBounds();
            auto heading = [&](const char* name) {
                area.removeFromTop(m.gap * 2);
                g.setColour(ink(Ink::Muted)); g.setFont(juce::FontOptions(small, juce::Font::bold));
                g.drawText(name, area.removeFromTop(int(small) + 2).reduced(pad + 4, 0), juce::Justification::centredLeft);
                area.removeFromTop(m.gap);
            };
            auto row = [&](const PartRow& p) {
                const auto meter = shell_.meters_.count(p.id) ? shell_.meters_.at(p.id) : std::vector<double>{};
                const int bars = std::max<int>(1, int(meter.size()));
                // Equal padding above the name and below the meters.
                const int height = pad + int(body) + 4 + int(small) + 6 + bars * 3 + (bars - 1) * 2 + pad;
                auto r = area.removeFromTop(height);
                rows_.push_back({p.id, r, {}, {}, p.mute, p.solo, p.level});
                const bool selected = p.id == shell_.selected_ && shell_.tab_ == Tab::Part;
                if(selected) { g.setColour(ink(Ink::Raised)); g.fillRect(r); g.setColour(ink(Ink::Accent)); g.fillRect(r.withWidth(3)); }
                r = r.reduced(pad + 4, pad);
                const float alpha = p.mute || p.silenced ? .5f : 1.f;
                auto top = r.removeFromTop(int(body) + 4);
                // Mute and solo at the right of the name: click to switch them.
                auto mark = [&](const char* label, bool on, Ink colour) {
                    auto b = top.removeFromRight(int(body) + 6);
                    top.removeFromRight(4);
                    drawMark(g, b.toFloat(), label, on, colour);
                    return b;
                };
                rows_.back().solo = mark("S", p.solo, Ink::Accent); rows_.back().mute = mark("M", p.mute, Ink::Danger);
                g.setColour(ink(Ink::Text).withAlpha(alpha)); g.setFont(juce::FontOptions(body, juce::Font::bold));
                g.drawText(text(p.name), top, juce::Justification::centredLeft, true);
                const auto detail = p.bus ? (p.chain.empty() ? std::string("Empty bus") : p.chain.front().plugin) : (p.sound.empty() ? std::string("No instrument") : p.sound) + (p.input.empty() ? "" : " · " + p.input);
                g.setColour(ink(Ink::Muted).withAlpha(alpha)); g.setFont(juce::FontOptions(small));
                g.drawText(text(detail), r.removeFromTop(int(small) + 2), juce::Justification::centredLeft, true);
                r.removeFromTop(4);
                // A meter per channel, and a tick where the fader is (meters run -60 to +6 dB).
                auto meters = r.toFloat();
                for(int c = 0; c < bars; ++c) { drawMeter(g, meters.removeFromTop(3), c < int(meter.size()) ? meter[size_t(c)] * alpha : 0.0, false); meters.removeFromTop(2); }
                const double db = p.level > 0 ? 20 * std::log10(p.level) : -100;
                const float x = r.getX() + r.getWidth() * float(std::clamp((db + 60) / 66, 0.0, 1.0));
                g.setColour(ink(Ink::Text).withAlpha(.8f * alpha)); g.fillRect(juce::Rectangle<float>(x - 1, float(r.getY()) - 2, 2, float(r.getHeight()) + 4));
            };
            heading("PARTS");
            for(const auto& p : shell_.state_.parts) row(p);
            if(shell_.state_.parts.empty()) { g.setColour(ink(Ink::Muted)); g.setFont(juce::FontOptions(small)); g.drawText("No parts yet", area.removeFromTop(m.row).reduced(pad + 4, 0), juce::Justification::centredLeft); }
            if(!shell_.state_.buses.empty()) { heading("BUSES"); for(const auto& p : shell_.state_.buses) row(p); }
        }
        void mouseDown(const juce::MouseEvent& e) override
        {
            for(const auto& r : rows_)
            {
                const auto id = r.part;
                if(r.mute.contains(e.getPosition())) { const bool m = !r.muted; const double level = r.level; shell_.engine_.edit([id, level, m](Control& c) { c.setPartLevel(id, level, m); c.commit(); }); return; }
                if(r.solo.contains(e.getPosition())) { const bool s = !r.soloed; shell_.engine_.edit([id, s](Control& c) { c.setPartSolo(id, s); c.commit(); }); return; }
                if(r.bounds.contains(e.getPosition())) { shell_.select(id); return; }
            }
        }
    private:
        DesktopShell& shell_;
        struct Row { uint32_t part; juce::Rectangle<int> bounds, mute, solo; bool muted, soloed; double level; };
        std::vector<Row> rows_;
    };

    /// Tabs over the view: the selected one joins the view below it, and carries a button that
    /// opens it in a window of its own.
    class DesktopShell::TabStrip : public juce::Component
    {
    public:
        explicit TabStrip(DesktopShell& shell) : shell_(shell)
        {
            detach_.setTooltip("Open this view in its own window");
            detach_.setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
            detach_.onClick = [this] { shell_.detach(shell_.tab_); };
            addAndMakeVisible(detach_);
        }
        void paint(juce::Graphics& g) override
        {
            const int mode = modeNumber(shell_.mode_);
            const float radius = themeData().radius;
            const bool segmented = themeData().style.tabs == ThemeStyle::Tabs::Segmented;
            if(segmented)
            {
                // A track holding the tabs, the selected one lifted out of it.
                auto track = tabs_.front().getUnion(tabs_.back()).toFloat().expanded(3);
                g.setColour(ink(Ink::Raised)); g.fillRoundedRectangle(track, radius + 4);
            }
            else { g.setColour(ink(Ink::Border).withAlpha(.6f)); g.fillRect(0, getHeight() - 1, getWidth(), 1); }
            for(size_t i = 0; i < tabs_.size(); ++i)
            {
                const auto r = tabs_[i].toFloat();
                const bool on = Tab(i) == shell_.tab_;
                if(on && segmented)
                {
                    if(themeData().style.depth == ThemeStyle::Depth::Soft) { g.setColour(juce::Colours::black.withAlpha(darkTheme() ? .35f : .12f)); g.fillRoundedRectangle(r.translated(0, 1), radius + 1); }
                    g.setColour(darkTheme() ? ink(Ink::Raised).brighter(.25f) : ink(Ink::Surface)); g.fillRoundedRectangle(r, radius + 1);
                }
                else if(on)
                {
                    // Rounded at the top, open at the bottom where it meets the view.
                    juce::Path p; p.addRoundedRectangle(r.getX(), r.getY(), r.getWidth(), r.getHeight() + radius, radius, radius, true, true, false, false);
                    g.setColour(ink(Ink::Background)); g.fillPath(p);
                    g.setColour(ink(Ink::Border).withAlpha(.6f)); g.strokePath(p, juce::PathStrokeType(1));
                    g.setColour(ink(Ink::Background)); g.fillRect(r.getX() + 1, r.getBottom() - 1, r.getWidth() - 2, radius + 1);
                    g.setColour(ink(Ink::Accent)); g.fillRect(r.getX() + radius, r.getY(), r.getWidth() - 2 * radius, 2.f);
                }
                g.setColour(on ? ink(Ink::Text) : ink(Ink::Muted));
                g.setFont(juce::FontOptions(textSize(TextRole::Button, mode), on ? juce::Font::bold : juce::Font::plain));
                g.drawText(tabNames[i], r.withTrimmedRight(on ? float(detach_.getWidth()) : 0.f), juce::Justification::centred);
            }
        }
        void resized() override
        {
            const int mode = modeNumber(shell_.mode_);
            const juce::Font font(juce::FontOptions(textSize(TextRole::Button, mode), juce::Font::bold));
            const bool segmented = themeData().style.tabs == ThemeStyle::Tabs::Segmented;
            auto area = segmented ? getLocalBounds().withTrimmedLeft(16).reduced(0, 4) : getLocalBounds().withTrimmedLeft(12);
            const int icon = getHeight() - 14;
            for(size_t i = 0; i < tabs_.size(); ++i)
            {
                const int width = juce::GlyphArrangement::getStringWidthInt(font, tabNames[i]) + 40 + (Tab(i) == shell_.tab_ ? icon : 0);
                tabs_[i] = area.removeFromLeft(width);
                area.removeFromLeft(2);
            }
            const auto on = tabs_[size_t(shell_.tab_)];
            const int glyph = std::min(icon, on.getHeight() - 4);
            detach_.setBounds(on.withTrimmedLeft(on.getWidth() - glyph - 10).withSizeKeepingCentre(glyph, glyph).translated(-4, segmented ? 0 : 1));
            repaint();
        }
        void mouseDown(const juce::MouseEvent& e) override
        {
            for(size_t i = 0; i < tabs_.size(); ++i)
                if(tabs_[i].contains(e.getPosition()))
                {
                    if(e.mods.isPopupMenu())
                    {
                        juce::PopupMenu menu; menu.addItem(1, "Open in its own window");
                        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this), [this, i](int r) { if(r == 1) shell_.detach(Tab(i)); });
                    }
                    else shell_.showTab(Tab(i));
                    return;
                }
        }
    private:
        DesktopShell& shell_;
        std::array<juce::Rectangle<int>, std::size(tabNames)> tabs_;
        IconButton detach_{IconButton::Glyph::Detach, "Open in its own window"};
    };

    DesktopShell::DesktopShell(Engine& engine, const std::string& scanner, InputMode mode)
        : engine_(engine), scanner_(scanner), mode_(mode), tabs_(std::make_unique<TabStrip>(*this)), list_(std::make_unique<PartList>(*this)), parts_(engine, scanner)
    {
        setDesktopMode(*this, modeNumber(mode));
        undo_.setTooltip("Undo"); redo_.setTooltip("Redo");
        undo_.onClick = [this] { engine_.edit([](Control& c) { c.undo(); }); };
        redo_.onClick = [this] { engine_.edit([](Control& c) { c.redo(); }); };
        tempo_.setRange(40, 240, .1);
        tempo_.setTextValueSuffix(" BPM");
        tempo_.setColour(juce::Slider::trackColourId, ink(Ink::Raised)); // a value field, not a level
        tempo_.setDoubleClickReturnValue(true, 120);
        tempo_.setTooltip("The tempo every plugin follows: drag, or double-click to type");
        tempo_.onValueChange = [this] { engine_.setTempo(tempo_.getValue()); };
        sceneBox_.setTextWhenNoChoicesAvailable("No scenes");
        sceneBox_.setTextWhenNothingSelected("None");
        sceneBox_.onChange = [this] {
            const auto id = uint32_t(sceneBox_.getSelectedId());
            if(id == 0xFFFF) { const auto n = scenes_.size() + 1; engine_.edit([n](Control& c) { c.addScene("Scene " + std::to_string(n)); }); return; }
            if(id && id != scene_) engine_.edit([id](Control& c) { c.selectScene(id); });
        };
        storeScene_.setTooltip("Save the parts' levels and mutes, and the scene controls, into this scene");
        storeScene_.onClick = [this] { if(const auto id = scene_) engine_.edit([id](Control& c) { c.storeScene(id); }); };
        for(auto* l : {&tempoLabel_, &sceneLabel_, &status_}) l->setColour(juce::Label::textColourId, ink(Ink::Muted));
        status_.setJustificationType(juce::Justification::centredRight);
        addPart_.onClick = [this] { const auto n = state_.parts.size() + 1; engine_.edit([n](Control& c) { addPart(c, "Part " + std::to_string(n), false); }); };
        addBus_.onClick = [this] { const auto n = state_.buses.size(); engine_.edit([n](Control& c) { addPart(c, std::string("Bus ") + char('A' + n % 26), true); }); };
        for(auto* c : std::initializer_list<juce::Component*>{&undo_, &redo_, &tempoLabel_, &tempo_, &sceneLabel_, &sceneBox_, &storeScene_, &status_, tabs_.get(), list_.get(), &addPart_, &addBus_})
            addAndMakeVisible(c);
        addAndMakeVisible(parts_);
        parts_.setEmbedded(true);
        refresh();
        showTab(Tab::Part);
        startTimer(50);
    }

    DesktopShell::~DesktopShell() { stopTimer(); windows_.clear(); }

    DesktopShell::Metrics DesktopShell::metrics() const { return touchSized(mode_) ? Metrics{44, 64, 10} : Metrics{30, 50, 8}; }

    void DesktopShell::setInputMode(InputMode mode)
    {
        mode_ = mode;
        setDesktopMode(*this, modeNumber(mode));
        for(auto& w : windows_) if(auto* c = w->getContentComponent()) { setDesktopMode(*c, modeNumber(mode)); c->resized(); }
        resized(); parts_.refresh(); repaint();
    }

    bool DesktopShell::tabNamed(const juce::String& name, Tab& tab)
    {
        for(size_t i = 0; i < std::size(tabNames); ++i) if(name.equalsIgnoreCase(tabNames[i])) { tab = Tab(i); return true; }
        return false;
    }

    void DesktopShell::showTab(Tab tab)
    {
        tab_ = tab;
        switch(tab)
        {
        case Tab::Part:
            if(!selected_ && !state_.parts.empty()) selected_ = state_.parts.front().id;
            if(parts_.page() != selected_) parts_.showPart(selected_);
            break;
        case Tab::Mixer: parts_.showPart(PartsView::mixerPage); break;
        case Tab::Routing: parts_.showRouting(0); break;
        case Tab::Controls: parts_.showPart(PartsView::controlsPage); break;
        }
        tabs_->resized();
        list_->repaint();
    }

    std::unique_ptr<juce::Component> DesktopShell::viewFor(Tab tab)
    {
        auto view = std::make_unique<PartsView>(engine_, scanner_);
        view->setEmbedded(true);
        if(tab == Tab::Part) view->showPart(selected_);
        else if(tab == Tab::Mixer) view->showPart(PartsView::mixerPage);
        else if(tab == Tab::Routing) view->showRouting(0);
        else view->showPart(PartsView::controlsPage);
        return view;
    }

    void DesktopShell::detach(Tab tab)
    {
        // A plain window that removes itself from the list when closed.
        struct Window : juce::DocumentWindow
        {
            Window(const juce::String& name, DesktopShell& shell) : DocumentWindow(name, ink(Ink::Background), DocumentWindow::allButtons), shell_(shell) {}
            void closeButtonPressed() override
            {
                juce::MessageManager::callAsync([safe = juce::Component::SafePointer<DesktopShell>(&shell_), self = this] {
                    if(safe) std::erase_if(safe->windows_, [self](const auto& w) { return w.get() == self; });
                });
            }
            DesktopShell& shell_;
        };
        juce::String name = juce::String("Oscairn - ") + tabNames[size_t(tab)];
        if(tab == Tab::Part) for(const auto* list : {&state_.parts, &state_.buses}) for(const auto& p : *list) if(p.id == selected_) name << ": " << text(p.name);
        auto window = std::make_unique<Window>(name, *this);
        auto view = viewFor(tab);
        setDesktopMode(*view, modeNumber(mode_));
        window->setUsingNativeTitleBar(true);
        window->setContentOwned(view.release(), false);
        window->setResizable(true, false);
        window->centreWithSize(tab == Tab::Mixer ? 900 : 1100, tab == Tab::Mixer ? 520 : 700);
        window->setVisible(true);
        windows_.push_back(std::move(window));
    }

    void DesktopShell::select(uint32_t part) { selected_ = part; parts_.showPart(part); showTab(Tab::Part); }

    void DesktopShell::timerCallback()
    {
        // Meters every tick; the rest five times a second.
        meters_.clear();
        for(const auto& m : engine_.meters()) meters_[m.part] = m.channels;
        if(++ticks_ % 4 == 0) refresh();
        list_->repaint();
    }

    void DesktopShell::refresh()
    {
        Control::GraphState graph;
        try { graph = engine_.graph(); } catch(const std::exception&) { return; }
        state_ = readParts(graph);
        if(selected_ && !std::any_of(state_.parts.begin(), state_.parts.end(), [&](const PartRow& p) { return p.id == selected_; }) &&
           !std::any_of(state_.buses.begin(), state_.buses.end(), [&](const PartRow& p) { return p.id == selected_; }))
        { selected_ = 0; if(tab_ == Tab::Part) showTab(Tab::Part); }
        // Scenes: rebuilt only when they change, so an open menu survives.
        if(graph.scene != scene_ || graph.scenes.size() != scenes_.size() || !std::equal(graph.scenes.begin(), graph.scenes.end(), scenes_.begin(), [](const Scene& a, const Scene& b) { return a.id == b.id && a.name == b.name; }))
        {
            scenes_ = graph.scenes; scene_ = graph.scene;
            sceneBox_.clear(juce::dontSendNotification);
            for(const auto& s : scenes_) sceneBox_.addItem(text(s.name), int(s.id));
            sceneBox_.addSeparator(); sceneBox_.addItem("+ New scene", 0xFFFF);
            sceneBox_.setSelectedId(int(scene_), juce::dontSendNotification);
            storeScene_.setEnabled(scene_ != 0);
        }
        const auto status = engine_.status();
        if(!tempo_.isMouseButtonDown()) tempo_.setValue(status.tempo, juce::dontSendNotification);
        juce::String inputs;
        for(const auto& m : status.midiInputs) if(m.kind != MidiEndpoint::Kind::BuiltIn) inputs << (inputs.isEmpty() ? "" : ", ") << text(m.name);
        status_.setText((status.audioDevice.empty() ? juce::String("No audio") : text(status.audioDevice)) + juce::String::fromUTF8("  ·  ") + (inputs.isEmpty() ? juce::String("No MIDI inputs") : inputs), juce::dontSendNotification);
        const auto history = engine_.history();
        undo_.setEnabled(!history.undo.empty()); redo_.setEnabled(!history.redo.empty());
        undo_.setTooltip(history.undo.empty() ? juce::String("Nothing to undo") : "Undo " + text(history.undo));
        redo_.setTooltip(history.redo.empty() ? juce::String("Nothing to redo") : "Redo " + text(history.redo));
    }

    void DesktopShell::paint(juce::Graphics& g)
    {
        g.fillAll(ink(Ink::Background));
        const auto m = metrics();
        // The toolbar, with a line under it, and the line between the list and the view.
        const int bar = m.control + 2 * m.gap;
        g.setColour(ink(Ink::Surface)); g.fillRect(0, 0, getWidth(), bar);
        g.setColour(ink(Ink::Border).withAlpha(.6f));
        g.fillRect(0, bar, getWidth(), 1);
        g.fillRect(list_->getRight(), bar, 1, getHeight() - bar);
    }

    void DesktopShell::resized()
    {
        const auto m = metrics();
        const int mode = modeNumber(mode_);
        for(auto* l : {&tempoLabel_, &sceneLabel_, &status_}) l->setFont(juce::FontOptions(textSize(TextRole::Small, mode)));
        auto area = getLocalBounds();
        auto bar = area.removeFromTop(m.control + 2 * m.gap).reduced(m.gap * 2, m.gap);
        auto labelled = [&](juce::Label& label, juce::Component& c, int width) {
            label.setBounds(bar.removeFromLeft(juce::GlyphArrangement::getStringWidthInt(label.getFont(), label.getText()) + 10));
            c.setBounds(bar.removeFromLeft(width));
        };
        labelled(tempoLabel_, tempo_, touchSized(mode_) ? 150 : 120); bar.removeFromLeft(m.gap * 3);
        labelled(sceneLabel_, sceneBox_, 160); bar.removeFromLeft(6);
        storeScene_.setBounds(bar.removeFromLeft(m.control * 2 + 8));
        redo_.setBounds(bar.removeFromRight(m.control)); bar.removeFromRight(4);
        undo_.setBounds(bar.removeFromRight(m.control)); bar.removeFromRight(m.gap * 2);
        status_.setBounds(bar);
        area.removeFromTop(1);
        // The parts and buses, with their add buttons under them.
        auto side = area.removeFromLeft(touchSized(mode_) ? 300 : 260);
        auto adds = side.removeFromBottom(m.control + 2 * m.gap).reduced(m.gap);
        addPart_.setBounds(adds.removeFromLeft(adds.getWidth() / 2).reduced(2, 0)); addBus_.setBounds(adds.reduced(2, 0));
        list_->setBounds(side);
        area.removeFromLeft(1);
        tabs_->setBounds(area.removeFromTop(m.control + m.gap * 2).withTrimmedTop(m.gap));
        parts_.setBounds(area);
        repaint();
    }
}
