// SPDX-License-Identifier: AGPL-3.0-or-later
#include "Engine.h"
#include "../../plugins/step-sequencer/Parameters.h"
#include "../audio/AudioEngine.h"
#include "EngineProcess.h"
#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdlib>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace oscairn::standalone
{
    static std::map<std::string,Engine::Editor> readEditors(Control& control) {
        std::map<std::string,Engine::Editor> result;const auto graph=control.graphState();
        for(const auto& node:graph.topology.nodes) {
            const auto& name=graph.names.at(node.id);if(name!="synth" && name!="delay")continue;
            result[name]={node.plugin.name,node.plugin.id,node.plugin.path,control.parameters(node.id),control.presets(node.id)};
        }
        return result;
    }
    Engine::Engine(const std::string& pluginDir,const std::string& socketPath,const std::string& helperExecutable,const host::AudioConfiguration& audio):socketPath_(socketPath)
    {
        pluginDir_=pluginDir;
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
            attach(); // once it is usable: a window that failed to start never kept it
        }
        refreshSounds();
        refreshPads();
        startTimer(16);
    }
    Engine::~Engine()
    {
        stopTimer();pendingSounds_.clear();if(soundJob_.valid()){soundJob_.wait();finishSounds();}
        // Quitting normally lets the managed engine stop once no other window keeps it; a crash
        // never gets here, so the engine plays on for the UI to come back to.
        if(process_ && client_)
            try
            {
                if(!client_->connected())client_->connect(socketPath_,100); // dropped after a timeout: say it once more
                client_->call("engine.release","{}",500);
            }
            catch(const std::exception&){}
    }
    // This window keeps its managed engine (it stops when every window that keeps it has quit).
    void Engine::attach()
    {
        if(!process_)return;
        try{client_->call("engine.attach");}catch(const std::exception& e){std::fprintf(stderr,"oscairn: engine.attach: %s\n",e.what());}
    }
    bool Engine::hasParam(const std::string& n,uint32_t p)const
    {try{return control_->hasParameter(control_->nodeId(n),p);}catch(const std::invalid_argument&){return false;}}
    Engine::ParamInfo Engine::paramInfo(const std::string& n,uint32_t p)const{return control_->parameterInfo(control_->nodeId(n),p);}
    double Engine::param(const std::string& n,uint32_t p)const
    {
        try{return control_->parameter(control_->nodeId(n),p);}
        catch(const std::invalid_argument&){return 0;} // another client may remove a displayed node
    }
    // A bare number written with more than three decimals ("0.0100000", JUCE's default for a
    // parameter with no text of its own) to three at most, without trailing zeros ("0.01"); other
    // text as the plugin writes it.
    std::string tidyNumber(const std::string& text)
    {
        const auto dot=text.find('.');
        if(dot==std::string::npos || text.size()-dot-1<=3)return text;
        for(size_t i=0;i<text.size();++i)
            if(!(std::isdigit(static_cast<unsigned char>(text[i])) || i==dot || (i==0 && text[i]=='-')))return text;
        char out[64];std::snprintf(out,sizeof out,"%.3f",std::strtod(text.c_str(),nullptr));
        std::string tidy=out;
        while(tidy.back()=='0')tidy.pop_back();
        if(tidy.back()=='.')tidy.pop_back();
        return tidy=="-0"?"0":tidy;
    }
    std::string Engine::paramText(const std::string& n,uint32_t p,double v)const
    {
        const auto key=std::make_pair(n,p);const auto it=textCache_.find(key);
        const auto bits=std::bit_cast<uint64_t>(v); // reuse formatting only for the exact cached value
        if(it!=textCache_.end() && it->second.first==bits)return it->second.second;
        if(soundJob_.valid())return std::to_string(v);
        try{auto text=tidyNumber(control_->parameterText(control_->nodeId(n),p,v));textCache_[key]={bits,text};return text;}
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
    int Engine::trackChannel()const{return static_cast<int>(param("sequencer",sequencer::channel(sequenceTrack_)));}
    void Engine::note(bool on,int key,float velocity)
    {
        if(key<0 || key>=128)return;
        const auto channel=trackChannel();
        command([this,on,key,velocity,channel]{
            auto& held=padChannels_[static_cast<size_t>(key)];
            if(on){
                if(control_->note(true,key,velocity,channel))held|=uint16_t(1u<<channel);
                else std::fprintf(stderr,"oscairn: note queue overflow\n");
            }else for(int c=0;c<16;++c)if(held&(1u<<c)){
                if(control_->note(false,key,0,c))held&=uint16_t(~(1u<<c));
                else std::fprintf(stderr,"oscairn: note release queue overflow\n");
            }
        });
    }

    void Engine::pad(int index,bool on,float velocity)
    {
        if(index<0 || index>=int(PadMap::pads))return;
        const auto channel=trackChannel();
        command([this,index,on,velocity,channel]{if(!control_->pad(uint32_t(index),on,juce::jlimit(0.0f,1.0f,velocity),channel))std::fprintf(stderr,"oscairn: note queue overflow\n");});
    }
    void Engine::padPressure(int index,float pressure)
    {
        if(index<0 || index>=int(PadMap::pads))return;
        command([this,index,pressure]{control_->padPressure(uint32_t(index),juce::jlimit(0.0f,1.0f,pressure));});
    }
    bool Engine::hasControl(const std::string& name) const
    {return std::any_of(macros_.begin(),macros_.end(),[&](const Macro& m){return m.name==name;});}
    double Engine::controlValue(const std::string& name) const
    {for(const auto& m:macros_)if(m.name==name)return m.value;return 0;}
    void Engine::setControl(const std::string& name,double value)
    {
        for(auto& m:macros_)if(m.name==name)m.value=value; // shown at once
        edit([name,value](Control& c){for(const auto& m:c.graphState().macros)if(m.name==name){c.setMacro(m.id,value);return;}});
    }
    void Engine::setControl(uint32_t id,double value)
    {
        for(auto& m:macros_)if(m.id==id)m.value=value; // shown at once
        edit([id,value](Control& c){c.setMacro(id,value);});
    }
    void Engine::edit(std::function<void(Control&)> change)
    {
        command([this,change=std::move(change)]{
            try{change(*control_);editError_.clear();}
            catch(const std::invalid_argument& e){editError_=e.what();}
        });
    }
    void Engine::ignoreMidiInputs(std::vector<std::string> names){ignoredInputs_=std::move(names);command([this]{control_->ignoreMidiInputs(ignoredInputs_);});}
    void Engine::setPadMap(const PadMap& map){padMap_=map;command([this,map]{control_->setPadMap(map);});}
    void Engine::refreshPads(){try{padMap_=control_->padMap();}catch(const std::exception& e){std::fprintf(stderr,"oscairn: pad map: %s\n",e.what());}}
    std::string Engine::pluginId(const std::string& name) const {
        const auto graph=control_->graphState();
        for(const auto& node:graph.topology.nodes)if(graph.names.at(node.id)==name)return node.plugin.id;
        return {};
    }
    void Engine::replacePlugin(const std::string& name,const std::string& path,const std::string& id,const std::string& preset) {
        SoundAction action;action.kind=SoundAction::Replace;action.node=control_->nodeId(name);action.pluginId=pluginId(name);
        action.replacementPath=path;action.replacementId=id;action.choice.preset=preset;soundAction(std::move(action));
    }
    void Engine::soundAction(SoundAction action)
    {
        if(!connected() || (soundBusy() && action.kind!=SoundAction::PrepareFromLibrary))return; // a preparation ahead waits its turn
        if((action.kind==SoundAction::Prepare || action.kind==SoundAction::PrepareFromLibrary || action.kind==SoundAction::Replace || action.kind==SoundAction::SavePerformance || action.kind==SoundAction::LoadPerformance || action.kind==SoundAction::SaveToLibrary || action.kind==SoundAction::LoadFromLibrary || action.kind==SoundAction::SaveSound || action.kind==SoundAction::LoadSound || action.kind==SoundAction::PartFromSound || action.kind==SoundAction::ImportPerformance ||
            action.kind==SoundAction::ExportPerformance || action.kind==SoundAction::InstallArchive || action.kind==SoundAction::AuditionStart || action.kind==SoundAction::AuditionStop) && !canPrepareSounds())
        {
            soundMessage_="Preparation requires the separate engine. Relaunch without --in-process.";
            return;
        }
        soundMessage_.clear();
        if(soundJob_.valid())
        {
            // Anything asked replaces a preparation still waiting: the latest is the one wanted.
            std::erase_if(pendingSounds_,[](const SoundAction& a){return a.kind==SoundAction::PrepareFromLibrary;});
            pendingSounds_.push_back(std::move(action));return;
        }
        refreshSounds(std::move(action));
    }

    void Engine::refreshSounds(SoundAction action)
    {
        if(socketPath_.empty())
        {
            try{if(const auto m=applySoundAction(*control_,action);!m.empty())soundMessage_=m;soundPicker_=readSoundPicker(*control_);editors_=readEditors(*control_);}
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
                try{result.message=applySoundAction(control,request);}catch(const std::exception& e){result.error=e.what();}
                result.state=readSoundPicker(control);
                result.editors=readEditors(control);
            }
            catch(const std::exception& e){result.error=e.what();result.disconnected=!client.connected();}
            return result;
        });
    }

    void Engine::finishSounds()
    {
        auto result=soundJob_.get();
        if(!result.error.empty())soundMessage_=result.error;else if(!result.message.empty())soundMessage_=result.message;
        for(const auto& [name,editor]:result.editors) {
            const auto old=editors_.find(name);
            if(old==editors_.end() || old->second.id!=editor.id || old->second.path!=editor.path)textCache_.clear();
        }
        soundPicker_=std::move(result.state);editors_=std::move(result.editors);
        const bool loaded=soundJobKind_==SoundAction::LoadPerformance || soundJobKind_==SoundAction::LoadFromLibrary || soundJobKind_==SoundAction::AuditionStart || soundJobKind_==SoundAction::AuditionStop;
        if(soundJobKind_==SoundAction::Replace || loaded)textCache_.clear();
        if(result.error.empty() && (soundJobKind_==SoundAction::SavePerformance || soundJobKind_==SoundAction::SaveToLibrary))soundMessage_="Performance saved.";
        if(result.error.empty() && loaded)soundMessage_="Performance opened. Press Play or play a note.";
        if(result.error.empty() && soundJobKind_==SoundAction::SaveSound)soundMessage_="Sound saved.";
        if(result.error.empty() && (soundJobKind_==SoundAction::LoadSound || soundJobKind_==SoundAction::PartFromSound)){textCache_.clear();soundMessage_="Sound loaded.";}
        if(result.error.empty() && soundJobKind_==SoundAction::ImportPerformance){textCache_.clear();soundMessage_="Performance imported.";}
        if(result.disconnected)
        {
            pendingControls_.clear();
            failed(result.error);
            return;
        }
        // Replay deferred releases on failure; successful replacement discarded their voices.
        if(loaded && result.error.empty()){pendingControls_.clear();padChannels_.fill(0);refreshPads();}
        auto pending=std::move(pendingControls_);pendingControls_.clear();
        for(auto& action:pending)command(std::move(action));
        if(!pendingSounds_.empty() && connected())
        {
            auto action=std::move(pendingSounds_.front());pendingSounds_.pop_front();refreshSounds(std::move(action));
        }
    }
    std::optional<Control::LibraryResult> Engine::library(const Control::LibraryQuery& query) const
    {
        if(!connected() || !controlReady())return {};
        try{return control_->libraryQuery(query);}catch(const std::exception& e){std::fprintf(stderr,"oscairn: library: %s\n",e.what());return {};}
    }
    std::optional<Control::LibraryItem> Engine::libraryItem(const std::string& id) const
    {
        Control::LibraryQuery one;one.id=id;
        const auto found=library(one);
        if(!found)return {};
        if(!found->items.empty())return found->items.front();
        return Control::LibraryItem{}; // not in the library: an empty ID
    }
    std::string Engine::archiveContents(const std::string& file, Control::Archive& out) const
    {
        if(!connected()) return "The engine is not running.";
        try{out=control_->archiveContents(file);return {};}catch(const std::exception& e){return e.what();}
    }
    std::vector<Control::InstalledLibrary> Engine::libraries() const
    {
        if(!connected() || !controlReady()) return {};
        try{return control_->libraries();}catch(const std::exception& e){std::fprintf(stderr,"oscairn: libraries: %s\n",e.what());return {};}
    }
    std::string Engine::deleteLibraryItem(const std::string& id)
    {
        if(!connected()) return "The engine is not running.";
        try{control_->deleteLibraryItem(id);return {};}catch(const std::exception& e){return e.what();}
    }
    std::string Engine::editLibraryItem(const std::string& id, const Control::PerformanceInfo& info)
    {
        if(!connected()) return "The engine is not running.";
        try{control_->libraryEdit(id,info);return {};}catch(const std::exception& e){return e.what();}
    }
    std::string Engine::copyLibraryItem(const std::string& id, std::string& copied)
    {
        if(!connected()) return "The engine is not running.";
        try{copied=control_->libraryCopy(id);return {};}catch(const std::exception& e){return e.what();}
    }
    std::vector<Control::InstalledPlugin> Engine::installedPlugins() const
    {
        if(!connected() || !controlReady()) return {};
        try{return control_->installedPlugins();}catch(const std::exception& e){std::fprintf(stderr,"oscairn: installed plugins: %s\n",e.what());return {};}
    }
    std::string Engine::removePlugin(const Control::InstalledPlugin& plugin)
    {
        if(!connected()) return "The engine is not running.";
        if(!controlReady()) return "The engine is busy; try again in a moment.";
        try{control_->removePlugin(plugin.format,plugin.id,plugin.version);return {};}catch(const std::exception& e){return e.what();}
    }
    std::string Engine::uninstallLibrary(const std::string& id)
    {
        if(!connected()) return "The engine is not running.";
        try{control_->uninstallLibrary(id);return {};}catch(const std::exception& e){return e.what();}
    }
    std::optional<Control::PerformanceContents> Engine::performanceContents(const std::string& directory) const
    {
        if(!connected() || !controlReady())return {};
        try{return control_->performanceContents(directory);}catch(const std::exception& e){std::fprintf(stderr,"oscairn: performance contents: %s\n",e.what());return {};}
    }
    std::optional<Control::Audition> Engine::audition() const
    {
        if(!connected() || !controlReady()) return {};
        try{return control_->audition();}catch(const std::exception&){return {};}
    }
    std::optional<Control::PerformanceInfo> Engine::performanceInfo() const
    {
        if(!connected() || !controlReady())return {};
        try{return control_->performanceInfo();}catch(const std::exception&){return {};}
    }
    const std::vector<AssignmentOverride>& Engine::assignments() const
    {
        if(connected() && controlReady())
            try{assignments_=control_->assignments();}catch(const std::exception&){}
        return assignments_;
    }
    Engine::Status Engine::status()const
    {
        const auto transport=control_->transportState();const auto host=control_->sessionStatus();
        return {connected() && transport.playing,transport.tempo,connected() && transport.playing?static_cast<int>(param("sequencer",sequencer::playhead(sequenceTrack_))):-1,
                failure_.empty()?(host.audioReady?host.audioDevice:"Audio unavailable: "+host.audioDevice):"Disconnected: "+failure_,host.midiInputs,host.failures,host.cpuLoad};
    }
    void Engine::failed(const std::string& message)
    {
        pendingControls_.clear();
        pendingSounds_.clear();
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
            remote_=std::move(fresh);control_=remote_.get();failure_.clear();textCache_.clear();recoveryAvailable_=false;refreshPads();
            attach();
            if(!ignoredInputs_.empty())control_->ignoreMidiInputs(ignoredInputs_);
        }
        catch(const std::exception& e){failed(e.what());}
    }
    void Engine::timerCallback()
    {
        try{if(connected())macros_=control_->graphState().macros;}catch(const std::exception&){}
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
                remote_=std::move(fresh);control_=remote_.get();failure_.clear();textCache_.clear();recoveryAvailable_=false;refreshPads();
                attach();
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
