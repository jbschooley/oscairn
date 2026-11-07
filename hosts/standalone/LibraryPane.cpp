// SPDX-License-Identifier: AGPL-3.0-or-later
#include "LibraryPane.h"
#include "PerformanceColors.h"
#include "Settings.h"
#include "oscairn/package.h"
#include "oscairn/repository.h"

namespace oscairn::standalone
{
    namespace
    {
        juce::String text(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }
        const juce::String separator = juce::String::fromUTF8("  ·  "); // a const char* added to a String is not read as UTF-8
        constexpr int longPress = 500; // ms: a press this long opens the menu a right click does
        bool menuGesture(const juce::MouseEvent& e) { return e.mods.isPopupMenu() || (e.getLengthOfMousePress() >= longPress && !e.mouseWasDraggedSinceMouseDown()); }
        // Compact or not: the user's, beside the theme selection.
        juce::File viewPreference()
        {
            auto* theme = dynamic_cast<Theme*>(&juce::LookAndFeel::getDefaultLookAndFeel());
            return theme ? theme->configuration().getChildFile("library-view.txt") : juce::File();
        }
        void drawStar(juce::Graphics& g, juce::Rectangle<float> r, bool on)
        {
            drawIcon(g, "star", r, on ? ink(Ink::Accent) : ink(Ink::Muted).withAlpha(.6f), on);
        }
        juce::String tagText(const std::vector<std::string>& tags)
        {
            juce::StringArray out; for(const auto& t : tags) out.add(text(t));
            return out.joinIntoString(", ");
        }
    }

    ExportChoices::ExportChoices(Engine& engine, bool performance) : engine_(engine), performance_(performance)
    {
        signWith.setTooltip("Signing shows others the export came from you; it is off unless you choose a key");
        signWith.onChange = [this] {
            const int id = signWith.getSelectedId();
            if(id != 1000) { chosen_ = id; return; }
            signWith.setSelectedId(chosen_, juce::dontSendNotification); // until a key is made
            askToMakeKey(engine_, this, [safe = juce::Component::SafePointer<ExportChoices>(this)](const std::string& name) { if(safe) safe->chooseKey(name); });
        };
        for(auto* c : std::initializer_list<juce::Component*>{&signWith, &go, &cancel}) addAndMakeVisible(c);
        if(performance_) addAndMakeVisible(editedSounds);
        chooseKey({});
        setSize(480, performance_ ? 250 : 200);
    }

    std::string ExportChoices::key() const { return chosen_ >= 2 && size_t(chosen_ - 2) < keys_.size() ? keys_[size_t(chosen_ - 2)].name : std::string(); }

    void ExportChoices::chooseKey(const std::string& name)
    {
        keys_ = engine_.signingKeys();
        signWith.clear(juce::dontSendNotification);
        signWith.addItem("Not signed", 1);
        chosen_ = 1;
        for(size_t i = 0; i < keys_.size(); ++i)
        {
            signWith.addItem("Sign with " + text(keys_[i].name), int(i) + 2);
            if(keys_[i].name == name) chosen_ = int(i) + 2;
        }
        signWith.addSeparator();
        signWith.addItem("Make a key...", 1000);
        signWith.setSelectedId(chosen_, juce::dontSendNotification);
    }

    void ExportChoices::resized()
    {
        const int control = desktopMode(*this) == 2 ? 44 : 30;
        auto area = getLocalBounds().reduced(20, 16);
        note_ = performance_ ? area.removeFromTop(control) : juce::Rectangle<int>();
        if(performance_) { editedSounds.setBounds(area.removeFromTop(control)); area.removeFromTop(10); }
        signLabel_ = area.removeFromTop(22);
        signWith.setBounds(area.removeFromTop(control).removeFromLeft(300));
        auto bar = area.removeFromBottom(control);
        go.setBounds(bar.removeFromRight(110)); bar.removeFromRight(8); cancel.setBounds(bar.removeFromRight(100));
    }

    void ExportChoices::paint(juce::Graphics& g)
    {
        g.fillAll(ink(Ink::Background));
        const int m = std::max(1, desktopMode(*this));
        g.setColour(ink(Ink::Muted));
        if(performance_) drawInked(g, "Samples and the Sounds of unedited parts always go.", note_, juce::Font(juce::FontOptions(textSize(TextRole::Body, m))));
        drawInked(g, "Signature", signLabel_, juce::Font(juce::FontOptions(textSize(TextRole::Small, m), juce::Font::bold)));
    }

    void showExport(Engine& engine, juce::Component* over, const juce::String& title, bool performance, std::function<void(bool, const std::string&)> start)
    {
        auto* content = new ExportChoices(engine, performance);
        if(over) setDesktopMode(*content, std::max(1, desktopMode(*over)));
        juce::DialogWindow::LaunchOptions options;
        options.dialogTitle = title;
        options.content.setOwned(content); options.componentToCentreAround = over; options.useNativeTitleBar = true; options.resizable = false;
        options.dialogBackgroundColour = ink(Ink::Background); // JUCE's default is light grey, under the theme's text
        auto* dialog = options.launchAsync();
        content->cancel.onClick = [dialog] { dialog->exitModalState(0); };
        content->go.onClick = [dialog, content, then = std::move(start)] {
            const bool edited = content->editedSounds.getToggleState(); const auto key = content->key();
            dialog->exitModalState(0);
            then(edited, key);
        };
    }

    InstallChoices::InstallChoices(const Control::Archive& a)
    {
        juce::String unknown, revoked;
        for(const auto& c : a.code)
        {
            const auto what = text(c.name + " " + c.version + " (" + c.platform + ")");
            if(c.status == "revoked") { revoked << "\n  " << what; ++codeLines_; }
            else if(c.status != "trusted") { unknown << "\n  " << what << (c.status == "unsigned" ? ", unsigned" : ", signed by unknown key " + text(c.key)); ++codeLines_; asks = true; }
        }
        juce::String warning;
        if(asks) warning << "These plugins run code from an unknown source:" << unknown;
        if(revoked.isNotEmpty()) warning << (asks ? "\n" : "") << "Signed by a revoked key, so not installed:" << revoked;
        if(warning.isNotEmpty()) { code.setText(warning, juce::dontSendNotification); codeLines_ += asks + revoked.isNotEmpty(); addAndMakeVisible(code); }
        if(asks) go.setButtonText("Install anyway");
        signer_ = describeSigner(a.signer);
        own.setRadioGroupId(1); locked.setRadioGroupId(1);
        (a.kind == "library" ? locked : own).setToggleState(true, juce::dontSendNotification);
        for(const bool sound : {false, true})
        {
            bool headed = false;
            for(const auto& i : a.items)
            {
                if((i.kind == "sound") != sound) continue;
                if(!headed) { list.addAndMakeVisible(headings.add(new juce::Label({}, sound ? "Sounds" : "Performances"))); headed = true; }
                auto* b = items.add(new juce::ToggleButton(text(i.name.empty() ? "Untitled" : i.name)));
                b->setToggleState(true, juce::dontSendNotification); list.addAndMakeVisible(b); ids.push_back(i.id);
            }
        }
        // The plugin builds it adds to the store, each with who signed it.
        if(!a.code.empty()) list.addAndMakeVisible(headings.add(new juce::Label({}, "Plugins")));
        for(const auto& c : a.code)
            list.addAndMakeVisible(plugins.add(new juce::Label({}, text(c.name + " " + c.version + " (" + c.platform + "): ") + describeSigner(c) + (c.status == "revoked" ? ", not installed" : ""))));
        // Its license, and where its source is (the GPL's directions to it).
        if(!a.license.empty() || !a.sourceUrl.empty()) list.addAndMakeVisible(headings.add(new juce::Label({}, "License")));
        if(!a.license.empty()) { license.setText(text(a.license), juce::dontSendNotification); list.addAndMakeVisible(license); ++aboutRows_; }
        if(!a.sourceUrl.empty())
        {
            source.setButtonText("Source: " + text(a.sourceUrl) + " at " + text(a.sourceRevision));
            source.setURL(juce::URL(text(a.sourceUrl)));
            source.setJustificationType(juce::Justification::centredLeft);
            list.addAndMakeVisible(source); ++aboutRows_;
        }
        pluginPackage = a.kind == "plugin" || a.kind == "asset"; // no items, so no choice of how: an asset package's library goes into the asset store
        view.setViewedComponent(&list, false);
        view.setScrollBarsShown(true, false);
        for(auto* c : std::initializer_list<juce::Component*>{&view, &go, &cancel}) addAndMakeVisible(c);
        if(!pluginPackage) { addAndMakeVisible(own); addAndMakeVisible(locked); } // a plugin package's builds go into the store either way
        setSize(420, std::min(630, int(a.items.size() + a.code.size() + size_t(aboutRows_)) * 30 + headings.size() * 26 + std::max(0, headings.size() - 1) * 4 + (pluginPackage ? 126 : 186) + codeLines_ * 18));
    }

    void InstallChoices::resized()
    {
        auto area = getLocalBounds().reduced(16, 12);
        signerArea_ = area.removeFromTop(30);
        if(!pluginPackage) { own.setBounds(area.removeFromTop(30)); locked.setBounds(area.removeFromTop(30)); }
        area.removeFromTop(6);
        auto bar = area.removeFromBottom(30);
        if(codeLines_) { area.removeFromBottom(6); code.setBounds(area.removeFromBottom(codeLines_ * 18)); }
        go.setBounds(bar.removeFromRight(120)); bar.removeFromRight(8); cancel.setBounds(bar.removeFromRight(100));
        view.setBounds(area);
        const int tall = (items.size() + plugins.size() + aboutRows_) * 30 + headings.size() * 26 + std::max(0, headings.size() - 1) * 4;
        list.setSize(area.getWidth() - (tall > area.getHeight() ? view.getScrollBarThickness() : 0), tall);
        auto rows = list.getLocalBounds();
        bool firstHeading = true; // headings and items, in order; 4 px above each heading after the first
        for(auto* c : list.getChildren()) { const bool heading = headings.contains(dynamic_cast<juce::Label*>(c)); if(heading) { if(!firstHeading) rows.removeFromTop(4); firstHeading = false; } c->setBounds(rows.removeFromTop(heading ? 26 : 30)); }
    }

    void InstallChoices::paint(juce::Graphics& g)
    {
        g.fillAll(ink(Ink::Background));
        g.setColour(ink(Ink::Muted));
        drawInked(g, signer_, signerArea_, juce::Font(juce::FontOptions(textSize(TextRole::Body, std::max(1, desktopMode(*this))))));
    }

    ImportChoices::ImportChoices(const Control::PerformanceContents& held, int maxHeight)
    {
        for(const auto& p : held.parts) { auto* b = parts.add(new juce::ToggleButton(text(p.name) + (p.bus ? " (bus)" : ""))); b->setToggleState(true, juce::dontSendNotification); partIds.push_back(p.id); }
        for(const auto& s : held.scenes) { auto* b = scenes.add(new juce::ToggleButton(text(s.name))); b->setToggleState(true, juce::dontSendNotification); sceneIds.push_back(s.id); }
        list.addAndMakeVisible(partsHeading);
        for(auto* b : parts) list.addAndMakeVisible(b);
        if(!held.scenes.empty()) list.addAndMakeVisible(scenesHeading);
        for(auto* b : scenes) list.addAndMakeVisible(b);
        view.setViewedComponent(&list, false);
        view.setScrollBarsShown(true, false);
        for(auto* c : std::initializer_list<juce::Component*>{&view, &import, &cancel}) addAndMakeVisible(c);
        setSize(360, std::min(maxHeight, int(held.parts.size() + held.scenes.size()) * 30 + (held.scenes.empty() ? 100 : 130)));
    }

    void ImportChoices::resized()
    {
        auto area = getLocalBounds().reduced(16, 12);
        auto bar = area.removeFromBottom(30);
        import.setBounds(bar.removeFromRight(100)); bar.removeFromRight(8); cancel.setBounds(bar.removeFromRight(100));
        area.removeFromBottom(20);
        view.setBounds(area);
        const int tall = 26 + parts.size() * 30 + (scenes.isEmpty() ? 0 : 30 + scenes.size() * 30);
        list.setSize(area.getWidth() - (tall > area.getHeight() ? view.getScrollBarThickness() : 0), tall);
        auto rows = list.getLocalBounds();
        partsHeading.setBounds(rows.removeFromTop(26));
        for(auto* b : parts) b->setBounds(rows.removeFromTop(30));
        if(!scenes.isEmpty()) { rows.removeFromTop(4); scenesHeading.setBounds(rows.removeFromTop(26)); }
        for(auto* b : scenes) b->setBounds(rows.removeFromTop(30));
    }

    std::vector<uint32_t> ImportChoices::ticked(const juce::OwnedArray<juce::ToggleButton>& boxes, const std::vector<uint32_t>& ids)
    {
        std::vector<uint32_t> out; for(int i = 0; i < boxes.size(); ++i) if(boxes[i]->getToggleState()) out.push_back(ids[size_t(i)]);
        return out;
    }

    RepositoryChoices::RepositoryChoices(const Control::PackageJob& listing)
    {
        heading_ = text(listing.name) + " (" + text(listing.repository) + ")";
        for(const auto& p : listing.packages)
        {
            // The newest stable version it can install, as the engine picks; else why the newest cannot.
            const Control::RepositoryVersion* pick = nullptr;
            const Control::RepositoryVersion* newest = nullptr;
            for(const auto& v : p.versions)
            {
                if(!newest || repository::older(newest->version, v.version)) newest = &v;
                if(v.incompatible.empty() && v.yanked.empty() && (v.channel.empty() || v.channel == "stable") && (!pick || repository::older(pick->version, v.version))) pick = &v;
            }
            juce::String line = text(p.name.empty() ? p.id : p.name);
            if(pick) line << " " << text(pick->version) << ", " << juce::File::descriptionOfSizeInBytes(int64_t(pick->installed)) << ", by " << text(p.publisher) << (p.summary.empty() ? juce::String() : ": " + text(p.summary));
            else line << ": cannot be installed here" << (newest && !newest->incompatible.empty() ? ", " + text(newest->incompatible) : juce::String());
            auto* row = rows.add(new juce::Label({}, line));
            row->setTooltip(line);
            auto* go = installs.add(new juce::TextButton("Install"));
            go->setEnabled(pick != nullptr);
            if(!pick) row->setColour(juce::Label::textColourId, ink(Ink::Muted));
            picks.push_back({p.id, pick ? pick->version : std::string()});
            const auto index = picks.size() - 1;
            go->onClick = [this, index] { if(onInstall) onInstall(picks[index].first, picks[index].second); };
            list.addAndMakeVisible(row); list.addAndMakeVisible(go);
        }
        view.setViewedComponent(&list, false);
        view.setScrollBarsShown(true, false);
        addAndMakeVisible(view); addAndMakeVisible(close);
        setSize(560, std::min(600, int(rows.size()) * 34 + 110));
    }

    void RepositoryChoices::resized()
    {
        auto area = getLocalBounds().reduced(16, 12);
        headingArea_ = area.removeFromTop(30);
        close.setBounds(area.removeFromBottom(30).removeFromRight(100));
        area.removeFromBottom(8);
        view.setBounds(area);
        const int tall = rows.size() * 34;
        list.setSize(area.getWidth() - (tall > area.getHeight() ? view.getScrollBarThickness() : 0), tall);
        auto lines = list.getLocalBounds();
        for(int i = 0; i < rows.size(); ++i)
        {
            auto line = lines.removeFromTop(34).reduced(0, 2);
            installs[i]->setBounds(line.removeFromRight(90));
            rows[i]->setBounds(line);
        }
    }

    void RepositoryChoices::paint(juce::Graphics& g)
    {
        g.fillAll(ink(Ink::Background));
        g.setColour(ink(Ink::Text));
        drawInked(g, heading_, headingArea_, juce::Font(juce::FontOptions(textSize(TextRole::Body, std::max(1, desktopMode(*this))), juce::Font::bold)));
    }

    juce::String describeSigner(const Control::Signer& s)
    {
        const auto who = s.keyName.empty() ? "key " + fingerprint(s.key) : text(s.keyName) + " (" + fingerprint(s.key) + ")";
        if(s.status == "trusted") return "Signed by " + who + ", a key you trust";
        if(s.status == "unknown") return "Signed by " + who + ", not a key you trust";
        if(s.status == "revoked") return "Signed by " + who + ", which is revoked";
        return "Not signed";
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
        favorites_.setClickingTogglesState(true);
        favorites_.onClick = [this] { query_.favorites = favorites_.getToggleState(); refresh(); };
        recent_.setClickingTogglesState(true);
        recent_.onClick = [this] { query_.recent = recent_.getToggleState(); refresh(); };
        compactButton_.setClickingTogglesState(true);
        compactButton_.setTooltip("One line a row, to see more at once");
        compactButton_.onClick = [this] { setCompact(compactButton_.getToggleState()); };
        list_.setOutlineThickness(0);
        list_.setRowSelectedOnMouseDown(false); // clicks come on release, so a long press can open the menu
        sideView_.setViewedComponent(&sidebar_, false);
        sideView_.setScrollBarsShown(true, false);
        send_.onClick = [this] { send(list_.getSelectedRow()); };
        cancel_.onClick = [this] { stopAudition(); if(onPicked) onPicked({}); };
        pick_.onClick = [this] { choose(list_.getSelectedRow()); };
        audition_.onClick = [this] { if(auditioning_.id.empty()) startAudition(list_.getSelectedRow()); else stopAudition(); };
        audition_.setTooltip("Try it without keeping it: a Sound in the part shown, a Performance in place of the open one. Space does the same");
        keep_.onClick = [this] { stopAudition(true); };
        keep_.setTooltip("Keep what is being auditioned, as if it had been loaded");
        options_.onClick = [this] { itemMenu(list_.getSelectedRow()); };
        options_.setTooltip("Everything for the selected item: edit, color, copy, delete. A right click or a long press on a row does the same");
        libraries_.onClick = [this] { librariesMenu(); };
        libraries_.setTooltip("Install a library or an exported Performance, uninstall a library, or show a hidden tag");
        for(auto* c : std::initializer_list<juce::Component*>{&search_, &favorites_, &recent_, &compactButton_, &sideView_, &list_, &options_, &audition_, &status_}) addAndMakeVisible(c);
        addChildComponent(keep_);
        for(auto* c : std::initializer_list<juce::Component*>{&send_, &part_, &cancel_, &pick_, &performances_, &sounds_, &libraries_}) addChildComponent(c);
        performances_.setVisible(mode_ == Mode::Play); sounds_.setVisible(mode_ == Mode::Play); libraries_.setVisible(mode_ == Mode::Play);
        cancel_.setVisible(mode_ == Mode::Pick); pick_.setVisible(mode_ == Mode::Pick);
        if(const auto f = viewPreference(); f.existsAsFile())
        {
            const auto words = juce::StringArray::fromTokens(f.loadFileAsString(), false);
            compact_ = words.contains("compact"); pills_ = words.contains("pills");
        }
        compactButton_.setToggleState(compact_, juce::dontSendNotification);
        (pills_ ? sidebar_.pills : sidebar_.list).setToggleState(true, juce::dontSendNotification);
        showKind(kind);
        startTimer(300);
    }

    LibraryPane::~LibraryPane() { stopAudition(); list_.setModel(nullptr); }

    void LibraryPane::showKind(const std::string& kind)
    {
        if(query_.kind != kind) query_.tags.clear();
        query_.kind = kind;
        const bool sound = kind == "sound";
        performances_.setToggleState(!sound, juce::dontSendNotification); sounds_.setToggleState(sound, juce::dontSendNotification);
        updatePlaceholder();
        send_.setVisible(mode_ == Mode::Play && !sound && actions_.offered(sendPerformance));
        part_.setVisible(mode_ == Mode::Play && sound);
        updateButtons();
        refresh();
        resized();
    }

    // What to type, as long as the box allows: touch sizes leave it narrower.
    void LibraryPane::updatePlaceholder()
    {
        const bool sound = query_.kind == "sound";
        const juce::String what = sound ? "Sounds" : "Performances";
        search_.setTextToShowWhenEmpty(mode() == 2 ? "Search " + what : "Search " + what + " by name, tag or description", ink(Ink::Muted));
    }

    void LibraryPane::showSource(const std::string& source) { query_.source = source; refresh(); }

    void LibraryPane::setCompact(bool compact)
    {
        compact_ = compact;
        compactButton_.setToggleState(compact, juce::dontSendNotification);
        savePreference();
        resized();
        list_.repaint();
    }

    void LibraryPane::setPills(bool pills)
    {
        pills_ = pills;
        (pills ? sidebar_.pills : sidebar_.list).setToggleState(true, juce::dontSendNotification);
        savePreference();
        rebuildSide();
    }

    void LibraryPane::savePreference() const
    {
        if(const auto f = viewPreference(); f != juce::File()) f.replaceWithText(juce::String(compact_ ? "compact" : "comfortable") + (pills_ ? " pills" : " list"));
    }

    void LibraryPane::tell(const juce::String& s) { message_ = s; status_.setText(s, juce::dontSendNotification); }

    void LibraryPane::newPart(int row)
    {
        const auto* item = at(row);
        if(!item || item->kind != "sound") return;
        SoundAction add; add.kind = SoundAction::PartFromSound; add.libraryId = item->id;
        engine_.soundAction(std::move(add));
        tell("Adding " + text(item->name) + " as a new part");
    }

    void LibraryPane::search(const juce::String& s) { search_.setText(s, juce::dontSendNotification); query_.text = s.toStdString(); refresh(); }

    void LibraryPane::toggleTag(const std::string& tag)
    {
        const auto found = std::find(query_.tags.begin(), query_.tags.end(), tag);
        if(found == query_.tags.end()) query_.tags.push_back(tag); else query_.tags.erase(found);
        refresh();
    }

    void LibraryPane::timerCallback()
    {
        // Saving into the library happens on the engine's worker; look again once it is done (a refresh changes nothing).
        if(!engine_.controlReady()) { stale_ = true; return; }
        // An audition ended elsewhere (a Performance opened keeps it): show that.
        if(!auditioning_.id.empty() && !engine_.soundBusy())
            if(const auto now = engine_.audition(); now && now->id != auditioning_.id) { auditioning_ = *now; updateButtons(); resized(); }
        if(!stale_) return;
        const auto found = engine_.library(query_);
        if(!found) return;
        stale_ = false;
        const auto* chosen = at(list_.getSelectedRow());
        const auto selected = chosen ? chosen->id : std::string();
        result_ = *found;
        installed_ = engine_.libraries();
        // A library uninstalled while it was shown: show everything.
        if(!query_.source.empty() && query_.source != "mine" && std::none_of(installed_.begin(), installed_.end(), [&](const auto& l) { return l.id == query_.source; }))
        {
            query_.source.clear(); stale_ = true; return;
        }
        list_.updateContent();
        list_.deselectAllRows();
        for(size_t i = 0; i < result_.items.size(); ++i) if(result_.items[i].id == selected) list_.selectRow(int(i), true, true);
        const bool sound = query_.kind == "sound";
        if(awaiting_) { awaiting_ = false; tell(text(engine_.soundMessage())); } // what an install or export did, or why not
        if(message_.isEmpty()) status_.setText(juce::String(result_.total) + (sound ? (result_.total == 1 ? " Sound" : " Sounds") : (result_.total == 1 ? " Performance" : " Performances")), juce::dontSendNotification);
        rebuildSide();
        updateButtons();
        repaint();
    }

    void LibraryPane::rebuildSide()
    {
        side_.clear();
        side_.push_back({Side::Heading, {}, "Library"});
        side_.push_back({Side::Source, {}, "Everything"});
        side_.push_back({Side::Source, "mine", "My own"});
        for(const auto& l : installed_) side_.push_back({Side::Source, l.id, text(l.name), l.items});
        // The chosen tags first, then the most used of the rest.
        auto counts = result_.tags;
        std::stable_sort(counts.begin(), counts.end(), [&](const auto& a, const auto& b) {
            const bool ca = std::count(query_.tags.begin(), query_.tags.end(), a.first) != 0, cb = std::count(query_.tags.begin(), query_.tags.end(), b.first) != 0;
            return ca != cb ? ca : a.second > b.second;
        });
        if(!counts.empty()) side_.push_back({Side::Heading, {}, "Tags"});
        for(const auto& [tag, count] : counts) side_.push_back({Side::Tag, tag, text(tag), count});
        const int width = sideView_.getMaximumVisibleWidth();
        sidebar_.setSize(width, std::max(sidebar_.layout(width), sideView_.getHeight()));
        sidebar_.repaint();
    }

    LibraryPane::Sidebar::Sidebar(LibraryPane& p) : pane(p)
    {
        for(auto* b : {&list, &pills}) { b->setClickingTogglesState(true); b->setRadioGroupId(1); addAndMakeVisible(b); }
        list.setTooltip("The libraries and tags as a list"); pills.setTooltip("The libraries and tags as pills, to see more at once");
        list.onClick = [this] { pane.setPills(false); };
        pills.onClick = [this] { pane.setPills(true); };
    }

    int LibraryPane::Sidebar::layout(int width)
    {
        const int m = pane.mode();
        const bool touch = m == 2;
        const int gap = touch ? 10 : 8, pad = gap + 2;
        const float body = textSize(TextRole::Body, m), small = textSize(TextRole::Small, m);
        const int row = std::max(touch ? 44 : 0, int(body) + 4 + 2 * (gap - 2)), pill = std::max(touch ? 44 : 0, int(body) + 4 + gap);
        // Measured in the theme's typeface, the one it is drawn in.
        const juce::Font font{juce::FontOptions(body).withTypeface(juce::LookAndFeel::getDefaultLookAndFeel().getTypefaceForFont(juce::Font(juce::FontOptions(body))))};
        auto area = juce::Rectangle<int>(0, 0, width, 100000).reduced(gap, 0);
        bool first = true;
        juce::Rectangle<int> flow; // the pills' current line
        for(auto& s : pane.side_)
        {
            if(s.kind == Side::Heading)
            {
                // Room above each heading, and below it before the first entry.
                if(!flow.isEmpty()) area.removeFromTop(gap);
                flow = {};
                area.removeFromTop(first ? gap : gap * 2);
                s.bounds = area.removeFromTop(int(small) + 2);
                if(first)
                {
                    // The two views' icons at the heading's end.
                    const int side = touch ? 44 : 24;
                    auto icons = s.bounds.withLeft(s.bounds.getRight() - 2 * side - 2).withSizeKeepingCentre(2 * side + 2, side);
                    pills.setBounds(icons.removeFromRight(side)); icons.removeFromRight(2); list.setBounds(icons.removeFromRight(side));
                }
                area.removeFromTop(gap);
                first = false;
                continue;
            }
            if(!pane.pills_) { s.bounds = area.removeFromTop(row); area.removeFromTop(2); continue; }
            // Pills flow in lines, each as wide as its name (and a tag's color dot).
            const int dot = s.kind == Side::Tag && !colorOf(pane.tagColor(s.value)).isTransparent() ? 14 : 0;
            const int w = std::min(area.getWidth(), int(std::ceil(juce::GlyphArrangement::getStringWidth(font, s.label))) + 2 + 2 * pad + dot); // rounded up, or the text is shortened
            if(flow.isEmpty() || flow.getWidth() < w) { if(!flow.isEmpty()) area.removeFromTop(gap / 2); flow = area.removeFromTop(pill); }
            s.bounds = flow.removeFromLeft(w);
            flow.removeFromLeft(gap / 2);
        }
        if(!flow.isEmpty() || pane.pills_) area.removeFromTop(gap);
        return area.getY() + gap;
    }

    void LibraryPane::Sidebar::paint(juce::Graphics& g)
    {
        g.fillAll(ink(Ink::Surface));
        const int m = pane.mode();
        const int gap = m == 2 ? 10 : 8;
        const float body = textSize(TextRole::Body, m), small = textSize(TextRole::Small, m);
        for(const auto& s : pane.side_)
        {
            auto r = s.bounds;
            if(s.kind == Side::Heading)
            {
                g.setColour(ink(Ink::Muted));
                drawInked(g, s.label.toUpperCase(), r.withTrimmedLeft(4), juce::Font(juce::FontOptions(small, juce::Font::bold)));
                continue;
            }
            const bool on = s.kind == Side::Source ? pane.query_.source == s.value : std::count(pane.query_.tags.begin(), pane.query_.tags.end(), s.value) != 0;
            const auto color = s.kind == Side::Tag ? colorOf(pane.tagColor(s.value)) : juce::Colours::transparentBlack;
            if(pane.pills_)
            {
                const float radius = buttonRadius(float(r.getHeight()));
                g.setColour(on ? ink(Ink::Accent).withAlpha(.22f) : ink(Ink::Raised));
                g.fillRoundedRectangle(r.toFloat(), radius);
                if(on) { g.setColour(ink(Ink::Accent)); g.drawRoundedRectangle(r.toFloat().reduced(.5f), radius, 1.f); }
                else drawEdge(g, r.toFloat().reduced(.5f), radius);
                r = r.reduced(gap + 2, 0);
            }
            else
            {
                // The chosen entry: an inset highlight with the accent at its edge.
                if(on)
                {
                    g.setColour(ink(Ink::Accent).withAlpha(.18f)); g.fillRoundedRectangle(r.toFloat(), std::min(6.f, themeData().radius));
                    g.setColour(ink(Ink::Accent)); g.fillRoundedRectangle(r.toFloat().withWidth(3), 1.5f);
                }
                r = r.reduced(gap + 4, 0);
                if(s.count || s.kind == Side::Tag)
                {
                    g.setColour(ink(Ink::Muted));
                    drawInked(g, juce::String(s.count), r.removeFromRight(int(body * 2.5f)), juce::Font(juce::FontOptions(small)), juce::Justification::right);
                }
            }
            if(!color.isTransparent())
            {
                const float d = std::min(10.f, r.getHeight() * .3f);
                g.setColour(color); g.fillEllipse(float(r.getX()), r.toFloat().getCentreY() - d / 2, d, d);
            }
            if(!color.isTransparent() || (s.kind == Side::Tag && !pane.pills_)) r.removeFromLeft(14); // tags line up whether or not they have a color
            g.setColour(on ? ink(Ink::Accent) : ink(Ink::Text));
            drawInked(g, s.label, r, juce::Font(juce::FontOptions(body, on && !pane.pills_ ? juce::Font::bold : juce::Font::plain)),
                         pane.pills_ ? juce::Justification::horizontallyCentred : juce::Justification::left);
        }
    }

    void LibraryPane::Sidebar::mouseUp(const juce::MouseEvent& e)
    {
        if(e.mouseWasDraggedSinceMouseDown()) return;
        for(const auto& s : pane.side_)
        {
            if(!s.bounds.contains(e.getPosition())) continue;
            const auto value = s.value;
            if(s.kind == Side::Source) pane.showSource(value);
            else if(s.kind == Side::Tag) { if(menuGesture(e)) pane.tagMenu(value); else pane.toggleTag(value); }
            return;
        }
    }

    void LibraryPane::updateButtons()
    {
        const bool any = list_.getSelectedRow() >= 0;
        send_.setEnabled(any); pick_.setEnabled(any); part_.setEnabled(any); options_.setEnabled(any);
        const bool on = !auditioning_.id.empty();
        audition_.setButtonText(on ? "Stop" : "Audition"); audition_.setToggleState(on, juce::dontSendNotification);
        audition_.setEnabled(on || any);
        keep_.setVisible(on && mode_ == Mode::Play);
    }

    void LibraryPane::selectedRowsChanged(int row)
    {
        updateButtons();
        // Browsing while auditioning: each item chosen is tried in turn.
        if(const auto* item = at(row); item && !auditioning_.id.empty() && item->id != auditioning_.id) startAudition(row);
    }

    void LibraryPane::startAudition(int row)
    {
        const auto* item = at(row);
        if(!item) return;
        // One engine job at a time: what is asked while another loads would be lost.
        if(engine_.soundBusy()) { tell("Still loading; try again in a moment."); return; }
        SoundAction start; start.kind = SoundAction::AuditionStart; start.libraryId = item->id;
        if(item->kind == "sound")
        {
            start.part = auditionPart ? auditionPart() : 0;
            if(!start.part) { tell("Show a part in the Part view to audition a Sound in it."); return; }
        }
        engine_.soundAction(std::move(start));
        auditioning_ = {item->id, item->kind, item->kind == "sound" ? start.part : 0};
        list_.selectRow(row); list_.repaint(); // its row, marked
        tell("Auditioning " + text(item->name.empty() ? "Untitled" : item->name) + (item->kind == "sound" ? " in the part shown." : " in place of the open Performance."));
        updateButtons(); resized();
    }

    void LibraryPane::stopAudition(bool keep)
    {
        if(auditioning_.id.empty()) return;
        // shortcut: closing the pane while an audition still loads leaves it playing; queue the stop if that matters.
        if(engine_.soundBusy()) { tell("Still loading; stop again in a moment."); return; }
        SoundAction stop; stop.kind = SoundAction::AuditionStop; stop.asNew = keep;
        engine_.soundAction(std::move(stop));
        auditioning_ = {};
        list_.repaint();
        tell(keep ? "Kept." : "Put back as it was.");
        updateButtons(); resized();
    }

    bool LibraryPane::keyPressed(const juce::KeyPress& key)
    {
        if(key != juce::KeyPress::spaceKey || search_.hasKeyboardFocus(true)) return false;
        if(auditioning_.id.empty()) startAudition(list_.getSelectedRow()); else stopAudition();
        return true;
    }

    void LibraryPane::choose(int row)
    {
        const auto* item = at(row);
        if(!item) return;
        if(mode_ == Mode::Pick) { const auto id = item->id; stopAudition(); if(!auditioning_.id.empty()) return; if(onPicked) onPicked(id); return; }
        if(item->kind != "performance") return; // a Sound goes into a part: New part, or a part's Load Sound
        SoundAction open; open.kind = SoundAction::LoadFromLibrary; open.libraryId = item->id;
        engine_.soundAction(std::move(open));
        tell("Opening " + text(item->name));
        stale_ = true; // it is now the most recent
    }

    void LibraryPane::send(int row)
    {
        const auto* item = at(row);
        if(!item) return;
        actions_.request(sendPerformance, item->id, [safe = juce::Component::SafePointer<LibraryPane>(this)](const std::string& done) {
            if(safe && !done.empty()) safe->tell(text(done));
        });
    }

    juce::Rectangle<int> LibraryPane::starArea(int width, int height) const { return {width - std::max(height, 40), 0, std::max(height, 40), height}; }

    void LibraryPane::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected)
    {
        const auto* item = at(row);
        if(!item) return;
        const int m = mode();
        drawListRow(g, {0, 0, width, height}, row, colorOf(item->color));
        if(selected) { g.setColour(ink(Ink::Accent).withAlpha(.18f)); g.fillRect(0, 0, width, height); g.setColour(ink(Ink::Accent)); g.fillRect(0, 0, 3, height); }
        auto area = juce::Rectangle<int>(0, 0, width, height).reduced(24, compact_ ? 0 : 4);
        area.removeFromRight(starArea(width, height).getWidth());
        const float body = textSize(TextRole::Body, m), small = textSize(TextRole::Small, m);
        const auto name = text(item->name.empty() ? "Untitled" : item->name);
        juce::StringArray tags; for(const auto& t : item->tags) tags.add(text(t));
        // A locked library's item says so first: Save makes a new one.
        auto detail = tags.joinIntoString(separator) + (item->description.empty() ? "" : (tags.isEmpty() ? "" : "   ") + text(item->description));
        if(!item->library.empty()) detail = "Locked" + (detail.isEmpty() ? juce::String() : separator + detail);
        if(item->id == auditioning_.id) detail = "Auditioning" + (detail.isEmpty() ? juce::String() : separator + detail);
        const juce::Font bold(juce::FontOptions(body, juce::Font::bold));
        // On a row tinted with its color, the muted ink may not read: the text's own, a little lighter.
        const bool tinted = themeData().style.rows == ThemeStyle::Rows::Fill && !colorOf(item->color).isTransparent();
        const auto detailInk = tinted ? ink(Ink::Text).withAlpha(.78f) : ink(Ink::Muted);
        if(compact_)
        {
            // One line: the name, then the rest in muted text.
            const int w = std::min(area.getWidth(), int(juce::GlyphArrangement::getStringWidth(bold, name)) + 1);
            g.setColour(ink(Ink::Text)); g.setFont(bold);
            g.drawText(name, area.removeFromLeft(w), juce::Justification::centredLeft, true);
            area.removeFromLeft(16);
            g.setColour(detailInk); g.setFont(juce::FontOptions(small));
            g.drawText(detail, area, juce::Justification::centredLeft, true);
        }
        else
        {
            auto top = area.removeFromTop(area.getHeight() / 2 + 2);
            g.setColour(ink(Ink::Text)); g.setFont(bold);
            g.drawText(name, top, juce::Justification::bottomLeft, true);
            g.setColour(detailInk); g.setFont(juce::FontOptions(small));
            g.drawText(detail, area, juce::Justification::topLeft, true);
        }
        drawStar(g, starArea(width, height).toFloat().withSizeKeepingCentre(float(height), float(height)).reduced(float(height) * (compact_ ? .2f : .28f)), item->favorite);
    }

    void LibraryPane::listBoxItemClicked(int row, const juce::MouseEvent& e)
    {
        const auto* item = at(row);
        if(!item) return;
        if(menuGesture(e)) { itemMenu(row); return; }
        if(starArea(list_.getVisibleRowWidth(), list_.getRowHeight()).contains(e.getEventRelativeTo(list_.getComponentForRowNumber(row)).getPosition()))
        {
            engine_.edit([id = item->id, on = !item->favorite](Control& c) { c.libraryFavorite(id, on); });
            refresh();
            return;
        }
        if(!auditioning_.id.empty()) { if(item->id != auditioning_.id) startAudition(row); return; } // browsing while auditioning
        if(mode_ == Mode::Play) choose(row); // played from the list: one click opens it
    }

    void LibraryPane::listBoxItemDoubleClicked(int row, const juce::MouseEvent&) { if(mode_ == Mode::Pick) choose(row); }

    void LibraryPane::paint(juce::Graphics& g)
    {
        // The theme can change while the pane is open: colors set once would keep the old one.
        list_.setColour(juce::ListBox::backgroundColourId, ink(Ink::Surface));
        status_.setColour(juce::Label::textColourId, ink(Ink::Muted));
        g.fillAll(ink(Ink::Background));
    }

    void LibraryPane::resized()
    {
        const bool touch = mode() == 2;
        const int control = touch ? 44 : 30, gap = touch ? 10 : 8;
        list_.setRowHeight(compact_ ? (touch ? 44 : 28) : (touch ? 64 : 48));
        auto area = getLocalBounds().reduced(16, 12);
        auto bar = area.removeFromTop(control);
        if(mode_ == Mode::Play)
        {
            performances_.setBounds(bar.removeFromLeft(touch ? 150 : 120)); bar.removeFromLeft(gap);
            sounds_.setBounds(bar.removeFromLeft(touch ? 110 : 90)); bar.removeFromLeft(gap);
        }
        compactButton_.setBounds(bar.removeFromRight(touch ? 120 : 96)); bar.removeFromRight(gap);
        recent_.setBounds(bar.removeFromRight(touch ? 110 : 90)); bar.removeFromRight(gap);
        favorites_.setBounds(bar.removeFromRight(touch ? 130 : 110)); bar.removeFromRight(gap);
        search_.setBounds(bar);
        search_.setFont(juce::FontOptions(textSize(TextRole::Body, mode())));
        updatePlaceholder();
        area.removeFromTop(gap);
        auto bottom = area.removeFromBottom(control);
        area.removeFromBottom(gap);
        // Sources and tags in a column on the left; narrow windows give it a third.
        sideView_.setBounds(area.removeFromLeft(std::min(touch ? 240 : 200, area.getWidth() / 3)));
        rebuildSide();
        area.removeFromLeft(gap);
        list_.setBounds(area);
        if(mode_ == Mode::Pick) { pick_.setBounds(bottom.removeFromRight(110)); bottom.removeFromRight(gap); cancel_.setBounds(bottom.removeFromRight(110)); bottom.removeFromRight(gap); }
        else
        {
            if(send_.isVisible()) send_.setBounds(bottom.removeFromRight(touch ? 170 : 140));
            else if(part_.isVisible()) part_.setBounds(bottom.removeFromRight(touch ? 150 : 120));
            bottom.removeFromRight(gap); libraries_.setBounds(bottom.removeFromRight(touch ? 140 : 110));
            bottom.removeFromRight(gap);
        }
        options_.setBounds(bottom.removeFromRight(touch ? 130 : 100)); bottom.removeFromRight(gap);
        audition_.setBounds(bottom.removeFromRight(touch ? 130 : 100)); bottom.removeFromRight(gap);
        if(keep_.isVisible()) { keep_.setBounds(bottom.removeFromRight(touch ? 100 : 80)); bottom.removeFromRight(gap); }
        status_.setFont(juce::FontOptions(textSize(TextRole::Small, mode())));
        status_.setBounds(bottom);
    }

    // Everything for one item, by kind and by whether it is the user's own or a locked library's.
    void LibraryPane::itemMenu(int row)
    {
        const auto* found = at(row);
        if(!found) return;
        const auto item = *found;
        list_.selectRow(row);
        const bool own = item.library.empty();
        juce::PopupMenu menu, colors;
        if(mode_ == Mode::Pick) menu.addItem(1, "Choose");
        else if(item.kind == "performance")
        {
            menu.addItem(1, "Open");
            if(actions_.offered(sendPerformance)) menu.addItem(2, "Add to setlist");
        }
        else menu.addItem(3, "New part");
        menu.addItem(9, auditioning_.id == item.id ? "Stop audition" : "Audition");
        menu.addItem(4, item.favorite ? "Remove from favorites" : "Add to favorites");
        menu.addSeparator();
        menu.addItem(5, "Edit name, description and tags...", own);
        addColors(colors, item.color, 100, "None");
        menu.addSubMenu("Color", colors, own);
        menu.addItem(6, own ? "Duplicate" : "Copy into my own, to edit");
        menu.addItem(8, "Export...");
        menu.addSeparator();
        menu.addItem(7, "Delete...");
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(list_.getComponentForRowNumber(row)), [safe = juce::Component::SafePointer<LibraryPane>(this), item](int chosen) {
            if(!safe || chosen <= 0) return;
            // The item's row now: the list may have been refreshed, and reordered, while the menu was open.
            int row = -1;
            for(size_t i = 0; i < safe->result_.items.size(); ++i) if(safe->result_.items[i].id == item.id) row = int(i);
            if(row < 0) { safe->tell(text(item.name.empty() ? "Untitled" : item.name) + " is no longer in the list."); return; }
            std::string color;
            switch(chosen)
            {
                case 1: safe->choose(row); return;
                case 2: safe->send(row); return;
                case 3: safe->newPart(row); return;
                case 4: safe->engine_.edit([id = item.id, on = !item.favorite](Control& c) { c.libraryFavorite(id, on); }); safe->refresh(); return;
                case 5: safe->editItem(row); return;
                case 6: safe->copyItem(row); return;
                case 7: safe->deleteItem(row); return;
                case 8: safe->exportItem(row); return;
                case 9: if(safe->auditioning_.id == item.id) safe->stopAudition(); else safe->startAudition(row); return;
                default: break;
            }
            if(colorChosen(chosen, 100, color))
            {
                Control::PerformanceInfo info; info.name = item.name; info.description = item.description; info.tags = item.tags; info.color = color;
                const auto error = safe->engine_.editLibraryItem(item.id, info);
                safe->refresh();
                if(!error.empty()) safe->tell(text(error));
            }
        });
    }

    void LibraryPane::editItem(int row)
    {
        const auto* found = at(row);
        if(!found || !found->library.empty()) return;
        const auto item = *found;
        editor_ = std::make_unique<juce::AlertWindow>("Edit " + text(item.name.empty() ? "Untitled" : item.name), juce::String(), juce::MessageBoxIconType::NoIcon, this);
        editor_->addTextEditor("name", text(item.name), "Name");
        editor_->addTextEditor("description", text(item.description), "Description");
        editor_->addTextEditor("tags", tagText(item.tags), "Tags, separated by commas");
        editor_->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
        editor_->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        editor_->enterModalState(true, juce::ModalCallbackFunction::create([safe = juce::Component::SafePointer<LibraryPane>(this), item](int chosen) {
            if(!safe || !safe->editor_) return;
            const auto window = std::move(safe->editor_);
            if(chosen != 1) return;
            Control::PerformanceInfo info;
            info.name = window->getTextEditorContents("name").trim().toStdString(); info.description = window->getTextEditorContents("description").trim().toStdString(); info.color = item.color;
            for(auto t : juce::StringArray::fromTokens(window->getTextEditorContents("tags"), ",", "\""))
                if(t.trim().isNotEmpty() && std::find(info.tags.begin(), info.tags.end(), t.trim().toStdString()) == info.tags.end()) info.tags.push_back(t.trim().toStdString());
            const auto error = safe->engine_.editLibraryItem(item.id, info);
            safe->refresh();
            safe->tell(error.empty() ? text(info.name.empty() ? "Untitled" : info.name) + " saved." : text(error));
        }), false);
    }

    std::string LibraryPane::copyItem(int row)
    {
        const auto* item = at(row);
        if(!item) return {};
        const auto name = text(item->name.empty() ? "Untitled" : item->name);
        const bool own = item->library.empty();
        std::string copied;
        const auto error = engine_.copyLibraryItem(item->id, copied);
        refresh();
        tell(error.empty() ? (own ? name + " duplicated." : name + " copied into your own, to edit.") : text(error));
        return copied;
    }

    // An archive to take to another device: samples always go; a Performance's unedited parts'
    // Sounds too, and its edited parts' when asked.
    void LibraryPane::exportItem(int row)
    {
        const auto* found = at(row);
        if(!found) return;
        const auto item = *found;
        const auto name = text(item.name.empty() ? "Untitled" : item.name);
        chooser_ = std::make_unique<juce::FileChooser>("Export " + name, juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile(name + ".olib"), "*.olib");
        chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::warnAboutOverwriting,
            [safe = juce::Component::SafePointer<LibraryPane>(this), item](const juce::FileChooser& fc) {
                if(!safe || fc.getResult() == juce::File()) return;
                auto file = fc.getResult(); if(!file.hasFileExtension("olib")) file = file.withFileExtension("olib");
                showExport(safe->engine_, safe.getComponent(), "Export " + file.getFileNameWithoutExtension(), item.kind == "performance",
                    [safe, id = item.id, path = file.getFullPathName().toStdString()](bool editedSounds, const std::string& key) {
                        if(!safe) return;
                        // An existing file is replaced only once the new archive is written (Engine): Cancel, a refusal or a failed export keeps the old one.
                        SoundAction out; out.kind = SoundAction::ExportPerformance; out.libraryId = id; out.directory = path; out.asNew = editedSounds; out.signKey = key;
                        safe->engine_.soundAction(std::move(out));
                        safe->tell("Exporting..."); safe->stale_ = true; safe->awaiting_ = true;
                    });
            });
    }

    void LibraryPane::deleteItem(int row)
    {
        const auto* found = at(row);
        if(!found) return;
        const auto item = *found;
        const auto name = text(item.name.empty() ? "Untitled" : item.name);
        auto options = juce::MessageBoxOptions::makeOptionsOkCancel(juce::MessageBoxIconType::QuestionIcon, "Delete " + name + "?",
            item.library.empty() ? juce::String("It is removed from your library. Performances that used it keep their parts.")
                                 : juce::String("It is removed. Installing its library again brings it back."), "Delete", "Cancel", this);
        juce::AlertWindow::showAsync(options, [safe = juce::Component::SafePointer<LibraryPane>(this), id = item.id, name](int chosen) {
            if(!safe || chosen != 1) return;
            const auto error = safe->engine_.deleteLibraryItem(id);
            safe->refresh();
            safe->tell(error.empty() ? name + " deleted." : text(error));
        });
    }

    void LibraryPane::librariesMenu()
    {
        juce::PopupMenu menu;
        menu.addItem(1, "Install from a file...");
        menu.addItem(999, "Install from a repository folder...");
        // Tags hidden from the tag list, to show again.
        juce::PopupMenu hidden;
        const auto hiddenTags = result_.hiddenTags;
        for(size_t i = 0; i < hiddenTags.size(); ++i) hidden.addItem(1000 + int(i), text(hiddenTags[i]));
        menu.addSubMenu("Show a hidden tag", hidden, !hiddenTags.empty());
        // Plugins installed with archives (RFC 0006): what uses each, another platform's build, removal.
        auto plugins = engine_.installedPlugins();
        const auto stored = plugins.size();
        // And the open Performance's CLAP plugins installed some other way, for their other platforms' builds.
        if(engine_.connected())
            for(const auto& n : engine_.graph().topology.nodes)
                if(juce::File(n.plugin.path).hasFileExtension("clap") && std::none_of(plugins.begin(), plugins.end(), [&](const auto& p) { return p.id == n.plugin.id && p.version == n.plugin.version; }))
                    plugins.push_back({"clap", n.plugin.id, n.plugin.version, n.plugin.name, {}, 0, {}, true});
        juce::PopupMenu pluginMenu;
        for(size_t i = 0; i < plugins.size(); ++i)
        {
            const auto& p = plugins[i];
            juce::PopupMenu one;
            if(i == stored) pluginMenu.addSectionHeader("In this Performance");
            if(i >= stored)
            {
                one.addItem(100000 + int(i) * 2, "Add a build for another platform...");
                pluginMenu.addSubMenu(text(p.name.empty() ? p.id : p.name) + " " + text(p.version), one);
                continue;
            }
            juce::StringArray used;
            for(const auto& u : p.usedBy) used.add(text(u));
            one.addItem(-1, used.isEmpty() ? juce::String("Nothing in the library uses it") : "Used by " + used.joinIntoString(", "), false);
            juce::StringArray platforms;
            for(const auto& s : p.platforms) platforms.add(text(s));
            one.addItem(-1, "Builds: " + platforms.joinIntoString(", ") + "  (" + juce::File::descriptionOfSizeInBytes(int64_t(p.bytes)) + ")", false);
            one.addSeparator();
            one.addItem(100000 + int(i) * 2, "Add a build for another platform...");
            one.addItem(100001 + int(i) * 2, "Remove from this device...");
            pluginMenu.addSubMenu(text(p.name.empty() ? p.id : p.name) + " " + text(p.version), one);
        }
        menu.addSubMenu("Plugins", pluginMenu, !plugins.empty());
        const auto installed = engine_.libraries();
        if(!installed.empty()) menu.addSeparator();
        for(size_t i = 0; i < installed.size(); ++i)
            menu.addItem(int(i) + 2, "Uninstall " + text(installed[i].name) + " (" + juce::String(int(installed[i].items)) + (installed[i].items == 1 ? " item)" : " items)"));
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&libraries_), [safe = juce::Component::SafePointer<LibraryPane>(this), installed, hiddenTags, plugins](int chosen) {
            if(!safe || chosen <= 0) return;
            if(chosen == 1) { safe->install(); return; }
            if(chosen == 999) { safe->installFromRepository(); return; }
            if(chosen >= 100000)
            {
                const auto& plugin = plugins[size_t((chosen - 100000) / 2)];
                if(chosen % 2 == 0) safe->addPluginBuild(plugin); else safe->removePlugin(plugin);
                return;
            }
            if(chosen >= 1000)
            {
                const auto tag = hiddenTags[size_t(chosen - 1000)];
                safe->engine_.edit([tag, color = safe->tagColor(tag)](Control& c) { c.libraryTag(tag, false, color); });
                safe->refresh();
                return;
            }
            const auto library = installed[size_t(chosen - 2)];
            auto options = juce::MessageBoxOptions::makeOptionsOkCancel(juce::MessageBoxIconType::QuestionIcon, "Uninstall " + text(library.name) + "?",
                "Its Sounds and Performances are removed. Your Performances keep their parts and samples, and your own copies stay.", "Uninstall", "Cancel", safe.getComponent());
            juce::AlertWindow::showAsync(options, [safe, library](int ok) {
                if(!safe || ok != 1) return;
                const auto error = safe->engine_.uninstallLibrary(library.id);
                safe->refresh();
                safe->tell(error.empty() ? text(library.name) + " uninstalled." : text(error));
            });
        });
    }

    void LibraryPane::removePlugin(const Control::InstalledPlugin& plugin)
    {
        const auto name = text(plugin.name.empty() ? plugin.id : plugin.name) + " " + text(plugin.version);
        const auto count = int(plugin.usedBy.size());
        auto options = juce::MessageBoxOptions::makeOptionsOkCancel(juce::MessageBoxIconType::QuestionIcon, "Remove " + name + "?",
            count == 0 ? juce::String("Nothing in the library uses it. Every platform's build is removed.")
                       : juce::String(count) + (count == 1 ? " item uses it. It keeps its place and saved state and plays silence" : " items use it. They keep its place and saved state and play silence")
                             + " until it is installed again.", "Remove", "Cancel", this);
        juce::AlertWindow::showAsync(options, [safe = juce::Component::SafePointer<LibraryPane>(this), plugin, name](int chosen) {
            if(!safe || chosen != 1) return;
            const auto error = safe->engine_.removePlugin(plugin);
            safe->tell(error.empty() ? name + " removed." : text(error));
        });
    }

    // Another platform's build, such as the device's while on the desktop, so exports carry it.
    void LibraryPane::addPluginBuild(const Control::InstalledPlugin& plugin)
    {
        chooser_ = std::make_unique<juce::FileChooser>("A build of " + text(plugin.name) + " " + text(plugin.version) + " for another platform", juce::File(), "*.clap");
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles | juce::FileBrowserComponent::canSelectDirectories,
                              [safe = juce::Component::SafePointer<LibraryPane>(this), plugin](const juce::FileChooser& fc) {
            if(!safe || fc.getResult() == juce::File()) return;
            const auto path = fc.getResult().getFullPathName().toStdString();
            juce::PopupMenu platforms;
            const std::vector<std::string> all{"linux-arm64", "linux-x86_64", "macos-arm64", "macos-x86_64", "windows-x86_64"};
            for(size_t i = 0; i < all.size(); ++i) if(all[i] != platform()) platforms.addItem(int(i) + 1, "For " + text(all[i]));
            platforms.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&safe->libraries_), [safe, plugin, path, all](int chosen) {
                if(!safe || chosen <= 0) return;
                if(safe->engine_.soundBusy()) { safe->tell("The engine is busy; try again in a moment."); return; }
                // Copied on the engine's worker, so the screen keeps going.
                SoundAction add; add.kind = SoundAction::AddPluginBuild; add.format = plugin.format; add.pluginId = plugin.id; add.version = plugin.version;
                add.platform = all[size_t(chosen - 1)]; add.directory = path;
                safe->engine_.soundAction(std::move(add));
                safe->tell("Adding the " + text(all[size_t(chosen - 1)]) + " build...");
                safe->stale_ = true; safe->awaiting_ = true;
            });
        });
    }

    // An archive (a library, or an exported Performance): how to install it (locked, or as the
    // user's own, the default for an exported Performance) and which items, all ticked.
    void LibraryPane::install()
    {
        chooser_ = std::make_unique<juce::FileChooser>("Install a library, an exported Performance or Sound, or a plugin package", juce::File::getSpecialLocation(juce::File::userDocumentsDirectory), "*.olib");
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe = juce::Component::SafePointer<LibraryPane>(this)](const juce::FileChooser& fc) {
            if(!safe || fc.getResult() == juce::File()) return;
            const auto file = fc.getResult().getFullPathName().toStdString();
            Control::Archive archive;
            if(const auto error = safe->engine_.archiveContents(file, archive); !error.empty())
            {
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Not installed", text(error));
                return;
            }
            auto* content = new InstallChoices(archive);
            juce::DialogWindow::LaunchOptions options;
            options.dialogTitle = "Install " + text(archive.name);
            options.content.setOwned(content); options.componentToCentreAround = safe.getComponent(); options.useNativeTitleBar = true; options.resizable = false;
            options.dialogBackgroundColour = ink(Ink::Background); // JUCE's default is light grey, under the theme's text
            auto* dialog = options.launchAsync();
            content->cancel.onClick = [dialog] { dialog->exitModalState(0); };
            content->go.onClick = [safe, dialog, content, file] {
                SoundAction install; install.kind = SoundAction::InstallArchive; install.directory = file; install.asNew = content->locked.getToggleState(); install.acceptCode = content->asks;
                for(int i = 0; i < content->items.size(); ++i) if(content->items[i]->getToggleState()) install.items.push_back(content->ids[size_t(i)]);
                if(install.items.empty() && !content->pluginPackage) { juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Install", "Choose at least one item."); return; }
                if(install.items.size() == content->ids.size()) install.items.clear(); // all of it
                dialog->exitModalState(0);
                if(!safe) return;
                safe->engine_.soundAction(std::move(install));
                safe->tell("Installing...");
                safe->stale_ = true; safe->awaiting_ = true;
            };
        });
    }

    void LibraryPane::installFromRepository()
    {
        chooser_ = std::make_unique<juce::FileChooser>("A package repository: the folder holding repository.json", juce::File());
        chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [safe = juce::Component::SafePointer<LibraryPane>(this)](const juce::FileChooser& fc) {
            if(safe && fc.getResult() != juce::File()) safe->listRepository(fc.getResult().getFullPathName().toStdString());
        });
    }

    void LibraryPane::awaitPackageJob(uint64_t job, std::function<void(const Control::PackageJob&)> then)
    {
        juce::Timer::callAfterDelay(100, [safe = juce::Component::SafePointer<LibraryPane>(this), job, then = std::move(then)]() mutable {
            if(!safe) return;
            Control::PackageJob state;
            if(const auto error = safe->engine_.packages([&](Control& c) { state = c.packageJob(job); }); !error.empty()) { safe->tell(text(error)); return; }
            if(state.state == "running") { safe->awaitPackageJob(job, std::move(then)); return; }
            then(state);
        });
    }

    // What it offers, once its root, index key and index verify; a root key not trusted yet is
    // offered by its fingerprint, and trusted for repositories only when the user says so.
    void LibraryPane::listRepository(const std::string& folder)
    {
        uint64_t job = 0;
        if(const auto error = engine_.packages([&](Control& c) { job = c.listRepository(folder); }); !error.empty()) { tell(text(error)); return; }
        tell("Reading the repository...");
        awaitPackageJob(job, [safe = juce::Component::SafePointer<LibraryPane>(this), folder](const Control::PackageJob& listing) {
            if(!safe) return;
            if(listing.state == "failed" && !listing.rootKey.empty() && listing.error.find("not a repository this device trusts") != std::string::npos)
            {
                auto options = juce::MessageBoxOptions::makeOptionsOkCancel(juce::MessageBoxIconType::QuestionIcon, "Trust this repository?",
                    "Its root key's fingerprint is " + fingerprint(signing::keyId(listing.rootKey)) + ". Trust it only if its publisher gave you that fingerprint.", "Trust", "Cancel", safe.getComponent());
                juce::AlertWindow::showAsync(options, [safe, folder, key = listing.rootKey](int chosen) {
                    if(!safe || chosen != 1) return;
                    const auto error = safe->engine_.packages([&](Control& c) { c.setTrustedKey({"", key, "Repository " + fingerprint(signing::keyId(key)).toStdString(), {"repositories"}}); });
                    if(error.empty()) safe->listRepository(folder); else safe->tell(text(error));
                });
                return;
            }
            if(listing.state == "failed") { safe->tell(""); juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Not read", text(listing.error)); return; }
            safe->tell(juce::String(int(listing.packages.size())) + (listing.packages.size() == 1 ? " package in " : " packages in ") + text(listing.name) + ".");
            auto* content = new RepositoryChoices(listing);
            juce::DialogWindow::LaunchOptions options;
            options.dialogTitle = "Install from " + text(listing.name);
            options.content.setOwned(content); options.componentToCentreAround = safe.getComponent(); options.useNativeTitleBar = true; options.resizable = false;
            options.dialogBackgroundColour = ink(Ink::Background);
            auto* dialog = options.launchAsync();
            content->close.onClick = [dialog] { dialog->exitModalState(0); };
            content->onInstall = [safe, dialog, folder](const std::string& id, const std::string& version) {
                dialog->exitModalState(0);
                if(safe) safe->installPackage(folder, id, version, false);
            };
        });
    }

    void LibraryPane::installPackage(const std::string& folder, const std::string& id, const std::string& version, bool acceptCode)
    {
        uint64_t job = 0;
        if(const auto error = engine_.packages([&](Control& c) { job = c.installFromRepository(folder, id, version, acceptCode); }); !error.empty()) { tell(text(error)); return; }
        tell("Installing " + text(id) + " " + text(version) + "...");
        awaitPackageJob(job, [safe = juce::Component::SafePointer<LibraryPane>(this), folder, id, version](const Control::PackageJob& done) {
            if(!safe) return;
            // Code from an unknown source asks first, as installing an archive does.
            if(done.state == "failed" && done.error.find("unknown source") != std::string::npos)
            {
                auto options = juce::MessageBoxOptions::makeOptionsOkCancel(juce::MessageBoxIconType::WarningIcon, "Install anyway?", text(done.error), "Install anyway", "Cancel", safe.getComponent());
                juce::AlertWindow::showAsync(options, [safe, folder, id, version](int chosen) { if(safe && chosen == 1) safe->installPackage(folder, id, version, true); });
                safe->tell("");
                return;
            }
            if(done.state == "failed") { safe->tell(""); juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Not installed", text(done.error)); return; }
            juce::String said = text(done.package) + " " + text(done.version) + " installed.";
            for(const auto& n : done.installed.notes) said << " " << text(n) << ".";
            safe->tell(said);
            safe->stale_ = true; // its items, listed again
        });
    }

    std::string LibraryPane::tagColor(const std::string& tag) const
    {
        for(const auto& [t, color] : result_.tagColors) if(t == tag) return color;
        return {};
    }

    // A tag as the user sees it: hidden from the tag list, and its color. A hidden tag chosen as
    // a filter stops filtering.
    void LibraryPane::tagMenu(const std::string& tag)
    {
        juce::PopupMenu menu, colors;
        menu.addItem(1, "Hide from the tag list");
        addColors(colors, tagColor(tag), 100, "None");
        menu.addSubMenu("Color", colors);
        menu.showMenuAsync(juce::PopupMenu::Options(), [safe = juce::Component::SafePointer<LibraryPane>(this), tag](int chosen) {
            if(!safe || chosen <= 0) return;
            const auto color = safe->tagColor(tag);
            std::string picked;
            if(chosen == 1)
            {
                std::erase(safe->query_.tags, tag);
                safe->engine_.edit([tag, color](Control& c) { c.libraryTag(tag, true, color); });
            }
            else if(colorChosen(chosen, 100, picked)) safe->engine_.edit([tag, picked](Control& c) { c.libraryTag(tag, false, picked); });
            safe->refresh();
        });
    }
}
