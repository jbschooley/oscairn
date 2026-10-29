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

    AudioEngine::AudioEngine( const std::string& pluginDir, AudioConfiguration audio ) : channelMap_(std::move(audio),maxFrames)
    {
        juce::AudioDeviceManager::AudioDeviceSetup preferred;
        preferred.bufferSize = OSCAIRN_AUDIO_BUFFER_FRAMES;
        const auto& config=channelMap_.configuration();
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
            auto describe=[](const auto& selected,const juce::StringArray& names,const juce::BigInteger& active){
                std::vector<Control::DeviceChannel> result;for(auto i:selected)result.push_back({i,i<static_cast<uint32_t>(names.size())?names[static_cast<int>(i)].toStdString():"Unavailable",active[static_cast<int>(i)]});return result;};
            status.audioInputs=describe(channelMap_.configuration().inputs,device->getInputChannelNames(),device->getActiveInputChannels());
            status.audioOutputs=describe(channelMap_.configuration().outputs,device->getOutputChannelNames(),device->getActiveOutputChannels());
        }
        status.audioReady=started_.load();
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
        if( message.isNoteOnOrOff() || (message.isController() && (message.getControllerNumber()==64 || message.getControllerNumber()==120 || message.getControllerNumber()==123)) || message.isAftertouch() || message.isChannelPressure() || message.isMidiClock() || message.isMidiStart() || message.isMidiContinue() || message.isMidiStop()
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
        const auto& config=channelMap_.configuration();
        const auto inputMask=device->getActiveInputChannels(),outputMask=device->getActiveOutputChannels();
        bool valid=std::abs(device->getCurrentSampleRate()-sampleRate_)<.5 && device->getName().toStdString()==initialDevice_ &&
            device->getInputChannelNames()==initialInputs_ && device->getOutputChannelNames()==initialOutputs_;
        for(uint32_t i=0;i<ChannelMap::required(config.inputs);++i)valid=valid && inputMask[static_cast<int>(i)];
        const auto required=(!config.explicitOutputs && initialOutputs_.size()==1)?1:ChannelMap::required(config.outputs);
        for(uint32_t i=0;i<required;++i)valid=valid && outputMask[static_cast<int>(i)];
        started_=valid;
    }

    void AudioEngine::audioDeviceIOCallbackWithContext( const float* const* inputs, int numInputs, float* const* outputs, int numOutputs, int numSamples,
                                                   const juce::AudioIODeviceCallbackContext& )
    {
        if(numSamples<=0)return;
        const double now=juce::Time::getMillisecondCounterHiRes()*0.001;
        if(!started_ || numOutputs<1)
        {for(int c=0;c<numOutputs;++c)if(outputs[c])std::fill_n(outputs[c],numSamples,0.f);return;}
        // Device timestamps belong to this adapter. The core receives sample times,
        // with the existing policy: notes now, clock timestamps in the preceding block.
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
                    if( clockCount < clocks_.size() )
                        clocks_[clockCount++] = { status, time, (uint16_t)( in.bytes[1] | ( in.bytes[2] << 7 ) ) };
                    return;
                }
                // Notes and aftertouch (as CLAP pressure).
                if( noteCount < notes_.size() && oscairn::midiToClap( in.bytes, in.size, 0, notes_[noteCount] ) )
                    ++noteCount;
            };
            for( int i = 0; i < scope.blockSize1; ++i )
                take( scope.startIndex1 + i );
            for( int i = 0; i < scope.blockSize2; ++i )
                take( scope.startIndex2 + i );
        }
        channelMap_.process(inputs,static_cast<uint32_t>(numInputs),outputs,static_cast<uint32_t>(numOutputs),static_cast<uint32_t>(numSamples),
            [&](const float* const* in,uint32_t ni,float* const* out,uint32_t no,uint32_t frames,uint32_t offset){
                return session_->process(in,ni,out,no,frames,{notes_.data(),offset==0?noteCount:0},{clocks_.data(),offset==0?clockCount:0});
            });
        blockSeconds_   = now;
        lastBlockStart_ = blockSample_;
        blockSample_ += numSamples;
    }
}
