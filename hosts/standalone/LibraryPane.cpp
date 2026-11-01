// SPDX-License-Identifier: AGPL-3.0-or-later
#include "LibraryPane.h"
#include "PerformanceColors.h"

namespace oscairn::standalone
{
    namespace
    {
        juce::String text(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }
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
            juce::Path star;
            star.addStar(r.getCentre(), 5, r.getWidth() * .2f, r.getWidth() * .45f, 0);
            g.setColour(on ? ink(Ink::Accent) : ink(Ink::Muted).withAlpha(.6f));
            if(on) g.fillPath(star); else g.strokePath(star, juce::PathStrokeType(1.2f));
        }
        // Text centered on its capital letters, so it sits in the middle of a row or pill whatever
        // the font's ascent and descent.
        void drawCentered(juce::Graphics& g, const juce::String& s, juce::Rectangle<int> r, const juce::Font& font, juce::Justification h = juce::Justification::left)
        {
            juce::GlyphArrangement caps; caps.addLineOfText(font, "H", 0, 0);
            const float capHeight = -caps.getBoundingBox(0, 1, true).getY();
            const float baseline = r.toFloat().getCentreY() + capHeight / 2;
            g.setFont(font);
            g.drawText(s, juce::Rectangle<float>(float(r.getX()), baseline - font.getAscent(), float(r.getWidth()), font.getHeight()), h.getOnlyHorizontalFlags() | juce::Justification::top, true);
        }
        juce::String tagText(const std::vector<std::string>& tags)
        {
            juce::StringArray out; for(const auto& t : tags) out.add(text(t));
            return out.joinIntoString(", ");
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
        favorites_.setClickingTogglesState(true);
        favorites_.onClick = [this] { query_.favorites = favorites_.getToggleState(); refresh(); };
        recent_.setClickingTogglesState(true);
        recent_.onClick = [this] { query_.recent = recent_.getToggleState(); refresh(); };
        compactButton_.setClickingTogglesState(true);
        compactButton_.setTooltip("One line a row, to see more at once");
        compactButton_.onClick = [this] { setCompact(compactButton_.getToggleState()); };
        list_.setColour(juce::ListBox::backgroundColourId, ink(Ink::Surface));
        list_.setOutlineThickness(0);
        list_.setRowSelectedOnMouseDown(false); // clicks come on release, so a long press can open the menu
        sideView_.setViewedComponent(&sidebar_, false);
        sideView_.setScrollBarsShown(true, false);
        send_.onClick = [this] { send(list_.getSelectedRow()); };
        cancel_.onClick = [this] { if(onPicked) onPicked({}); };
        pick_.onClick = [this] { choose(list_.getSelectedRow()); };
        options_.onClick = [this] { itemMenu(list_.getSelectedRow()); };
        options_.setTooltip("Everything for the selected item: edit, color, copy, delete. A right click or a long press on a row does the same");
        libraries_.onClick = [this] { librariesMenu(); };
        libraries_.setTooltip("Install a library or an exported Performance, uninstall a library, or show a hidden tag");
        status_.setColour(juce::Label::textColourId, ink(Ink::Muted));
        for(auto* c : std::initializer_list<juce::Component*>{&search_, &favorites_, &recent_, &compactButton_, &sideView_, &list_, &options_, &status_}) addAndMakeVisible(c);
        for(auto* c : std::initializer_list<juce::Component*>{&send_, &part_, &cancel_, &pick_, &performances_, &sounds_, &libraries_}) addChildComponent(c);
        performances_.setVisible(mode_ == Mode::Play); sounds_.setVisible(mode_ == Mode::Play); libraries_.setVisible(mode_ == Mode::Play);
        cancel_.setVisible(mode_ == Mode::Pick); pick_.setVisible(mode_ == Mode::Pick);
        if(const auto f = viewPreference(); f.existsAsFile())
        {
            const auto words = juce::StringArray::fromTokens(f.loadFileAsString(), false);
            compact_ = words.contains("compact"); pills_ = words.contains("pills");
        }
        compactButton_.setToggleState(compact_, juce::dontSendNotification);
        sidebar_.pills.setToggleState(pills_, juce::dontSendNotification);
        showKind(kind);
        startTimer(300);
    }

    LibraryPane::~LibraryPane() { list_.setModel(nullptr); }

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
        sidebar_.pills.setToggleState(pills, juce::dontSendNotification);
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
        // Saving into the library happens on the engine's worker; look again once it is done.
        if(!engine_.controlReady()) { stale_ = true; return; }
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
        pills.setClickingTogglesState(true);
        pills.setTooltip("Show the libraries and tags as pills, to see more at once, or as a list");
        pills.onClick = [this] { pane.setPills(pills.getToggleState()); };
        addAndMakeVisible(pills);
    }

    int LibraryPane::Sidebar::layout(int width)
    {
        const int m = pane.mode();
        const bool touch = m == 2;
        const int gap = touch ? 10 : 8, pad = gap + 2;
        const float body = textSize(TextRole::Body, m), small = textSize(TextRole::Small, m);
        const int row = std::max(touch ? 44 : 0, int(body) + 4 + 2 * (gap - 2)), pill = std::max(touch ? 40 : 0, int(body) + 4 + gap);
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
                if(first) { const int w = touch ? 76 : 58; pills.setBounds(s.bounds.withLeft(s.bounds.getRight() - w).withSizeKeepingCentre(w, int(small) + 10)); }
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
                drawCentered(g, s.label.toUpperCase(), r.withTrimmedLeft(4), juce::Font(juce::FontOptions(small, juce::Font::bold)));
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
                    drawCentered(g, juce::String(s.count), r.removeFromRight(int(body * 2.5f)), juce::Font(juce::FontOptions(small)), juce::Justification::right);
                }
            }
            if(!color.isTransparent())
            {
                const float d = std::min(10.f, r.getHeight() * .3f);
                g.setColour(color); g.fillEllipse(float(r.getX()), r.toFloat().getCentreY() - d / 2, d, d);
            }
            if(!color.isTransparent() || (s.kind == Side::Tag && !pane.pills_)) r.removeFromLeft(14); // tags line up whether or not they have a color
            g.setColour(on ? ink(Ink::Accent) : ink(Ink::Text));
            drawCentered(g, s.label, r, juce::Font(juce::FontOptions(body, on && !pane.pills_ ? juce::Font::bold : juce::Font::plain)));
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
    }

    void LibraryPane::choose(int row)
    {
        const auto* item = at(row);
        if(!item) return;
        if(mode_ == Mode::Pick) { if(onPicked) onPicked(item->id); return; }
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
        auto detail = tags.joinIntoString("  ·  ") + (item->description.empty() ? "" : (tags.isEmpty() ? "" : "   ") + text(item->description));
        if(!item->library.empty()) detail = "Locked" + (detail.isEmpty() ? juce::String() : "  ·  " + detail);
        const juce::Font bold(juce::FontOptions(body, juce::Font::bold));
        if(compact_)
        {
            // One line: the name, then the rest in muted text.
            const int w = std::min(area.getWidth(), int(juce::GlyphArrangement::getStringWidth(bold, name)) + 1);
            g.setColour(ink(Ink::Text)); g.setFont(bold);
            g.drawText(name, area.removeFromLeft(w), juce::Justification::centredLeft, true);
            area.removeFromLeft(16);
            g.setColour(ink(Ink::Muted)); g.setFont(juce::FontOptions(small));
            g.drawText(detail, area, juce::Justification::centredLeft, true);
        }
        else
        {
            auto top = area.removeFromTop(area.getHeight() / 2 + 2);
            g.setColour(ink(Ink::Text)); g.setFont(bold);
            g.drawText(name, top, juce::Justification::bottomLeft, true);
            g.setColour(ink(Ink::Muted)); g.setFont(juce::FontOptions(small));
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
        if(mode_ == Mode::Play) choose(row); // played from the list: one click opens it
    }

    void LibraryPane::listBoxItemDoubleClicked(int row, const juce::MouseEvent&) { if(mode_ == Mode::Pick) choose(row); }

    void LibraryPane::paint(juce::Graphics& g) { g.fillAll(ink(Ink::Background)); }

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
        menu.addItem(4, item.favorite ? "Remove from favorites" : "Add to favorites");
        menu.addSeparator();
        menu.addItem(5, "Edit name, description and tags...", own);
        addColors(colors, item.color, 100, "None");
        menu.addSubMenu("Color", colors, own);
        menu.addItem(6, own ? "Duplicate" : "Copy into my own, to edit");
        menu.addItem(8, "Export...");
        menu.addSeparator();
        menu.addItem(7, "Delete...");
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(list_.getComponentForRowNumber(row)), [safe = juce::Component::SafePointer<LibraryPane>(this), item, row](int chosen) {
            if(!safe || chosen <= 0) return;
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
                if(file.existsAsFile() && !file.deleteFile()) { safe->tell("Cannot replace " + file.getFullPathName()); return; }
                auto start = [safe, id = item.id, path = file.getFullPathName().toStdString()](bool editedSounds) {
                    if(!safe) return;
                    SoundAction out; out.kind = SoundAction::ExportPerformance; out.libraryId = id; out.directory = path; out.asNew = editedSounds;
                    safe->engine_.soundAction(std::move(out));
                    safe->tell("Exporting..."); safe->stale_ = true; safe->awaiting_ = true;
                };
                if(item.kind != "performance") { start(false); return; }
                auto options = juce::MessageBoxOptions().withIconType(juce::MessageBoxIconType::QuestionIcon).withTitle("Export " + file.getFileNameWithoutExtension())
                    .withMessage("Samples and the Sounds of parts unedited since they were loaded always go. The Sounds of edited parts can go too, as the library has them.")
                    .withButton("Export").withButton("With edited parts' Sounds").withButton("Cancel").withAssociatedComponent(safe.getComponent());
                juce::AlertWindow::showAsync(options, [start](int chosen) { if(chosen != 0) start(chosen == 2); }); // 1: Export, 2: with edited Sounds, 0: Cancel
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
        // Tags hidden from the tag list, to show again.
        juce::PopupMenu hidden;
        const auto hiddenTags = result_.hiddenTags;
        for(size_t i = 0; i < hiddenTags.size(); ++i) hidden.addItem(1000 + int(i), text(hiddenTags[i]));
        menu.addSubMenu("Show a hidden tag", hidden, !hiddenTags.empty());
        const auto installed = engine_.libraries();
        if(!installed.empty()) menu.addSeparator();
        for(size_t i = 0; i < installed.size(); ++i)
            menu.addItem(int(i) + 2, "Uninstall " + text(installed[i].name) + " (" + juce::String(int(installed[i].items)) + (installed[i].items == 1 ? " item)" : " items)"));
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&libraries_), [safe = juce::Component::SafePointer<LibraryPane>(this), installed, hiddenTags](int chosen) {
            if(!safe || chosen <= 0) return;
            if(chosen == 1) { safe->install(); return; }
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
                safe->tell("Installing...");
                safe->stale_ = true; safe->awaiting_ = true;
            };
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
