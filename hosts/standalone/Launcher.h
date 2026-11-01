// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "Engine.h"
#include "Icons.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace oscairn::standalone
{
    /// Home (RFC 0030): the product's apps as tiles under the top bar, each with a line of its
    /// state. In the design the launcher is an app answering oscairn.home, replaceable; until
    /// apps are packages (RFC 0022 step 2) this one is built in.
    class Launcher : public juce::Component, private juce::Timer
    {
    public:
        struct App
        {
            juce::String name;
            const char* icon;                     // a Lucide icon the app carries (Icons.h)
            std::function<juce::String()> state;  // a line of the app's state, or empty
            std::function<void()> open;
        };
        Launcher(Engine& engine, std::vector<App> apps);
        int apps() const { return int(apps_.size()); }
        const juce::String& name(int app) const { return apps_[size_t(app)].name; }
        void open(int app); // after this click is done with the launcher: opening may replace it

        void paint(juce::Graphics&) override;
        void resized() override;
        void mouseUp(const juce::MouseEvent&) override;

    private:
        void timerCallback() override;
        int mode() const { return std::max(1, desktopMode(*this)); }

        Engine& engine_;
        std::vector<App> apps_;
        std::vector<juce::Rectangle<int>> tiles_;
        std::vector<juce::String> states_;
        juce::String context_; // the open Performance
    };

    /// An app on a device's screen (RFC 0030): the top bar, with Home where the panel has no
    /// button for it and the app's name, over the app.
    class AppFrame : public juce::Component
    {
    public:
        AppFrame(const juce::String& name, juce::Component& app, bool home, std::function<void()> goHome);
        void paint(juce::Graphics&) override;
        void resized() override;

    private:
        juce::String name_;
        juce::Component& app_;
        IconButton home_{IconButton::Glyph::Home, "Home"};
    };
}
