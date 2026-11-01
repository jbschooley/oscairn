// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Setlist.h"
#include "PerformanceColors.h"
#include "oscairn/library.h"

namespace oscairn::standalone
{
    namespace
    {
        juce::String text(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }
        constexpr int maxEntries = 1000, maxSide = 8, colorMenuBase = 100;
        const char* const modeNames[] = {"list", "scroll", "grid"};
        std::string str(const juce::var& v, const char* what)
        {
            if(!v.isString()) throw std::runtime_error(std::string("a setlist's ") + what + " must be text");
            return v.toString().toStdString();
        }
    }

    juce::File setlistFolder() { return juce::File(juce::String::fromUTF8(dataHome().string().c_str())).getChildFile("Setlists"); }

    std::vector<juce::File> setlistFiles(const juce::File& folder)
    {
        auto files = folder.findChildFiles(juce::File::findFiles, false, "*.json");
        std::vector<juce::File> out(files.begin(), files.end());
        std::sort(out.begin(), out.end(), [](const juce::File& a, const juce::File& b) { return a.getLastModificationTime() > b.getLastModificationTime(); });
        return out;
    }

    Setlist readSetlist(const juce::File& file)
    {
        if(file.getSize() > 1024 * 1024) throw std::runtime_error("the setlist file is too large");
        juce::var doc;
        if(juce::JSON::parse(file.loadFileAsString(), doc).failed() || !doc.isObject()) throw std::runtime_error("not a setlist file");
        if(doc["format"] != juce::var("oscairn.setlist") || !doc["version"].isInt() || int(doc["version"]) != 1) throw std::runtime_error("not a version 1 setlist");
        Setlist s;
        s.name = str(doc["name"], "name");
        const auto display = doc["display"];
        const auto mode = str(display["mode"], "display mode");
        const auto at = std::find(std::begin(modeNames), std::end(modeNames), mode);
        if(at == std::end(modeNames)) throw std::runtime_error("unknown setlist display " + mode);
        s.display.mode = SetlistDisplay::Mode(at - std::begin(modeNames));
        for(auto [field, value] : {std::pair{"columns", &s.display.columns}, std::pair{"rows", &s.display.rows}})
        {
            if(!display[field].isInt() || int(display[field]) < 1 || int(display[field]) > maxSide) throw std::runtime_error(std::string("a setlist grid's ") + field + " must be 1 to 8");
            *value = int(display[field]);
        }
        const auto* entries = doc["entries"].getArray();
        if(!entries || entries->size() > maxEntries) throw std::runtime_error("a setlist's entries must be a list of at most 1000");
        for(const auto& e : *entries)
        {
            SetlistEntry entry{str(e["id"], "entry ID"), str(e["name"], "entry name"), str(e["color"], "entry color")};
            if(entry.id.empty()) throw std::runtime_error("a setlist entry without an ID");
            if(!validColor(entry.color)) throw std::runtime_error("a setlist entry's color must be #rrggbb or empty");
            s.entries.push_back(std::move(entry));
        }
        return s;
    }

    juce::File setlistFile(const juce::File& folder, const std::string& name)
    {
        auto safe = juce::File::createLegalFileName(text(name)).trim();
        return folder.getChildFile((safe.isEmpty() ? juce::String("Setlist") : safe) + ".json");
    }

    void writeSetlist(const juce::File& folder, const Setlist& s)
    {
        juce::Array<juce::var> entries;
        for(const auto& e : s.entries)
        {
            auto* entry = new juce::DynamicObject();
            entry->setProperty("id", text(e.id)); entry->setProperty("name", text(e.name)); entry->setProperty("color", text(e.color));
            entries.add(juce::var(entry));
        }
        auto* doc = new juce::DynamicObject();
        doc->setProperty("format", "oscairn.setlist"); doc->setProperty("version", 1);
        auto* display = new juce::DynamicObject();
        display->setProperty("mode", modeNames[int(s.display.mode)]);
        display->setProperty("columns", s.display.columns); display->setProperty("rows", s.display.rows);
        doc->setProperty("name", text(s.name)); doc->setProperty("display", juce::var(display)); doc->setProperty("entries", entries);
        if(!folder.createDirectory()) throw std::runtime_error("cannot make the setlists folder");
        const auto file = setlistFile(folder, s.name);
        juce::TemporaryFile temporary(file);
        if(!temporary.getFile().replaceWithText(juce::JSON::toString(juce::var(doc)) + "\n") || !temporary.overwriteTargetFileWithTemporary())
            throw std::runtime_error("cannot save the setlist");
    }

    /// The grid: a page of columns × rows, the page holding the entry playing. A tap opens one;
    /// a right click gives its color.
    class SetlistPane::Grid : public juce::Component
    {
    public:
        explicit Grid(SetlistPane& pane) : pane_(pane) {}
        int perPage() const { const auto& d = pane_.setlist_.display; return d.columns * d.rows; }
        int page() const { return pane_.current_ < 0 ? 0 : pane_.current_ / perPage(); }
        juce::Rectangle<int> cell(int slot) const
        {
            const auto& d = pane_.setlist_.display;
            const int gap = pane_.mode() == 2 ? 10 : 8;
            const int w = (getWidth() - gap * (d.columns - 1)) / d.columns, h = (getHeight() - gap * (d.rows - 1)) / d.rows;
            return {(slot % d.columns) * (w + gap), (slot / d.columns) * (h + gap), w, h};
        }
        int at(juce::Point<int> p) const
        {
            for(int slot = 0; slot < perPage(); ++slot)
                if(cell(slot).contains(p)) { const int index = page() * perPage() + slot; return index < int(pane_.setlist_.entries.size()) ? index : -1; }
            return -1;
        }
        void paint(juce::Graphics& g) override
        {
            const int first = page() * perPage(), m = pane_.mode();
            for(int slot = 0; slot < perPage(); ++slot)
            {
                const int index = first + slot;
                const auto r = cell(slot).toFloat();
                const float radius = std::min(themeData().radius, 10.f);
                if(index >= int(pane_.setlist_.entries.size())) { g.setColour(ink(Ink::Surface).withAlpha(.5f)); g.fillRoundedRectangle(r, radius); continue; }
                const bool playing = index == pane_.current_;
                const auto color = pane_.colorAt(index);
                g.setColour(color.isTransparent() ? ink(Ink::Raised) : color.interpolatedWith(ink(Ink::Surface), playing ? .15f : .55f));
                g.fillRoundedRectangle(r, radius);
                if(playing) { g.setColour(ink(Ink::Accent)); g.drawRoundedRectangle(r.reduced(1.5f), radius, 3); }
                auto area = r.toNearestInt().reduced(10, 8);
                g.setColour(ink(Ink::Text).withAlpha(.7f)); g.setFont(juce::FontOptions(textSize(TextRole::Small, m), juce::Font::bold));
                g.drawText(juce::String(index + 1), area.removeFromTop(int(textSize(TextRole::Small, m)) + 4), juce::Justification::topLeft);
                const auto& entry = pane_.setlist_.entries[size_t(index)];
                g.setColour(pane_.shown_[size_t(index)].missing ? ink(Ink::Muted) : ink(Ink::Text));
                g.setFont(juce::FontOptions(textSize(TextRole::Title, m), juce::Font::bold));
                g.drawFittedText(text(entry.name.empty() ? "Untitled" : entry.name), area, juce::Justification::centred, 3);
            }
        }
        void mouseUp(const juce::MouseEvent& e) override
        {
            const int index = at(e.getPosition());
            if(index < 0) return;
            if(e.mods.isPopupMenu()) pane_.colorMenu(index); else pane_.go(index);
        }
    private:
        SetlistPane& pane_;
    };

    SetlistPane::~SetlistPane() = default;

    SetlistPane::SetlistPane(Engine& engine, Actions& actions, juce::File folder) : engine_(engine), actions_(actions), folder_(std::move(folder))
    {
        chooser_.setTextWhenNothingSelected("No setlist");
        chooser_.onChange = [this] { const int i = chooser_.getSelectedItemIndex(); if(i >= 0 && size_t(i) < files_.size()) open(files_[size_t(i)]); };
        new_.onClick = [this] {
            auto* w = new juce::AlertWindow("New setlist", "Its name:", juce::MessageBoxIconType::NoIcon, this);
            w->addTextEditor("name", "", "Name");
            w->addButton("Make", 1, juce::KeyPress(juce::KeyPress::returnKey)); w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
            w->enterModalState(true, juce::ModalCallbackFunction::create([safe = juce::Component::SafePointer<SetlistPane>(this), w](int result) {
                if(safe && result == 1 && w->getTextEditorContents("name").trim().isNotEmpty()) safe->create(w->getTextEditorContents("name").trim().toStdString());
            }), true);
        };
        previous_.onClick = [this] { step(-1); };
        next_.onClick = [this] { step(1); };
        add_.onClick = [this] { addFromLibrary(); };
        remove_.onClick = [this] { remove(selected()); };
        up_.onClick = [this] { move(selected(), -1); };
        down_.onClick = [this] { move(selected(), 1); };
        view_.addItemList({"List", "Scrolling list", "Grid"}, 1);
        view_.onChange = [this] { auto d = setlist_.display; d.mode = SetlistDisplay::Mode(view_.getSelectedItemIndex()); setDisplay(d); };
        for(int i = 1; i <= maxSide; ++i) { columns_.addItem(juce::String(i) + (i == 1 ? " column" : " columns"), i); rows_.addItem(juce::String(i) + (i == 1 ? " row" : " rows"), i); }
        columns_.onChange = [this] { auto d = setlist_.display; d.columns = columns_.getSelectedId(); setDisplay(d); };
        rows_.onChange = [this] { auto d = setlist_.display; d.rows = rows_.getSelectedId(); setDisplay(d); };
        grid_ = std::make_unique<Grid>(*this);
        addChildComponent(*grid_);
        list_.setColour(juce::ListBox::backgroundColourId, ink(Ink::Surface));
        list_.setOutlineThickness(0);
        status_.setColour(juce::Label::textColourId, ink(Ink::Muted));
        for(auto* c : std::initializer_list<juce::Component*>{&chooser_, &view_, &columns_, &rows_, &new_, &previous_, &next_, &add_, &remove_, &up_, &down_, &list_, &status_}) addAndMakeVisible(c);
        setWantsKeyboardFocus(true);
        refreshChooser();
        if(!files_.empty()) open(files_.front());
        showDisplay();
        updateButtons();
    }

    void SetlistPane::refreshChooser()
    {
        files_ = setlistFiles(folder_);
        chooser_.clear(juce::dontSendNotification);
        for(size_t i = 0; i < files_.size(); ++i) chooser_.addItem(files_[i].getFileNameWithoutExtension(), int(i) + 1);
        for(size_t i = 0; i < files_.size(); ++i) if(files_[i] == setlistFile(folder_, setlist_.name)) chooser_.setSelectedItemIndex(int(i), juce::dontSendNotification);
    }

    void SetlistPane::open(const juce::File& file)
    {
        try { setlist_ = readSetlist(file); }
        catch(const std::exception& e) { say(file.getFileName() + ": " + e.what()); return; }
        current_ = -1;
        // Names and colors as the library has them now; entries it no longer has are marked.
        shown_.assign(setlist_.entries.size(), {});
        Control::LibraryQuery all; all.limit = 1000;
        if(const auto found = engine_.library(all))
            for(size_t i = 0; i < setlist_.entries.size(); ++i)
            {
                const auto at = std::find_if(found->items.begin(), found->items.end(), [&](const auto& item) { return item.id == setlist_.entries[i].id; });
                if(at == found->items.end()) shown_[i].missing = true; else { setlist_.entries[i].name = at->name; shown_[i].color = at->color; }
            }
        list_.updateContent(); list_.deselectAllRows();
        refreshChooser();
        say(juce::String(setlist_.entries.size()) + (setlist_.entries.size() == 1 ? " Performance" : " Performances"));
        showDisplay(); updateButtons(); repaint();
    }

    void SetlistPane::create(const std::string& name)
    {
        const auto file = setlistFile(folder_, name);
        if(file.existsAsFile()) { open(file); return; }
        setlist_ = {name, {}, {}}; shown_.clear(); current_ = -1;
        save();
        list_.updateContent();
        refreshChooser(); showDisplay(); updateButtons(); repaint();
    }

    void SetlistPane::save()
    {
        try { writeSetlist(folder_, setlist_); }
        catch(const std::exception& e) { say(e.what()); }
    }

    juce::String SetlistPane::add(const std::string& id)
    {
        const auto item = engine_.libraryItem(id);
        if(!item) { say("The engine is busy; add it again in a moment."); return {}; }
        if(item->id.empty()) { say("That Performance is not in the library."); return {}; }
        if(setlist_.name.empty()) create("Setlist");
        setlist_.entries.push_back({item->id, item->name, {}});
        shown_.push_back({false, item->color});
        save();
        list_.updateContent(); list_.selectRow(int(setlist_.entries.size()) - 1);
        const auto done = "Added to " + text(setlist_.name);
        say(done); updateButtons();
        return done;
    }

    void SetlistPane::addFromLibrary()
    {
        const bool asked = actions_.request(pickPerformance, {}, [safe = juce::Component::SafePointer<SetlistPane>(this)](const std::string& id) {
            if(safe && !id.empty()) safe->add(id);
        });
        if(!asked) say("No app is installed that picks Performances.");
    }

    void SetlistPane::remove(int index)
    {
        if(index < 0 || index >= int(setlist_.entries.size())) return;
        setlist_.entries.erase(setlist_.entries.begin() + index);
        shown_.erase(shown_.begin() + index);
        if(current_ == index) current_ = -1; else if(current_ > index) --current_;
        save();
        list_.updateContent();
        if(!setlist_.entries.empty()) list_.selectRow(std::min(index, int(setlist_.entries.size()) - 1));
        updateButtons(); repaint();
    }

    void SetlistPane::move(int index, int by)
    {
        const int to = index + by;
        if(index < 0 || to < 0 || index >= int(setlist_.entries.size()) || to >= int(setlist_.entries.size())) return;
        std::swap(setlist_.entries[size_t(index)], setlist_.entries[size_t(to)]);
        std::swap(shown_[size_t(index)], shown_[size_t(to)]);
        if(current_ == index) current_ = to; else if(current_ == to) current_ = index;
        save();
        list_.updateContent(); list_.selectRow(to);
        repaint();
    }

    void SetlistPane::go(int index)
    {
        if(setlist_.entries.empty()) return;
        index = std::clamp(index, 0, int(setlist_.entries.size()) - 1);
        if(engine_.soundBusy()) { say("Still opening the last one."); return; }
        const auto& entry = setlist_.entries[size_t(index)];
        if(shown_[size_t(index)].missing) { say(text(entry.name) + " is no longer in the library."); return; }
        SoundAction open; open.kind = SoundAction::LoadFromLibrary; open.libraryId = entry.id;
        engine_.soundAction(std::move(open));
        current_ = index;
        list_.selectRow(index);
        say(juce::String(index + 1) + ". " + text(entry.name));
        updateButtons(); repaint(); grid_->repaint();
    }

    bool SetlistPane::keyPressed(const juce::KeyPress& key)
    {
        // Page turners and foot switches send arrows or page keys.
        if(key == juce::KeyPress::rightKey || key == juce::KeyPress::pageDownKey || key == juce::KeyPress::spaceKey) { step(1); return true; }
        if(key == juce::KeyPress::leftKey || key == juce::KeyPress::pageUpKey) { step(-1); return true; }
        return false;
    }

    void SetlistPane::say(const juce::String& t) { message_ = t; status_.setText(t, juce::dontSendNotification); }

    void SetlistPane::updateButtons()
    {
        const int row = selected(), n = int(setlist_.entries.size());
        previous_.setEnabled(n > 0 && current_ > 0); next_.setEnabled(n > 0 && current_ < n - 1);
        remove_.setEnabled(row >= 0); up_.setEnabled(row > 0); down_.setEnabled(row >= 0 && row < n - 1);
        add_.setEnabled(actions_.offered(pickPerformance));
    }

    void SetlistPane::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected)
    {
        if(row < 0 || row >= int(setlist_.entries.size())) return;
        const auto& entry = setlist_.entries[size_t(row)];
        const bool playing = row == current_;
        drawListRow(g, {0, 0, width, height}, row, colorAt(row));
        if(playing || selected) { g.setColour(playing ? ink(Ink::Accent).withAlpha(.22f) : ink(Ink::Raised).withAlpha(.6f)); g.fillRect(0, 0, width, height); }
        if(playing) { g.setColour(ink(Ink::Accent)); g.fillRect(0, 0, 4, height); }
        const float body = textSize(TextRole::Body, mode()), title = textSize(TextRole::Title, mode());
        auto area = juce::Rectangle<int>(0, 0, width, height).reduced(24, 0);
        g.setColour(playing ? ink(Ink::Accent) : ink(Ink::Muted)); g.setFont(juce::FontOptions(body, juce::Font::bold));
        g.drawText(juce::String(row + 1), area.removeFromLeft(int(body * 2.4f)), juce::Justification::centredLeft);
        g.setColour(shown_[size_t(row)].missing ? ink(Ink::Muted) : ink(Ink::Text)); g.setFont(juce::FontOptions(title, playing ? juce::Font::bold : juce::Font::plain));
        g.drawText(text(entry.name.empty() ? "Untitled" : entry.name) + (shown_[size_t(row)].missing ? "  (not in the library)" : ""), area, juce::Justification::centredLeft, true);
    }

    juce::Colour SetlistPane::colorAt(int index) const
    {
        if(index < 0 || index >= int(setlist_.entries.size())) return juce::Colours::transparentBlack;
        const auto& own = setlist_.entries[size_t(index)].color;
        return colorOf(own.empty() && size_t(index) < shown_.size() ? shown_[size_t(index)].color : own);
    }

    void SetlistPane::setColor(int index, const std::string& color)
    {
        if(index < 0 || index >= int(setlist_.entries.size()) || !validColor(color)) return;
        setlist_.entries[size_t(index)].color = color;
        save(); repaint(); list_.repaint(); grid_->repaint();
    }

    void SetlistPane::colorMenu(int index)
    {
        if(index < 0 || index >= int(setlist_.entries.size())) return;
        juce::PopupMenu menu;
        addColors(menu, setlist_.entries[size_t(index)].color, colorMenuBase, "The Performance's color");
        menu.showMenuAsync(juce::PopupMenu::Options(), [safe = juce::Component::SafePointer<SetlistPane>(this), index](int result) {
            std::string color;
            if(safe && colorChosen(result, colorMenuBase, color)) safe->setColor(index, color);
        });
    }

    void SetlistPane::listBoxItemClicked(int row, const juce::MouseEvent& e)
    {
        if(e.mods.isPopupMenu()) colorMenu(row);
        else if(setlist_.display.mode == SetlistDisplay::Mode::Scroll) go(row); // a list to tap
    }

    int SetlistPane::selected() const { return setlist_.display.mode == SetlistDisplay::Mode::Grid ? current_ : list_.getSelectedRow(); }

    void SetlistPane::setDisplay(SetlistDisplay display)
    {
        display.columns = std::clamp(display.columns, 1, maxSide); display.rows = std::clamp(display.rows, 1, maxSide);
        setlist_.display = display;
        if(!setlist_.name.empty()) save();
        showDisplay();
    }

    void SetlistPane::showDisplay()
    {
        const auto& d = setlist_.display;
        const bool grid = d.mode == SetlistDisplay::Mode::Grid;
        view_.setSelectedItemIndex(int(d.mode), juce::dontSendNotification);
        columns_.setSelectedId(d.columns, juce::dontSendNotification); rows_.setSelectedId(d.rows, juce::dontSendNotification);
        columns_.setVisible(grid); rows_.setVisible(grid);
        grid_->setVisible(grid); list_.setVisible(!grid);
        previous_.setVisible(d.mode != SetlistDisplay::Mode::Scroll); next_.setVisible(d.mode != SetlistDisplay::Mode::Scroll);
        updateButtons(); resized(); repaint();
    }

    void SetlistPane::paint(juce::Graphics& g) { g.fillAll(ink(Ink::Background)); }

    void SetlistPane::resized()
    {
        const bool touch = mode() == 2;
        const int control = touch ? 44 : 30, gap = touch ? 10 : 8, big = touch ? 72 : 56;
        list_.setRowHeight(touch ? 64 : 48);
        auto area = getLocalBounds().reduced(16, 12);
        auto bar = area.removeFromTop(control);
        new_.setBounds(bar.removeFromRight(touch ? 90 : 70)); bar.removeFromRight(gap);
        chooser_.setBounds(bar.removeFromLeft(std::min(bar.getWidth() / 2, 280)));
        bar.removeFromLeft(gap);
        view_.setBounds(bar.removeFromLeft(touch ? 170 : 140));
        bar.removeFromLeft(gap);
        columns_.setBounds(bar.removeFromLeft(touch ? 140 : 110)); bar.removeFromLeft(gap);
        rows_.setBounds(bar.removeFromLeft(touch ? 120 : 90));
        area.removeFromTop(gap);
        // Previous and Next across the bottom, as big as the stage needs.
        if(previous_.isVisible())
        {
            auto stage = area.removeFromBottom(big);
            previous_.setBounds(stage.removeFromLeft(stage.getWidth() / 2 - gap / 2)); stage.removeFromLeft(gap);
            next_.setBounds(stage);
            area.removeFromBottom(gap);
        }
        auto edit = area.removeFromBottom(control);
        area.removeFromBottom(gap);
        for(auto* b : {&down_, &up_, &remove_, &add_}) { b->setBounds(edit.removeFromRight(touch ? 100 : 80)); edit.removeFromRight(gap); }
        status_.setFont(juce::FontOptions(textSize(TextRole::Small, mode())));
        status_.setBounds(edit);
        list_.setBounds(area);
        grid_->setBounds(area);
    }
}
