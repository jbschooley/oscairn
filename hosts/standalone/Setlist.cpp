// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Setlist.h"
#include "oscairn/library.h"

namespace oscairn::standalone
{
    namespace
    {
        juce::String text(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }
        constexpr int maxEntries = 1000;
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
        const auto* entries = doc["entries"].getArray();
        if(!entries || entries->size() > maxEntries) throw std::runtime_error("a setlist's entries must be a list of at most 1000");
        for(const auto& e : *entries)
        {
            SetlistEntry entry{str(e["id"], "entry ID"), str(e["name"], "entry name")};
            if(entry.id.empty()) throw std::runtime_error("a setlist entry without an ID");
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
            entry->setProperty("id", text(e.id)); entry->setProperty("name", text(e.name));
            entries.add(juce::var(entry));
        }
        auto* doc = new juce::DynamicObject();
        doc->setProperty("format", "oscairn.setlist"); doc->setProperty("version", 1);
        doc->setProperty("name", text(s.name)); doc->setProperty("entries", entries);
        if(!folder.createDirectory()) throw std::runtime_error("cannot make the setlists folder");
        const auto file = setlistFile(folder, s.name);
        juce::TemporaryFile temporary(file);
        if(!temporary.getFile().replaceWithText(juce::JSON::toString(juce::var(doc)) + "\n") || !temporary.overwriteTargetFileWithTemporary())
            throw std::runtime_error("cannot save the setlist");
    }

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
        remove_.onClick = [this] { remove(list_.getSelectedRow()); };
        up_.onClick = [this] { move(list_.getSelectedRow(), -1); };
        down_.onClick = [this] { move(list_.getSelectedRow(), 1); };
        list_.setColour(juce::ListBox::backgroundColourId, ink(Ink::Surface));
        list_.setOutlineThickness(0);
        status_.setColour(juce::Label::textColourId, ink(Ink::Muted));
        for(auto* c : std::initializer_list<juce::Component*>{&chooser_, &new_, &previous_, &next_, &add_, &remove_, &up_, &down_, &list_, &status_}) addAndMakeVisible(c);
        setWantsKeyboardFocus(true);
        refreshChooser();
        if(!files_.empty()) open(files_.front());
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
        // Names as the library has them now; entries it no longer has are marked.
        missing_.assign(setlist_.entries.size(), false);
        Control::LibraryQuery all; all.limit = 1000;
        if(const auto found = engine_.library(all))
            for(size_t i = 0; i < setlist_.entries.size(); ++i)
            {
                const auto at = std::find_if(found->items.begin(), found->items.end(), [&](const auto& item) { return item.id == setlist_.entries[i].id; });
                if(at == found->items.end()) missing_[i] = true; else setlist_.entries[i].name = at->name;
            }
        list_.updateContent(); list_.deselectAllRows();
        refreshChooser();
        say(juce::String(setlist_.entries.size()) + (setlist_.entries.size() == 1 ? " Performance" : " Performances"));
        updateButtons(); repaint();
    }

    void SetlistPane::create(const std::string& name)
    {
        const auto file = setlistFile(folder_, name);
        if(file.existsAsFile()) { open(file); return; }
        setlist_ = {name, {}}; missing_.clear(); current_ = -1;
        save();
        list_.updateContent();
        refreshChooser(); updateButtons(); repaint();
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
        setlist_.entries.push_back({item->id, item->name});
        missing_.push_back(false);
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
        missing_.erase(missing_.begin() + index);
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
        std::swap(missing_[size_t(index)], missing_[size_t(to)]);
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
        if(missing_[size_t(index)]) { say(text(entry.name) + " is no longer in the library."); return; }
        SoundAction open; open.kind = SoundAction::LoadFromLibrary; open.libraryId = entry.id;
        engine_.soundAction(std::move(open));
        current_ = index;
        list_.selectRow(index);
        say(juce::String(index + 1) + ". " + text(entry.name));
        updateButtons(); repaint();
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
        const int row = list_.getSelectedRow(), n = int(setlist_.entries.size());
        previous_.setEnabled(n > 0 && current_ > 0); next_.setEnabled(n > 0 && current_ < n - 1);
        remove_.setEnabled(row >= 0); up_.setEnabled(row > 0); down_.setEnabled(row >= 0 && row < n - 1);
        add_.setEnabled(actions_.offered(pickPerformance));
    }

    void SetlistPane::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected)
    {
        if(row < 0 || row >= int(setlist_.entries.size())) return;
        const auto& entry = setlist_.entries[size_t(row)];
        const bool playing = row == current_;
        g.setColour(playing ? ink(Ink::Accent).withAlpha(.22f) : selected ? ink(Ink::Raised) : (row % 2 ? ink(Ink::Raised).withAlpha(.35f) : ink(Ink::Surface)));
        g.fillRect(0, 0, width, height);
        if(playing) { g.setColour(ink(Ink::Accent)); g.fillRect(0, 0, 4, height); }
        const float body = textSize(TextRole::Body, mode()), title = textSize(TextRole::Title, mode());
        auto area = juce::Rectangle<int>(0, 0, width, height).reduced(16, 0);
        g.setColour(playing ? ink(Ink::Accent) : ink(Ink::Muted)); g.setFont(juce::FontOptions(body, juce::Font::bold));
        g.drawText(juce::String(row + 1), area.removeFromLeft(int(body * 2.4f)), juce::Justification::centredLeft);
        g.setColour(missing_[size_t(row)] ? ink(Ink::Muted) : ink(Ink::Text)); g.setFont(juce::FontOptions(title, playing ? juce::Font::bold : juce::Font::plain));
        g.drawText(text(entry.name.empty() ? "Untitled" : entry.name) + (missing_[size_t(row)] ? "  (not in the library)" : ""), area, juce::Justification::centredLeft, true);
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
        chooser_.setBounds(bar.removeFromLeft(std::min(bar.getWidth(), 320)));
        area.removeFromTop(gap);
        // Previous and Next across the bottom, as big as the stage needs.
        auto stage = area.removeFromBottom(big);
        previous_.setBounds(stage.removeFromLeft(stage.getWidth() / 2 - gap / 2)); stage.removeFromLeft(gap);
        next_.setBounds(stage);
        area.removeFromBottom(gap);
        auto edit = area.removeFromBottom(control);
        area.removeFromBottom(gap);
        for(auto* b : {&down_, &up_, &remove_, &add_}) { b->setBounds(edit.removeFromRight(touch ? 100 : 80)); edit.removeFromRight(gap); }
        status_.setFont(juce::FontOptions(textSize(TextRole::Small, mode())));
        status_.setBounds(edit);
        list_.setBounds(area);
    }
}
