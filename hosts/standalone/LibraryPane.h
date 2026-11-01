// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "Apps.h"
#include "Engine.h"
#include "Theme.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace oscairn::standalone
{
    /// The Library app (RFC 0021): the saved Performances, found by typing or by tags (each
    /// chosen tag narrows the list), and by favourites. In Play mode a click opens one, like the
    /// MODX's Category Search played from the list, and "Add to setlist" sends it to whichever
    /// app takes Performances (RFC 0022). In Pick mode it answers another app's pick.
    class LibraryPane : public juce::Component, private juce::Timer, private juce::ListBoxModel
    {
    public:
        enum class Mode { Play, Pick };
        LibraryPane(Engine& engine, Actions& actions, Mode mode);
        std::function<void(const std::string& id)> onPicked; // Pick mode; empty when cancelled

        void search(const juce::String& text);
        void toggleTag(const std::string& tag);
        void refresh() { stale_ = true; message_.clear(); timerCallback(); } // query again now, or once the engine is free
        const std::vector<Control::LibraryItem>& items() const { return result_.items; }
        const std::vector<std::pair<std::string, uint32_t>>& tags() const { return result_.tags; }
        void choose(int row);                   // Play: opens it; Pick: answers with it
        void send(int row);                     // to the app that takes Performances
        const juce::String& message() const { return message_; }

        void paint(juce::Graphics&) override;
        void resized() override;

    private:
        void timerCallback() override;
        void rebuildTags();
        int getNumRows() override { return int(result_.items.size()); }
        void paintListBoxItem(int row, juce::Graphics&, int width, int height, bool selected) override;
        void listBoxItemClicked(int row, const juce::MouseEvent&) override;
        void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;
        void returnKeyPressed(int row) override { choose(row); }
        void selectedRowsChanged(int) override { updateButtons(); }
        void updateButtons();
        int mode() const { return std::max(1, desktopMode(*this)); }
        juce::Rectangle<int> starArea(int width, int height) const;

        Engine& engine_;
        Actions& actions_;
        Mode mode_;
        Control::LibraryQuery query_;
        Control::LibraryResult result_;
        bool stale_ = true;
        juce::String message_;
        juce::TextEditor search_;
        juce::TextButton favourites_{"Favourites"}, recent_{"Recent"};
        juce::OwnedArray<juce::TextButton> tagButtons_;
        juce::ListBox list_{"Performances", this};
        juce::TextButton send_{"Add to setlist"}, cancel_{"Cancel"}, pick_{"Choose"};
        juce::Label status_;
    };
}
