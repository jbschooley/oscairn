// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Engine.h"
#include "../audio/AudioEngine.h"
#include "EngineProcess.h"
#include <bit>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace oscairn::standalone
{
    Engine::Engine(const std::string& pluginDir,const std::string& socketPath,const std::string& helperExecutable,const host::AudioConfiguration& audio):socketPath_(socketPath)
    {
        if(socketPath.empty() && helperExecutable.empty())
        {
            audio_=std::make_unique<host::AudioEngine>(pluginDir,audio);control_=&audio_->control();
        }
        else
        {
            client_=std::make_unique<LocalClient>();
            if(!helperExecutable.empty())
            {
                process_=std::make_unique<host::EngineProcess>(helperExecutable,pluginDir,std::string{},audio);
                process_->connectOrStart(*client_);socketPath_=process_->socketPath();
            }
            else client_->connect(socketPath);
            remote_=std::make_unique<RemoteControl>(*client_);control_=remote_.get();
        }
        refreshSounds();
        startTimer(16);
    }
    Engine::~Engine(){stopTimer();pendingSound_.reset();if(soundJob_.valid()){soundJob_.wait();finishSounds();}}
    bool Engine::hasParam(const std::string& n,uint32_t p)const
    {try{return control_->hasParameter(control_->nodeId(n),p);}catch(const std::invalid_argument&){return false;}}
    Engine::ParamInfo Engine::paramInfo(const std::string& n,uint32_t p)const{return control_->parameterInfo(control_->nodeId(n),p);}
    double Engine::param(const std::string& n,uint32_t p)const
    {
        try{return control_->parameter(control_->nodeId(n),p);}
        catch(const std::invalid_argument&){return 0;} // another client may remove a displayed node
    }
    std::string Engine::paramText(const std::string& n,uint32_t p,double v)const
    {
        const auto key=std::make_pair(n,p);const auto it=textCache_.find(key);
        const auto bits=std::bit_cast<uint64_t>(v); // reuse formatting only for the exact cached value
        if(it!=textCache_.end() && it->second.first==bits)return it->second.second;
        if(soundJob_.valid())return std::to_string(v);
        try{auto text=control_->parameterText(control_->nodeId(n),p,v);textCache_[key]={bits,text};return text;}
        catch(const std::exception&){return std::to_string(v);}
    }
    void Engine::setParam(const std::string& n,uint32_t p,double v)
    {command([this,n,p,v]{if(!control_->setParameter(control_->nodeId(n),p,v))std::fprintf(stderr,"oscairn: parameter command refused\n");});}
    void Engine::paramGesture(const std::string& n,uint32_t p,bool begin)
    {command([this,n,p,begin]{if(!control_->parameterGesture(control_->nodeId(n),p,begin))std::fprintf(stderr,"oscairn: gesture command refused\n");});}
    void Engine::setPlaying(bool v){command([this,v]{control_->setPlaying(v);});}
    void Engine::setTempo(double v){command([this,v]{control_->setTempo(v);});}
    void Engine::setFollowMidiClock(bool v){command([this,v]{control_->setClockSource(v?Transport::ClockSource::MidiClock:Transport::ClockSource::Internal);});}
    bool Engine::followsMidiClock()const{return control_->clockSource()==Transport::ClockSource::MidiClock;}
    void Engine::note(bool on,int key,float velocity){command([this,on,key,velocity]{if(!control_->note(on,key,velocity))std::fprintf(stderr,"oscairn: note queue overflow\n");});}

    void Engine::soundAction(SoundAction action)
    {
        if(soundBusy() || !connected())return;
        if(action.kind==SoundAction::Prepare && !canPrepareSounds())
        {
            soundMessage_="Preparation requires the separate engine. Relaunch without --in-process.";
            return;
        }
        soundMessage_.clear();
        if(soundJob_.valid()){pendingSound_=std::move(action);return;}
        refreshSounds(std::move(action));
    }

    void Engine::refreshSounds(SoundAction action)
    {
        if(socketPath_.empty())
        {
            try{applySoundAction(*control_,action);soundPicker_=readSoundPicker(*control_);}
            catch(const std::exception& e){soundMessage_=e.what();}
            return;
        }
        // A separate client belongs entirely to this worker; the UI's cached client
        // is not serviced until preparation finishes, keeping long RPC waits off the UI.
        soundJobKind_=action.kind;
        soundJob_=std::async(std::launch::async,[socket=socketPath_,request=std::move(action)] {
            SoundResult result;LocalClient client;
            try
            {
                client.connect(socket);RemoteControl control(client);
                try{applySoundAction(control,request);}catch(const std::exception& e){result.error=e.what();}
                result.state=readSoundPicker(control);
            }
            catch(const std::exception& e){result.error=e.what();result.disconnected=!client.connected();}
            return result;
        });
    }

    void Engine::finishSounds()
    {
        auto result=soundJob_.get();
        if(!result.error.empty())soundMessage_=result.error;
        soundPicker_=std::move(result.state);
        if(result.disconnected)
        {
            pendingControls_.clear();
            failed(result.error);
            return;
        }
        auto pending=std::move(pendingControls_);pendingControls_.clear();
        for(auto& action:pending)command(std::move(action));
        if(pendingSound_ && connected())
        {
            auto action=std::move(*pendingSound_);pendingSound_.reset();refreshSounds(std::move(action));
        }
    }
    Engine::Status Engine::status()const
    {
        const auto transport=control_->transportState();const auto host=control_->sessionStatus();
        return {connected() && transport.playing,transport.tempo,connected() && transport.playing?static_cast<int>(static_cast<int64_t>(std::floor(transport.beat/.25+1e-9))%16):-1,
                failure_.empty()?(host.audioReady?host.audioDevice:"Audio unavailable: "+host.audioDevice):"Disconnected: "+failure_,host.midiInputs};
    }
    void Engine::failed(const std::string& message)
    {
        pendingControls_.clear();
        pendingSound_.reset();
        failure_=message;std::fprintf(stderr,"oscairn: control connection: %s\n",message.c_str());
        if(client_)client_->disconnect();
    }
    void Engine::restart()
    {
        if(!process_ || !recoveryAvailable_)return;
        try
        {
            process_->connectOrStart(*client_);
            auto fresh=std::make_unique<RemoteControl>(*client_);
            remote_=std::move(fresh);control_=remote_.get();failure_.clear();textCache_.clear();recoveryAvailable_=false;
        }
        catch(const std::exception& e){failed(e.what());}
    }
    void Engine::timerCallback()
    {
        if(soundJob_.valid())
        {
            if(soundJob_.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return;
            finishSounds();
            if(soundJob_.valid())return;
        }
        if(!remote_)
        {
            if(++soundTicks_>=30){soundTicks_=0;refreshSounds();}
            return;
        }
        if(failure_.empty())
        {
            try{remote_->service();}catch(const std::exception& e){failed(e.what());}
            if(failure_.empty() && ++soundTicks_>=30){soundTicks_=0;refreshSounds();}
        }
        else if(++reconnectTicks_>=60)
        {
            reconnectTicks_=0;
            try
            {
                client_->connect(socketPath_,100);
                auto fresh=std::make_unique<RemoteControl>(*client_);
                remote_=std::move(fresh);control_=remote_.get();failure_.clear();textCache_.clear();recoveryAvailable_=false;
            }
            catch(const std::exception&)
            {
                client_->disconnect();
                try{recoveryAvailable_=process_ && !process_->running();}
                catch(const std::exception&){recoveryAvailable_=false;}
            }
        }
    }
}
