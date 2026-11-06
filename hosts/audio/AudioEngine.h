// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "oscairn/session.h"
#include "ChannelMap.h"
#include "MidiBlock.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <functional>
#include <map>
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
    class AudioEngine : private juce::AudioIODeviceCallback, private juce::MidiInputCallback, private juce::Timer, private juce::AsyncUpdater
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
        /// Called on the main thread after service() and after a control's move reached the model:
        /// the engine sends what changed then, not on a timer of its own.
        void setServiced( std::function<void()> serviced ) { serviced_ = std::move( serviced ); }

    private:
        // Audio thread
        void audioDeviceIOCallbackWithContext( const float* const* inputs, int numInputs, float* const* outputs, int numOutputs,
                                               int numSamples, const juce::AudioIODeviceCallbackContext& ) override;
        void audioDeviceAboutToStart( juce::AudioIODevice* ) override;
        void audioDeviceStopped() override { started_=false; }
        // MIDI input threads
        void handleIncomingMidiMessage( juce::MidiInput*, const juce::MidiMessage& ) override;

        uint8_t push( const juce::MidiMessage& message, const juce::MidiInput* input ); // the input's source number
        void handleAsyncUpdate() override { session_->serviceControllers(); if( serviced_ ) serviced_(); } // a control moved: its model, at once
        void openMidiInputs();
        void openMidiOutputs();
        void queueMidiOut( uint32_t offset, double seconds ) noexcept; // audio thread: the session's MIDI out, for the sender
        void timerCallback() override { service(); }

        juce::AudioDeviceManager devices_;
        double                   sampleRate_ = 48000.0;
        static constexpr uint32_t maxFrames  = 512;

        std::unique_ptr<Session> session_;
        std::function<void()> serviced_;
        ChannelMap channelMap_;
        std::string initialDevice_;
        juce::StringArray initialInputs_,initialOutputs_;
        std::atomic<bool>        started_{ false }; // the device's rate matched the graph's

        // Physical MIDI to the audio thread: many producers, serialized by the mutex
        // (never taken on the audio thread), one consumer.
        struct Incoming
        {
            uint8_t bytes[3];
            uint8_t size; // bytes used
            uint8_t source; // the input's source number (note_zone.h)
            double  seconds; // Time::getMillisecondCounterHiRes() / 1000 at arrival
        };
        std::mutex                    pushLock_;
        juce::AbstractFifo            fifo_{ 1024 };
        std::array<Incoming, 1024>    incoming_{};
        std::atomic<uint64_t>         droppedMidi_{ 0 };
        std::atomic<bool> midiOverflow_{false};

        // Audio thread preallocated event buffers:
        MidiBlock notes_;
        std::array<Session::MidiClockEvent, 1024> clocks_{};

        // Audio thread: when the last block started (seconds and sample), and where the
        // next one starts, on the transport's timeline. Places MIDI clock in time.
        double  blockSeconds_   = 0.0;
        int64_t lastBlockStart_ = 0, blockSample_ = 0;

        // MIDI inputs, main thread; each with its source number. The input threads read both
        // under pushLock_, which the main thread holds to change them. A device keeps its
        // number while the app runs, so selectors resolved before it was unplugged still match.
        std::vector<std::unique_ptr<juce::MidiInput>> midiInputs_;
        std::vector<uint8_t>                          midiSources_;
        std::map<juce::String, uint8_t>               sourceOf_; // by device identifier
        juce::MidiDeviceListConnection                midiDevicesChanged_;

        // MIDI outputs (RFC 0012), each numbered by device like the inputs, apart from them. The
        // audio thread queues each message with the wall-clock time it is due (its sample, plus the
        // audio output's latency, so it sounds with the block it belongs to); the sender thread
        // hands it to JUCE's output, which sends it then. The lock is the sender's and the main
        // thread's (reopening), never the audio thread's.
        struct Outgoing { double ms; uint64_t ports; uint8_t size; uint8_t bytes[3]; };
        juce::AbstractFifo                            outFifo_{ 4096 };
        std::array<Outgoing, 4096>                    outgoing_{};
        std::atomic<uint64_t>                         droppedOut_{ 0 };
        double                                        outputLatencyMs_ = 0; // set before the device starts
        std::mutex                                    outputLock_;
        std::vector<std::unique_ptr<juce::MidiOutput>> midiOutputs_; // started, each with its number
        std::vector<uint8_t>                          outputNumbers_;
        std::map<juce::String, uint8_t>               outputNumberOf_; // by device identifier
        struct Sender : juce::Thread
        {
            explicit Sender( AudioEngine& e ) : juce::Thread( "oscairn MIDI out" ), engine( e ) {}
            void run() override;
            AudioEngine& engine;
        } sender_{ *this };
        std::atomic<bool>                             midiDevicesDirty_{ false };
    };
}
