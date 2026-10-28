// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "oscairn/graph.h"
#include "oscairn/transport.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace oscairn::standalone
{
    /// The demo instrument: the step sequencer and pads playing the basic synth, through
    /// the delay, on the default audio device, with every MIDI input as a source of notes,
    /// aftertouch and clock.
    ///
    /// The UI talks to the engine only through the public methods below: nodes by name,
    /// parameters by id, plain values. Nothing here hands out the graph or a plugin. When
    /// the UI moves to its own process, these methods become the control API.
    class Engine : private juce::AudioIODeviceCallback, private juce::MidiInputCallback
    {
    public:
        /// Loads the plugins from `pluginDir` and opens the default audio output.
        /// Throws std::runtime_error if a plugin does not load or no audio output opens.
        explicit Engine( const std::string& pluginDir );
        ~Engine() override;

        // Main thread --------------------------------------------------------------

        struct ParamInfo
        {
            std::string name;
            double      minimum = 0, maximum = 1, defaultValue = 0;
            bool        stepped = false;
        };
        /// Nodes: "sequencer", "synth", "delay".
        bool        hasParam( const std::string& node, uint32_t param ) const;
        ParamInfo   paramInfo( const std::string& node, uint32_t param ) const;
        double      param( const std::string& node, uint32_t param ) const;
        std::string paramText( const std::string& node, uint32_t param, double value ) const;
        void        setParam( const std::string& node, uint32_t param, double value );

        void setPlaying( bool playing ) { playing_ = playing; }
        void setTempo( double bpm ) { tempo_ = bpm; }
        void setFollowMidiClock( bool follow ) { followClock_ = follow; }
        bool followsMidiClock() const { return followClock_; }

        /// Notes from the UI (pads, on-screen keys), as if from a MIDI input.
        void note( bool on, int key, float velocity );

        struct Status
        {
            bool        playing = false;
            double      tempo   = 120.0; // the estimate while following MIDI clock
            int         step    = -1;    // the sequencer step under the playhead; -1 when stopped
            std::string audioDevice;
            std::vector<std::string> midiInputs;
        };
        Status status() const;

        /// Main-loop work: plugin callbacks, freeing retired graph plans, reopening MIDI
        /// inputs after devices change. Call regularly (the UI does, from a timer).
        void service();

    private:
        // Audio thread
        void audioDeviceIOCallbackWithContext( const float* const* inputs, int numInputs, float* const* outputs, int numOutputs,
                                               int numSamples, const juce::AudioIODeviceCallbackContext& ) override;
        void audioDeviceAboutToStart( juce::AudioIODevice* ) override;
        void audioDeviceStopped() override {}
        // MIDI input threads
        void handleIncomingMidiMessage( juce::MidiInput*, const juce::MidiMessage& ) override;

        void push( const juce::MidiMessage& message );
        void openMidiInputs();
        NodeId nodeOf( const std::string& name ) const;

        juce::AudioDeviceManager devices_;
        double                   sampleRate_ = 48000.0;
        static constexpr uint32_t maxFrames  = 512;

        std::unique_ptr<Graph>        graph_;
        std::map<std::string, NodeId> nodes_;
        std::optional<Transport>      transport_; // audio thread only, once running
        std::vector<float>            scratch_;   // a silent second channel for mono devices

        // Control values read by the audio thread.
        std::atomic<bool>   playing_{ false }, followClock_{ false };
        std::atomic<double> tempo_{ 120.0 };
        // Read by the UI.
        std::atomic<bool>   statusPlaying_{ false };
        std::atomic<double> statusTempo_{ 120.0 };
        std::atomic<int>    statusStep_{ -1 };
        std::atomic<bool>   started_{ false }; // the device's rate matched the graph's

        // MIDI and UI notes to the audio thread: many producers, serialised by the mutex
        // (never taken on the audio thread), one consumer.
        struct Incoming
        {
            uint8_t bytes[3];
            uint8_t size; // bytes used
            double  seconds; // Time::getMillisecondCounterHiRes() / 1000 at arrival
        };
        std::mutex                    pushLock_;
        juce::AbstractFifo            fifo_{ 1024 };
        std::array<Incoming, 1024>    incoming_{};
        std::atomic<uint64_t>         droppedMidi_{ 0 };

        bool following_ = false; // audio thread: the transport is following MIDI clock

        // Audio thread: when the last block started (seconds and sample), and where the
        // next one starts, on the transport's timeline. Places MIDI clock in time.
        double  blockSeconds_   = 0.0;
        int64_t lastBlockStart_ = 0, blockSample_ = 0;

        // MIDI inputs, main thread.
        std::vector<std::unique_ptr<juce::MidiInput>> midiInputs_;
        juce::MidiDeviceListConnection                midiDevicesChanged_;
        std::atomic<bool>                             midiDevicesDirty_{ false };
    };
}
