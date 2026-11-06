// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "Apps.h"
#include "Engine.h"
#include "Icons.h"
#include "Theme.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace oscairn::standalone
{
    /// An export's choices (RFC 0021, RFC 0006): for a Performance, its edited parts' Sounds too;
    /// and a signature, off unless a key is chosen (Make a key... makes one and chooses it).
    class ExportChoices : public juce::Component
    {
    public:
        ExportChoices(Engine& engine, bool performance);
        juce::ToggleButton editedSounds{"With edited parts' Sounds"};
        juce::ComboBox signWith;
        juce::TextButton go{"Export"}, cancel{"Cancel"};
        std::string key() const;                 // the key chosen to sign with; empty: not signed
        void chooseKey(const std::string& name); // the keys listed again, this one chosen
        void paint(juce::Graphics&) override;
        void resized() override;

    private:
        Engine& engine_;
        bool performance_;
        std::vector<Control::SigningKey> keys_;
        int chosen_ = 1;
        juce::Rectangle<int> note_, signLabel_;
    };
    /// The export's choices in a dialog over `over`; `start` gets them when Export is pressed.
    void showExport(Engine& engine, juce::Component* over, const juce::String& title, bool performance, std::function<void(bool editedSounds, const std::string& key)> start);
    /// An archive's install choices: locked or as the user's own, and which items, all ticked; who
    /// signed it and any plugin builds from an unknown source or a revoked key (RFC 0006). The
    /// items scroll; the options above and the buttons below stay in view.
    class InstallChoices : public juce::Component
    {
    public:
        explicit InstallChoices(const Control::Archive& archive);
        juce::ToggleButton own{"Into my Sounds and Performances, to edit"}, locked{"As a locked library"};
        juce::Label heading{{}, "Items"}, code; // code: plugin builds from an unknown source or a revoked key
        juce::Viewport view;                    // holds list, the items' column
        juce::Component list;
        juce::OwnedArray<juce::ToggleButton> items;
        std::vector<std::string> ids;
        juce::TextButton go{"Install"}, cancel{"Cancel"};
        bool asks = false; // installing says yes to code from an unknown source
        void paint(juce::Graphics&) override;
        void resized() override;

    private:
        juce::String signer_; juce::Rectangle<int> signerArea_;
        int codeLines_ = 0;
    };
    /// Who signed an archive (RFC 0006), as the install dialog says it.
    juce::String describeSigner(const Control::Signer& signer);
    /// Another Performance's parts and scenes to import (RFC 0021), all ticked, under headings.
    /// The dialog is no taller than `maxHeight`; past that the list scrolls, the buttons in view.
    class ImportChoices : public juce::Component
    {
    public:
        ImportChoices(const Control::PerformanceContents& held, int maxHeight);
        juce::OwnedArray<juce::ToggleButton> parts, scenes;
        juce::Label partsHeading{{}, "Parts"}, scenesHeading{{}, "Scenes"};
        juce::Viewport view; // holds list, the headings and the toggles
        juce::Component list;
        juce::TextButton import{"Import"}, cancel{"Cancel"};
        std::vector<uint32_t> partIds, sceneIds;
        static std::vector<uint32_t> ticked(const juce::OwnedArray<juce::ToggleButton>& boxes, const std::vector<uint32_t>& ids);
        void resized() override;
    };

    /// The Library app (RFC 0021): the saved Performances and Sounds, found by typing, by where
    /// they are from (the user's own or an installed library) and by tags in the column on the
    /// left (each chosen tag narrows the list), and by favorites. In Play mode a click opens a
    /// Performance, like the MODX's Category Search played from the list, and "Add to setlist"
    /// sends it to whichever app takes Performances (RFC 0022); a Sound is added as a new part.
    /// In Pick mode it answers another app's pick, of the one kind asked for. Every item's
    /// actions are on a menu: a right click, a long press, or Options for the selected one.
    class LibraryPane : public juce::Component, private juce::Timer, private juce::ListBoxModel
    {
    public:
        enum class Mode { Play, Pick };
        LibraryPane(Engine& engine, Actions& actions, Mode mode, const std::string& kind = "performance");
        ~LibraryPane() override;
        std::function<void(const std::string& id)> onPicked; // Pick mode; empty when canceled
        std::function<uint32_t()> auditionPart;               // the part a Sound is auditioned in: the one shown; 0 for none

        void search(const juce::String& text);
        void toggleTag(const std::string& tag);
        void showKind(const std::string& kind);     // "performance" or "sound"; tags chosen for the other kind are cleared
        void showSource(const std::string& source); // "mine", an installed library's ID, or empty for all
        void setCompact(bool compact);              // one line a row, to see more; kept for the next time
        bool compact() const { return compact_; }
        void setPills(bool pills);                  // the left column's libraries and tags as pills, not rows; kept too
        bool pills() const { return pills_; }
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
        void removePlugin(const Control::InstalledPlugin& plugin);
        void addPluginBuild(const Control::InstalledPlugin& plugin);
        void itemMenu(int row);                              // everything that can be done with it
        void editItem(int row);                              // its name, description and tags, asked for
        std::string copyItem(int row);                       // into the user's own; the copy's ID, or empty
        void exportItem(int row);                            // to an archive file, chosen and asked about
        // Audition (RFC 0021): the item at row tried in place, a Sound in auditionPart's; while
        // one plays, choosing another row tries that one. Stop puts back what was there; keep
        // makes it the change. Picking, canceling or closing stops it.
        void startAudition(int row);
        void stopAudition(bool keep = false);
        const Control::Audition& auditioning() const { return auditioning_; }
        void tagMenu(const std::string& tag);                // hide it from the tag list, or color it
        std::string tagColor(const std::string& tag) const; // the user's color for it, or empty
        const juce::String& message() const { return message_; }

        void paint(juce::Graphics&) override;
        void resized() override;
        bool keyPressed(const juce::KeyPress&) override; // Space starts or stops an audition

    private:
        // The left column: the library (everything, the user's own, each installed library), then
        // the tags, as rows or as pills that wrap.
        struct Side { enum Kind { Heading, Source, Tag } kind; std::string value; juce::String label; uint32_t count = 0; juce::Rectangle<int> bounds{}; };
        struct Sidebar : juce::Component
        {
            explicit Sidebar(LibraryPane& p);
            LibraryPane& pane;
            IconButton list{IconButton::Glyph::List, "Show as a list"}, pills{IconButton::Glyph::Pills, "Show as pills"};
            int layout(int width); // places every entry; returns the height they take
            void paint(juce::Graphics&) override;
            void mouseUp(const juce::MouseEvent&) override;
        };
        void timerCallback() override;
        void rebuildSide();
        void tell(const juce::String& text);
        void updatePlaceholder();
        void savePreference() const;
        int getNumRows() override { return int(result_.items.size()); }
        void paintListBoxItem(int row, juce::Graphics&, int width, int height, bool selected) override;
        void listBoxItemClicked(int row, const juce::MouseEvent&) override;
        void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;
        void returnKeyPressed(int row) override { choose(row); }
        void selectedRowsChanged(int row) override;
        void updateButtons();
        int mode() const { return std::max(1, desktopMode(*this)); }
        juce::Rectangle<int> starArea(int width, int height) const;
        const Control::LibraryItem* at(int row) const { return row >= 0 && size_t(row) < result_.items.size() ? &result_.items[size_t(row)] : nullptr; }

        Engine& engine_;
        Actions& actions_;
        Mode mode_;
        Control::LibraryQuery query_;
        Control::LibraryResult result_;
        Control::Audition auditioning_;
        std::vector<Control::InstalledLibrary> installed_;
        std::vector<Side> side_;
        bool stale_ = true, awaiting_ = false, compact_ = false, pills_ = false;
        juce::String message_;
        juce::TextEditor search_;
        juce::TextButton favorites_{"Favorites"}, recent_{"Recent"}, performances_{"Performances"}, sounds_{"Sounds"}, compactButton_{"Compact"};
        Sidebar sidebar_{*this};
        juce::Viewport sideView_;
        juce::ListBox list_{"Performances", this};
        juce::TextButton send_{"Add to setlist"}, part_{"New part"}, cancel_{"Cancel"}, pick_{"Choose"}, options_{"Options..."}, libraries_{"Libraries..."}, audition_{"Audition"}, keep_{"Keep"};
        std::unique_ptr<juce::FileChooser> chooser_;
        std::unique_ptr<juce::AlertWindow> editor_;
        juce::Label status_;
    };
}
