// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Recording.h"

namespace oscairn::standalone
{
    namespace
    {
        juce::File captureFile(const juce::File& configuration) { return configuration.getChildFile("capture"); }
        juce::String text(const std::string& s) { return juce::String::fromUTF8(s.c_str()); }
        juce::String clockText(double seconds)
        {
            const auto s = juce::roundToInt(std::floor(std::max(0.0, seconds)));
            return juce::String(s / 60) + ":" + juce::String(s % 60).paddedLeft('0', 2);
        }
        bool live(const Control::Take& t) { return t.state == "recording" || t.state == "waiting" || t.state == "finishing"; }
        // "2026-11-01 21-04-17 Open Road": the time a take's folder is named by, and the rest.
        bool stamped(const juce::String& folder) { return folder.length() >= 19 && folder.substring(0, 19).containsOnly("0123456789- ") && folder[4] == '-' && folder[10] == ' '; }
        constexpr double keepChoices[] = {0, 60, 120, 300, 600};
    }

    double readCaptureSeconds(const juce::File& configuration, const Profile& profile)
    {
        const auto saved = captureFile(configuration).loadFileAsString().trim();
        if(saved.isNotEmpty() && saved.containsOnly("0123456789.")) return std::clamp(saved.getDoubleValue(), 0.0, 3600.0);
        return profile.compute.memory > 0 && profile.compute.memory <= 2048 ? 120 : 600; // RFC 0020's proposed defaults
    }

    void writeCaptureSeconds(const juce::File& configuration, double seconds)
    {
        configuration.createDirectory();
        captureFile(configuration).replaceWithText(juce::String(seconds, 0) + "\n");
    }

    RecordButtons::RecordButtons(Engine& engine) : engine_(engine)
    {
        for(auto* b : {&record_, &capture_})
        {
            b->setColour(juce::TextButton::buttonColourId, juce::Colours::transparentBlack); // icons alone, as the top bar's
            addAndMakeVisible(b);
        }
        record_.onClick = [this] { record(); };
        capture_.onClick = [this] { capture(); };
        timerCallback();
        startTimerHz(4);
    }

    int RecordButtons::side() const { return std::min(getHeight(), 44); }

    int RecordButtons::preferredWidth() const
    {
        const juce::Font small{juce::FontOptions(textSize(TextRole::Small, std::max(1, desktopMode(*this))))};
        const int buttons = 2 * side() + 4;
        return message_.isEmpty() ? buttons : buttons + 8 + juce::GlyphArrangement::getStringWidthInt(small, message_) + 4;
    }

    void RecordButtons::record()
    {
        const auto error = engine_.record(recording_ == 0);
        if(!error.empty()) { message_ = text(error); danger_ = true; quiet_ = 16; }
        timerCallback();
    }

    void RecordButtons::capture()
    {
        const auto seconds = std::min(captureSeconds, engine_.captureSeconds());
        const auto error = engine_.capture(seconds);
        message_ = error.empty() ? juce::String("Captured to Recordings") : text(error);
        danger_ = !error.empty();
        quiet_ = 12; // three seconds
        if(auto* p = getParentComponent()) p->resized();
        repaint();
    }

    void RecordButtons::timerCallback()
    {
        // Nothing while hidden; the constructor's call, before the timer, fills it.
        if(!isShowing() && isTimerRunning()) return;
        const auto before = message_;
        recording_ = engine_.recordingTake();
        record_.setToggleState(recording_ != 0, juce::dontSendNotification);
        record_.setTooltip(recording_ ? "Stop recording" : "Record Main and the notes played");
        const bool capturing = engine_.captureSeconds() > 0;
        capture_.setEnabled(capturing);
        capture_.setTooltip(capturing ? "Capture: save the last " + clockText(std::min(captureSeconds, engine_.captureSeconds())) + " played" : "Capture is off: turn it on in Recordings");
        if(quiet_ > 0) --quiet_;
        else if(recording_)
        {
            message_.clear();
            for(const auto& t : engine_.takes())
                if(t.take == recording_)
                    message_ = "REC " + clockText(t.sampleRate > 0 ? double(t.frames) / t.sampleRate : 0) + (t.behind ? juce::String::fromUTF8("  \xC2\xB7  disk slow") : juce::String());
            danger_ = true;
        }
        else message_.clear();
        if(message_ != before) { if(auto* p = getParentComponent()) p->resized(); }
        repaint();
    }

    void RecordButtons::resized()
    {
        const int s = side(), top = (getHeight() - s) / 2;
        record_.setBounds(0, top, s, s);
        capture_.setBounds(s + 4, top, s, s);
    }

    void RecordButtons::paint(juce::Graphics& g)
    {
        if(message_.isEmpty()) return;
        const juce::Font small{juce::FontOptions(textSize(TextRole::Small, std::max(1, desktopMode(*this))))};
        g.setColour(danger_ ? ink(Ink::Danger) : ink(Ink::Muted));
        drawInked(g, message_, getLocalBounds().withTrimmedLeft(capture_.getRight() + 8), small);
    }

    RecordingsPane::RecordingsPane(Engine& engine, const juce::File& configuration, bool browser) : engine_(engine), configuration_(configuration)
    {
        setTitle("Recordings");
        addAndMakeVisible(keepLabel_);
        keep_.addItemList({"Off", "1 minute", "2 minutes", "5 minutes", "10 minutes"}, 1);
        int chosen = 0;
        for(int i = 0; i < int(std::size(keepChoices)); ++i) if(std::abs(keepChoices[i] - engine_.captureSeconds()) < .5) chosen = i + 1;
        if(chosen) keep_.setSelectedId(chosen, juce::dontSendNotification);
        else keep_.setText(clockText(engine_.captureSeconds()), juce::dontSendNotification); // a length set elsewhere
        keep_.onChange = [this] { if(const auto id = keep_.getSelectedId(); id > 0) setCaptureSeconds(keepChoices[id - 1]); };
        keep_.setTooltip("How much of what was just played Capture can save. Kept in memory, never written unless captured.");
        addAndMakeVisible(keep_);
        addAndMakeVisible(list_);
        list_.setColour(juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
#if JUCE_MAC
        reveal_.setButtonText("Show in Finder");
#elif JUCE_WINDOWS
        reveal_.setButtonText("Show in Explorer");
#else
        reveal_.setButtonText("Show in Folder");
#endif
        play_.onClick = [this] { play(); };
        rename_.onClick = [this] { askRename(); };
        delete_.onClick = [this] { askRemove(); };
        reveal_.onClick = [this] { reveal(); };
        for(auto* b : {&play_, &rename_, &delete_}) addAndMakeVisible(b);
        addChildComponent(reveal_);
        reveal_.setVisible(browser); // a computer's file browser; a device has none
        refresh();
        startTimer(500);
    }

    void RecordingsPane::refresh()
    {
        const auto chosen = at(selected()) ? at(selected())->take : 0;
        const auto& all = engine_.takes();
        auto next = std::vector<Control::Take>(all.rbegin(), all.rend()); // newest first
        const bool same = next.size() == takes_.size() && std::equal(next.begin(), next.end(), takes_.begin(), [](const auto& a, const auto& b) {
            return a.take == b.take && a.folder == b.folder && a.state == b.state && a.frames == b.frames && a.playing == b.playing; });
        if(same) return;
        takes_ = std::move(next);
        list_.updateContent();
        for(size_t i = 0; i < takes_.size(); ++i) if(takes_[i].take == chosen) list_.selectRow(int(i), false, true);
        updateButtons();
        list_.repaint();
        repaint();
    }

    void RecordingsPane::select(int row) { list_.selectRow(row); updateButtons(); }

    void RecordingsPane::updateButtons()
    {
        const auto* t = at(selected());
        const bool closed = t && !live(*t);
        play_.setButtonText(t && t->playing ? "Stop" : "Play");
        play_.setEnabled(closed && !t->files.empty());
        for(auto* b : {&rename_, &delete_}) b->setEnabled(closed);
        reveal_.setEnabled(t != nullptr);
    }

    void RecordingsPane::play()
    {
        const auto* t = at(selected());
        if(!t) return;
        const bool stop = t->playing;
        const auto name = nameOf(*t);
        const auto error = engine_.playTake(stop ? 0 : t->take);
        engine_.refreshTakes();
        refresh();
        tell(!error.empty() ? text(error) : stop ? juce::String() : "Playing " + name + ".");
    }

    void RecordingsPane::rename(const juce::String& name)
    {
        const auto* t = at(selected());
        if(!t || name.trim().isEmpty()) return;
        const auto error = engine_.renameTake(t->take, name.trim().toStdString());
        engine_.refreshTakes();
        refresh();
        tell(error.empty() ? "Renamed to " + name.trim() + "." : text(error));
    }

    void RecordingsPane::askRename()
    {
        const auto* t = at(selected());
        if(!t) return;
        dialog_ = std::make_unique<juce::AlertWindow>("Rename " + nameOf(*t), juce::String(), juce::MessageBoxIconType::NoIcon, this);
        dialog_->addTextEditor("name", nameOf(*t), "Name");
        dialog_->addButton("Rename", 1, juce::KeyPress(juce::KeyPress::returnKey));
        dialog_->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        dialog_->enterModalState(true, juce::ModalCallbackFunction::create([safe = juce::Component::SafePointer<RecordingsPane>(this)](int chosen) {
            if(!safe || !safe->dialog_) return;
            const auto window = std::move(safe->dialog_);
            if(chosen == 1) safe->rename(window->getTextEditorContents("name"));
        }));
    }

    void RecordingsPane::remove()
    {
        const auto* t = at(selected());
        if(!t) return;
        const auto name = nameOf(*t);
        const auto error = engine_.deleteTake(t->take);
        engine_.refreshTakes();
        refresh();
        tell(error.empty() ? name + " deleted." : text(error));
    }

    void RecordingsPane::askRemove()
    {
        const auto* t = at(selected());
        if(!t) return;
        auto options = juce::MessageBoxOptions::makeOptionsOkCancel(juce::MessageBoxIconType::QuestionIcon, "Delete " + nameOf(*t) + "?",
            "Its files are removed from your recordings.", "Delete", "Cancel", this);
        juce::AlertWindow::showAsync(options, [safe = juce::Component::SafePointer<RecordingsPane>(this), take = t->take](int chosen) {
            if(!safe || chosen != 1) return;
            for(size_t i = 0; i < safe->takes_.size(); ++i) if(safe->takes_[i].take == take) { safe->list_.selectRow(int(i)); safe->remove(); }
        });
    }

    void RecordingsPane::reveal()
    {
        if(const auto* t = at(selected())) juce::File(text(t->folder)).revealToUser();
    }

    void RecordingsPane::setCaptureSeconds(double seconds)
    {
        const auto error = engine_.setCapture(seconds);
        if(error.empty()) writeCaptureSeconds(configuration_, seconds);
        tell(!error.empty() ? text(error) : seconds > 0 ? "Capture keeps the last " + clockText(seconds) + " in memory." : juce::String("Capture is off."));
    }

    juce::String RecordingsPane::nameOf(const Control::Take& t)
    {
        auto folder = juce::File(text(t.folder)).getFileName();
        if(stamped(folder)) folder = folder.substring(19).trim();
        return folder.isEmpty() ? juce::String("Untitled") : folder;
    }

    juce::String RecordingsPane::dateOf(const Control::Take& t)
    {
        const juce::File folder(text(t.folder));
        const auto name = folder.getFileName();
        juce::Time when = folder.getCreationTime();
        if(stamped(name))
            when = juce::Time(name.substring(0, 4).getIntValue(), name.substring(5, 7).getIntValue() - 1, name.substring(8, 10).getIntValue(),
                              name.substring(11, 13).getIntValue(), name.substring(14, 16).getIntValue(), name.substring(17, 19).getIntValue(), 0, true);
        return when.formatted("%b ") + juce::String(when.getDayOfMonth()) + when.formatted(", %Y, %H:%M");
    }

    juce::String RecordingsPane::lengthOf(const Control::Take& t)
    {
        return t.sampleRate > 0 ? clockText(double(t.frames) / t.sampleRate) : juce::String("?");
    }

    void RecordingsPane::paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected)
    {
        const auto* t = at(row);
        if(!t) return;
        const int m = mode();
        drawListRow(g, {0, 0, width, height}, row, juce::Colours::transparentBlack);
        if(selected) { g.setColour(ink(Ink::Accent).withAlpha(.18f)); g.fillRect(0, 0, width, height); g.setColour(ink(Ink::Accent)); g.fillRect(0, 0, 3, height); }
        auto area = juce::Rectangle<int>(0, 0, width, height).reduced(24, height / 6); // the two lines close together, the pair centered
        const juce::Font bold(juce::FontOptions(textSize(TextRole::Body, m), juce::Font::bold)), small(juce::FontOptions(textSize(TextRole::Small, m)));
        // The length at the right, as a column; the name over its date and state.
        const auto separator = juce::String::fromUTF8("  \xC2\xB7  ");
        const auto length = area.removeFromRight(juce::GlyphArrangement::getStringWidthInt(bold, "000:00") + 8);
        g.setColour(ink(Ink::Text)); drawInked(g, lengthOf(*t), length, bold, juce::Justification::right);
        auto detail = dateOf(*t);
        if(t->playing) detail = "Playing" + separator + detail;
        if(t->state == "failed") detail += separator + "Failed" + (t->error.empty() ? juce::String() : ": " + text(t->error));
        else if(live(*t)) detail += separator + (t->state == "waiting" ? "Waiting" : t->state == "finishing" ? "Saving" : "Recording");
        if(!t->overruns.empty()) detail += separator + juce::String(int(t->overruns.size())) + (t->overruns.size() == 1 ? " gap" : " gaps") + " where the disk fell behind";
        auto top = area.removeFromTop(area.getHeight() / 2);
        g.setColour(ink(Ink::Text)); drawInked(g, nameOf(*t), top, bold);
        g.setColour(t->state == "failed" ? ink(Ink::Danger) : ink(Ink::Muted)); drawInked(g, detail, area, small);
    }

    void RecordingsPane::resized()
    {
        const bool touch = mode() == 2;
        const int control = touch ? 44 : 30, gap = touch ? 10 : 8;
        list_.setRowHeight(touch ? 64 : 48);
        auto area = getLocalBounds().reduced(16, 12);
        auto bar = area.removeFromTop(control);
        keepLabel_.setFont(juce::FontOptions(textSize(TextRole::Body, mode())));
        keepLabel_.setBounds(bar.removeFromLeft(juce::GlyphArrangement::getStringWidthInt(keepLabel_.getFont(), keepLabel_.getText()) + 16));
        keep_.setBounds(bar.removeFromLeft(touch ? 170 : 140));
        area.removeFromTop(gap);
        auto bottom = area.removeFromBottom(control);
        area.removeFromBottom(gap);
        list_.setBounds(area);
        for(auto* b : {&reveal_, &delete_, &rename_, &play_})
        {
            if(!b->isVisible()) continue;
            const juce::Font font{juce::FontOptions(textSize(TextRole::Button, mode()))};
            b->setBounds(bottom.removeFromRight(juce::GlyphArrangement::getStringWidthInt(font, b->getButtonText()) + (touch ? 40 : 32)));
            bottom.removeFromRight(gap);
        }
    }

    void RecordingsPane::paint(juce::Graphics& g)
    {
        g.fillAll(ink(Ink::Background));
        const int m = mode();
        const juce::Font small{juce::FontOptions(textSize(TextRole::Small, m))};
        if(takes_.empty())
        {
            g.setColour(ink(Ink::Muted));
            drawInked(g, "No recordings yet. Record, or Capture what was just played, from the top bar.", list_.getBounds().withHeight(list_.getRowHeight() * 2),
                      juce::Font(juce::FontOptions(textSize(TextRole::Body, m))), juce::Justification::horizontallyCentred);
        }
        if(message_.isNotEmpty())
        {
            auto bottom = getLocalBounds().reduced(16, 12).removeFromBottom(mode() == 2 ? 44 : 30);
            int left = bottom.getRight();
            for(auto* b : {&reveal_, &delete_, &rename_, &play_}) if(b->isVisible()) left = std::min(left, b->getX());
            g.setColour(ink(Ink::Muted));
            drawInked(g, message_, bottom.withRight(left - 12), small);
        }
    }
}
