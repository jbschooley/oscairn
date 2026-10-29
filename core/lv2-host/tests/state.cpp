// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/plugin_host.h"
#include "oscairn/wav_file.h"
#include "../../plugin-host/src/StateFiles.h"
#include "AllocationGuard.h"
#include <array>
#include <atomic>
#include <iostream>
#include <thread>

using namespace oscairn;
using Event=kick::clap::Event;
namespace fs=std::filesystem;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
template<class F> void rejects(F f,const char* message){bool failed=false;try{f();}catch(const std::exception&){failed=true;}check(failed,message);}
struct Audio
{
    std::array<float,512> l{},r{};
    bool run(PluginHost& host,const Event* events=nullptr,uint32_t count=0,bool live=false)
    {
        float* out[]{l.data(),r.data()};forbidAllocation=live;
        const auto ok=host.process(nullptr,0,out,2,64,events,count,nullptr);forbidAllocation=false;return ok;
    }
};
int main(int argc,char** argv)
{
    try
    {
        if(argc!=3)return 2;
        state_files::TemporaryDirectory temp;
        const auto a=(fs::canonical(temp.path)/"source a.wav").string(),b=(fs::canonical(temp.path)/"source b.wav").string();
        writeWavFile(a,std::vector<float>(48000,.25f),1,48000);writeWavFile(b,std::vector<float>(48000,.75f),1,48000);
        const std::string sample="http://lv2plug.in/plugins/eg-sampler#sample";
        auto make=[&](ProcessingMode mode=ProcessingMode::Offline){return loadPlugin(argv[1],"http://lv2plug.in/plugins/eg-sampler",48000,512,mode);};
        Audio audio;auto on=Event::note(true,60,1);auto host=make();
        check(host->supportsState(),"upstream sampler state advertised");
        check(audio.run(*host,&on,1),"default state loads through normal host");
        host->service();check(host->propertyValue(sample).has_value(),"default sample notification reaches control thread");
        check(!host->setProperty(sample,std::string(a)),"wrong property type rejected");
        check(!host->setProperty(sample,FilePath{a+"missing"}),"missing sample request rejected");
        check(host->setProperty(sample,FilePath{a}),"queue sample A");
        rejects([&]{host->saveState((temp.path/"premature").string());},"pending property request cannot silently save old state");
        check(audio.run(*host,&on,1) && audio.l[0]==.25f,"offline patch load through normal host");host->service();
        check(std::get<FilePath>(*host->propertyValue(sample)).value==a,"plugin confirms loaded sample");
        const auto saved=temp.path/"saved";host->saveState(saved.string());
        check(fs::exists(saved/"state.ttl"),"state bundle written");
        rejects([&]{host->saveState(saved.string());},"existing state bundle never overwritten");
        for(const auto& f:fs::recursive_directory_iterator(saved))check(!f.is_symlink(),"state assets are owned copies");
        check(host->setProperty(sample,FilePath{b}) && audio.run(*host,&on,1) && audio.l[0]==.75f,"sample replacement");
        const auto relocated=temp.path/"relocated";fs::rename(saved,relocated);fs::remove(a);
        auto restored=make();restored->loadState(relocated.string());
        check(audio.run(*restored,&on,1) && audio.l[0]==.25f,"relocated state works without original sample");
        // Preparing another state must not modify an outgoing held sound. This is
        // the instance-pool foundation, not automatic note-ownership routing.
        check(audio.run(*host,&on,1) && audio.l[0]==.75f,"outgoing sound held");
        restored->loadState(relocated.string());
        check(audio.run(*host) && audio.l[0]==.75f,"preparing incoming state preserves outgoing held note");
        check(audio.run(*restored,&on,1) && audio.l[0]==.25f,"same key can play a different sound in prepared instance");
        // Live Worker load and state operations with concurrent audio: callback
        // only tries the gate and emits silence while control owns the instance.
        auto live=make(ProcessingMode::Realtime);Audio liveAudio;
        check(live->setProperty(sample,FilePath{b}),"live sample request accepted");
        bool loaded=false;
        for(int i=0;i<2000 && !loaded;++i){check(liveAudio.run(*live,&on,1,true),"live allocation-free render");live->service();loaded=live->propertyValue(sample) && std::get<FilePath>(*live->propertyValue(sample)).value==b;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        check(loaded,"live Worker confirms sample replacement");
        std::atomic<bool> stop=false,valid=true;std::thread render([&]{while(!stop){if(!liveAudio.run(*live,nullptr,0,true))valid=false;std::this_thread::yield();}});
        try{live->saveState((temp.path/"live").string());live->loadState(relocated.string());}
        catch(...){stop=true;render.join();throw;}
        stop=true;render.join();check(valid,"state operations exclude live processing safely");
        check(audio.run(*live,&on,1,true) && audio.l[0]==.25f,"live state restore resumes with correct sample");
        check(!live->statistics().processErrors && !live->statistics().droppedEvents,"state/Worker transitions have no errors or drops");
        // CLAP's existing state implementation is now reachable through the same API.
        auto clap=loadPlugin(argv[2],"",48000,512);
        check(clap->supportsState() && clap->setParameter(7,.375),"CLAP state and queued parameter");
        clap->saveState((temp.path/"clap").string());clap->setParameter(7,.9);clap->loadState((temp.path/"clap").string());
        check(clap->parameterValue(7)==.375,"CLAP opaque state restored through common API");
        rejects([&]{restored->loadState((temp.path/"clap").string());},"wrong format rejected");
        // Removing a copied asset must be reported even if the plugin returns success.
        for(const auto& f:fs::recursive_directory_iterator(relocated))if(f.path().extension()==".wav")fs::remove(f.path());
        rejects([&]{restored->loadState(relocated.string());},"missing saved asset reported");
        check(!audio.run(*restored,&on,1),"failed restore does not render partial plugin state");
        std::cout<<"ok: common CLAP/LV2 state, default state, patch sample loading, relocation, live Worker exclusion and overlapping instances\n";
        return 0;
    }
    catch(const std::exception& e){forbidAllocation=false;std::cerr<<e.what()<<'\n';return 1;}
}
