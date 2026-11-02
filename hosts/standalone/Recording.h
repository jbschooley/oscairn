// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "Engine.h"
#include "Icons.h"
#include "Profile.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace oscairn::standalone
{
    /// The retroactive buffer's length (RFC 0020): what was saved in the configuration folder, or
    /// the device's default, 2 minutes with 2 GB or less and 10 minutes above. 0 is off.
    double readCaptureSeconds(const juce::File& configuration, const Profile& profile);
    void writeCaptureSeconds(const juce::File& configuration, double seconds);

    /// Record and Capture (RFC 0020), for the top bar and the desktop's toolbar beside the tempo:
    /// Record starts a take of Main and the notes, and stops it; Capture saves the last 30
    /// seconds of the retroactive buffer as a take. While a take records, Record is filled in
    /// the danger color and its time shows beside it.
    class RecordButtons : public juce::Component, private juce::Timer
    {
    public:
        static constexpr double captureSeconds = 30; // RFC 0020's default for Capture
        explicit RecordButtons(Engine& engine);
        int preferredWidth() const; // the buttons, and the time while recording
        void record();              // starts or stops a take
        void capture();
        const juce::String& message() const { return message_; } // the time recording, or what Capture saved
        bool recording() const { return recording_ != 0; }

        void paint(juce::Graphics&) override;
        void resized() override;

    private:
        void timerCallback() override;
        int side() const;
        Engine& engine_;
        IconButton record_{IconButton::Glyph::Record, "Record"}, capture_{IconButton::Glyph::Capture, "Capture"};
        uint32_t recording_ = 0;
        juce::String message_;
        bool danger_ = false;
        int quiet_ = 0; // ticks a message from Capture stays
    };

    /// The Recordings app (RFC 0020's Recordings pane): the takes in the data folder's
    /// recordings/, this run's and earlier ones, with their name, date and length. A take plays
    /// back only when Play is pressed; it can be renamed, deleted (asked first) and shown in the
    /// computer's file browser. The retroactive buffer's length is set here.
    class RecordingsPane : public juce::Component, private juce::Timer, private juce::ListBoxModel
    {
    public:
        RecordingsPane(Engine& engine, const juce::File& configuration, bool browser = true); // browser: Show in the computer's file browser
        const std::vector<Control::Take>& takes() const { return takes_; }
        void refresh();
        void select(int row);
        int selected() const { return list_.getSelectedRow(); }
        void play();                         // the selected take, or stops the one playing
        void rename(const juce::String& name);
        void askRename();
        void remove();                       // the selected take, without asking
        void askRemove();
        void reveal();                       // in Finder, Explorer or the file manager
        void setCaptureSeconds(double seconds);
        const juce::String& message() const { return message_; }
        static juce::String nameOf(const Control::Take&);    // the folder's name without its time
        static juce::String dateOf(const Control::Take&);    // the time it started, from its folder
        static juce::String lengthOf(const Control::Take&);  // m:ss

        void paint(juce::Graphics&) override;
        void resized() override;

    private:
        void timerCallback() override { refresh(); }
        int getNumRows() override { return int(takes_.size()); }
        void paintListBoxItem(int row, juce::Graphics&, int width, int height, bool selected) override;
        void selectedRowsChanged(int) override { updateButtons(); }
        void updateButtons();
        void tell(const juce::String& text) { message_ = text; repaint(); }
        int mode() const { return std::max(1, desktopMode(*this)); }
        const Control::Take* at(int row) const { return row >= 0 && size_t(row) < takes_.size() ? &takes_[size_t(row)] : nullptr; }

        Engine& engine_;
        juce::File configuration_;
        std::vector<Control::Take> takes_;
        juce::String message_;
        juce::Label keepLabel_{{}, "Capture keeps the last"};
        juce::ComboBox keep_;
        juce::ListBox list_{"Recordings", this};
        juce::TextButton play_{"Play"}, rename_{"Rename..."}, delete_{"Delete..."}, reveal_;
        std::unique_ptr<juce::AlertWindow> dialog_;
    };
}
