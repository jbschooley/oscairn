// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/graph.h"
#include "oscairn/session.h"
#include "oscairn/json_rpc.h"
#include "oscairn/transport.h"
#include "AllocationGuard.h"
#include "../Urids.h"
#include <clap/ext/params.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <iostream>
#include <thread>

namespace
{
using namespace oscairn;
using Event=kick::clap::Event;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
Event param(double value,uint32_t time=0,uint32_t id=0)
{
    clap_event_param_value_t e{};e.header={sizeof(e),time,CLAP_CORE_EVENT_SPACE_ID,CLAP_EVENT_PARAM_VALUE,0};
    e.param_id=id;e.value=value;e.note_id=-1;e.port_index=e.channel=e.key=-1;return Event::from(e);
}
Event pressure(int channel,int key,double value,uint32_t time)
{
    clap_event_note_expression_t e{};e.header={sizeof(e),time,CLAP_CORE_EVENT_SPACE_ID,CLAP_EVENT_NOTE_EXPRESSION,0};
    e.expression_id=CLAP_NOTE_EXPRESSION_PRESSURE;e.note_id=-1;e.port_index=0;e.channel=static_cast<int16_t>(channel);e.key=static_cast<int16_t>(key);e.value=value;return Event::from(e);
}
struct Audio {std::array<float,512> l{},r{};float* out[2]{l.data(),r.data()};};
bool render(PluginHost& host,Audio& audio,const Event* events=nullptr,uint32_t count=0,const clap_event_transport_t* time=nullptr)
{
    forbidAllocation=true;const auto ok=host.process(nullptr,0,audio.out,2,64,events,count,time);forbidAllocation=false;return ok;
}
void policy(const char* bundle)
{
    auto reject=[&](const char* id,const char* part){bool rejected=false;try{loadPlugin(bundle,id,48000,512);}catch(const std::runtime_error& e){rejected=std::string(e.what()).find(part)!=std::string::npos;}check(rejected,"unsupported LV2 rejected with useful reason");};
    reject("","pluginId");reject("urn:oscairn:test:bad-worker","Worker interface");reject("urn:oscairn:test:cv","port");reject("urn:missing","matching");
    reject("urn:oscairn:test:required-option","unsupported-option");
    reject("urn:oscairn:test:required-atom","buffer type");
    auto host=loadPlugin(std::string(bundle)+"/","urn:oscairn:test:synth",48000,512);
    check(host->descriptor().name=="Oscairn synth fixture" && host->parameters().size()==5,"metadata loaded from Turtle");
    check(host->noteInputPorts()==1 && host->noteOutputPorts()==1 && host->outputPorts().size()==1,"port inventory");
    check(!host->setParameter(5,3) && !host->setParameter(42,3) && !host->setParameter(0,NAN),"readonly, unknown, nonfinite controls rejected");
    Audio audio;
    for(uint32_t i=0;i<kick::clap::Host::maxEvents;++i)check(host->setParameter(0,.75),"parameter queue fills");
    check(!host->setParameter(0,1),"queue overflow reported");
    for(int i=0;i<5;++i)check(render(*host,audio),"queue drains without allocation");
    check(host->setParameter(0,.5),"queue reusable");
    auto invalid=Event::note(true,60,1);reinterpret_cast<clap_event_header_t*>(invalid.bytes.data())->size=1;
    check(render(*host,audio,&invalid,1) && host->statistics().droppedEvents>0,"malformed events dropped");
    for(int mode=1;mode<=3;++mode)
    {
        check(host->setParameter(8,mode),"bad output mode");auto note=Event::note(true,60,1);
        const auto before=host->statistics();check(render(*host,audio,&note,1),"malformed plugin output contained");
        const auto after=host->statistics();check(after.processErrors>before.processErrors || after.droppedEvents>before.droppedEvents,"bad output reported");
        uint32_t count=0;const auto* output=host->outputEvents(count);
        check(count && output[count-1].header()->type==CLAP_EVENT_NOTE_OFF &&
              reinterpret_cast<const clap_event_note_t*>(output[count-1].header())->key==-1,"malformed MIDI output releases downstream voices even with no valid events");
    }
}
void uridMapping()
{
    lv2::Urids urids;
    const std::string longUri="urn:oscairn:"+std::string(2048,'x');
    const auto longId=urids(longUri.c_str());const char* stable=lv2::Urids::unmap(&urids,longId);
    check(longId && stable && longUri==stable,"long URI round trips");
    std::atomic<bool> good{true};std::array<std::thread,4> threads;
    for(auto& thread:threads)thread=std::thread([&]{
        for(int i=0;i<1500;++i)
        {
            const auto uri="urn:oscairn:concurrent:"+std::to_string(i);
            const auto id=urids(uri.c_str());const auto* unmapped=lv2::Urids::unmap(&urids,id);
            if(!id || !unmapped || uri!=unmapped || urids(uri.c_str())!=id)good=false;
        }
    });
    for(auto& thread:threads)thread.join();
    check(good && lv2::Urids::unmap(&urids,longId)==stable && longUri==stable,"concurrent URI mapping grows beyond 512 and preserves pointers");
    check(!lv2::Urids::unmap(&urids,0) && !lv2::Urids::unmap(&urids,100000) && !urids(nullptr),"invalid URI mapping inputs");
}
void audit(const char* bundle)
{
    auto host=loadPlugin(bundle,"urn:oscairn:test:audit",48000,512);Audio audio;
    check(render(*host,audio),"audit fixture renders with required options and optional ports");
    check(audio.l[0]==440 && audio.l[1]==1 && audio.l[2]==-1 && audio.l[3]==0,"sample-rate default, toggle, integer and disconnected optional atom ports");
    const auto& frequency=*std::find_if(host->parameters().begin(),host->parameters().end(),[](const auto& p){return p.id==9;});
    check(frequency.minimum==0 && frequency.maximum==24000 && frequency.defaultValue==440,"sample rate scales bounds only");
    const std::array edits{param(.25,0,10),param(-1.5,0,11),param(30000,0,9)};
    check(render(*host,audio,edits.data(),edits.size()) && audio.l[0]==24000 && audio.l[1]==1 && audio.l[2]==-1,"sample-timed controls normalize and clamp correctly");
    check(host->setParameter(10,-.2) && host->setParameter(11,2.5),"queue audit controls");
    check(render(*host,audio) && audio.l[1]==0 && audio.l[2]==2,"queued controls use the same normalization");
    clap_event_transport_t t{};
    t.flags=CLAP_TRANSPORT_HAS_TEMPO|CLAP_TRANSPORT_HAS_BEATS_TIMELINE|CLAP_TRANSPORT_HAS_SECONDS_TIMELINE|CLAP_TRANSPORT_HAS_TIME_SIGNATURE|CLAP_TRANSPORT_IS_PLAYING;
    t.tempo=120;t.tempo_inc=.1;t.tsig_num=6;t.tsig_denom=8;
    t.song_pos_beats=static_cast<int64_t>(8.9999*CLAP_BEATTIME_FACTOR);
    t.song_pos_seconds=CLAP_SECTIME_FACTOR;t.bar_start=6*CLAP_BEATTIME_FACTOR;t.bar_number=42;
    const auto split=param(.5,17);
    check(render(*host,audio,&split,1,&t),"6/8 transport across parameter split");
    const auto quarter=static_cast<double>(t.song_pos_beats)/CLAP_BEATTIME_FACTOR+(17*120+17*16*.5*.1)/48000/60;
    check(std::abs(host->parameterValue(5)-243.4)<1e-4 && std::abs(host->parameterValue(7)-quarter*2)<1e-5,"meter-beat tempo and position include tempo increment");
    check(host->parameterValue(13)==43 && std::abs(host->parameterValue(14)-(quarter-9)*2)<1e-5,"bar origin survives meter history and split crossing");
    check(host->parameterValue(6)==48017 && host->parameterValue(15)==8 && host->parameterValue(16)==6 && host->parameterValue(17)==1,"frame and meter telemetry");
    t.flags&=~CLAP_TRANSPORT_IS_PLAYING;
    check(render(*host,audio,&split,1,&t) && host->parameterValue(6)==48000 && host->parameterValue(13)==42 && host->parameterValue(17)==0,"stopped transport does not advance at a split");
    t.tsig_num=3;t.tsig_denom=4;t.tempo_inc=0;t.song_pos_beats=-CLAP_BEATTIME_FACTOR;t.bar_start=-3*CLAP_BEATTIME_FACTOR;t.bar_number=-1;
    check(render(*host,audio,nullptr,0,&t) && host->parameterValue(7)==-1 && host->parameterValue(13)==-1 && host->parameterValue(14)==2,"negative preroll positions");
}
void events(const char* bundle)
{
    auto host=loadPlugin(bundle,"urn:oscairn:test:synth",48000,512);Audio audio;Transport transport(48000,137);transport.advance(48000);const auto t=transport.current();
    const std::array events{Event::note(true,60,1,7,2),param(.25,17),pressure(2,60,1,23),Event::note(false,60,0,41,2)};
    check(render(*host,audio,events.data(),events.size(),&t),"sample-timed LV2 render");
    for(int i=0;i<64;++i){const float expected=i<7 || i>=41?0:i<17?.5f:i<23?.25f:.5f;check(audio.l[i]==expected,"notes, pressure, parameter at exact sample");}
    check(host->parameterValue(5)==137 && host->parameterValue(6)==48017,"transport rebased at split");
    check(std::abs(host->parameterValue(7)-(1+17/48000.)*137/60)<1e-5,"beat position preserved");
    uint32_t count=0;auto* output=host->outputEvents(count);check(count==3,"MIDI output round trip");
    check(output[0].header()->time==7 && output[1].header()->time==23 && output[1].header()->type==CLAP_EVENT_NOTE_EXPRESSION && output[2].header()->time==41,"output offsets restored across splits");
    const std::array two{Event::note(true,61,1,0,2),Event::note(true,62,1,0,2),pressure(2,61,1,10),pressure(2,-1,1,20)};
    check(render(*host,audio,two.data(),two.size()),"poly and channel pressure render");
    check(audio.l[0]==.5f && audio.l[10]==.75f && audio.l[20]==1.f,"poly pressure targets one note, channel pressure targets both");
    const auto panic=Event::note(false,-1,0,0,-1,-1);
    check(render(*host,audio,&panic,1) && audio.l[0]==0,"wildcard note-off releases all channels");
    const auto held=Event::note(true,60,1,0,2);
    check(render(*host,audio,&held,1),"held note before atom overflow");
    std::array<Event,kick::clap::Host::maxBlockEvents> burst;
    burst.fill(pressure(2,60,0,0));
    check(host->setParameter(0,.75) && render(*host,audio,burst.data(),burst.size()) && audio.l[0]==.75f,"full graph event budget cannot starve queued controls");
    burst.fill(pressure(-1,-1,1,0)); // expands across all 16 channels
    check(render(*host,audio,burst.data(),burst.size()),"bounded atom overflow handled");
    output=host->outputEvents(count);
    check(count>0 && output[count-1].header()->type==CLAP_EVENT_NOTE_OFF &&
          reinterpret_cast<const clap_event_note_t*>(output[count-1].header())->key==-1,
          "MIDI output overflow releases downstream voices");
    check(render(*host,audio) && audio.l[0]==0,"MIDI input overflow releases held voices on next block");
}
void worker(const char* bundle)
{
    const auto note=Event::note(true,60,1,7);
    auto offline=loadPlugin(bundle,"urn:oscairn:test:worker",48000,512,ProcessingMode::Offline);
    Audio audio;check(render(*offline,audio,&note,1),"offline worker render");
    check(audio.l[6]==0 && audio.l[7]==1,"offline work and response affect the scheduling sample");
    check(offline->parameterValue(5)==1 && offline->parameterValue(6)==1,"response precedes end_run");
    const std::array edits{param(.25,10),param(.5,20)};
    check(render(*offline,audio,edits.data(),edits.size()) && offline->parameterValue(6)==4,"end_run after every split run, even without replies");
    auto realtime=loadPlugin(bundle,"urn:oscairn:test:worker",48000,512);
    check(render(*realtime,audio,&note,1),"realtime worker render");
    check(audio.l[7]==.5f,"live work response is delivered outside plugin run");
    bool applied=false;
    for(int i=0;i<10000 && !applied;++i){check(render(*realtime,audio),"live worker follow-up");applied=audio.l[0]==1;std::this_thread::yield();}
    check(applied,"background result reaches live audio");
    {
        Graph live(48000,512);const auto node=live.add(bundle,"urn:oscairn:test:worker");
        live.connectNotes(graphInput,node);live.connectAudio(node,graphOutput);live.commit();
        std::array<Event,64> burst;burst.fill(Event::note(true,60,1));
        check(live.process(audio.out,64,nullptr,burst.data(),burst.size()),"live graph schedules worker requests");
        live.remove(node);live.commit();
        forbidAllocation=true;const auto ok=live.process(audio.out,64,nullptr);forbidAllocation=false;
        live.collect(); // Plugin/worker teardown belongs to the control thread.
        check(ok && !live.contains(node) && audio.l[0]==0,"worker node retires while graph rendering continues");
    }
    std::vector<float> reference;
    for(uint32_t block:{37u,512u})
    {
        Graph graph(48000,block,ProcessingMode::Offline);auto node=graph.add(bundle,"urn:oscairn:test:worker");
        graph.connectNotes(graphInput,node);graph.connectAudio(node,graphOutput);graph.commit();
        std::vector<float> result;Audio out;
        for(uint32_t offset=0;offset<2048;offset+=block)
        {
            const auto frames=std::min(block,2048-offset);
            auto event=Event::note(true,60,1,offset<=100 && 100<offset+frames?100-offset:0);
            const bool send=offset<=100 && 100<offset+frames;
            check(graph.process(out.out,frames,nullptr,send?&event:nullptr,send?1:0),"offline worker graph render");
            result.insert(result.end(),out.l.begin(),out.l.begin()+frames);
        }
        if(reference.empty())reference=result;else check(reference==result,"offline worker graph is independent of block size");
    }
}
void graph(const char* bundle,const char* seq,const char* synth)
{
    for(uint32_t block:{37u,512u})
    {
        Graph graph(48000,block);const auto source=graph.add(seq),target=graph.add(bundle,"urn:oscairn:test:synth");
        graph.connectNotes(source,target);graph.connectAudio(target,graphOutput);graph.mapParamTarget(source,0,target,0);graph.commit();
        Transport transport(48000);Audio audio;
        for(uint32_t offset=0;offset<48000;offset+=block)
        {
            const auto t=transport.current();forbidAllocation=true;const auto ok=graph.process(audio.out,block,&t);forbidAllocation=false;check(ok,"mixed graph renders");
            constexpr float locks[]{.8f,.2f,.5f,.35f,.8f,.2f,.5f,.35f,.65f,.15f,.4f,.25f,.65f,.15f,.4f,.25f};
            for(uint32_t i=0;i<block;++i){const auto sample=offset+i;check(audio.l[i]==(sample%6000<3000?2*locks[(sample/6000)%16]:0.f),"CLAP lock to LV2 gain on exact sample");check(audio.l[i]==audio.r[i],"mono LV2 centered");}
            transport.advance(block);
        }
    }
    Graph direct(48000,512),mixed(48000,512);const auto a=direct.add(synth),b=mixed.add(synth),effect=mixed.add(bundle,"urn:oscairn:test:effect");
    direct.connectNotes(graphInput,a);direct.connectAudio(a,graphOutput);direct.commit();mixed.connectNotes(graphInput,b);mixed.connectAudio(b,effect);mixed.connectAudio(effect,graphOutput);mixed.commit();
    mixed.host(effect).setParameter(0,.25);Audio expected,actual;const auto note=Event::note(true,60,1);
    for(int i=0;i<10;++i)
    {
        forbidAllocation=true;const bool ok=direct.process(expected.out,64,nullptr,i?nullptr:&note,i?0:1) && mixed.process(actual.out,64,nullptr,i?nullptr:&note,i?0:1);forbidAllocation=false;check(ok,"CLAP synth to LV2 effect");
        for(int k=0;k<64;++k)check(actual.l[k]==expected.l[k]*.25f,"LV2 effect processes CLAP audio exactly");
    }
    // Atom MIDI output enters a CLAP synth in the same block, including pressure.
    Graph routed(48000,512);const auto router=routed.add(bundle,"urn:oscairn:test:synth"),destination=routed.add(synth);
    routed.connectNotes(graphInput,router);routed.connectNotes(router,destination);routed.connectAudio(destination,graphOutput);routed.commit();
    Graph reference(48000,512);const auto referenceSynth=reference.add(synth);reference.connectNotes(graphInput,referenceSynth);reference.connectAudio(referenceSynth,graphOutput);reference.commit();
    const std::array messages{Event::note(true,60,1,5),pressure(0,60,1,19),Event::note(false,60,0,50)};
    forbidAllocation=true;const bool ok=routed.process(actual.out,64,nullptr,messages.data(),messages.size()) && reference.process(expected.out,64,nullptr,messages.data(),messages.size());forbidAllocation=false;
    check(ok && std::equal(actual.l.begin(),actual.l.begin()+64,expected.l.begin()),"LV2 MIDI output routes notes and pressure into CLAP");
}
void control(const char* bundle)
{
    Session session(48000,64);JsonRpc rpc(session.control());
    const auto response=rpc.dispatch(std::string(R"({"jsonrpc":"2.0","id":1,"method":"graph.add","params":{"path":")")+bundle+R"(","pluginId":"urn:oscairn:test:synth","name":"lv2"}})");
    check(response && response->find("error")==std::string::npos,"JSON adds LV2 bundle by URI");
    const auto node=session.control().nodeId("lv2");session.connectNotes(graphInput,node);session.connectAudio(node,graphOutput);session.commit();
    check(session.control().setParameter(node,0,.75) && session.control().note(true,60,1),"session controls LV2");session.service();Audio audio;
    forbidAllocation=true;const auto ok=session.process(audio.out,64);forbidAllocation=false;
    check(ok && audio.l[0]==.75f,"session control reaches LV2 render");
    // Control telemetry is atomic while render consumes the bounded queue.
    std::atomic<bool> run{true},good{true};std::atomic<uint32_t> blocks{0};
    std::thread thread([&]{Audio block;while(run.load()){forbidAllocation=true;if(!session.process(block.out,64))good=false;forbidAllocation=false;++blocks;}});
    while(!blocks.load())std::this_thread::yield();
    for(int i=0;i<1000;++i){session.control().setParameter(node,0,(i%100)/100.0);session.service();session.control().parameter(node,0);}
    run=false;thread.join();check(good && blocks>0,"concurrent controls and rendering");
}
}
int main(int argc,char** argv)
{
    try{if(argc!=4)return 2;uridMapping();policy(argv[1]);audit(argv[1]);events(argv[1]);worker(argv[1]);graph(argv[1],argv[2],argv[3]);control(argv[1]);std::cout<<"ok: LV2 audit regressions, policy, MIDI/pressure/time, sample-accurate mixed graphs, control API and concurrent rendering\n";return 0;}
    catch(const std::exception& e){forbidAllocation=false;std::cerr<<e.what()<<'\n';return 1;}
}
