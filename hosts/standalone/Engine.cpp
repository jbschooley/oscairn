// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Engine.h"

#include "oscairn/midi_input.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace oscairn::standalone
{
    namespace
    {
        constexpr clap_id brightnessParam = 1; // plugins/basic-synth
        constexpr double  stepBeats       = 0.25;
        constexpr int     steps           = 16;
    }

    Engine::Engine( const std::string& pluginDir )
    {
        const auto error = devices_.initialiseWithDefaultDevices( 0, 2 );
        auto*      device = devices_.getCurrentAudioDevice();
        if( error.isNotEmpty() || !device )
            throw std::runtime_error( "no audio output: " + ( error.isNotEmpty() ? error.toStdString() : std::string( "no device" ) ) );
        sampleRate_ = device->getCurrentSampleRate();
        std::fprintf( stderr, "oscairn: audio output \"%s\", %.0f Hz, %d-sample buffer\n", device->getName().toRawUTF8(), sampleRate_,
                      device->getCurrentBufferSizeSamples() );
        transport_.emplace( sampleRate_, tempo_ );
        transport_->setPlaying( false );
        scratch_.resize( maxFrames );

        graph_          = std::make_unique<Graph>( sampleRate_, maxFrames );
        const auto seq   = graph_->add( pluginDir + "/oscairn-step-sequencer.clap" );
        const auto synth = graph_->add( pluginDir + "/oscairn-basic-synth.clap" );
        const auto delay = graph_->add( pluginDir + "/oscairn-delay.clap" );
        nodes_           = { { "sequencer", seq }, { "synth", synth }, { "delay", delay } };
        graph_->connectNotes( seq, synth );
        graph_->connectNotes( graphInput, synth );
        graph_->connectAudio( synth, delay );
        graph_->connectAudio( delay, graphOutput );
        graph_->mapParamTarget( seq, 0, synth, brightnessParam ); // Lock A opens the filter
        graph_->commit();

        openMidiInputs();
        midiDevicesChanged_ = juce::MidiDeviceListConnection::make( [this] { midiDevicesDirty_ = true; } );
        devices_.addAudioCallback( this );
    }

    Engine::~Engine()
    {
        devices_.removeAudioCallback( this ); // waits for the callback to finish
        for( auto& input : midiInputs_ )
            input->stop();
        midiInputs_.clear();
    }

    NodeId Engine::nodeOf( const std::string& name ) const
    {
        const auto it = nodes_.find( name );
        if( it == nodes_.end() )
            throw std::invalid_argument( "no node " + name );
        return it->second;
    }

    bool Engine::hasParam( const std::string& node, uint32_t param ) const
    {
        const auto it = nodes_.find( node );
        if( it == nodes_.end() )
            return false;
        const auto& params = graph_->host( it->second ).parameters();
        return std::any_of( params.begin(), params.end(), [&]( const kick::clap::Parameter& p ) { return p.id == param; } );
    }

    Engine::ParamInfo Engine::paramInfo( const std::string& node, uint32_t param ) const
    {
        for( const auto& p : graph_->host( nodeOf( node ) ).parameters() )
            if( p.id == param )
                return { p.name, p.minimum, p.maximum, p.defaultValue, ( p.flags & CLAP_PARAM_IS_STEPPED ) != 0 };
        throw std::invalid_argument( node + " has no parameter " + std::to_string( param ) );
    }

    double Engine::param( const std::string& node, uint32_t param ) const
    {
        return graph_->host( nodeOf( node ) ).parameterValue( param );
    }

    std::string Engine::paramText( const std::string& node, uint32_t param, double value ) const
    {
        return graph_->host( nodeOf( node ) ).parameterText( param, value );
    }

    void Engine::setParam( const std::string& node, uint32_t param, double value )
    {
        graph_->host( nodeOf( node ) ).setParameter( param, value );
    }

    void Engine::note( bool on, int key, float velocity )
    {
        push( on ? juce::MidiMessage::noteOn( 1, key, velocity ) : juce::MidiMessage::noteOff( 1, key ) );
    }

    Engine::Status Engine::status() const
    {
        Status s;
        s.playing = statusPlaying_;
        s.tempo   = statusTempo_;
        s.step    = statusStep_;
        if( auto* device = devices_.getCurrentAudioDevice() )
            s.audioDevice = device->getName().toStdString();
        for( const auto& input : midiInputs_ )
            s.midiInputs.push_back( input->getName().toStdString() );
        return s;
    }

    void Engine::service()
    {
        for( const auto& [name, node] : nodes_ )
            graph_->host( node ).service();
        graph_->collect();
        if( midiDevicesDirty_.exchange( false ) )
            openMidiInputs();
    }

    void Engine::openMidiInputs()
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

    void Engine::handleIncomingMidiMessage( juce::MidiInput*, const juce::MidiMessage& message )
    {
        if( message.isNoteOnOrOff() || message.isAftertouch() || message.isChannelPressure() || message.isMidiClock() || message.isMidiStart() || message.isMidiContinue() || message.isMidiStop()
            || message.isSongPositionPointer() )
            push( message );
    }

    void Engine::push( const juce::MidiMessage& message )
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

    void Engine::audioDeviceAboutToStart( juce::AudioIODevice* device )
    {
        // The graph is built for the rate the device had at startup. Another rate plays
        // silence rather than out of tune.
        started_ = std::abs( device->getCurrentSampleRate() - sampleRate_ ) < 0.5;
    }

    void Engine::audioDeviceIOCallbackWithContext( const float* const*, int, float* const* outputs, int numOutputs, int numSamples,
                                                   const juce::AudioIODeviceCallbackContext& )
    {
        for( int c = 0; c < numOutputs; ++c )
            if( outputs[c] )
                std::fill_n( outputs[c], numSamples, 0.0f );
        const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
        if( !started_ || numOutputs < 1 || !outputs[0] )
            return;
        auto& transport = *transport_;

        // Messages that arrived while the last block played: clock messages are placed at
        // their time within it, notes play at the start of this one.
        const bool follow = followClock_;
        if( follow != following_ ) // switching resets the clock state, so only on a change
        {
            transport.setClockSource( follow ? Transport::ClockSource::MidiClock : Transport::ClockSource::Internal );
            following_ = follow;
        }
        std::array<kick::clap::Event, 128> notes;
        uint32_t                           noteCount = 0;
        {
            const auto scope = fifo_.read( fifo_.getNumReady() );
            auto       take  = [&]( int index ) {
                const auto&   in     = incoming_[(size_t)index];
                const uint8_t status = in.bytes[0];
                if( status >= 0xF0 )
                {
                    if( !follow || blockSample_ == lastBlockStart_ )
                        return;
                    const auto offset = (int64_t)std::llround( ( in.seconds - blockSeconds_ ) * sampleRate_ );
                    const auto time   = std::clamp<int64_t>( lastBlockStart_ + offset, lastBlockStart_, blockSample_ - 1 );
                    transport.midiClock( status, time, (uint16_t)( in.bytes[1] | ( in.bytes[2] << 7 ) ) );
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
        if( !follow )
        {
            transport.setTempo( tempo_ );
            transport.setPlaying( playing_ );
        }

        float* right = numOutputs >= 2 && outputs[1] ? outputs[1] : scratch_.data();
        for( int done = 0; done < numSamples; )
        {
            const auto frames = (uint32_t)std::min<int>( (int)maxFrames, numSamples - done );
            const auto t      = transport.current();
            if( done == 0 )
            {
                statusPlaying_ = ( t.flags & CLAP_TRANSPORT_IS_PLAYING ) != 0;
                statusTempo_   = t.tempo;
                const auto beat = (double)t.song_pos_beats / (double)CLAP_BEATTIME_FACTOR;
                statusStep_     = statusPlaying_ ? (int)( (int64_t)std::floor( beat / stepBeats + 1e-9 ) % steps ) : -1;
            }
            float* out[] = { outputs[0] + done, right == scratch_.data() ? scratch_.data() : right + done };
            graph_->process( out, frames, &t, done == 0 ? notes.data() : nullptr, done == 0 ? noteCount : 0 );
            transport.advance( frames );
            done += (int)frames;
        }
        blockSeconds_   = now;
        lastBlockStart_ = blockSample_;
        blockSample_ += numSamples;
    }
}
