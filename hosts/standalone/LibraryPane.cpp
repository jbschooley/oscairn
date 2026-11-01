// SPDX-License-Identifier: AGPL-3.0-or-later
#include "LibraryPane.h"
#include "PerformanceColors.h"

namespace oscairn::standalone
{
    namespace
    {
        juce::String text(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }
        constexpr size_t shownTags = 16;
        void drawStar(juce::Graphics& g, juce::Rectangle<float> r, bool on)
        {
            juce::Path star;
            star.addStar(r.getCentre(), 5, r.getWidth() * .2f, r.getWidth() * .45f, 0);
            g.setColour(on ? ink(Ink::Accent) : ink(Ink::Muted).withAlpha(.6f));
            if(on) g.fillPath(star); else g.strokePath(star, juce::PathStrokeType(1.2f));
        }
    }

    LibraryPane::LibraryPane(Engine& engine, Actions& actions, Mode mode, const std::string& kind) : engine_(engine), actions_(actions), mode_(mode)
    {
        query_.kind = kind;
        for(auto* b : {&performances_, &sounds_}) { b->setClickingTogglesState(true); b->setRadioGroupId(1); }
        performances_.onClick = [this] { showKind("performance"); };
        sounds_.onClick = [this] { showKind("sound"); };
        part_.onClick = [this] { newPart(list_.getSelectedRow()); };
        search_.setJustification(juce::Justification::centredLeft); search_.setIndents(10, 0);
        search_.onTextChange = [this] { query_.text = search_.getText().toStdString(); refresh(); };
        favourites_.setClickingTogglesState(true);
        favourites_.onClick = [this] { query_.favourites = favourites_.getToggleState(); refresh(); };
        recent_.setClickingTogglesState(true);
        recent_.onClick = [this] { query_.recent = recent_.getToggleState(); refresh(); };
        list_.setColour(juce::ListBox::backgroundColourId, ink(Ink::Surface));
        list_.setOutlineThickness(0);
        send_.onClick = [this] { send(list_.getSelectedRow()); };
        cancel_.onClick = [this] { if(onPicked) onPicked({}); };
        pick_.onClick = [this] { choose(list_.getSelectedRow()); };
        delete_.onClick = [this] { deleteItem(list_.getSelectedRow()); };
        delete_.setTooltip("Delete the selected item from the library. A locked library's comes back when the library is installed again");
        libraries_.onClick = [this] { librariesMenu(); };
        libraries_.setTooltip("Install a library or an exported Performance, or uninstall a library");
        status_.setColour(juce::Label::textColourId, ink(Ink::Muted));
        for(auto* c : std::initializer_list<juce::Component*>{&search_, &favourites_, &recent_, &list_, &status_}) addAndMakeVisible(c);
        for(auto* c : std::initializer_list<juce::Component*>{&send_, &part_, &cancel_, &pick_, &performances_, &sounds_, &delete_, &libraries_}) addChildComponent(c);
        performances_.setVisible(mode_ == Mode::Play); sounds_.setVisible(mode_ == Mode::Play);
        delete_.setVisible(mode_ == Mode::Play); libraries_.setVisible(mode_ == Mode::Play);
        cancel_.setVisible(mode_ == Mode::Pick); pick_.setVisible(mode_ == Mode::Pick);
        showKind(kind);
        startTimer(300);
    }

    void LibraryPane::showKind(const std::string& kind)
    {
        if(query_.kind != kind) query_.tags.clear();
        query_.kind = kind;
        const bool sound = kind == "sound";
        performances_.setToggleState(!sound, juce::dontSendNotification); sounds_.setToggleState(sound, juce::dontSendNotification);
        search_.setTextToShowWhenEmpty(sound ? "Search Sounds by name, tag or description" : "Search Performances by name, tag or description", ink(Ink::Muted));
        send_.setVisible(mode_ == Mode::Play && !sound && actions_.offered(sendPerformance));
        part_.setVisible(mode_ == Mode::Play && sound);
        updateButtons();
        refresh();
        resized();
    }

    void LibraryPane::newPart(int row)
    {
        if(row < 0 || size_t(row) >= result_.items.size() || result_.items[size_t(row)].kind != "sound") return;
        SoundAction add; add.kind = SoundAction::PartFromSound; add.libraryId = result_.items[size_t(row)].id;
        engine_.soundAction(std::move(add));
        message_ = "Adding " + text(result_.items[size_t(row)].name) + " as a new part";
        status_.setText(message_, juce::dontSendNotification);
    }

    void LibraryPane::search(const juce::String& s) { search_.setText(s, juce::dontSendNotification); query_.text = s.toStdString(); refresh(); }

    void LibraryPane::toggleTag(const std::string& tag)
    {
        const auto at = std::find(query_.tags.begin(), query_.tags.end(), tag);
        if(at == query_.tags.end()) query_.tags.push_back(tag); else query_.tags.erase(at);
        refresh();
    }

    void LibraryPane::timerCallback()
    {
        // Saving into the library happens on the engine's worker; look again once it is done.
        if(!engine_.controlReady()) { stale_ = true; return; }
        if(!stale_) return;
        const auto found = engine_.library(query_);
        if(!found) return;
        stale_ = false;
        const auto selected = list_.getSelectedRow() >= 0 && size_t(list_.getSelectedRow()) < result_.items.size() ? result_.items[size_t(list_.getSelectedRow())].id : std::string();
        result_ = *found;
        list_.updateContent();
        list_.deselectAllRows();
        for(size_t i = 0; i < result_.items.size(); ++i) if(result_.items[i].id == selected) list_.selectRow(int(i), true, true);
        const bool sound = query_.kind == "sound";
        if(installing_) { installing_ = false; message_ = text(engine_.soundMessage()); status_.setText(message_, juce::dontSendNotification); } // what it added, or why not
        if(message_.isEmpty()) status_.setText(juce::String(result_.total) + (sound ? (result_.total == 1 ? " Sound" : " Sounds") : (result_.total == 1 ? " Performance" : " Performances")), juce::dontSendNotification);
        rebuildTags();
        updateButtons();
        repaint();
    }

    void LibraryPane::rebuildTags()
    {
        // The chosen tags first, then the most used of the rest.
        auto counts = result_.tags;
        std::stable_sort(counts.begin(), counts.end(), [&](const auto& a, const auto& b) {
            const bool ca = std::count(query_.tags.begin(), query_.tags.end(), a.first) != 0, cb = std::count(query_.tags.begin(), query_.tags.end(), b.first) != 0;
            return ca != cb ? ca : a.second > b.second;
        });
        if(counts.size() > shownTags) counts.resize(shownTags);
        tagButtons_.clear();
        for(const auto& [tag, count] : counts)
        {
            auto* b = tagButtons_.add(new juce::TextButton(text(tag)));
            b->setClickingTogglesState(true);
            b->setToggleState(std::count(query_.tags.begin(), query_.tags.end(), tag) != 0, juce::dontSendNotification);
            b->setTooltip(juce::String(count) + (count == 1 ? " match" : " matches"));
            b->onClick = [this, t = tag] { toggleTag(t); };
            addAndMakeVisible(b);
        }
        resized();
    }

    void LibraryPane::updateButtons()
    {
        const bool any = list_.getSelectedRow() >= 0;
        send_.setEnabled(any); pick_.setEnabled(any); part_.setEnabled(any); delete_.setEnabled(any);
    }

    void LibraryPane::choose(int row)
    {
        if(row < 0 || size_t(row) >= result_.items.size()) return;
        const auto& item = result_.items[size_t(row)];
        if(mode_ == Mode::Pick) { if(onPicked) onPicked(item.id); return; }
        if(item.kind != "performance") return; // a Sound goes into a part: New part, or a part's Load Sound
        SoundAction open; open.kind = SoundAction::LoadFromLibrary; open.libraryId = item.id;
        engine_.soundAction(std::move(open));
        message_ = "Opening " + text(item.name);
        status_.setText(message_, juce::dontSendNotification);
        stale_ = true; // it is now the most recent
    }

    void LibraryPane::send(int row)
    {
        if(row < 0 || size_t(row) >= result_.items.size()) return;
        actions_.request(sendPerformance, result_.items[size_t(row)].id, [safe = juce::Component::SafePointer<LibraryPane>(this)](const std::string& done) {
            if(!safe || done.empty()) return;
            safe->message_ = text(done);
            safe->status_.setText(safe->message_, juce::dontSendNotification);
        });
    }

    juce::Rectangle<int> LibraryPane::starArea(int width, int height) const { return {width - height, 0, height, height}; }

    void LibraryPane::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected)
    {
        if(row < 0 || size_t(row) >= result_.items.size()) return;
        const auto& item = result_.items[size_t(row)];
        const int m = mode();
        g.setColour(selected ? ink(Ink::Accent).withAlpha(.18f) : (row % 2 ? ink(Ink::Raised).withAlpha(.35f) : ink(Ink::Surface)));
        g.fillRect(0, 0, width, height);
        if(selected) { g.setColour(ink(Ink::Accent)); g.fillRect(0, 0, 3, height); }
        if(const auto color = colorOf(item.color); !color.isTransparent()) { g.setColour(color); g.fillRoundedRectangle(juce::Rectangle<float>(10, height * .22f, 6, height * .56f), 3); }
        auto area = juce::Rectangle<int>(0, 0, width, height).reduced(24, 4);
        area.removeFromRight(height);
        const float body = textSize(TextRole::Body, m), small = textSize(TextRole::Small, m);
        auto top = area.removeFromTop(area.getHeight() / 2 + 2);
        g.setColour(ink(Ink::Text)); g.setFont(juce::FontOptions(body, juce::Font::bold));
        g.drawText(text(item.name.empty() ? "Untitled" : item.name), top, juce::Justification::bottomLeft, true);
        juce::StringArray tags; for(const auto& t : item.tags) tags.add(text(t));
        // A locked library's item says so first: Save makes a new one.
        auto detail = tags.joinIntoString("  ·  ") + (item.description.empty() ? "" : (tags.isEmpty() ? "" : "   ") + text(item.description));
        if(!item.library.empty()) detail = "Locked" + (detail.isEmpty() ? juce::String() : "  ·  " + detail);
        g.setColour(ink(Ink::Muted)); g.setFont(juce::FontOptions(small));
        g.drawText(detail, area, juce::Justification::topLeft, true);
        drawStar(g, starArea(width, height).toFloat().reduced(float(height) * .28f), item.favourite);
    }

    void LibraryPane::listBoxItemClicked(int row, const juce::MouseEvent& e)
    {
        if(row < 0 || size_t(row) >= result_.items.size()) return;
        if(starArea(list_.getVisibleRowWidth(), list_.getRowHeight()).contains(e.getEventRelativeTo(list_.getComponentForRowNumber(row)).getPosition()))
        {
            const auto& item = result_.items[size_t(row)];
            engine_.edit([id = item.id, on = !item.favourite](Control& c) { c.libraryFavourite(id, on); });
            refresh();
            return;
        }
        if(mode_ == Mode::Play) choose(row); // played from the list: one click opens it
    }

    void LibraryPane::listBoxItemDoubleClicked(int row, const juce::MouseEvent&) { if(mode_ == Mode::Pick) choose(row); }

    void LibraryPane::paint(juce::Graphics& g) { g.fillAll(ink(Ink::Background)); }

    void LibraryPane::resized()
    {
        const bool touch = mode() == 2;
        const int control = touch ? 44 : 30, gap = touch ? 10 : 8;
        list_.setRowHeight(touch ? 64 : 48);
        auto area = getLocalBounds().reduced(16, 12);
        auto bar = area.removeFromTop(control);
        if(mode_ == Mode::Play)
        {
            performances_.setBounds(bar.removeFromLeft(touch ? 150 : 120)); bar.removeFromLeft(gap);
            sounds_.setBounds(bar.removeFromLeft(touch ? 110 : 90)); bar.removeFromLeft(gap);
        }
        recent_.setBounds(bar.removeFromRight(touch ? 110 : 90)); bar.removeFromRight(gap);
        favourites_.setBounds(bar.removeFromRight(touch ? 130 : 110)); bar.removeFromRight(gap);
        search_.setBounds(bar);
        search_.setFont(juce::FontOptions(textSize(TextRole::Body, mode())));
        area.removeFromTop(gap);
        // Tags flow in rows, each button as wide as its text.
        if(!tagButtons_.isEmpty())
        {
            const int tagHeight = touch ? 36 : 24;
            auto row = area.removeFromTop(tagHeight);
            const juce::Font font(juce::FontOptions(textSize(TextRole::Small, mode())));
            for(auto* b : tagButtons_)
            {
                const int w = int(juce::GlyphArrangement::getStringWidth(font, b->getButtonText())) + (touch ? 28 : 20);
                if(w > row.getWidth() && row.getX() > area.getX()) { area.removeFromTop(gap / 2); row = area.removeFromTop(tagHeight); }
                b->setBounds(row.removeFromLeft(w)); row.removeFromLeft(gap / 2);
            }
            area.removeFromTop(gap);
        }
        auto bottom = area.removeFromBottom(control);
        area.removeFromBottom(gap);
        list_.setBounds(area);
        if(mode_ == Mode::Pick) { pick_.setBounds(bottom.removeFromRight(110)); bottom.removeFromRight(gap); cancel_.setBounds(bottom.removeFromRight(110)); }
        else
        {
            if(send_.isVisible()) send_.setBounds(bottom.removeFromRight(touch ? 170 : 140));
            else if(part_.isVisible()) part_.setBounds(bottom.removeFromRight(touch ? 150 : 120));
            bottom.removeFromRight(gap); delete_.setBounds(bottom.removeFromRight(touch ? 110 : 90));
            bottom.removeFromRight(gap); libraries_.setBounds(bottom.removeFromRight(touch ? 140 : 110));
            bottom.removeFromRight(gap);
        }
        status_.setFont(juce::FontOptions(textSize(TextRole::Small, mode())));
        status_.setBounds(bottom);
    }

    void LibraryPane::deleteItem(int row)
    {
        if(row < 0 || size_t(row) >= result_.items.size()) return;
        const auto item = result_.items[size_t(row)];
        const auto name = text(item.name.empty() ? "Untitled" : item.name);
        auto options = juce::MessageBoxOptions::makeOptionsOkCancel(juce::MessageBoxIconType::QuestionIcon, "Delete " + name + "?",
            item.library.empty() ? juce::String("It is removed from your library. Performances that used it keep their parts.")
                                 : juce::String("It is removed. Installing its library again brings it back."), "Delete", "Cancel", this);
        juce::AlertWindow::showAsync(options, [safe = juce::Component::SafePointer<LibraryPane>(this), id = item.id, name](int chosen) {
            if(!safe || chosen != 1) return;
            const auto error = safe->engine_.deleteLibraryItem(id);
            safe->refresh();
            safe->message_ = error.empty() ? name + " deleted." : text(error);
            safe->status_.setText(safe->message_, juce::dontSendNotification);
        });
    }

    void LibraryPane::librariesMenu()
    {
        juce::PopupMenu menu;
        menu.addItem(1, "Install from a file...");
        const auto installed = engine_.libraries();
        if(!installed.empty()) menu.addSeparator();
        for(size_t i = 0; i < installed.size(); ++i)
            menu.addItem(int(i) + 2, "Uninstall " + text(installed[i].name) + " (" + juce::String(int(installed[i].items)) + (installed[i].items == 1 ? " item)" : " items)"));
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&libraries_), [safe = juce::Component::SafePointer<LibraryPane>(this), installed](int chosen) {
            if(!safe || chosen <= 0) return;
            if(chosen == 1) { safe->install(); return; }
            const auto library = installed[size_t(chosen - 2)];
            auto options = juce::MessageBoxOptions::makeOptionsOkCancel(juce::MessageBoxIconType::QuestionIcon, "Uninstall " + text(library.name) + "?",
                "Its Sounds and Performances are removed. Your Performances keep their parts and samples, and your own copies stay.", "Uninstall", "Cancel", safe.getComponent());
            juce::AlertWindow::showAsync(options, [safe, library](int ok) {
                if(!safe || ok != 1) return;
                const auto error = safe->engine_.uninstallLibrary(library.id);
                safe->refresh();
                safe->message_ = error.empty() ? text(library.name) + " uninstalled." : text(error);
                safe->status_.setText(safe->message_, juce::dontSendNotification);
            });
        });
    }

    // An archive (a library, or an exported Performance): how to install it (locked, or as the
    // user's own, the default for an exported Performance) and which items, all ticked.
    void LibraryPane::install()
    {
        chooser_ = std::make_unique<juce::FileChooser>("Install a library or an exported Performance", juce::File::getSpecialLocation(juce::File::userDocumentsDirectory), "*.olib");
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe = juce::Component::SafePointer<LibraryPane>(this)](const juce::FileChooser& fc) {
            if(!safe || fc.getResult() == juce::File()) return;
            const auto file = fc.getResult().getFullPathName().toStdString();
            Control::Archive archive;
            if(const auto error = safe->engine_.archiveContents(file, archive); !error.empty())
            {
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Not installed", text(error));
                return;
            }
            struct Choose : juce::Component
            {
                juce::ToggleButton own{"Into my Sounds and Performances, to edit"}, locked{"As a locked library"};
                juce::Label heading{{}, "Items"};
                juce::OwnedArray<juce::ToggleButton> items;
                std::vector<std::string> ids;
                juce::TextButton go{"Install"}, cancel{"Cancel"};
                explicit Choose(const Control::Archive& a)
                {
                    own.setRadioGroupId(1); locked.setRadioGroupId(1);
                    (a.kind == "library" ? locked : own).setToggleState(true, juce::dontSendNotification);
                    for(const auto& i : a.items)
                    {
                        auto* b = items.add(new juce::ToggleButton(text(i.name.empty() ? "Untitled" : i.name) + (i.kind == "sound" ? "  (Sound)" : "  (Performance)")));
                        b->setToggleState(true, juce::dontSendNotification); addAndMakeVisible(b); ids.push_back(i.id);
                    }
                    for(auto* c : std::initializer_list<juce::Component*>{&own, &locked, &heading, &go, &cancel}) addAndMakeVisible(c);
                    setSize(420, std::min(600, int(a.items.size()) * 30 + 180));
                }
                void resized() override
                {
                    auto area = getLocalBounds().reduced(16, 12);
                    own.setBounds(area.removeFromTop(30)); locked.setBounds(area.removeFromTop(30));
                    area.removeFromTop(6); heading.setBounds(area.removeFromTop(24));
                    auto bar = area.removeFromBottom(30);
                    go.setBounds(bar.removeFromRight(100)); bar.removeFromRight(8); cancel.setBounds(bar.removeFromRight(100));
                    for(auto* b : items) b->setBounds(area.removeFromTop(30));
                }
            };
            auto* content = new Choose(archive);
            juce::DialogWindow::LaunchOptions options;
            options.dialogTitle = "Install " + text(archive.name);
            options.content.setOwned(content); options.componentToCentreAround = safe.getComponent(); options.useNativeTitleBar = true; options.resizable = false;
            auto* dialog = options.launchAsync();
            content->cancel.onClick = [dialog] { dialog->exitModalState(0); };
            content->go.onClick = [safe, dialog, content, file] {
                SoundAction install; install.kind = SoundAction::InstallArchive; install.directory = file; install.asNew = content->locked.getToggleState();
                for(int i = 0; i < content->items.size(); ++i) if(content->items[i]->getToggleState()) install.items.push_back(content->ids[size_t(i)]);
                if(install.items.empty()) { juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Install", "Choose at least one item."); return; }
                if(install.items.size() == content->ids.size()) install.items.clear(); // all of it
                dialog->exitModalState(0);
                if(!safe) return;
                safe->engine_.soundAction(std::move(install));
                safe->message_ = "Installing...";
                safe->status_.setText(safe->message_, juce::dontSendNotification);
                safe->stale_ = true; safe->installing_ = true;
            };
        });
    }
}
