// SPDX-License-Identifier: AGPL-3.0-or-later
#include "../SoundPickerModel.h"
#include "oscairn/session.h"
#include "oscairn/remote_control.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include "oscairn/wav_file.h"
#include "../../../core/plugin-host/src/StateFiles.h"
#include <future>
#include <iostream>
#include <sys/socket.h>

using namespace oscairn;
using namespace oscairn::standalone;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}

struct Fixture
{
    Session session{48000,128};LocalServer server{session};
    std::array<float,128> left{},right{};
    explicit Fixture(const char* path,const std::string& pluginId={})
    {
        const auto node=session.add("synth",path,pluginId);
        session.connectNotes(graphInput,node);session.connectAudio(node,graphOutput);session.commit();
    }
    void tick()
    {
        const auto now=std::chrono::steady_clock::now().time_since_epoch();
        server.poll(static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count()));
        session.service();float* output[]{left.data(),right.data()};check(session.process(output,128),"render failed");
    }
    template<class F> auto remote(F action)
    {
        int sockets[2];check(socketpair(AF_UNIX,SOCK_STREAM,0,sockets)==0,"socketpair");
        check(server.adopt(sockets[0]),"adopt server socket");
        auto worker=std::async(std::launch::async,[fd=sockets[1],action] {
            LocalClient client;client.adopt(fd);RemoteControl control(client);return action(control);
        });
        while(worker.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready){tick();std::this_thread::yield();}
        return worker.get();
    }
};

void presets(const char* path)
{
    Fixture f(path,"urn:oscairn:test:instrument");
    auto state=f.remote([](Control& c){return readSoundPicker(c);});
    check(state.sounds.mode=="instances" && state.choices.size()==2,"loaded LV2 instrument presets exposed");
    SoundAction prepare;prepare.kind=SoundAction::Prepare;prepare.node=state.node;
    prepare.pluginId=state.pluginId;prepare.choice=state.choices[1];
    check(prepare.choice.preset=="urn:oscairn:test:instrument-preset","preset URI preserved");
    state=f.remote([&](Control& c){applySoundAction(c,prepare);return readSoundPicker(c);});
    check(state.sounds.contexts[0].selectedChannels==65535,"preset preparation does not replace playing sound");
    SoundAction select=prepare;select.kind=SoundAction::Select;select.context=state.sounds.contexts[1].id;
    f.remote([&](Control& c){applySoundAction(c,select);});f.tick();
    check(f.session.parameter(state.node,0)==.75,"selected preset restores actual parameter value");
    const auto prepared=select.context;
    prepare.choice.preset="urn:missing";
    check(f.remote([&](Control& c){try{applySoundAction(c,prepare);return false;}catch(const std::exception&){return true;}}),"missing preset reported");
    state=f.remote([](Control& c){return readSoundPicker(c);});
    check(state.sounds.contexts.size()==2 && state.sounds.contexts[1].selectedChannels==65535,"bad preset preserves current instance");
    select.context=0;f.remote([&](Control& c){applySoundAction(c,select);});f.tick();
    SoundAction remove=select;remove.kind=SoundAction::Discard;remove.context=prepared;
    check(f.remote([&](Control& c){try{applySoundAction(c,remove);return false;}catch(const std::exception&){return true;}}),"unknown effect tail requires explicit retirement");
    remove.cutTail=true;f.remote([&](Control& c){applySoundAction(c,remove);});
    state=f.remote([](Control& c){return readSoundPicker(c);});
    check(state.sounds.contexts.size()==1,"confirmed tail retirement removes instance");
}

void sequencedSounds(const char* synth,const char* sequencer)
{
    auto render=[&](size_t choice,bool locks) {
        Fixture f(synth);
        const auto seq=f.session.add("sequencer",sequencer);
        const auto node=f.session.nodeId("synth");
        f.session.connectNotes(seq,node);f.session.commit();f.tick();
        auto state=readSoundPicker(f.session);
        check(state.canToggleBrightnessLocks[0] && !state.brightnessLocks[0],"brightness locks default off");
        SoundAction action;action.node=node;action.pluginId=state.pluginId;
        action.kind=SoundAction::Prepare;action.choice=state.choices[choice];
        applySoundAction(f.session,action);
        action.kind=SoundAction::Select;action.context=readSoundPicker(f.session).sounds.contexts.back().id;
        applySoundAction(f.session,action);f.tick();
        action.kind=SoundAction::BrightnessLocks;action.enableLocks=locks;
        f.session.mapParameter(seq,0,node,1);
        bool stagedRefused=false;
        try{applySoundAction(f.session,action);}catch(const std::runtime_error&){stagedRefused=true;}
        check(stagedRefused && f.session.graphState().staged,"toggle must not commit another pending edit");
        f.session.commit();f.tick();
        f.remote([&](Control& c){applySoundAction(c,action);});
        check(readSoundPicker(f.session).brightnessLocks[0]==locks,"lock routing readback");
        std::array<std::vector<float>,2> audio;
        f.session.setPlaying(true);
        for(int i=0;i<750;++i){f.tick();audio[0].insert(audio[0].end(),f.left.begin(),f.left.end());}
        f.session.setPlaying(false);
        for(int i=0;i<750;++i)f.tick();
        check(f.session.note(true,60,.7),"pad note accepted");
        for(int i=0;i<100;++i){f.tick();audio[1].insert(audio[1].end(),f.left.begin(),f.left.end());}
        action.enableLocks=false;f.remote([&](Control& c){applySoundAction(c,action);});
        check(!readSoundPicker(f.session).brightnessLocks[0],"locks can be disabled");
        f.session.mapParameter(seq,0,node,2);f.session.commit();f.tick();
        check(!readSoundPicker(f.session).canToggleBrightnessLocks[0],"other lane mappings protected");
        bool refused=false;
        try{applySoundAction(f.session,action);}catch(const std::runtime_error&){refused=true;}
        check(refused,"toggle must not remove another mapping");
        return audio;
    };
    for(bool locks:{false,true})
    {
        const auto warm=render(0,locks),bright=render(1,locks);
        for(size_t phase=0;phase<2;++phase)
        {
            double difference=0;
            for(size_t i=0;i<warm[phase].size();++i)difference+=std::abs(warm[phase][i]-bright[phase][i]);
            check(locks?difference<1e-5:difference>1.0,
                "Warm/Bright must differ in sequencer and subsequent pads unless explicitly overridden by locks");
        }
    }
}

void samples(const char* path)
{
    state_files::TemporaryDirectory temp;setenv("OSCAIRN_DATA_HOME",temp.path.c_str(),1); // never the user's own asset store
    Fixture f(path);
    const auto dir=temp.path/"My samples";std::filesystem::create_directory(dir);
    writeWavFile(dir/"Soft pad.wav",std::vector<float>(4800,.25f),1,48000);
    {std::ofstream(dir/"Pitched.sfz")<<"<region> sample=Soft pad.wav transpose=12\n";std::ofstream(dir/"Broken.sfz")<<"<region>\nsample=Soft pad.wav pan=10\n";}
    auto state=f.remote([](Control& c){return readSoundPicker(c);});
    check(state.importsSamples && state.choices.empty(),"sampler offers import instead of presets");
    const auto wav=f.remote([&](Control& c){return importSound(c,(dir/"Soft pad.wav").string());});
    check(wav.sfz=="<region> ampeg_release=0.03 sample=Soft pad.wav\n" && wav.library=="user" && wav.directory.size()==16,"WAV imports are copied into the user library and fade out over 30 ms");
    const auto sfz=f.remote([&](Control& c){return importSound(c,(dir/"Pitched.sfz").string());});
    check(sfz.library.size()==16 && sfz.directory.empty(),"an SFZ's folder becomes a library used in place");
    SoundAction action;action.kind=SoundAction::Prepare;action.node=state.node;action.pluginId=state.pluginId;
    std::string longName;for(int i=0;i<40;++i)longName+="\xD0\xB6"; // 80 bytes of two-byte characters
    std::filesystem::copy_file(dir/"Soft pad.wav",dir/(longName+".wav"));
    for(const auto& file:{std::string("Soft pad.wav"),std::string("Pitched.sfz"),longName+".wav"}){action.importPath=(dir/file).string();f.remote([&](Control& c){applySoundAction(c,action);return 0;});}
    state=f.remote([](Control& c){return readSoundPicker(c);});
    check(state.sounds.contexts.size()==4 && state.sounds.contexts[1].name=="Soft pad" && state.sounds.contexts[2].name=="Pitched","WAV and SFZ imports prepared");
    check(state.sounds.contexts[3].name==longName.substr(0,62),"long non-ASCII names are cut between characters");
    const auto libraries=f.remote([](Control& c){return c.assetLibraries();});
    check(libraries.size()==2 && std::any_of(libraries.begin(),libraries.end(),[](const auto& l){return l.id=="user" && l.files==2;}),"re-importing a WAV reuses its copy; each file name is kept once");
    for(const auto& [file,expected]:std::initializer_list<std::pair<const char*,const char*>>{{"Broken.sfz","line 2: unsupported opcode pan"},{"notes.txt","Choose a .wav"}}){
        action.importPath=(dir/file).string();
        const auto error=f.remote([&](Control& c){try{applySoundAction(c,action);}catch(const std::exception& e){return std::string(e.what());}return std::string();});
        check(error.find(expected)!=std::string::npos,"import failure explained");
    }
}

int main(int argc,char** argv)
{
    try
    {
        check(argc==3 || argc==4,"need Basic Synth, sequencer and optional sampler paths, or LV2 bundle and --lv2");
        if(argc==4)samples(argv[3]);
        if(std::string(argv[2])=="--lv2"){presets(argv[1]);std::cout<<"ok: picker LV2 presets, failed preparation and explicit tail retirement\n";return 0;}
        sequencedSounds(argv[1],argv[2]);
        {
            Fixture parts(argv[1]);const auto seq=parts.session.add("sequencer",argv[2]);parts.session.commit();parts.tick();
            auto state=readSoundPicker(parts.session);SoundAction action;action.node=state.node;action.pluginId=state.pluginId;
            action.kind=SoundAction::Prepare;action.choice=state.choices[3];applySoundAction(parts.session,action);
            action.kind=SoundAction::Select;action.context=readSoundPicker(parts.session).sounds.contexts.back().id;action.channel=1;
            parts.remote([&](Control& c){applySoundAction(c,action);});parts.tick();state=readSoundPicker(parts.session);
            check(state.sounds.contexts[0].selectedChannels==65533 && state.sounds.contexts[1].selectedChannels==2,"track selection changes only its MIDI channel");
            action.kind=SoundAction::BrightnessLocks;action.track=1;action.enableLocks=true;
            parts.remote([&](Control& c){applySoundAction(c,action);});parts.tick();state=readSoundPicker(parts.session);
            check(!state.brightnessLocks[0] && state.brightnessLocks[1],"second track has independent lock mapping");
            parts.session.mapParameter(seq,0,state.node,2);parts.session.commit();parts.tick();
            action.enableLocks=false;parts.remote([&](Control& c){applySoundAction(c,action);});parts.tick();state=readSoundPicker(parts.session);
            check(!state.canToggleBrightnessLocks[0] && state.canToggleBrightnessLocks[1] && !state.brightnessLocks[1],"second track edits preserve unrelated first-track mapping");
        }
        Fixture f(argv[1]);
        auto state=f.remote([](Control& c){return readSoundPicker(c);});
        check(state.choices.size()==5 && state.sounds.contexts.size()==1,"initial choices/default");
        const auto node=state.node;
        f.session.note(true,60,.7);f.tick();
        SoundAction prepare;prepare.kind=SoundAction::Prepare;prepare.node=node;
        prepare.pluginId=state.pluginId;prepare.choice=state.choices[0];
        state=f.remote([&](Control& c){applySoundAction(c,prepare);return readSoundPicker(c);});
        check(state.sounds.contexts.size()==2 && state.sounds.contexts[0].selectedChannels==65535,"preparing must not select");
        const auto warm=state.sounds.contexts[1].id;
        SoundAction select;select.kind=SoundAction::Select;select.node=node;select.pluginId=state.pluginId;select.context=warm;
        f.remote([&](Control& c){applySoundAction(c,select);});f.tick();
        state=f.remote([](Control& c){return readSoundPicker(c);});
        check(state.sounds.contexts[1].selectedChannels==65535,"selection confirmed after processing");
        check(state.sounds.contexts[0].activeVoices==1,"held note retained its original context");
        f.session.note(true,64,.7);f.tick();
        select.context=0;f.remote([&](Control& c){applySoundAction(c,select);});f.tick();
        SoundAction remove=select;remove.kind=SoundAction::Discard;remove.context=warm;
        check(f.remote([&](Control& c){try{applySoundAction(c,remove);return false;}catch(const std::runtime_error&){return true;}}),"held sound cannot be removed");
        f.session.note(false,60,0);f.session.note(false,64,0);
        for(int i=0;i<2000;++i)f.tick();
        f.remote([&](Control& c){applySoundAction(c,remove);});
        state=f.remote([](Control& c){return readSoundPicker(c);});
        check(state.sounds.contexts.size()==1,"unselected idle sound removed; reconnect reads engine state");
        prepare.choice.parameters={{999,1}};
        check(f.remote([&](Control& c){try{applySoundAction(c,prepare);return false;}catch(const std::exception&){return true;}}),"bad preparation refused");
        state=f.remote([](Control& c){return readSoundPicker(c);});
        check(state.sounds.contexts.size()==1 && state.sounds.contexts[0].selectedChannels==65535,"failed prepare preserves current sound");
        prepare.choice=state.choices[1];
        for(uint32_t i=1;i<state.sounds.capacity;++i)f.remote([&](Control& c){applySoundAction(c,prepare);});
        check(f.remote([&](Control& c){try{applySoundAction(c,prepare);return false;}catch(const std::exception&){return true;}}),"capacity exhaustion reported");
        state=f.remote([](Control& c){return readSoundPicker(c);});
        check(state.sounds.contexts[0].selectedChannels==65535,"capacity failure preserves selected sound");
        const auto beforeReplacement=f.session.graphState().revision;
        check(f.remote([&](Control& c){try{c.replace(node,"/missing/plugin.clap",state.pluginId);return false;}catch(const std::exception& e){return std::string(e.what())!="Internal error";}}),"remote replacement reports the loader failure");
        check(f.session.graphState().revision==beforeReplacement,"failed remote replacement preserves graph");
        f.remote([&](Control& c){c.replace(node,argv[1],state.pluginId);});f.tick();
        check(f.remote([](Control& c){return readSoundPicker(c);}).sounds.contexts.size()==1,"remote replacement refreshes prepared sound pool");
        select.pluginId="different-engine";
        check(f.remote([&](Control& c){try{applySoundAction(c,select);return false;}catch(const std::exception&){return true;}}),"stale engine choice refused");
        std::cout<<"ok: picker prepare/select/retire, held voices, failure/capacity and reconnect\n";
    }
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
