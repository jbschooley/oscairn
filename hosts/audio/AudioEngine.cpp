// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AudioEngine.h"

#include "oscairn/midi_input.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace oscairn::host
{
    namespace
    {
        constexpr clap_id brightnessParam = 1; // plugins/basic-synth
    }

    AudioEngine::AudioEngine( const std::string& pluginDir )
    {
        juce::AudioDeviceManager::AudioDeviceSetup preferred;
        preferred.bufferSize = OSCAIRN_AUDIO_BUFFER_FRAMES;
        const auto error = devices_.initialise( 0, 2, nullptr, true, {}, &preferred );
        auto*      device = devices_.getCurrentAudioDevice();
        if( error.isNotEmpty() || !device )
            throw std::runtime_error( "no audio output: " + ( error.isNotEmpty() ? error.toStdString() : std::string( "no device" ) ) );
        sampleRate_ = device->getCurrentSampleRate();
        std::fprintf( stderr, "oscairn: audio output \"%s\", %.0f Hz, %d-sample buffer\n", device->getName().toRawUTF8(), sampleRate_,
                      device->getCurrentBufferSizeSamples() );
        if( preferred.bufferSize > 0 && device->getCurrentBufferSizeSamples() != preferred.bufferSize )
            std::fprintf( stderr, "oscairn: requested %d frames; using the device's supported size above\n", preferred.bufferSize );
        scratch_.resize( maxFrames );

        session_        = std::make_unique<Session>( sampleRate_, maxFrames );
        const auto seq   = session_->add( "sequencer", pluginDir + "/oscairn-step-sequencer.clap" );
        const auto synth = session_->add( "synth", pluginDir + "/oscairn-basic-synth.clap" );
        const auto delay = session_->add( "delay", pluginDir + "/oscairn-delay.clap" );
        session_->connectNotes( seq, synth );
        session_->connectNotes( graphInput, synth );
        session_->connectAudio( synth, delay );
        session_->connectAudio( delay, graphOutput );
        session_->mapParameter( seq, 0, synth, brightnessParam ); // Lock A opens the filter
        session_->commit();

        openMidiInputs();
        midiDevicesChanged_ = juce::MidiDeviceListConnection::make( [this] { midiDevicesDirty_ = true; } );
        devices_.addAudioCallback( this );
        service();
        startTimer( 5 ); // control ticks are independent of the panel's repaint rate
    }

    AudioEngine::~AudioEngine()
    {
        stopTimer();
        devices_.removeAudioCallback( this ); // waits for the callback to finish
        for( auto& input : midiInputs_ )
            input->stop();
        midiInputs_.clear();
    }

    void AudioEngine::service()
    {
        session_->service();
        if( midiDevicesDirty_.exchange( false ) )
            openMidiInputs();
        Control::SessionStatus status;
        if(auto* device = devices_.getCurrentAudioDevice())
        {
            status.audioDevice = device->getName().toStdString();
            status.sampleRate = device->getCurrentSampleRate();
            status.bufferSize = static_cast<uint32_t>(device->getCurrentBufferSizeSamples());
        }
        status.cpuLoad = devices_.getCpuUsage();
        for(const auto& input : midiInputs_) status.midiInputs.push_back(input->getName().toStdString());
        session_->setHostStatus(std::move(status));
    }

    void AudioEngine::openMidiInputs()
    {
        for( auto& input : midiInputs_ )
            input->stop();
        midiInputs_.clear();
        for( const auto& info : juce::MidiInput::getAvailableDevices() )
            if( auto input = juce::MidiInput::openDevice( info.identifier, this ) )
            {
                input->start();
                midiInputs_.push_back( std::move( input ) );
            }
    }

    void AudioEngine::handleIncomingMidiMessage( juce::MidiInput*, const juce::MidiMessage& message )
    {
        if( message.isNoteOnOrOff() || message.isAftertouch() || message.isChannelPressure() || message.isMidiClock() || message.isMidiStart() || message.isMidiContinue() || message.isMidiStop()
            || message.isSongPositionPointer() )
            push( message );
    }

    void AudioEngine::push( const juce::MidiMessage& message )
    {
        Incoming in{};
        const auto size = std::min( message.getRawDataSize(), 3 );
        std::copy_n( message.getRawData(), size, in.bytes );
        in.size = (uint8_t)size;
        in.seconds = juce::Time::getMillisecondCounterHiRes() * 0.001;

        std::lock_guard lock( pushLock_ );
        const auto      scope = fifo_.write( 1 );
        if( scope.blockSize1 + scope.blockSize2 == 0 )
            ++droppedMidi_;
        else
            incoming_[(size_t)( scope.blockSize1 > 0 ? scope.startIndex1 : scope.startIndex2 )] = in;
    }

    void AudioEngine::audioDeviceAboutToStart( juce::AudioIODevice* device )
    {
        // The graph is built for the rate the device had at startup. Another rate plays
        // silence rather than out of tune.
        started_ = std::abs( device->getCurrentSampleRate() - sampleRate_ ) < 0.5;
    }

    void AudioEngine::audioDeviceIOCallbackWithContext( const float* const*, int, float* const* outputs, int numOutputs, int numSamples,
                                                   const juce::AudioIODeviceCallbackContext& )
    {
        for( int c = 0; c < numOutputs; ++c )
            if( outputs[c] )
                std::fill_n( outputs[c], numSamples, 0.0f );
        const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
        if( !started_ || numOutputs < 1 || !outputs[0] )
            return;
        // Device timestamps belong to this adapter. The core receives sample times,
        // with the existing policy: notes now, clock timestamps in the preceding block.
        std::array<kick::clap::Event, 128> notes;
        std::array<Session::MidiClockEvent, 1024> clocks;
        uint32_t noteCount = 0, clockCount = 0;
        {
            const auto scope = fifo_.read( fifo_.getNumReady() );
            auto       take  = [&]( int index ) {
                const auto&   in     = incoming_[(size_t)index];
                const uint8_t status = in.bytes[0];
                if( status >= 0xF0 )
                {
                    if( blockSample_ == lastBlockStart_ )
                        return;
                    const auto offset = (int64_t)std::llround( ( in.seconds - blockSeconds_ ) * sampleRate_ );
                    const auto time   = std::clamp<int64_t>( lastBlockStart_ + offset, lastBlockStart_, blockSample_ - 1 );
                    clocks[clockCount++] = { status, time, (uint16_t)( in.bytes[1] | ( in.bytes[2] << 7 ) ) };
                    return;
                }
                // Notes and aftertouch (as CLAP pressure).
                if( noteCount < notes.size() && oscairn::midiToClap( in.bytes, in.size, 0, notes[noteCount] ) )
                    ++noteCount;
            };
            for( int i = 0; i < scope.blockSize1; ++i )
                take( scope.startIndex1 + i );
            for( int i = 0; i < scope.blockSize2; ++i )
                take( scope.startIndex2 + i );
        }
        float* right = numOutputs >= 2 && outputs[1] ? outputs[1] : scratch_.data();
        for( int done = 0; done < numSamples; )
        {
            const auto frames = (uint32_t)std::min<int>( (int)maxFrames, numSamples - done );
            float* out[] = { outputs[0] + done, right == scratch_.data() ? scratch_.data() : right + done };
            session_->process( out, frames,
                               { notes.data(), done == 0 ? noteCount : 0 },
                               { clocks.data(), done == 0 ? clockCount : 0 } );
            done += (int)frames;
        }
        blockSeconds_   = now;
        lastBlockStart_ = blockSample_;
        blockSample_ += numSamples;
    }
}
