// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "Apps.h"
#include "Engine.h"
#include "Theme.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace oscairn::standalone
{
    /// A setlist: Performances in the order they are played, each by its library ID (RFC 0021),
    /// with the name it had when last seen, shown if it has left the library, and a color of
    /// its own in this setlist (empty: the Performance's). How it is shown is the setlist's too.
    /// One file per setlist in the data folder's Setlists/, named after it (docs/setlist-format.md).
    struct SetlistEntry { std::string id, name, color; };
    struct SetlistDisplay
    {
        enum class Mode { List, Scroll, Grid } mode = Mode::List; // a list with Previous and Next; a list to scroll and tap; a grid with Previous and Next
        int columns = 4, rows = 3;                                 // the grid's, 1 to 8 each
    };
    struct Setlist { std::string name; std::vector<SetlistEntry> entries; SetlistDisplay display; };
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
        ~SetlistPane() override;

        void open(const juce::File& file);
        void create(const std::string& name);
        juce::String add(const std::string& id); // to the open setlist; what was done, or empty if it is not in the library
        void addFromLibrary();                   // asks for a pick
        void remove(int index);
        void move(int index, int by);
        void go(int index);                      // opens that entry's Performance
        void setColor(int index, const std::string& color); // its color in this setlist; empty: the Performance's
        juce::Colour colorAt(int index) const;  // what it shows: its own, else its Performance's; transparent for none
        void setDisplay(SetlistDisplay display);
        void step(int by) { go(current_ < 0 ? 0 : current_ + by); }
        const Setlist& setlist() const { return setlist_; }
        int current() const { return current_; }
        const juce::String& message() const { return message_; }

        void paint(juce::Graphics&) override;
        void resized() override;
        bool keyPressed(const juce::KeyPress&) override;

    private:
        class Grid;
        int getNumRows() override { return int(setlist_.entries.size()); }
        void paintListBoxItem(int row, juce::Graphics&, int width, int height, bool selected) override;
        void listBoxItemClicked(int row, const juce::MouseEvent&) override;
        void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override { go(row); }
        void returnKeyPressed(int row) override { go(row); }
        void selectedRowsChanged(int) override { updateButtons(); }
        void save();
        void refreshChooser();
        void updateButtons();
        void say(const juce::String& text);
        void colorMenu(int index);
        int selected() const;                    // what Remove and the moves act on
        void showDisplay();
        int mode() const { return std::max(1, desktopMode(*this)); }

        Engine& engine_;
        Actions& actions_;
        juce::File folder_;
        Setlist setlist_;
        struct Shown { bool missing = false; std::string color; }; // per entry, as the library has it
        std::vector<Shown> shown_;
        int current_ = -1;
        juce::String message_;
        juce::ComboBox chooser_, view_, columns_, rows_;
        std::unique_ptr<Grid> grid_;
        juce::TextButton new_{"New"}, previous_{"Previous"}, next_{"Next"}, add_{"Add"}, remove_{"Remove"}, up_{"Up"}, down_{"Down"};
        juce::ListBox list_{"Setlist", this};
        juce::Label status_;
        std::vector<juce::File> files_;
    };
}
