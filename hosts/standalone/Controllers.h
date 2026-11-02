// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "Engine.h"
#include "Profile.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <atomic>
#include <map>
#include <mutex>

namespace oscairn::standalone
{
    /// External MIDI controllers (RFC 0024): opens the MIDI inputs whose names match a
    /// controller's profile, joins its control groups to the device's, and plays what its
    /// controls move through RFC 0011's controller-mapping service, as Play's knobs would. Notes
    /// and the standard controllers still reach the parts through the engine's own inputs.
    class Controllers : private juce::MidiInputCallback, private juce::Timer
    {
    public:
        Controllers(Engine& engine, std::vector<ControlGroup> device, std::vector<Profile> profiles);
        ~Controllers() override;

    private:
        void open();
        void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage&) override;
        void timerCallback() override;
        double value(const Assignment&);
        void apply(const ControllerMapping::Action&);
        std::vector<Control::ParamInfo> parametersOf(NodeId);

        Engine& engine_;
        std::vector<ControlGroup> device_;
        std::vector<Profile> profiles_;
        // MIDI input threads queue messages under the lock; the message thread takes them.
        struct Incoming { const juce::MidiInput* input; uint8_t bytes[3]; };
        std::mutex lock_;
        std::vector<Incoming> queue_;
        std::vector<std::unique_ptr<juce::MidiInput>> inputs_; // message thread; read under the lock
        std::vector<std::pair<std::string, uint32_t>> ports_;  // each input's controller and port
        std::vector<std::string> ignored_;                     // message thread: the inputs the engine was told to leave out
        juce::MidiDeviceListConnection changed_;
        std::atomic<bool> dirty_{false};
        // Message thread: the mapping, what it reads, and values it set lately (read back late).
        ControllerMapping mapping_;
        Control::GraphState graph_;
        AssignmentSource source_;
        double sourceTime_ = -1;
        std::map<NodeId, std::pair<std::string, std::vector<Control::ParamInfo>>> parameters_;
        std::map<std::tuple<int, NodeId, uint32_t, uint32_t>, std::pair<double, double>> recent_; // value, when
    };
}
