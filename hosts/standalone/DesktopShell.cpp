// SPDX-License-Identifier: AGPL-3.0-or-later
#include "DesktopShell.h"

namespace oscairn::standalone
{
    namespace
    {
        juce::String text(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }
        const char* const tabNames[] = {"Part", "Mixer", "Routing", "Controls", "Sequencer"};
        juce::File modeFile(const juce::File& configuration) { return configuration.getChildFile("input-mode.txt"); }
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

    /// The parts and buses down the left: name, what plays, mute and solo, and level. Clicking
    /// one shows it in the Part tab.
    class DesktopShell::PartList : public juce::Component
    {
    public:
        explicit PartList(DesktopShell& shell) : shell_(shell) {}
        void paint(juce::Graphics& g) override
        {
            const auto m = shell_.metrics();
            g.fillAll(ink(Ink::Surface));
            rows_.clear();
            auto area = getLocalBounds().reduced(0, 4);
            auto heading = [&](const char* name) {
                g.setColour(ink(Ink::Muted)); g.setFont(juce::FontOptions(m.font - 3, juce::Font::bold));
                g.drawText(name, area.removeFromTop(m.control).reduced(14, 0), juce::Justification::bottomLeft);
            };
            auto row = [&](const PartRow& p) {
                auto r = area.removeFromTop(m.row);
                rows_.push_back({p.id, r});
                const bool selected = p.id == shell_.selected_ && shell_.tab_ == Tab::Part;
                if(selected) { g.setColour(ink(Ink::Raised)); g.fillRect(r); g.setColour(ink(Ink::Accent)); g.fillRect(r.removeFromLeft(3)); }
                else r.removeFromLeft(3);
                r = r.reduced(11, 6);
                // Mute and solo marks on the right; a level bar along the bottom.
                auto marks = r.removeFromRight(2 * m.control / 2 + 4);
                auto mark = [&](const char* label, bool on, Ink colour) {
                    auto b = marks.removeFromLeft(m.control / 2).withSizeKeepingCentre(m.control / 2, m.control / 2).toFloat().reduced(1);
                    g.setColour(on ? ink(colour) : ink(Ink::Border).withAlpha(.5f)); g.fillRoundedRectangle(b, 3);
                    g.setColour(on ? ink(Ink::Background) : ink(Ink::Muted)); g.setFont(juce::FontOptions(m.font - 4, juce::Font::bold));
                    g.drawText(label, b, juce::Justification::centred);
                    marks.removeFromLeft(4);
                };
                mark("M", p.mute, Ink::Danger); mark("S", p.solo, Ink::Accent);
                auto bar = r.removeFromBottom(3).toFloat();
                g.setColour(ink(Ink::Border).withAlpha(.6f)); g.fillRoundedRectangle(bar, 1.5f);
                g.setColour(p.mute || p.silenced ? ink(Ink::Muted) : ink(Ink::Positive)); g.fillRoundedRectangle(bar.withWidth(bar.getWidth() * float(std::clamp(p.level, 0.0, 1.0))), 1.5f);
                const float alpha = p.mute || p.silenced ? .5f : 1.f;
                g.setColour(ink(Ink::Text).withAlpha(alpha)); g.setFont(juce::FontOptions(m.font, juce::Font::bold));
                g.drawText(text(p.name), r.removeFromTop(r.getHeight() / 2), juce::Justification::bottomLeft, true);
                const auto detail = p.bus ? (p.chain.empty() ? std::string("Empty bus") : p.chain.front().name) : (p.sound.empty() ? std::string("No instrument") : p.sound) + (p.input.empty() ? "" : " · " + p.input);
                g.setColour(ink(Ink::Muted).withAlpha(alpha)); g.setFont(juce::FontOptions(m.font - 2));
                g.drawText(text(detail), r, juce::Justification::topLeft, true);
            };
            heading("PARTS");
            for(const auto& p : shell_.state_.parts) row(p);
            if(shell_.state_.parts.empty()) { g.setColour(ink(Ink::Muted)); g.setFont(juce::FontOptions(m.font - 1)); g.drawText("No parts yet", area.removeFromTop(m.row).reduced(14, 0), juce::Justification::centredLeft); }
            if(!shell_.state_.buses.empty()) { area.removeFromTop(m.gap); heading("BUSES"); for(const auto& p : shell_.state_.buses) row(p); }
        }
        void mouseDown(const juce::MouseEvent& e) override
        {
            for(const auto& r : rows_) if(r.bounds.contains(e.getPosition())) { shell_.select(r.part); return; }
        }
    private:
        DesktopShell& shell_;
        struct Row { uint32_t part; juce::Rectangle<int> bounds; };
        std::vector<Row> rows_;
    };

    DesktopShell::DesktopShell(Engine& engine, const std::string& scanner, InputMode mode)
        : engine_(engine), mode_(mode), list_(std::make_unique<PartList>(*this)), parts_(engine, scanner), sequencer_(engine)
    {
        play_.onClick = [this] { engine_.setPlaying(true); };
        stop_.onClick = [this] { engine_.setPlaying(false); };
        undo_.onClick = [this] { engine_.edit([](Control& c) { c.undo(); }); };
        redo_.onClick = [this] { engine_.edit([](Control& c) { c.redo(); }); };
        tempo_.setRange(40, 240, .1);
        tempo_.setTextValueSuffix(" BPM");
        tempo_.setColour(juce::Slider::trackColourId, ink(Ink::Raised)); // a value field, not a level
        tempo_.setDoubleClickReturnValue(true, 120);
        tempo_.setTooltip("Tempo: drag, or double-click to type");
        tempo_.onValueChange = [this] { engine_.setTempo(tempo_.getValue()); };
        sceneBox_.setTextWhenNoChoicesAvailable("No scenes");
        sceneBox_.setTextWhenNothingSelected("Scene");
        sceneBox_.onChange = [this] {
            const auto id = uint32_t(sceneBox_.getSelectedId());
            if(id == 0xFFFF) { const auto n = scenes_.size() + 1; engine_.edit([n](Control& c) { c.addScene("Scene " + std::to_string(n)); }); return; }
            if(id && id != scene_) engine_.edit([id](Control& c) { c.selectScene(id); });
        };
        storeScene_.setTooltip("Save the parts' levels and mutes, and the scene controls, into this scene");
        storeScene_.onClick = [this] { if(const auto id = scene_) engine_.edit([id](Control& c) { c.storeScene(id); }); };
        status_.setColour(juce::Label::textColourId, ink(Ink::Muted));
        status_.setJustificationType(juce::Justification::centredRight);
        for(size_t i = 0; i < tabs_.size(); ++i)
        {
            tabs_[i].setButtonText(tabNames[i]);
            tabs_[i].setClickingTogglesState(false);
            tabs_[i].onClick = [this, i] { showTab(Tab(i)); };
            addAndMakeVisible(tabs_[i]);
        }
        addPart_.onClick = [this] { const auto n = state_.parts.size() + 1; engine_.edit([n](Control& c) { addPart(c, "Part " + std::to_string(n), false); }); };
        addBus_.onClick = [this] { const auto n = state_.buses.size(); engine_.edit([n](Control& c) { addPart(c, std::string("Bus ") + char('A' + n % 26), true); }); };
        for(auto* c : std::initializer_list<juce::Component*>{&play_, &stop_, &undo_, &redo_, &tempo_, &sceneBox_, &storeScene_, &status_, list_.get(), &addPart_, &addBus_})
            addAndMakeVisible(c);
        addChildComponent(parts_); addChildComponent(sequencer_);
        parts_.setEmbedded(true);
        refresh();
        showTab(Tab::Part);
        startTimer(200);
    }

    DesktopShell::~DesktopShell() { stopTimer(); }

    DesktopShell::Metrics DesktopShell::metrics() const
    {
        const float scale = themeData().fontScale;
        return touchSized(mode_) ? Metrics{44, 64, 12, 16 * scale} : Metrics{30, 50, 8, 14 * scale};
    }

    void DesktopShell::setInputMode(InputMode mode) { mode_ = mode; resized(); repaint(); }

    bool DesktopShell::tabNamed(const juce::String& name, Tab& tab)
    {
        for(size_t i = 0; i < std::size(tabNames); ++i) if(name.equalsIgnoreCase(tabNames[i])) { tab = Tab(i); return true; }
        return false;
    }

    void DesktopShell::showTab(Tab tab)
    {
        tab_ = tab;
        for(size_t i = 0; i < tabs_.size(); ++i)
        {
            const bool on = Tab(i) == tab;
            tabs_[i].setColour(juce::TextButton::buttonColourId, on ? ink(Ink::Raised) : ink(Ink::Background));
            tabs_[i].setColour(juce::TextButton::textColourOffId, on ? ink(Ink::Text) : ink(Ink::Muted));
        }
        sequencer_.setVisible(tab == Tab::Sequencer);
        parts_.setVisible(tab != Tab::Sequencer);
        switch(tab)
        {
        case Tab::Part:
            if(!selected_ && !state_.parts.empty()) selected_ = state_.parts.front().id;
            if(parts_.page() != selected_) parts_.showPart(selected_);
            break;
        case Tab::Mixer: parts_.showPart(PartsView::mixerPage); break;
        case Tab::Routing: parts_.showRouting(0); break;
        case Tab::Controls: parts_.showPart(PartsView::controlsPage); break;
        case Tab::Sequencer: sequencer_.refresh(); break;
        }
        list_->repaint();
    }

    void DesktopShell::select(uint32_t part) { selected_ = part; parts_.showPart(part); showTab(Tab::Part); }

    void DesktopShell::timerCallback() { refresh(); }

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
        play_.setColour(juce::TextButton::buttonColourId, status.playing ? ink(Ink::Positive) : ink(Ink::Raised));
        play_.setColour(juce::TextButton::textColourOffId, status.playing ? ink(Ink::Background) : ink(Ink::Text));
        if(!tempo_.isMouseButtonDown()) tempo_.setValue(status.tempo, juce::dontSendNotification);
        juce::String inputs;
        for(const auto& m : status.midiInputs) if(m.kind != MidiEndpoint::Kind::BuiltIn) inputs << (inputs.isEmpty() ? "" : ", ") << text(m.name);
        status_.setText((status.audioDevice.empty() ? juce::String("No audio") : text(status.audioDevice)) + "   " + juce::String::fromUTF8("·") + "   " + (inputs.isEmpty() ? juce::String("No MIDI inputs") : inputs), juce::dontSendNotification);
        const auto history = engine_.history();
        undo_.setEnabled(!history.undo.empty()); redo_.setEnabled(!history.redo.empty());
        undo_.setTooltip(history.undo.empty() ? juce::String("Nothing to undo") : "Undo " + text(history.undo));
        redo_.setTooltip(history.redo.empty() ? juce::String("Nothing to redo") : "Redo " + text(history.redo));
        if(tab_ == Tab::Sequencer) sequencer_.refresh();
        list_->repaint();
    }

    void DesktopShell::paint(juce::Graphics& g)
    {
        g.fillAll(ink(Ink::Background));
        const auto m = metrics();
        // The toolbar and the tab strip, each with a line under it.
        g.setColour(ink(Ink::Surface)); g.fillRect(getLocalBounds().removeFromTop(m.control + 2 * m.gap));
        g.setColour(ink(Ink::Border).withAlpha(.6f));
        g.fillRect(0, m.control + 2 * m.gap, getWidth(), 1);
        g.fillRect(list_->getRight(), list_->getY(), 1, getHeight() - list_->getY());
        g.fillRect(list_->getRight() + 1, tabs_[0].getBottom() + m.gap / 2, getWidth(), 1);
        g.setColour(ink(Ink::Muted)); g.setFont(juce::FontOptions(m.font - 2));
        g.drawText("Tempo", tempo_.getBounds().withWidth(60).translated(-64, 0), juce::Justification::centredRight);
    }

    void DesktopShell::resized()
    {
        const auto m = metrics();
        auto area = getLocalBounds();
        auto bar = area.removeFromTop(m.control + 2 * m.gap).reduced(m.gap * 2, m.gap);
        play_.setBounds(bar.removeFromLeft(m.control * 2 + 16)); bar.removeFromLeft(4);
        stop_.setBounds(bar.removeFromLeft(m.control * 2 + 16)); bar.removeFromLeft(80);
        tempo_.setBounds(bar.removeFromLeft(140)); bar.removeFromLeft(24);
        sceneBox_.setBounds(bar.removeFromLeft(170)); bar.removeFromLeft(4);
        storeScene_.setBounds(bar.removeFromLeft(m.control * 2)); bar.removeFromLeft(16);
        redo_.setBounds(bar.removeFromRight(m.control * 2 + 8)); bar.removeFromRight(4);
        undo_.setBounds(bar.removeFromRight(m.control * 2 + 8)); bar.removeFromRight(16);
        status_.setFont(juce::FontOptions(m.font - 1));
        status_.setBounds(bar);
        area.removeFromTop(1);
        // The parts and buses, with their add buttons under them.
        auto side = area.removeFromLeft(touchSized(mode_) ? 300 : 260);
        auto adds = side.removeFromBottom(m.control + 2 * m.gap).reduced(m.gap);
        addPart_.setBounds(adds.removeFromLeft(adds.getWidth() / 2).reduced(2, 0)); addBus_.setBounds(adds.reduced(2, 0));
        list_->setBounds(side);
        area.removeFromLeft(1);
        auto strip = area.removeFromTop(m.control + m.gap).reduced(m.gap, 0).withTrimmedTop(m.gap);
        for(auto& t : tabs_) { t.setBounds(strip.removeFromLeft(juce::jmax(90, t.getBestWidthForHeight(m.control) + 24))); strip.removeFromLeft(4); }
        area.removeFromTop(m.gap / 2 + 1);
        parts_.setBounds(area); sequencer_.setBounds(area.reduced(m.gap * 2));
        repaint();
    }
}
