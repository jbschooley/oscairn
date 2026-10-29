// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/graph.h"
#include "oscairn/session.h"
#include "oscairn/json_rpc.h"
#include "AllocationGuard.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <span>
#include <thread>
using namespace oscairn;
using kick::clap::Event;
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
struct Instrument {
    Graph graph{48000,512};NodeId node;clap_id gain;
    std::array<float,512> left{},right{};float* out[2]{left.data(),right.data()};
    Instrument(const char* path,const char* id,clap_id parameter):node(graph.add(path,id)),gain(parameter){
        graph.connectNotes(graphInput,node);graph.connectAudio(node,graphOutput);graph.commit();}
    PluginHost& host(){return graph.host(node);}
    void run(std::span<const Event> events={},uint32_t frames=512){forbidAllocation=true;const bool ok=graph.process(out,frames,nullptr,events.data(),events.size());forbidAllocation=false;check(ok,"prepared graph render");}
    void one(const Event& e){run({&e,1});}
    uint32_t prepare(double value){return host().prepareSound("Prepared",{{gain,value}});}
    auto info(uint32_t id){for(const auto& s:host().soundContexts())if(s.id==id)return s;throw std::runtime_error("missing sound");}
    void level(float value){check(std::abs(left[511]-value)<1e-6f,"unexpected mixed audio level");}
};
void switching(const char* path,const char* id,clap_id gain){
    Instrument s(path,id,gain);check(std::string(s.host().soundContextMode())=="instances" && s.host().soundContextCapacity()==8,"hosted capability");
    auto a=s.prepare(.25),b=s.prepare(.75);s.host().selectSound(a);s.one(Event::midi(0x90,60,100));s.level(.25);
    s.host().selectSound(b);s.run();s.level(.25);s.one(Event::midi(0x90,60,100));s.level(1);
    check(!s.host().discardSound(a,true),"held outgoing instance stays pinned");
    s.one(Event::midi(0x80,60,0));s.level(.75);check(s.info(a).active_voices==0,"FIFO release reaches outgoing MIDI instance");
    check(!s.host().discardSound(a),"unknown tail requires explicit retirement");check(s.host().discardSound(a,true),"explicit tail retirement");
    s.one(Event::midi(0x80,60,0));s.level(0);check(s.info(b).active_voices==0,"second FIFO release reaches new instance");
    s.host().selectSound(0);s.run();check(s.host().discardSound(b,true),"retire second instance");
    auto c=s.prepare(.25),d=s.prepare(.75);check(c!=a && !s.host().selectSound(a),"context IDs never reused");
    s.host().selectSound(c);s.one(Event::midi(0xb0,64,127));s.one(Event::midi(0x90,60,100));
    s.host().selectSound(d);s.one(Event::midi(0x90,64,100));s.one(Event::midi(0x80,60,0));s.one(Event::midi(0x80,64,0));s.level(1);
    check(!s.host().discardSound(c,true),"sustain owns outgoing notes");s.one(Event::midi(0xb0,64,0));s.level(0);
    check(s.host().discardSound(c,true),"pedal release unpins outgoing instance");
    auto e=s.prepare(.25);s.host().selectSound(e,1);
    const std::array notes{Event::note(true,60,1,0,0,0,1),Event::note(true,64,1,0,1,0,2)};s.run(notes);s.level(1);
    const auto off=Event::note(false,60,0,123,0,0,1);s.one(off);check(s.left[122]==1 && s.left[123]==.25f,"sample-exact owner release across parts");
    s.one(Event::note(false,64,0,0,1,0,2));s.level(0);
    s.host().selectSound(e);s.host().selectSound(0);check(!s.host().discardSound(e,true),"pending selection pins context until an audio boundary");s.run();check(s.host().discardSound(e,true),"cancelled selection eventually retires");
    bool failed=false;try{s.host().prepareSound("Bad",{{gain,99}});}catch(const std::invalid_argument&){failed=true;}check(failed,"patch bounds validated");
    s.one(Event::note(true,60,1));failed=false;try{s.host().prepareSound("Missing state",{},"/no/such/oscairn/state");}catch(const std::exception&){failed=true;}check(failed,"bad state rejected");s.run();s.level(.5);
    s.host().panic();s.run();s.level(0);
    for(int i=0;i<1200;++i){s.one(Event::midi(0x90,60,100));s.one(Event::midi(0x80,60,0));}check(s.info(0).active_voices==0,"synthetic NOTE_END prevents outer graph ledger exhaustion");
}
void tails(const char* path){
    Instrument s(path,"org.kick.test.tail",7),reference(path,"org.kick.test.tail",7);
    auto a=s.prepare(.25),b=s.prepare(.75),r=reference.prepare(.25);s.host().selectSound(a);reference.host().selectSound(r);
    const auto on=Event::note(true,60,1,0,0,0,41),off=Event::note(false,60,0,37,0,0,41);
    s.one(on);reference.one(on);s.host().selectSound(b);
    clap_event_param_value_t parameter{};parameter.header={sizeof(parameter),77,CLAP_CORE_EVENT_SPACE_ID,CLAP_EVENT_PARAM_VALUE,0};
    parameter.param_id=7;parameter.note_id=41;parameter.channel=0;parameter.port_index=0;parameter.key=60;parameter.value=.5;
    s.one(Event::from(parameter));reference.one(Event::from(parameter));check(s.left==reference.left && s.left[76]==.25f && s.left[77]==.5f,"per-note parameter reaches outgoing owner");
    parameter.note_id=-1;parameter.value=.4;s.one(Event::from(parameter));reference.one(Event::from(parameter));
    check(s.left==reference.left && s.left[77]==.4f,"key-scoped parameter reaches outgoing owner");
    s.host().setParameter(7,.6);s.run();reference.run();check(s.left==reference.left,"global edit leaves outgoing patch unchanged");
    s.one(off);reference.one(off);
    check(s.left==reference.left && s.info(a).active_voices==1,"outgoing CLAP release matches independent reference");
    check(!s.host().discardSound(a,true),"CLAP voice remains owned until NOTE_END");
    s.run();reference.run();check(s.left==reference.left,"outgoing tail survives second block");s.run();reference.run();check(s.left==reference.left && s.info(a).active_voices==0,"NOTE_END maps back to original owner");
    check(!s.host().discardSound(a) && s.host().discardSound(a,true),"unknown plugin tail stays conservative even after voice end");
}
void pressure(const char* path){
    Instrument s(path,"urn:oscairn:test:instrument",0);auto a=s.prepare(.25),b=s.prepare(.75);
    s.host().selectSound(a);s.one(Event::midi(0x90,60,127));s.host().selectSound(b);s.one(Event::midi(0x90,64,127));s.level(1);
    s.one(Event::midi(0xa0,60,127,111));check(s.left[110]==1 && s.left[111]==1.25f,"poly pressure reaches outgoing key at exact sample");
    s.one(Event::midi(0xd0,127,0));s.level(2);
    s.one(Event::midi(0x80,60,0));s.level(1.5);s.one(Event::midi(0x80,64,0));s.level(0);
}
void persistence(const char* path,const char* id,clap_id gain,const std::filesystem::path& directory){
    auto raw=loadPlugin(path,id,48000,512);check(raw->setParameter(gain,.25),"state gain");raw->saveState(directory.string());
    Instrument s(path,id,gain);s.one(Event::midi(0x90,60,100));
    auto restored=s.host().prepareSound("Saved",{},directory.string());s.run();s.level(.5);
    s.host().selectSound(restored);s.one(Event::midi(0x90,64,100));s.level(.75);
    s.one(Event::midi(0x80,60,0));s.level(.25);s.one(Event::midi(0x80,64,0));s.level(0);
    if(gain==0){auto preset=s.host().prepareSound("Preset",{},"","urn:oscairn:test:instrument-preset");s.host().selectSound(preset);s.one(Event::midi(0x90,60,100));s.level(.75);}
}
void bounds(const char* path){
    Instrument s(path,"org.kick.test.midi",7);std::array<uint32_t,7> ids{};for(auto& id:ids)id=s.prepare(.25);
    bool full=false;try{s.prepare(.75);}catch(const std::runtime_error&){full=true;}check(full,"instance budget enforced");
    check(!s.host().discardSound(0,true),"default protected");check(s.host().discardSound(ids[0]),"never-processed slot can be reclaimed");
    const auto replacement=s.prepare(.75);check(replacement!=ids[0],"slot reuse changes identity");
    for(int i=0;i<100;++i)s.host().selectSound(i%2?replacement:ids[1]);s.run();
    check(s.info(replacement).selected_channels==65535,"unapplied selections coalesce to latest request");
    s.host().selectSound(0);s.run();check(!s.host().discardSound(replacement),"even a quiet processed plugin may retain a future tail");
    check(s.host().discardSound(replacement,true),"explicit retirement of previously processed slot");
}
void concurrent(const char* path){
    Instrument s(path,"org.kick.test.midi",7);std::atomic<bool> done{false},ok{true};
    std::thread audio([&]{forbidAllocation=true;const std::array events{Event::note(true,60,1,0,0,0,1),Event::note(false,60,0,100,0,0,1)};
        while(!done.load())if(!s.graph.process(s.out,512,nullptr,events.data(),events.size()))ok=false;forbidAllocation=false;});
    unsigned prepared=0;
    for(int i=0;i<400;++i){s.host().service();s.host().selectSound(0);for(const auto& c:s.host().soundContexts())if(c.id)s.host().discardSound(c.id,true);
        if(s.host().soundContexts().size()<8){auto id=s.prepare(i%2?.25:.75);s.host().selectSound(id);++prepared;}}
    done=true;audio.join();check(ok && prepared>20,"concurrent preparation, selection, retirement and rendering");
}
void protocol(const char* path){
    Session s(48000,512);JsonRpc rpc(s);auto n=s.add("instrument",path,"org.kick.test.midi");
    auto call=[&](const std::string& method,const std::string& params){auto r=rpc.dispatch("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\""+method+"\",\"params\":"+params+"}");check(r.has_value(),"RPC reply");return *r;};
    check(call("sounds.get",R"({"node":"instrument"})").find("\"mode\":\"instances\"")!=std::string::npos,"wire distinguishes native and hosted contexts");
    check(call("sounds.prepare",R"({"node":"instrument","name":"Defaults"})").find("\"context\":1")!=std::string::npos,"omitted patch defaults");
    check(call("sounds.discard",R"({"node":"instrument","context":1,"cutTail":true})").find("\"accepted\":true")!=std::string::npos,"wire retirement");
    check(call("sounds.prepare",R"({"node":"instrument","name":"Bad","state":"a","preset":"b"})").find("-32602")!=std::string::npos,"state and preset exclusive");
    check(call("sounds.discard",R"({"node":"instrument","context":0,"cutTail":1})").find("-32602")!=std::string::npos,"cutTail type checked");
    (void)n;
}
void production(const char* path,const char* id,clap_id parameter,double value){
    Instrument mixed(path,id,parameter),a(path,id,parameter),b(path,id,parameter);
    check(std::string(mixed.host().soundContextMode())=="instances","production instrument pool available");
    auto first=mixed.host().prepareSound("A",{}),second=mixed.prepare(value),reference=b.prepare(value);
    mixed.host().selectSound(first);b.host().selectSound(reference);
    double energy=0;
    for(int block=0;block<160;++block){
        std::array<Event,2> m{},x{},y{};size_t nm=0,nx=0,ny=0;
        if(block==0)m[nm++]=x[nx++]=Event::note(true,60,.8,0,0,0,11);
        if(block==20){mixed.host().selectSound(second);m[nm++]=y[ny++]=Event::note(true,67,.8,37,0,0,12);}
        if(block==50)m[nm++]=x[nx++]=Event::note(false,60,0,101,0,0,11);
        if(block==90)m[nm++]=y[ny++]=Event::note(false,67,0,243,0,0,12);
        mixed.run({m.data(),nm});a.run({x.data(),nx});if(block>=20)b.run({y.data(),ny});
        for(size_t f=0;f<512;++f){
            const float expected=a.left[f]+(block>=20?b.left[f]:0.f);
            check(std::isfinite(mixed.left[f]) && std::abs(mixed.left[f]-expected)<1e-5f,"production switch differs from independent reference instances");
            const float right=a.right[f]+(block>=20?b.right[f]:0.f);
            check(std::isfinite(mixed.right[f]) && std::abs(mixed.right[f]-right)<1e-5f,"production right-channel switch mismatch");
            energy+=mixed.left[f]*mixed.left[f];
        }
        mixed.host().service();a.host().service();b.host().service();
    }
    check(energy>1e-5,"production transition is audible");check(mixed.info(first).active_voices==0 && mixed.host().discardSound(first,true),"production outgoing notes released");
    check(!mixed.host().statistics().droppedEvents && !mixed.host().statistics().processErrors,"production event and process health");
    std::cout<<"ok: production prepared-instance transition "<<id<<'\n';
}
int main(int argc,char** argv){try{
    if(argc<2)return 2;
    if(argc==6 && std::string(argv[1])=="--production"){production(argv[2],argv[3],std::stoul(argv[4]),std::stod(argv[5]));return 0;}
    const auto directory=std::filesystem::temp_directory_path()/("oscairn-prepared-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    switching(argv[1],"org.kick.test.midi",7);tails(argv[1]);bounds(argv[1]);concurrent(argv[1]);protocol(argv[1]);persistence(argv[1],"org.kick.test.midi",7,directory/"clap");
    if(argc>2){pressure(argv[2]);switching(argv[2],"urn:oscairn:test:instrument",0);persistence(argv[2],"urn:oscairn:test:instrument",0,directory/"lv2");}
    std::filesystem::remove_all(directory);std::cout<<"ok: prepared CLAP/LV2 ownership, state, parts, tails, concurrency and protocol\n";
}catch(const std::exception& e){forbidAllocation=false;std::cerr<<e.what()<<'\n';return 1;}}
