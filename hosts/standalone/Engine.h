// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "oscairn/remote_control.h"
#include "../audio/AudioConfiguration.h"
#include "SoundPickerModel.h"
#include <juce_events/juce_events.h>
#include <memory>
#include <algorithm>
#include <cstdio>
#include <future>
#include <deque>
#include <functional>

namespace oscairn::host { class AudioEngine; class EngineProcess; }
namespace oscairn::standalone
{
    /// UI facade over either an in-process Control or a remote engine's cached Control.
    class Engine : private juce::Timer
    {
    public:
        /// helperExecutable selects managed mode; empty helper + empty socket is in-process.
        explicit Engine(const std::string& pluginDir, const std::string& socketPath = {}, const std::string& helperExecutable = {}, const host::AudioConfiguration& audio = {});
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
        // Pads play through the Performance's pad map (pads.h), on the selected track's MIDI
        // channel unless a pad has its own. The map is cached here and refreshed after a load.
        void pad(int index,bool on,float velocity);
        void padPressure(int index,float pressure);
        const PadMap& padMap() const {return padMap_;}
        void setPadMap(const PadMap& map);
        std::function<void()> openPads;
        // The Parts views (RFC 0011): the graph with its parts, and edits made through the control
        // API in order; a failed edit is reported in editError().
        Control::GraphState graph() const {return control_->graphState();}
        void edit(std::function<void(Control&)> change);
        const std::string& editError() const {return editError_;}
        const std::string& pluginDir() const {return pluginDir_;}
        std::function<void()> openParts;
        void selectTrack(uint32_t track){sequenceTrack_=std::min(track,1u);}
        int trackChannel()const;
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
        const SoundPickerState& soundPicker() const { return soundPicker_; }
        const std::string& soundMessage() const { return soundMessage_; }
        bool controlReady() const { return !soundJob_.valid(); }
        bool soundBusy() const { return pendingSound_.has_value() || (soundJob_.valid() && soundJobKind_!=SoundAction::Refresh); }
        bool canPrepareSounds() const { return !socketPath_.empty(); }
        void soundAction(SoundAction action);
        std::function<void()> openBrowser;
        std::string pluginId(const std::string& node) const;
        void replacePlugin(const std::string& node,const std::string& path,const std::string& id,const std::string& preset={});
        struct Editor { std::string name,id,path;std::vector<ParamInfo> parameters;std::vector<PresetInfo> presets; };
        const std::map<std::string,Editor>& editors() const {return editors_;}

    private:
        void timerCallback()override;
        template<class F> void command(F&& f)
        {
            if(!failure_.empty() || (soundJob_.valid() && soundJobKind_==SoundAction::Replace))return;
            if(soundJob_.valid()) { pendingControls_.emplace_back(std::forward<F>(f)); return; }
            try{f();}catch(const std::exception& e)
            {
                if(client_ && !client_->connected())failed(e.what());
                else std::fprintf(stderr,"oscairn: control command: %s\n",e.what());
            }
        }
        void failed(const std::string& message);
        void refreshSounds(SoundAction action = {});
        void finishSounds();
        struct SoundResult { std::map<std::string,Editor> editors;SoundPickerState state; std::string error; bool disconnected = false; };
        std::future<SoundResult> soundJob_;
        SoundAction::Kind soundJobKind_=SoundAction::Refresh;
        std::optional<SoundAction> pendingSound_;
        SoundPickerState soundPicker_;
        std::map<std::string,Editor> editors_;
        std::string soundMessage_;
        uint32_t soundTicks_ = 0;
        std::deque<std::function<void()>> pendingControls_;
        std::unique_ptr<host::AudioEngine> audio_;
        std::unique_ptr<host::EngineProcess> process_;
        std::unique_ptr<LocalClient> client_;
        std::unique_ptr<RemoteControl> remote_;
        Control* control_=nullptr;
        uint32_t sequenceTrack_=0;
        std::array<uint16_t,128> padChannels_{};
        PadMap padMap_;
        std::string editError_, pluginDir_;
        void refreshPads();
        std::string socketPath_,failure_;
        uint32_t reconnectTicks_=0;
        bool recoveryAvailable_=false;
        mutable std::map<std::pair<std::string,uint32_t>,std::pair<uint64_t,std::string>> textCache_;
    };
}
