// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "Apps.h"
#include "Engine.h"
#include "Theme.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace oscairn::standalone
{
    /// A setlist: Performances in the order they are played, each by its library ID (RFC 0021),
    /// with the name it had when last seen, shown if it has left the library. One file per
    /// setlist in the data folder's Setlists/, named after it (docs/setlist-format.md).
    struct SetlistEntry { std::string id, name; };
    struct Setlist { std::string name; std::vector<SetlistEntry> entries; };
    juce::File setlistFolder();                                     // the data folder's Setlists/
    std::vector<juce::File> setlistFiles(const juce::File& folder); // the most recently changed first
    Setlist readSetlist(const juce::File& file);                    // throws on a file it cannot read
    juce::File setlistFile(const juce::File& folder, const std::string& name);
    void writeSetlist(const juce::File& folder, const Setlist&);    // all at once: the old file or the new, never half

    /// The Setlist app: the open setlist's Performances in order, the one playing marked, with
    /// big Previous and Next for the stage. Add asks whichever app picks Performances (the
    /// Library, RFC 0022); it also takes Performances other apps send.
    class SetlistPane : public juce::Component, private juce::ListBoxModel
    {
    public:
        SetlistPane(Engine& engine, Actions& actions, juce::File folder = setlistFolder());

        void open(const juce::File& file);
        void create(const std::string& name);
        juce::String add(const std::string& id); // to the open setlist; what was done, or empty if it is not in the library
        void addFromLibrary();                   // asks for a pick
        void remove(int index);
        void move(int index, int by);
        void go(int index);                      // opens that entry's Performance
        void step(int by) { go(current_ < 0 ? 0 : current_ + by); }
        const Setlist& setlist() const { return setlist_; }
        int current() const { return current_; }
        const juce::String& message() const { return message_; }

        void paint(juce::Graphics&) override;
        void resized() override;
        bool keyPressed(const juce::KeyPress&) override;

    private:
        int getNumRows() override { return int(setlist_.entries.size()); }
        void paintListBoxItem(int row, juce::Graphics&, int width, int height, bool selected) override;
        void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override { go(row); }
        void returnKeyPressed(int row) override { go(row); }
        void selectedRowsChanged(int) override { updateButtons(); }
        void save();
        void refreshChooser();
        void updateButtons();
        void say(const juce::String& text);
        int mode() const { return std::max(1, desktopMode(*this)); }

        Engine& engine_;
        Actions& actions_;
        juce::File folder_;
        Setlist setlist_;
        std::vector<bool> missing_; // per entry: no longer in the library
        int current_ = -1;
        juce::String message_;
        juce::ComboBox chooser_;
        juce::TextButton new_{"New"}, previous_{"Previous"}, next_{"Next"}, add_{"Add"}, remove_{"Remove"}, up_{"Up"}, down_{"Down"};
        juce::ListBox list_{"Setlist", this};
        juce::Label status_;
        std::vector<juce::File> files_;
    };
}
