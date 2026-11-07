// SPDX-License-Identifier: AGPL-3.0-or-later
#include "AudioEngine.h"
#include "WebTransport.h"

#include "oscairn/midi_input.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <cstdio>
#include <stdexcept>

namespace oscairn::host
{
    AudioEngine::AudioEngine( const std::string& pluginDir, AudioConfiguration audio ) : channelMap_(std::move(audio),maxFrames)
    {
        useWebTransport(); // package repositories on the web (RFC 0031)
        juce::AudioDeviceManager::AudioDeviceSetup preferred;
        const auto& config=channelMap_.configuration();
        preferred.bufferSize = config.bufferFrames ? int(config.bufferFrames) : OSCAIRN_AUDIO_BUFFER_FRAMES;
        const auto inputs=ChannelMap::required(config.inputs),outputs=ChannelMap::required(config.outputs);
        preferred.inputDeviceName=config.inputDevice;preferred.outputDeviceName=config.outputDevice;
        preferred.useDefaultInputChannels=false;preferred.useDefaultOutputChannels=false;
        preferred.inputChannels.setRange(0,inputs,true);preferred.outputChannels.setRange(0,outputs,true);
        const auto error = devices_.initialise(inputs,outputs,nullptr,config.inputDevice.empty() && config.outputDevice.empty(),{},&preferred);
        auto*      device = devices_.getCurrentAudioDevice();
        if( error.isNotEmpty() || !device )
            throw std::runtime_error( "no audio output: " + ( error.isNotEmpty() ? error.toStdString() : std::string( "no device" ) ) );
        sampleRate_ = device->getCurrentSampleRate();
        std::fprintf( stderr, "oscairn: audio output \"%s\", %.0f Hz, %d-sample buffer\n", device->getName().toRawUTF8(), sampleRate_,
                      device->getCurrentBufferSizeSamples() );
        if( preferred.bufferSize > 0 && device->getCurrentBufferSizeSamples() != preferred.bufferSize )
            std::fprintf( stderr, "oscairn: requested %d frames; using the device's supported size above\n", preferred.bufferSize );
        initialDevice_=device->getName().toStdString();initialInputs_=device->getInputChannelNames();initialOutputs_=device->getOutputChannelNames();
        juce::AudioDeviceManager::AudioDeviceSetup actual;devices_.getAudioDeviceSetup(actual);
        if((!config.inputDevice.empty() && actual.inputDeviceName.toStdString()!=config.inputDevice) ||
           (!config.outputDevice.empty() && actual.outputDeviceName.toStdString()!=config.outputDevice))
            throw std::runtime_error("Requested audio device was not opened");
        audioDeviceAboutToStart(device);
        if(!started_)throw std::runtime_error("Requested audio channels are unavailable; no output fallback is permitted for explicit mappings");

        session_        = std::make_unique<Session>( sampleRate_, maxFrames, Lv2BufferLimits{}, channelMap_.layout() );
        const auto seq   = session_->add( "sequencer", pluginDir + "/oscairn-step-sequencer.clap" );
        const auto synth = session_->add( "synth", pluginDir + "/oscairn-basic-synth.clap" );
        const auto delay = session_->add( "delay", pluginDir + "/oscairn-delay.clap" );
        // The startup Performance, in parts like any other: a Synth part the keys play (the
        // Basic Synth into the Delay), and a Groovebox part whose step sequencer plays the same
        // synth. The nodes keep the names the Groovebox and the panel layouts bind to.
        const auto synthPart = session_->addPart( "Synth" );
        session_->insertIntoPart( synthPart, 0, synth );
        session_->insertIntoPart( synthPart, 1, delay );
        PartInput generates; generates.kind = PartInput::Kind::None;
        const auto groovebox = session_->addPart( "Groovebox", generates );
        session_->insertIntoPart( groovebox, 0, seq );
        session_->connectNotes( seq, synth );
        session_->commit();
        session_->clearHistory(); // undo starts from the startup instrument

        // RFC 0011's instant controls: a control's move that waits for the control thread wakes it.
        session_->setControllerWake( [this] { triggerAsyncUpdate(); } );
        openMidiInputs();
        openMidiOutputs();
        sender_.startThread();
        midiDevicesChanged_ = juce::MidiDeviceListConnection::make( [this] { midiDevicesDirty_ = true; } );
        devices_.addAudioCallback( this );
        service();
        startTimer( 5 ); // control ticks are independent of the panel's repaint rate
    }

    AudioEngine::~AudioEngine()
    {
        stopTimer();
        session_->setControllerWake( {} );
        cancelPendingUpdate();
        devices_.removeAudioCallback( this ); // waits for the callback to finish
        sender_.stopThread( 1000 );
        for( auto& input : midiInputs_ )
            input->stop();
        midiInputs_.clear();
        midiOutputs_.clear();
    }

    void AudioEngine::service()
    {
        session_->service();
        if( midiDevicesDirty_.exchange( false ) )
            { openMidiInputs(); openMidiOutputs(); }
        Control::SessionStatus status;
        if(auto* device = devices_.getCurrentAudioDevice())
        {
            status.audioDevice = device->getName().toStdString();
            status.sampleRate = device->getCurrentSampleRate();
            status.bufferSize = static_cast<uint32_t>(device->getCurrentBufferSizeSamples());
            auto describe=[](const auto& selected,const juce::StringArray& names,const juce::BigInteger& active){
                std::vector<Control::DeviceChannel> result;for(auto i:selected)result.push_back({i,i<static_cast<uint32_t>(names.size())?names[static_cast<int>(i)].toStdString():"Unavailable",active[static_cast<int>(i)]});return result;};
            status.audioInputs=describe(channelMap_.configuration().inputs,device->getInputChannelNames(),device->getActiveInputChannels());
            status.audioOutputs=describe(channelMap_.configuration().outputs,device->getOutputChannelNames(),device->getActiveOutputChannels());
            // ALSA's underruns and overruns, CoreAudio's overload notifications; -1 where JUCE has none.
            const auto xruns=device->getXRunCount();
            status.audioUnderruns=xruns>=0?uint64_t(xruns):lateCallbacks_.load(std::memory_order_relaxed);
        }
        status.audioReady=started_.load();
        status.cpuLoad = devices_.getCpuUsage();
        status.midiEventDrops=droppedMidi_.load()+droppedOut_.load();
        // shortcut: every desktop input is reported as USB; port and network kinds need the device's own input table (RFC 0012).
        for(size_t i = 0; i < midiInputs_.size(); ++i) status.midiInputs.push_back({MidiEndpoint::Kind::Usb, midiSources_[i], midiInputs_[i]->getName().toStdString()});
        for(size_t i = 0; i < midiOutputs_.size(); ++i) status.midiOutputs.push_back({MidiEndpoint::Kind::Usb, outputNumbers_[i], midiOutputs_[i]->getName().toStdString()});
        session_->setHostStatus(std::move(status));
        if( serviced_ ) serviced_();
    }

    void AudioEngine::openMidiOutputs()
    {
        std::vector<std::unique_ptr<juce::MidiOutput>> outputs;
        std::vector<uint8_t> numbers;
        for( const auto& info : juce::MidiOutput::getAvailableDevices() )
        {
            auto known = outputNumberOf_.find( info.identifier );
            if( known == outputNumberOf_.end() )
            {
                std::set<uint8_t> used; // the lowest free number, 1 to 63; a device keeps its number while the app runs
                for( const auto& [id, number] : outputNumberOf_ ) used.insert( number );
                uint8_t number = 1;
                while( number < maxSources && used.count( number ) ) ++number;
                if( number == maxSources ) continue; // shortcut: past 63 outputs ever seen, new ones are not opened until the app restarts
                known = outputNumberOf_.emplace( info.identifier, number ).first;
            }
            if( auto output = juce::MidiOutput::openDevice( info.identifier ) )
            {
                output->startBackgroundThread(); // sends each block's messages at their times
                outputs.push_back( std::move( output ) );
                numbers.push_back( known->second );
            }
        }
        std::lock_guard lock( outputLock_ );
        midiOutputs_.swap( outputs );
        outputNumbers_ = std::move( numbers );
    }

    void AudioEngine::queueMidiOut( uint32_t offset, double seconds ) noexcept
    {
        const auto messages = session_->midiOutput();
        if( messages.empty() ) return;
        const auto scope = outFifo_.write( (int)messages.size() );
        const auto written = size_t( scope.blockSize1 + scope.blockSize2 );
        for( size_t i = 0; i < written; ++i )
        {
            const auto& m = messages[i];
            auto& o = outgoing_[(size_t)( (int)i < scope.blockSize1 ? scope.startIndex1 + (int)i : scope.startIndex2 + (int)i - scope.blockSize1 )];
            o = { seconds * 1000 + ( offset + m.time ) * 1000 / sampleRate_ + outputLatencyMs_, m.ports, m.size, { m.bytes[0], m.bytes[1], m.bytes[2] } };
        }
        droppedOut_ += messages.size() - written;
    }

    void AudioEngine::Sender::run()
    {
        while( !threadShouldExit() )
        {
            {
                const auto scope = engine.outFifo_.read( engine.outFifo_.getNumReady() );
                std::lock_guard lock( engine.outputLock_ );
                auto send = [&]( int index ) {
                    const auto& o = engine.outgoing_[(size_t)index];
                    juce::MidiBuffer block;
                    block.addEvent( o.bytes, o.size, 0 );
                    for( size_t i = 0; i < engine.midiOutputs_.size(); ++i )
                        if( engine.outputNumbers_[i] < 64 && ( o.ports >> engine.outputNumbers_[i] & 1 ) )
                            engine.midiOutputs_[i]->sendBlockOfMessages( block, o.ms, engine.sampleRate_ );
                };
                for( int i = 0; i < scope.blockSize1; ++i ) send( scope.startIndex1 + i );
                for( int i = 0; i < scope.blockSize2; ++i ) send( scope.startIndex2 + i );
            }
            wait( 1 ); // well inside the audio output's latency, which every message is due after
        }
    }

    void AudioEngine::openMidiInputs()
    {
        {
            // Stopped outside the lock: a callback waiting for it must not block stop().
            std::vector<std::unique_ptr<juce::MidiInput>> old;
            {
                std::lock_guard lock( pushLock_ );
                old.swap( midiInputs_ );
                midiSources_.clear();
            }
            for( auto& input : old )
                input->stop();
        }
        std::vector<std::unique_ptr<juce::MidiInput>> inputs;
        std::vector<uint8_t> sources;
        for( const auto& info : juce::MidiInput::getAvailableDevices() )
        {
            auto known = sourceOf_.find( info.identifier );
            if( known == sourceOf_.end() )
            {
                // The lowest free number; past 63 devices, one of an absent device.
                std::set<uint8_t> used;
                for( const auto& [id, source] : sourceOf_ ) used.insert( source );
                uint8_t source = 1;
                while( source < maxSources && used.count( source ) ) ++source;
                if( source == maxSources )
                    for( auto it = sourceOf_.begin(); it != sourceOf_.end(); ++it )
                        if( std::none_of( sources.begin(), sources.end(), [&]( uint8_t s ) { return s == it->second; } ) )
                            { source = it->second; sourceOf_.erase( it ); break; }
                if( source == maxSources ) continue; // 63 inputs open at once
                known = sourceOf_.emplace( info.identifier, source ).first;
            }
            if( auto input = juce::MidiInput::openDevice( info.identifier, this ) )
            {
                inputs.push_back( std::move( input ) );
                sources.push_back( known->second );
            }
        }
        {
            std::lock_guard lock( pushLock_ );
            midiInputs_ = std::move( inputs );
            midiSources_ = std::move( sources );
        }
        for( auto& input : midiInputs_ )
            input->start();
    }

    void AudioEngine::handleIncomingMidiMessage( juce::MidiInput* input, const juce::MidiMessage& message )
    {
        kick::clap::Event converted; // channel messages pass if the block will take them (midiToClap)
        if( oscairn::midiToClap( message.getRawData(), (size_t)message.getRawDataSize(), 0, converted ) || message.isMidiClock() || message.isMidiStart() || message.isMidiContinue() || message.isMidiStop()
            || message.isSongPositionPointer() )
        {
            const auto source = push( message, input );
            // A controller's control (RFC 0011's instant controls), mapped here on the MIDI thread.
            if( message.isController() || message.isNoteOnOrOff() || message.isPitchWheel() || message.isChannelPressure() )
                session_->controllerMidi( source, message.getRawData(), (size_t)message.getRawDataSize() );
        }
    }

    uint8_t AudioEngine::push( const juce::MidiMessage& message, const juce::MidiInput* input )
    {
        Incoming in{};
        const auto size = std::min( message.getRawDataSize(), 3 );
        std::copy_n( message.getRawData(), size, in.bytes );
        in.size = (uint8_t)size;
        in.source = untaggedSource;
        in.seconds = juce::Time::getMillisecondCounterHiRes() * 0.001;

        std::lock_guard lock( pushLock_ );
        for( size_t i = 0; i < midiInputs_.size(); ++i )
            if( midiInputs_[i].get() == input ) in.source = midiSources_[i];
        const auto      scope = fifo_.write( 1 );
        if( scope.blockSize1 + scope.blockSize2 == 0 )
            {++droppedMidi_;midiOverflow_.store(true,std::memory_order_release);}
        else
            incoming_[(size_t)( scope.blockSize1 > 0 ? scope.startIndex1 : scope.startIndex2 )] = in;
        return in.source;
    }

    void AudioEngine::audioDeviceAboutToStart( juce::AudioIODevice* device )
    {
        const auto& config=channelMap_.configuration();
        const auto inputMask=device->getActiveInputChannels(),outputMask=device->getActiveOutputChannels();
        bool valid=std::abs(device->getCurrentSampleRate()-sampleRate_)<.5 && device->getName().toStdString()==initialDevice_ &&
            device->getInputChannelNames()==initialInputs_ && device->getOutputChannelNames()==initialOutputs_;
        for(uint32_t i=0;i<ChannelMap::required(config.inputs);++i)valid=valid && inputMask[static_cast<int>(i)];
        const auto required=(!config.explicitOutputs && initialOutputs_.size()==1)?1:ChannelMap::required(config.outputs);
        for(uint32_t i=0;i<required;++i)valid=valid && outputMask[static_cast<int>(i)];
        outputLatencyMs_=(device->getOutputLatencyInSamples()+device->getCurrentBufferSizeSamples())*1000.0/device->getCurrentSampleRate();
        lastCallback_=0;lateCallbacks_=0;
        started_=valid;
    }

    void AudioEngine::audioDeviceIOCallbackWithContext( const float* const* inputs, int numInputs, float* const* outputs, int numOutputs, int numSamples,
                                                   const juce::AudioIODeviceCallbackContext& )
    {
        if(numSamples<=0)return;
        const double now=juce::Time::getMillisecondCounterHiRes()*0.001;
        if(lastCallback_>0 && now-lastCallback_>2.0*numSamples/sampleRate_)lateCallbacks_.fetch_add(1,std::memory_order_relaxed);
        lastCallback_=now;
        if(!started_ || numOutputs<1)
        {for(int c=0;c<numOutputs;++c)if(outputs[c])std::fill_n(outputs[c],numSamples,0.f);return;}
        // Device timestamps belong to this adapter. The core receives sample times,
        // with the existing policy: notes now, clock timestamps in the preceding block.
        uint32_t clockCount = 0;notes_.reset();
        const bool inputOverflow=midiOverflow_.exchange(false,std::memory_order_acquire);
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
                    if( clockCount < clocks_.size() )
                        clocks_[clockCount++] = { status, time, (uint16_t)( in.bytes[1] | ( in.bytes[2] << 7 ) ) };
                    return;
                }
                // Notes, aftertouch (as CLAP pressure), pitch bend and the performance controllers.
                notes_.append(in.bytes,in.size,in.source);
            };
            for( int i = 0; i < scope.blockSize1; ++i )
                take( scope.startIndex1 + i );
            for( int i = 0; i < scope.blockSize2; ++i )
                take( scope.startIndex2 + i );
        }
        droppedMidi_+=notes_.dropped;
        const bool panic=inputOverflow || notes_.dropped;
        if(panic){droppedMidi_+=notes_.count;notes_.count=0;}
        channelMap_.process(inputs,static_cast<uint32_t>(numInputs),outputs,static_cast<uint32_t>(numOutputs),static_cast<uint32_t>(numSamples),
            [&](const float* const* in,uint32_t ni,float* const* out,uint32_t no,uint32_t frames,uint32_t offset){
                if(offset==0 && panic)session_->panicInput();
                const bool rendered=session_->process(in,ni,out,no,frames,{notes_.events.data(),offset==0?notes_.count:0},{clocks_.data(),offset==0?clockCount:0},
                    {notes_.sources.data(),offset==0?notes_.count:0});
                queueMidiOut(offset,now);
                return rendered;
            });
        blockSeconds_   = now;
        lastBlockStart_ = blockSample_;
        blockSample_ += numSamples;
    }
}
