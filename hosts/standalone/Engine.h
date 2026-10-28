// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "oscairn/remote_control.h"
#include <juce_events/juce_events.h>
#include <memory>
#include <cstdio>

namespace oscairn::host { class AudioEngine; class EngineProcess; }
namespace oscairn::standalone
{
    /// UI facade over either an in-process Control or a remote engine's cached Control.
    class Engine : private juce::Timer
    {
    public:
        /// helperExecutable selects managed mode; empty helper + empty socket is in-process.
        explicit Engine(const std::string& pluginDir, const std::string& socketPath = {}, const std::string& helperExecutable = {});
        ~Engine() override;
        using ParamInfo = Control::ParamInfo;
        bool hasParam(const std::string& node,uint32_t param)const;
        ParamInfo paramInfo(const std::string& node,uint32_t param)const;
        double param(const std::string& node,uint32_t param)const;
        std::string paramText(const std::string& node,uint32_t param,double value)const;
        void setParam(const std::string& node,uint32_t param,double value);
        void paramGesture(const std::string& node,uint32_t param,bool begin);
        void setPlaying(bool playing);
        void setTempo(double bpm);
        void setFollowMidiClock(bool follow);
        bool followsMidiClock()const;
        void note(bool on,int key,float velocity);
        bool connected()const{return failure_.empty();}
        bool canRestart()const{return recoveryAvailable_;}
        void restart();
        struct Status
        {
            bool playing=false;
            double tempo=120;
            int step=-1;
            std::string audioDevice;
            std::vector<std::string> midiInputs;
        };
        Status status()const;
    private:
        void timerCallback()override;
        template<class F> void command(F&& f)
        {
            if(!failure_.empty())return;
            try{f();}catch(const std::exception& e)
            {
                if(client_ && !client_->connected())failed(e.what());
                else std::fprintf(stderr,"oscairn: control command: %s\n",e.what());
            }
        }
        void failed(const std::string& message);
        std::unique_ptr<host::AudioEngine> audio_;
        std::unique_ptr<host::EngineProcess> process_;
        std::unique_ptr<LocalClient> client_;
        std::unique_ptr<RemoteControl> remote_;
        Control* control_=nullptr;
        std::string socketPath_,failure_;
        uint32_t reconnectTicks_=0;
        bool recoveryAvailable_=false;
        mutable std::map<std::pair<std::string,uint32_t>,std::pair<uint64_t,std::string>> textCache_;
    };
}
