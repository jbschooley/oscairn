// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "Engine.h"
#include "PartsModel.h"
#include "PartsView.h"
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

    /// The Desktop surface (RFC 0019): the desktop app's own window rather than a device's
    /// panel. A toolbar with the transport, scenes and undo; the parts and buses down the left;
    /// tabs for the selected part, the Mixer, Routing, Controls and the Sequencer.
    class DesktopShell : public juce::Component, private juce::Timer
    {
    public:
        enum class Tab { Part, Mixer, Routing, Controls, Sequencer };
        DesktopShell(Engine& engine, const std::string& scanner, InputMode mode);
        ~DesktopShell() override;

        void setInputMode(InputMode mode);
        InputMode inputMode() const { return mode_; }
        void showTab(Tab tab);
        static bool tabNamed(const juce::String& name, Tab& tab);

        void paint(juce::Graphics&) override;
        void resized() override;

    private:
        class PartList;
        struct Metrics { int control, row, gap; float font; };
        Metrics metrics() const;
        void timerCallback() override;
        void refresh();
        void select(uint32_t part);

        Engine& engine_;
        InputMode mode_;
        Tab tab_ = Tab::Part;
        uint32_t selected_ = 0; // the part the Part tab shows
        PartsState state_;
        std::vector<Scene> scenes_;
        uint32_t scene_ = 0;

        // The toolbar.
        juce::TextButton play_{"Play"}, stop_{"Stop"}, undo_{"Undo"}, redo_{"Redo"};
        juce::Slider tempo_{juce::Slider::LinearBar, juce::Slider::TextBoxLeft};
        juce::ComboBox sceneBox_;
        juce::TextButton storeScene_{"Store"};
        juce::Label status_;
        // The tabs and what they show.
        std::array<juce::TextButton, 5> tabs_;
        std::unique_ptr<PartList> list_;
        juce::TextButton addPart_{"+ Part"}, addBus_{"+ Bus"};
        PartsView parts_;
        Screen sequencer_;
    };
}
