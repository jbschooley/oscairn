// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "Apps.h"
#include "Engine.h"
#include "Theme.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace oscairn::standalone
{
    /// The Library app (RFC 0021): the saved Performances and Sounds, found by typing or by tags
    /// (each chosen tag narrows the list), and by favorites. In Play mode a click opens a
    /// Performance, like the MODX's Category Search played from the list, and "Add to setlist"
    /// sends it to whichever app takes Performances (RFC 0022); a Sound is added as a new part.
    /// In Pick mode it answers another app's pick, of the one kind asked for.
    class LibraryPane : public juce::Component, private juce::Timer, private juce::ListBoxModel
    {
    public:
        enum class Mode { Play, Pick };
        LibraryPane(Engine& engine, Actions& actions, Mode mode, const std::string& kind = "performance");
        std::function<void(const std::string& id)> onPicked; // Pick mode; empty when canceled

        void search(const juce::String& text);
        void toggleTag(const std::string& tag);
        void showKind(const std::string& kind); // "performance" or "sound"; tags chosen for the other kind are cleared
        void refresh() { stale_ = true; message_.clear(); timerCallback(); } // query again now, or once the engine is free
        const std::vector<Control::LibraryItem>& items() const { return result_.items; }
        const std::vector<std::pair<std::string, uint32_t>>& tags() const { return result_.tags; }
        void choose(int row);                   // Play: opens it; Pick: answers with it
        void send(int row);                     // to the app that takes Performances
        void newPart(int row);                  // a Sound, as a new part
        // Libraries (RFC 0021): install an archive file, chosen and asked about; delete the item
        // at row, asked first; the installed libraries, to install another or uninstall one.
        void install();
        void deleteItem(int row);
        void librariesMenu();
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
        bool stale_ = true, installing_ = false;
        juce::String message_;
        juce::TextEditor search_;
        juce::TextButton favorites_{"Favorites"}, recent_{"Recent"}, performances_{"Performances"}, sounds_{"Sounds"};
        juce::OwnedArray<juce::TextButton> tagButtons_;
        juce::ListBox list_{"Performances", this};
        juce::TextButton send_{"Add to setlist"}, part_{"New part"}, cancel_{"Cancel"}, pick_{"Choose"}, delete_{"Delete"}, libraries_{"Libraries..."};
        std::unique_ptr<juce::FileChooser> chooser_;
        juce::Label status_;
    };
}
