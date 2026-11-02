// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "Engine.h"
#include "Icons.h"
#include "PartsModel.h"
#include "PartsView.h"
#include "PlayView.h"
#include "Recording.h"
#include "Screen.h"
#include "Theme.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace oscairn::standalone
{
    /// How the desktop is used (RFC 0019): a keyboard and mouse, or touch. It changes control
    /// sizes and spacing, not what shows. A per-user preference, like the theme.
    enum class InputMode { Automatic, Mouse, Touch };
    InputMode readInputMode(const juce::File& configuration);
    void writeInputMode(const juce::File& configuration, InputMode);
    bool touchSized(InputMode);
    juce::String inputModeName(InputMode);

    /// The Groovebox app: the step sequencer's screen with its transport above it, in a window
    /// of its own. Apps are separate from the sound editor; Play and Stop belong to the app.
    class SequencerPane : public juce::Component, private juce::Timer
    {
    public:
        explicit SequencerPane(Engine& engine, bool transport = true); // transport: Play, Stop and clock on screen
        void refresh();
        void resized() override;
    private:
        void timerCallback() override { refresh(); }
        Engine& engine_;
        juce::TextButton play_{"Play"}, stop_{"Stop"};
        juce::ToggleButton sync_{"Follow MIDI clock"};
        Screen screen_;
    };

    /// The Desktop surface (RFC 0019): the desktop app's own window rather than a device's
    /// panel. A toolbar with the tempo, scenes and undo; Play (RFC 0011) under it, first; and
    /// Edit's: the parts and buses down the left with their meters, and tabs for the selected
    /// part, the Mixer, Routing and Controls, each of which can also open in a window of its own.
    class DesktopShell : public juce::Component, private juce::Timer
    {
    public:
        enum class Tab { Part, Mixer, Routing, Controls, Play }; // Play is not one of Edit's tabs
        DesktopShell(Engine& engine, const std::string& scanner, InputMode mode);
        ~DesktopShell() override;

        std::function<void()> onHome; // Home (RFC 0030): shown when set
        std::function<void()> onSave; // Save the Performance: shown when set
        void setInputMode(InputMode mode);
        InputMode inputMode() const { return mode_; }
        void showTab(Tab tab);
        uint32_t selectedPart() const { return selected_; } // the part the Part tab shows
        PlayView& play() { return play_; }
        void detach(Tab tab); // the tab's view in a window of its own, beside this one
        static bool tabNamed(const juce::String& name, Tab& tab);

        void paint(juce::Graphics&) override;
        void resized() override;

    private:
        class PartList;
        class TabStrip;
        struct Metrics { int control, row, gap; };
        Metrics metrics() const;
        void timerCallback() override;
        void refresh();
        void select(uint32_t part);
        std::unique_ptr<juce::Component> viewFor(Tab tab); // a fresh view, for a window of its own

        Engine& engine_;
        std::string scanner_;
        InputMode mode_;
        Tab tab_ = Tab::Play, editTab_ = Tab::Part; // editTab_: the tab Edit goes to
        uint32_t selected_ = 0; // the part the Part tab shows
        PartsState state_;
        std::map<uint32_t, std::vector<double>> meters_; // per part, per channel
        std::vector<Scene> scenes_;
        uint32_t scene_ = 0;
        int ticks_ = 0;

        // The toolbar.
        IconButton undo_{IconButton::Glyph::Undo, "Undo"}, redo_{IconButton::Glyph::Redo, "Redo"}, home_{IconButton::Glyph::Home, "Home"};
        juce::Label performance_;              // the open Performance's name
        juce::TextButton save_{"Save"}, edit_{"Edit"}; // Edit, or Play from Edit's views
        juce::Slider tempo_{juce::Slider::LinearBar, juce::Slider::TextBoxLeft};
        juce::Label tempoLabel_{{}, "Tempo"}, sceneLabel_{{}, "Scene"};
        RecordButtons record_{engine_}; // beside the tempo (RFC 0020)
        juce::ComboBox sceneBox_;
        juce::TextButton storeScene_{"Store"};
        juce::Label status_;
        // The tabs and what they show.
        std::unique_ptr<TabStrip> tabs_;
        std::unique_ptr<PartList> list_;
        juce::TextButton addPart_{"+ Part"}, addBus_{"+ Bus"};
        PartsView parts_;
        PlayView play_;
        std::vector<std::unique_ptr<juce::DocumentWindow>> windows_; // detached tabs
    };
}
