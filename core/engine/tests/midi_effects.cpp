// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/graph.h"
#include "oscairn/transport.h"
#include "AllocationGuard.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <span>
#include <unistd.h>
using kick::clap::Event;
using namespace oscairn;
namespace {
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
Event param(double value,uint32_t time=0){clap_event_param_value_t e{};e.header={sizeof(e),time,0,CLAP_EVENT_PARAM_VALUE,0};e.param_id=1;e.note_id=e.port_index=e.channel=e.key=-1;e.value=value;return Event::from(e);}
Event midi(uint8_t status,uint8_t key,uint8_t value,uint32_t time=0){clap_event_midi_t e{};e.header={sizeof(e),time,0,CLAP_EVENT_MIDI,0};e.data[0]=status;e.data[1]=key;e.data[2]=value;return Event::from(e);}
Event choke(int key=-1,uint32_t time=0){auto e=Event::note(false,key,0,time);reinterpret_cast<clap_event_note_t*>(e.bytes.data())->header.type=CLAP_EVENT_NOTE_CHOKE;return e;}
struct Timed {uint64_t time;Event event;};
struct Runner {
    std::shared_ptr<PluginHost> host;uint64_t now=0;std::vector<Timed> events;
    explicit Runner(const char* path):host(loadPlugin(path,"",48000,512)){}
    void run(std::span<const Event> in={},uint32_t frames=512,bool playing=true){
        clap_event_transport_t t{};t.flags=playing?CLAP_TRANSPORT_IS_PLAYING:0;
        forbidAllocation=true;const bool ok=host->process(nullptr,0,nullptr,0,frames,in.data(),in.size(),&t);forbidAllocation=false;
        check(ok,"processor failed");uint32_t count=0;const auto* out=host->outputEvents(count);
        for(uint32_t i=0;i<count;++i)events.push_back({now+out[i].header()->time,out[i]});now+=frames;
    }
    std::vector<Timed> notes(uint16_t kind){std::vector<Timed> out;for(auto e:events)if(e.event.header()->type==kind)out.push_back(e);return out;}
};
const clap_event_note_t& note(const Timed& e){return *reinterpret_cast<const clap_event_note_t*>(e.event.header());}
void transpose(const char* path){
    Runner r(path);
    const std::array input{param(12),Event::note(true,60,.8,7,0,0,4),param(-12,8),Event::note(true,60,.8,9,0,0,5),Event::from(clap_event_note_expression_t{{sizeof(clap_event_note_expression_t),10,0,CLAP_EVENT_NOTE_EXPRESSION,0},CLAP_NOTE_EXPRESSION_PRESSURE,4,0,0,60,.7}),Event::note(false,60,0,11,0,0,4),Event::note(false,60,0,12,0,0,5)};
    r.run(input);auto on=r.notes(CLAP_EVENT_NOTE_ON),off=r.notes(CLAP_EVENT_NOTE_OFF),pressure=r.notes(CLAP_EVENT_NOTE_EXPRESSION),end=r.notes(CLAP_EVENT_NOTE_END);
    check(on.size()==2 && off.size()==2 && end.size()==2 && pressure.size()==1,"transpose event counts");
    check(note(on[0]).key==72 && note(on[1]).key==48 && on[0].time==7,"transpose notes and exact sample");
    check(note(off[0]).key==72 && note(off[1]).key==48 && note(off[0]).note_id==note(on[0]).note_id && note(off[1]).note_id==note(on[1]).note_id,"held releases keep original transpose and IDs");
    const auto& ex=*reinterpret_cast<const clap_event_note_expression_t*>(pressure[0].event.header());
    check(ex.key==72 && ex.note_id==note(on[0]).note_id,"pressure follows mapped voice");
    check(note(end[0]).key==60 && note(end[0]).note_id==4,"input ownership released with original address");
    r.events.clear();const std::array high{param(48),Event::note(true,100,.8)};r.run(high);
    check(r.notes(CLAP_EVENT_NOTE_ON).empty() && r.notes(CLAP_EVENT_NOTE_END).size()==1,"out of range note ends without clamping");
    Runner sustain(path);const std::array held{param(12),midi(0x90,60,100),midi(0xb0,64,127,1),midi(0x80,60,0,2),midi(0xa0,60,100,3)};sustain.run(held);
    check(sustain.notes(CLAP_EVENT_NOTE_END).empty() && sustain.notes(CLAP_EVENT_NOTE_EXPRESSION).size()==1,"sustained voice retains pressure ownership");
    const std::array up{midi(0xb0,64,0,5)};sustain.run(up);check(sustain.notes(CLAP_EVENT_NOTE_END).size()==1,"pedal up reclaims voice");
    Runner repeated(path);const std::array raw{midi(0x90,60,100),midi(0x90,60,100,1),midi(0x80,60,0,2),midi(0x80,60,0,3)};repeated.run(raw);
    on=repeated.notes(CLAP_EVENT_NOTE_ON);off=repeated.notes(CLAP_EVENT_NOTE_OFF);check(off.size()==2 && note(off[0]).note_id==note(on[0]).note_id && note(off[1]).note_id==note(on[1]).note_id,"raw MIDI repeated notes release FIFO");
}
std::vector<std::pair<uint64_t,uint16_t>> delayTimeline(const char* path,uint32_t block){
    Runner r(path);std::vector<Timed> source{{0,param(10)},{23,Event::note(true,60,.8)},{500,param(0)},{1500,Event::note(false,60,0)}};
    while(r.now<2400){std::array<Event,4> input{};size_t count=0;const auto frames=std::min<uint64_t>(block,2400-r.now);
        for(auto& e:source)if(e.time>=r.now && e.time<r.now+frames){input[count]=e.event;reinterpret_cast<clap_event_header_t*>(input[count++].bytes.data())->time=e.time-r.now;}
        r.run({input.data(),count},frames);
    }
    std::vector<std::pair<uint64_t,uint16_t>> result;for(auto e:r.events)result.push_back({e.time,e.event.header()->type});
    check(result==std::vector<std::pair<uint64_t,uint16_t>>{{503,CLAP_EVENT_NOTE_ON},{1980,CLAP_EVENT_NOTE_OFF},{1980,CLAP_EVENT_NOTE_END}},"delay sample schedule and held delay edit");return result;
}
void delaySafety(const char* path){
    Runner cancel(path);const std::array input{param(20),Event::note(true,60,.8),choke(60,10)};cancel.run(input);cancel.run();cancel.run();
    check(cancel.notes(CLAP_EVENT_NOTE_ON).empty() && cancel.notes(CLAP_EVENT_NOTE_END).size()==1,"choke cancels future note");
    Runner stop(path);const std::array on{param(20),Event::note(true,60,.8)};stop.run(on);stop.run({},512,false);stop.run({},512,false);
    check(stop.notes(CLAP_EVENT_NOTE_ON).empty() && !stop.notes(CLAP_EVENT_NOTE_CHOKE).empty(),"stop cancels delayed notes");
    Runner overflow(path);const std::array setup{param(2000)};overflow.run(setup);
    std::array<Event,128> controls;controls.fill(midi(0xb0,1,64));for(int i=0;i<9;++i)overflow.run(controls);
    check(!overflow.notes(CLAP_EVENT_NOTE_CHOKE).empty(),"queue overflow chokes safely");
    unsigned pedalUps=0;for(const auto& e:overflow.events)if(e.event.header()->type==CLAP_EVENT_MIDI){const auto& m=*reinterpret_cast<const clap_event_midi_t*>(e.event.header());if((m.data[0]&0xf0)==0xb0 && m.data[1]==64 && m.data[2]==0)++pedalUps;}
    check(pedalUps>=16,"overflow recovery clears downstream sustain on every channel");
    overflow.host->panic();overflow.run();overflow.events.clear();const std::array restart{param(0),Event::note(true,61,.8),Event::note(false,61,0,1)};overflow.run(restart);
    check(overflow.notes(CLAP_EVENT_NOTE_ON).size()==1 && overflow.notes(CLAP_EVENT_NOTE_OFF).size()==1,"processor recovers after overflow");
}
void outputFailure(const char* path){
    Runner r(path);std::array<Event,512> burst{};
    for(size_t i=0;i<burst.size();++i)burst[i]=Event::note(i%2==0,60,i%2==0?.8:0);
    r.run(burst);check(r.host->statistics().droppedEvents>0,"test must fill host output queue");
    r.run();r.events.clear();const std::array next{Event::note(true,64,.8),Event::note(false,64,0,1)};r.run(next);
    check(r.notes(CLAP_EVENT_NOTE_ON).size()==1 && r.notes(CLAP_EVENT_NOTE_OFF).size()==1,"recovery after rejected output releases new notes");
}
void state(const char* path){
    Runner a(path);const std::array set{param(7)};a.run(set);a.host->service();
    const auto dir=std::filesystem::temp_directory_path()/("oscairn-midi-state-"+std::to_string(getpid()));std::filesystem::remove_all(dir);
    a.host->saveState(dir.string());Runner b(path);b.host->loadState(dir.string());check(b.host->parameterValue(1)==7,"portable state round trip");std::filesystem::remove_all(dir);
}
void graph(const char* transposePath,const char* delayPath,const char* synthPath){
    Graph graph(48000,512),reference(48000,512);const auto tr=graph.add(transposePath),dl=graph.add(delayPath),synth=graph.add(synthPath),ref=reference.add(synthPath);
    graph.connectNotes(graphInput,tr);graph.connectNotes(tr,dl);graph.connectNotes(dl,synth);graph.connectAudio(synth,graphOutput);graph.host(tr).setParameter(1,12);graph.host(dl).setParameter(1,10);graph.commit();
    reference.connectNotes(graphInput,ref);reference.connectAudio(ref,graphOutput);reference.commit();
    std::array<float,512> left{},right{},expectedL{},expectedR{};float* outputs[]{left.data(),right.data()};float* expected[]{expectedL.data(),expectedR.data()};
    double energy=0;for(int block=0;block<30;++block){const auto a=Event::note(block==0,60,block==0?.8:0,7),b=Event::note(block==0,72,block==0?.8:0,487);const bool send=block==0 || block==10;
        forbidAllocation=true;const bool ok=graph.process(outputs,512,nullptr,send?&a:nullptr,send?1:0) && reference.process(expected,512,nullptr,send?&b:nullptr,send?1:0);forbidAllocation=false;check(ok,"graph rendering");
        for(size_t f=0;f<512;++f){check(left[f]==expectedL[f] && right[f]==expectedR[f],"MIDI processor chain differs from direct scheduled notes");energy+=left[f]*left[f];}
    }check(energy>0,"MIDI chain produces sound");
    graph.connectNotes(dl,tr);bool rejected=false;try{graph.commit();}catch(const std::exception&){rejected=true;}check(rejected,"ordinary MIDI delay must not legalize feedback cycles");
}
void hybrid(const char* fixture,const char* transposePath,const char* synthPath){
    AudioLayout layout;layout.inputs={{0,"Capture",1,0}};
    Graph graph(48000,512,ProcessingMode::Realtime,{},layout),reference(48000,512);
    const auto analyzer=graph.add(fixture),tr=graph.add(transposePath),synth=graph.add(synthPath),ref=reference.add(synthPath);
    graph.connectAudio({{graphInput,0,0},{analyzer,0,0},1});graph.connectAudio({{analyzer,0,0},{graphOutput,0,1},1});
    graph.connectNotes(analyzer,tr);graph.connectNotes(tr,synth);graph.connectAudio(synth,graphOutput);graph.host(tr).setParameter(1,12);graph.commit();
    reference.connectNotes(graphInput,ref);reference.connectAudio(ref,graphOutput);reference.commit();
    std::array<float,512> input{},left{},right{},refL{},refR{};const float* inputs[]{input.data()};float* outputs[]{left.data(),right.data()};float* expected[]{refL.data(),refR.data()};
    for(int block=0;block<8;++block){for(size_t i=0;i<512;++i){const auto time=block*512+i;input[i]=time>=23 && time<1109?1.f:0.f;}
        std::array<Event,1> notes{};size_t count=0;if(block==0)notes[count++]=Event::note(true,72,.8,23);if(block==2)notes[count++]=Event::note(false,72,0,85);
        forbidAllocation=true;const bool ok=graph.process(inputs,1,outputs,2,512,nullptr) && reference.process(expected,512,nullptr,notes.data(),count);forbidAllocation=false;check(ok,"hybrid render");
        for(size_t i=0;i<512;++i)check(left[i]==refL[i] && std::abs(right[i]-(refR[i]+input[i]))<1e-6f,"audio and MIDI outputs preserved through mixed dependencies");
    }
    graph.connectAudio(synth,analyzer);bool rejected=false;try{graph.commit();}catch(const std::exception&){rejected=true;}check(rejected,"mixed audio/MIDI cycle rejected");
}

}
int main(int argc,char** argv){try{if(argc!=5)return 2;transpose(argv[1]);outputFailure(argv[1]);check(delayTimeline(argv[2],37)==delayTimeline(argv[2],512),"block invariant delay");delaySafety(argv[2]);state(argv[1]);state(argv[2]);graph(argv[1],argv[2],argv[3]);hybrid(argv[4],argv[1],argv[3]);std::cout<<"ok: mapped ownership, sustain, sample timing, stop/overflow recovery, state and graph audio\n";}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
