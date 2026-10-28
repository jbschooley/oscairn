// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Engine.h"
#include "../audio/AudioEngine.h"
#include <bit>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace oscairn::standalone
{
    Engine::Engine(const std::string& pluginDir,const std::string& socketPath):socketPath_(socketPath)
    {
        if(socketPath.empty())
        {
            audio_=std::make_unique<host::AudioEngine>(pluginDir);control_=&audio_->control();
        }
        else
        {
            client_=std::make_unique<LocalClient>();client_->connect(socketPath);
            remote_=std::make_unique<RemoteControl>(*client_);control_=remote_.get();
            startTimer(16);
        }
    }
    Engine::~Engine(){stopTimer();}
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
        try{auto text=control_->parameterText(control_->nodeId(n),p,v);textCache_[key]={bits,text};return text;}
        catch(const std::exception&){return std::to_string(v);}
    }
    void Engine::setParam(const std::string& n,uint32_t p,double v)
    {command([&]{if(!control_->setParameter(control_->nodeId(n),p,v))std::fprintf(stderr,"oscairn: parameter command refused\n");});}
    void Engine::paramGesture(const std::string& n,uint32_t p,bool begin)
    {command([&]{if(!control_->parameterGesture(control_->nodeId(n),p,begin))std::fprintf(stderr,"oscairn: gesture command refused\n");});}
    void Engine::setPlaying(bool v){command([&]{control_->setPlaying(v);});}
    void Engine::setTempo(double v){command([&]{control_->setTempo(v);});}
    void Engine::setFollowMidiClock(bool v){command([&]{control_->setClockSource(v?Transport::ClockSource::MidiClock:Transport::ClockSource::Internal);});}
    bool Engine::followsMidiClock()const{return control_->clockSource()==Transport::ClockSource::MidiClock;}
    void Engine::note(bool on,int key,float velocity){command([&]{if(!control_->note(on,key,velocity))std::fprintf(stderr,"oscairn: note queue overflow\n");});}
    Engine::Status Engine::status()const
    {
        const auto transport=control_->transportState();const auto host=control_->sessionStatus();
        return {transport.playing,transport.tempo,transport.playing?static_cast<int>(static_cast<int64_t>(std::floor(transport.beat/.25+1e-9))%16):-1,
                failure_.empty()?host.audioDevice:"Disconnected: "+failure_,host.midiInputs};
    }
    void Engine::failed(const std::string& message)
    {
        failure_=message;std::fprintf(stderr,"oscairn: control connection: %s\n",message.c_str());
        if(client_)client_->disconnect();
    }
    void Engine::timerCallback()
    {
        if(failure_.empty())
        {
            try{remote_->service();}catch(const std::exception& e){failed(e.what());}
        }
        else if(++reconnectTicks_>=60)
        {
            reconnectTicks_=0;
            try
            {
                client_->connect(socketPath_,100);
                auto fresh=std::make_unique<RemoteControl>(*client_);
                remote_=std::move(fresh);control_=remote_.get();failure_.clear();textCache_.clear();
            }
            catch(const std::exception&){client_->disconnect();}
        }
    }
}
