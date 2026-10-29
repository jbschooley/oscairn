// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "oscairn/session.h"
#include "ChannelMap.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace oscairn::host
{
    /// The demo instrument: the step sequencer and pads playing the basic synth, through
    /// the delay, on the default audio device, with every MIDI input as a source of notes,
    /// aftertouch and clock.
    ///
    /// Device ownership shared by the standalone in-process mode and engine helper.
    /// The UI and socket adapter reach the session through Control.
    class AudioEngine : private juce::AudioIODeviceCallback, private juce::MidiInputCallback, private juce::Timer
    {
    public:
        /// Loads the plugins from `pluginDir` and opens the default audio output.
        /// Throws std::runtime_error if a plugin does not load or no audio output opens.
        explicit AudioEngine( const std::string& pluginDir, AudioConfiguration audio = {} );
        ~AudioEngine() override;

        Control& control() noexcept { return session_->control(); }

        /// Main-loop work: plugin callbacks, freeing retired graph plans, reopening MIDI
        /// inputs after devices change. Serviced by its own main-thread timer.
        void service();

    private:
        // Audio thread
        void audioDeviceIOCallbackWithContext( const float* const* inputs, int numInputs, float* const* outputs, int numOutputs,
                                               int numSamples, const juce::AudioIODeviceCallbackContext& ) override;
        void audioDeviceAboutToStart( juce::AudioIODevice* ) override;
        void audioDeviceStopped() override { started_=false; }
        // MIDI input threads
        void handleIncomingMidiMessage( juce::MidiInput*, const juce::MidiMessage& ) override;

        void push( const juce::MidiMessage& message );
        void openMidiInputs();
        void timerCallback() override { service(); }

        juce::AudioDeviceManager devices_;
        double                   sampleRate_ = 48000.0;
        static constexpr uint32_t maxFrames  = 512;

        std::unique_ptr<Session> session_;
        ChannelMap channelMap_;
        std::string initialDevice_;
        juce::StringArray initialInputs_,initialOutputs_;
        std::atomic<bool>        started_{ false }; // the device's rate matched the graph's

        // Physical MIDI to the audio thread: many producers, serialised by the mutex
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
