// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>

namespace oscairn::standalone
{
    /// The splash screen, while the app starts and connects to its engine: the mark (or the
    /// theme's splash image), the name and a line of status. The theme chooses its image and
    /// colors (ThemeSplash); a device's panel shows it full screen, the desktop in a small window.
    class Splash : public juce::Component
    {
    public:
        Splash() { setTitle("Oscairn"); }
        void setStatus(const juce::String& status) { status_ = status; repaint(); }
        const juce::String& status() const { return status_; }
        void paint(juce::Graphics&) override;

    private:
        juce::String status_ = juce::String::fromUTF8("Starting the engine…");
    };
}
