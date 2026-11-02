// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "Engine.h"
#include "Theme.h"
#include <juce_gui_basics/juce_gui_basics.h>

namespace oscairn::standalone
{
    /// A key's ID as people read it (RFC 0006's fingerprint): its 16 hex digits in groups of four.
    juce::String fingerprint(const std::string& id);
    /// Asks for a new signing key's name and makes it (RFC 0006); `made` gets its name once it is
    /// made, and an error is shown instead if it is not.
    void askToMakeKey(Engine& engine, juce::Component* over, std::function<void(const std::string& name)> made);

    /// Settings: the device's signing keys (RFC 0006), which sign exports, each by name and
    /// fingerprint (never the private key), and Make a key.
    class Settings : public juce::Component
    {
    public:
        explicit Settings(Engine& engine);
        void refresh(); // reads the keys again
        const std::vector<Control::SigningKey>& keys() const { return keys_; }
        juce::Button& makeKeyButton() { return makeKey_; }

        void paint(juce::Graphics&) override;
        void resized() override;

    private:
        int mode() const { return std::max(1, desktopMode(*this)); }
        Engine& engine_;
        std::vector<Control::SigningKey> keys_;
        juce::TextButton makeKey_{"Make a key..."};
        juce::Rectangle<int> keysArea_;
    };
}
