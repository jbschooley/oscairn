// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/graph.h"
#include "AllocationGuard.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <iostream>
#include <span>
#include <thread>
#include <tuple>
using namespace oscairn;
using kick::clap::Event;
void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
struct Instrument {
    Graph graph{48000,512};NodeId node;
    std::array<float,512> left{},right{};float* out[2]{left.data(),right.data()};
    explicit Instrument(const char* path):node(graph.add(path)){graph.connectNotes(graphInput,node);graph.connectAudio(node,graphOutput);graph.commit();}
    PluginHost& host(){return graph.host(node);}
    void run(std::span<const Event> events={},uint32_t frames=512){forbidAllocation=true;const bool ok=graph.process(out,frames,nullptr,events.data(),events.size());forbidAllocation=false;check(ok,"context graph render");}
    void one(const Event& event,uint32_t frames=512){run({&event,1},frames);}
    auto info(uint32_t id){for(const auto& s:host().soundContexts())if(s.id==id)return s;throw std::runtime_error("missing context");}
    void tail(){for(int i=0;i<120;++i)run();}
};
int main(int argc,char** argv){try{
    if(argc!=2)return 2;
    const std::vector<oscairn_sound_parameter_t> dark{{1,.08},{2,.3},{3,.5}},bright{{1,.95},{2,.8},{3,.5}};
    Instrument mixed(argv[1]),a(argv[1]),b(argv[1]);
    auto x=mixed.host().prepareSound("Dark",dark),y=mixed.host().prepareSound("Bright",bright);
    auto ax=a.host().prepareSound("Dark",dark),by=b.host().prepareSound("Bright",bright);
    check(mixed.host().selectSound(x) && a.host().selectSound(ax) && b.host().selectSound(by),"prepare and select");
    const auto first=Event::note(true,60,1,0,0,0,101),second=Event::note(true,60,1,0,0,0,102);
    mixed.one(first);a.one(first);b.run();
    check(mixed.left==a.left,"first context matches standalone sound");
    check(mixed.host().selectSound(y),"switch while held");mixed.run();a.run();b.run();
    check(mixed.left==a.left,"switch leaves held sound byte-identical");
    mixed.one(second,37);a.run({},37);b.one(second,37);
    for(int i=0;i<37;++i)check(std::abs(mixed.left[i]-(a.left[i]+b.left[i]))<1e-7,"new and old sounds render simultaneously in one engine");
    clap_event_note_expression_t expression{};expression.header={sizeof(expression),17,CLAP_CORE_EVENT_SPACE_ID,CLAP_EVENT_NOTE_EXPRESSION,0};
    expression.expression_id=CLAP_NOTE_EXPRESSION_PRESSURE;expression.note_id=101;expression.key=60;expression.value=.9;
    const auto pressure=Event::from(expression);mixed.one(pressure);a.one(pressure);b.run();
    for(int i=0;i<512;++i)check(std::abs(mixed.left[i]-(a.left[i]+b.left[i]))<1e-7,"expression follows the old note ID across switching");
    mixed.host().setParameter(1,.4);b.host().setParameter(1,.4);mixed.run();a.run();b.run();
    for(int i=0;i<512;++i)check(std::abs(mixed.left[i]-(a.left[i]+b.left[i]))<1e-7,"parameter edits affect selected sound while outgoing patch stays frozen");
    check(!mixed.host().discardSound(x),"sounding old context cannot be discarded");
    const auto off=Event::note(false,60,0,13,0,0,101);
    mixed.one(off);a.one(off);b.run();
    for(int i=0;i<512;++i)check(std::abs(mixed.left[i]-(a.left[i]+b.left[i]))<1e-7,"note ID releases its old context at the exact sample");
    mixed.tail();check(mixed.info(x).active_voices==0 && mixed.info(y).active_voices==1,"old voice ends while new voice remains");
    check(mixed.host().discardSound(x),"silent unselected context is reclaimable");
    const auto replacement=mixed.host().prepareSound("Reused slot",dark);check(replacement!=x,"context IDs are never reused");
    check(!mixed.host().selectSound(x),"stale context ID rejected");

    Instrument fifo(argv[1]);auto fx=fifo.host().prepareSound("A",dark),fy=fifo.host().prepareSound("B",bright);
    fifo.host().selectSound(fx);fifo.one(Event::note(true,60,1));fifo.host().selectSound(fy);fifo.one(Event::note(true,60,1));
    fifo.one(Event::note(false,60,0));fifo.tail();
    check(fifo.info(fx).active_voices==0 && fifo.info(fy).active_voices==1,"ID-less repeated key releases oldest owning context");
    fifo.one(Event::midi(0xb0,64,127));fifo.one(Event::note(false,60,0));fifo.host().selectSound(fx);fifo.tail();
    check(fifo.info(fy).active_voices==1 && !fifo.host().discardSound(fy),"sustain retains outgoing context");
    fifo.one(Event::midi(0xb0,64,0));fifo.tail();check(fifo.info(fy).active_voices==0,"pedal release reaches outgoing context");
    fifo.host().selectSound(fy,1);
    std::array<Event,2> channels{Event::note(true,62,1,0,0),Event::note(true,65,1,0,1)};fifo.run(channels);
    check(fifo.info(fx).active_voices==1 && fifo.info(fy).active_voices==1,"different channels use independent sounds");
    fifo.one(Event::midi(0xb0,120,0));check(fifo.info(fx).active_voices==0 && fifo.info(fy).active_voices==1,"all sound off is channel scoped");
    fifo.one(Event::note(false,-1,0,0,-1,-1));fifo.tail();check(fifo.info(fy).active_voices==0,"wildcard panic releases every context");

    fifo.one(Event::midi(0x91,67,127));check(fifo.info(fy).active_voices==1,"advertised MIDI dialect accepts note on");
    fifo.one(Event::midi(0x81,67,0));fifo.tail();check(fifo.info(fy).active_voices==0,"advertised MIDI dialect accepts note off");

    Instrument bounded(argv[1]);std::vector<uint32_t> ids;
    for(int i=0;i<7;++i)ids.push_back(bounded.host().prepareSound("Ready",{}));
    bool full=false;try{bounded.host().prepareSound("Too many",{});}catch(const std::runtime_error&){full=true;}check(full,"context budget enforced");
    for(int i=0;i<32;++i)check(bounded.host().selectSound(ids[0]),"bounded selection accepted");
    check(!bounded.host().selectSound(ids[0]) && !bounded.host().discardSound(ids[0]),"full queue refuses selection and pending context stays pinned");
    bounded.run();check(bounded.info(ids[0]).selected_channels==65535,"queued selection applied");
    bounded.host().selectSound(0);bounded.run();check(bounded.host().discardSound(ids[0]),"queue releases its context pins");

    auto renderTimeline=[&](uint32_t block){
        Instrument instrument(argv[1]);auto firstSound=instrument.host().prepareSound("A",dark),nextSound=instrument.host().prepareSound("B",bright);
        instrument.host().selectSound(firstSound);std::vector<float> result;
        for(uint32_t pos=0;pos<4096;){
            if(pos==256)instrument.host().selectSound(nextSound);
            const auto n=std::min({block,4096-pos,pos<256?256-pos:4096-pos});std::array<Event,4> events;size_t count=0;
            for(auto [at,on,id]:{std::tuple{64u,true,1},std::tuple{300u,true,2},std::tuple{700u,false,1},std::tuple{900u,false,2}})
                if(at>=pos && at<pos+n)events[count++]=Event::note(on,60,on?1:0,at-pos,0,0,id);
            instrument.run({events.data(),count},n);result.insert(result.end(),instrument.left.begin(),instrument.left.begin()+n);pos+=n;
        }return result;
    };
    check(renderTimeline(37)==renderTimeline(512),"context switching is byte-identical across uneven block sizes");

    Instrument concurrent(argv[1]);std::atomic<bool> stop=false,valid=true;std::atomic<uint32_t> blocks=0;uint32_t preparedCount=0;
    std::thread audio([&]{try{uint32_t i=0;while(!stop){concurrent.one(Event::note((i++%2)==0,60,1));++blocks;}}catch(...){valid=false;}});
    while(blocks<5)std::this_thread::yield();
    for(int i=0;i<500;++i){
        auto all=concurrent.host().soundContexts();
        for(const auto& c:all)if(c.id && c.idle)concurrent.host().discardSound(c.id);
        if(concurrent.host().soundContexts().size()<8){auto id=concurrent.host().prepareSound("Concurrent",i%2?dark:bright);++preparedCount;concurrent.host().selectSound(id);}
        else concurrent.host().selectSound(0);
        if(i%10==0){const auto before=blocks.load();while(blocks<=before)std::this_thread::yield();}
    }
    stop=true;audio.join();check(valid && preparedCount>7,"concurrent preparation reuses retired contexts without interrupting rendering");
    std::cout<<"ok: held sounds, independent parts, note ownership, sustain, capacity, reclamation and concurrent preparation\n";return 0;
}catch(const std::exception& e){forbidAllocation=false;std::cerr<<e.what()<<'\n';return 1;}}
