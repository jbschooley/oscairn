// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "Engine.h"
#include "Profile.h"
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
    /// fingerprint (never the private key), and Make a key; and the connected MIDI controllers
    /// (RFC 0024), each port with the profile it is played by, a choice where several match.
    class Settings : public juce::Component
    {
    public:
        explicit Settings(Engine& engine);
        void refresh(); // reads the keys and the ports again
        const std::vector<Control::SigningKey>& keys() const { return keys_; }
        juce::Button& makeKeyButton() { return makeKey_; }
        // The controllers' side: the profiles, the MIDI inputs' names now, the choices kept and
        // where a choice goes (kept for the port's name).
        struct ControllerPorts
        {
            std::vector<Profile> profiles;
            std::function<std::vector<std::string>()> inputs;
            std::function<ProfileChoices()> choices;
            std::function<void(const std::string& port, const std::vector<std::string>& profiles)> choose; // several: identical controllers by channel
        };
        void setControllers(ControllerPorts ports);
        // A port a profile matches: the profiles that do (their places), the one in use, and what
        // it does with the port.
        struct PortRow { std::string port; std::vector<size_t> profiles; size_t chosen = 0; juce::String note; bool all = false; }; // all: every profile that fits, by channel
        const std::vector<PortRow>& ports() const { return ports_; }
        juce::ComboBox* profileBox(size_t row) { return row < boxes_.size() ? boxes_[row].get() : nullptr; }

        void paint(juce::Graphics&) override;
        void resized() override;

    private:
        int mode() const { return std::max(1, desktopMode(*this)); }
        Engine& engine_;
        std::vector<Control::SigningKey> keys_;
        juce::TextButton makeKey_{"Make a key..."};
        juce::Rectangle<int> keysArea_, portsHeading_, portsArea_;
        ControllerPorts controllers_;
        std::vector<PortRow> ports_;
        std::vector<std::unique_ptr<juce::ComboBox>> boxes_;
        void readPorts();
    };
}
