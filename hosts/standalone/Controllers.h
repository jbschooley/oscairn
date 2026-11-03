// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "Engine.h"
#include "Profile.h"
#include "oscairn/control_surface.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <atomic>
#include <map>
#include <mutex>

namespace oscairn::standalone
{
    /// External MIDI controllers (RFC 0024): matches the MIDI inputs whose names match a
    /// controller's profile, joins its control groups to the device's, and tells the engine,
    /// which plays what the controls move through RFC 0011's controller-mapping service on its
    /// own MIDI inputs (instant controls). A Mackie Control port is read here by MackieControl
    /// (control_surface.h), and the engine leaves it out; its LEDs, faders and names, and the
    /// values of knobs whose profile takes them, go back on the output of the port's name.
    class Controllers : private juce::MidiInputCallback, private juce::Timer
    {
    public:
        // choices: the file the musician's choices of profile per port are kept in (RFC 0024).
        Controllers(Engine& engine, std::vector<ControlGroup> device, std::vector<Profile> profiles, std::string choices = {});
        ~Controllers() override;
        const std::vector<Profile>& profiles() const { return profiles_; }
        const ProfileChoices& choices() const { return choices_; }
        void choose(const std::string& port, const std::string& profile); // kept, and the ports opened again

    private:
        void open();
        void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage&) override;
        void timerCallback() override;
        double value(const Assignment&);
        void apply(const ControllerMapping::Action&);
        void feedback(); // timer: what changed, back to the surfaces and knobs that take it
        bool playing();
        std::vector<Control::ParamInfo> parametersOf(NodeId);

        Engine& engine_;
        std::vector<ControlGroup> device_;
        std::vector<Profile> profiles_;
        std::string choicesFile_;
        ProfileChoices choices_;
        // MIDI input threads queue messages under the lock; the message thread takes them.
        struct Incoming { const juce::MidiInput* input; uint8_t bytes[3]; };
        std::mutex lock_;
        std::vector<Incoming> queue_;
        std::vector<std::unique_ptr<juce::MidiInput>> inputs_; // message thread; read under the lock
        std::vector<std::pair<std::string, uint32_t>> ports_;  // each input's controller and port
        std::vector<bool> mackie_;                             // each input: a Mackie port
        std::vector<std::string> ignored_;                     // message thread: the inputs the engine was told to leave out
        juce::MidiDeviceListConnection changed_;
        std::atomic<bool> dirty_{false};
        // Message thread: what the surfaces and feedback read, and values set lately (read back late).
        Control::GraphState graph_;
        Control::LiveValues values_; // as last seen: a change is read and sent back at once
        AssignmentSource source_;
        double sourceTime_ = -1;
        std::map<NodeId, std::pair<std::string, std::vector<Control::ParamInfo>>> parameters_;
        std::map<std::tuple<int, NodeId, uint32_t, uint32_t>, std::pair<double, double>> recent_; // value, when
        // Message thread: each Mackie port's mapping and whether it has a display; outputs back to
        // controllers, by controller and port; the knobs' values sent back.
        std::map<std::pair<std::string, uint32_t>, std::pair<MackieControl, bool>> surfaces_;
        std::map<std::pair<std::string, uint32_t>, std::unique_ptr<juce::MidiOutput>> outputs_;
        KnobFeedback knobs_;
    };
}
