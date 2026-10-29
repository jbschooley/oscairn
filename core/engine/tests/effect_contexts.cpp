// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/session.h"
#include "oscairn/json_rpc.h"
#include "AllocationGuard.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <thread>
#include <atomic>

using namespace oscairn;
using kick::clap::Event;
static void check(bool ok,const char* message){if(!ok){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}}
struct Audio {
    std::array<std::array<float,127>,18> data{};
    std::array<float*,18> out{};
    Audio(){for(size_t i=0;i<out.size();++i)out[i]=data[i].data();}
};
static Event parameter(uint32_t id,double value,uint32_t time) {
    clap_event_param_value_t e{};e.header={sizeof(e),time,CLAP_CORE_EVENT_SPACE_ID,CLAP_EVENT_PARAM_VALUE,0};
    e.param_id=id;e.value=value;e.note_id=e.port_index=e.channel=e.key=-1;return Event::from(e);
}
static void process(PluginHost& host,Audio& audio,const float* const* in,uint32_t frames,const Event* events=nullptr,uint32_t count=0) {
    forbidAllocation=true;const bool ok=host.process(in,in?2:0,audio.out.data(),18,frames,events,count,nullptr);forbidAllocation=false;check(ok,"render");
}
int main(int argc,char** argv) {
    check(argc==3,"delay and synth paths");
    auto root=loadPlugin(argv[1],"",48000,127),child=root->createEffectContext();
    auto reference=loadPlugin(argv[1],"",48000,127);
    auto initial=root->effectResources();check(initial && initial->prepared==2 && !initial->tail_active,"two contexts share one engine");
    check(initial->context_bytes>=768008,"per-context history accounted");
    check(child->setParameter(1,.125) && child->setParameter(2,.7) && child->setParameter(3,.8),"child parameters");
    check(reference->setParameter(1,.125) && reference->setParameter(2,.7) && reference->setParameter(3,.8),"reference parameters");
    Audio a,b,silent;std::array<float,127> input{};const float* inputs[]{input.data(),input.data()};
    for(int block=0;block<1400;++block) {
        const uint32_t n=block%2?37:127;input.fill(0);if(block<4)input[0]=.6f;
        const auto event=parameter(2,.25,13);const bool edit=block==127;
        process(*child,a,inputs,n,edit?&event:nullptr,edit?1:0);process(*reference,b,inputs,n,edit?&event:nullptr,edit?1:0);
        process(*root,silent,nullptr,n);
        for(uint32_t c=0;c<2;++c)for(uint32_t i=0;i<n;++i){check(a.data[c][i]==b.data[c][i],"shared context equals separate instance, uneven blocks and sample automation");check(silent.data[c][i]==0,"history isolation");}
    }
    check(root->parameterValue(2)==.45,"parameter isolation");
    check(child->effectResources()->tail_active,"outgoing tail retained");
    // A tail can retire authoritatively after sufficient silence, without a control reset.
    check(child->setParameter(2,0),"stop feedback");
    for(int i=0;i<1800 && child->effectResources()->tail_active;++i)process(*child,a,nullptr,127);
    check(!child->effectResources()->tail_active,"tail completion");
    const auto temp=std::filesystem::temp_directory_path()/("oscairn-effects-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    child->saveState(temp.string());auto restored=loadPlugin(argv[1],"",48000,127);restored->loadState(temp.string());
    check(restored->parameterValue(1)==.125 && restored->parameterValue(2)==0 && restored->parameterValue(3)==.8,"context patch state round trip");
    for(const char* bad:{"oscairn-delay-1 nan .2 .3\n","oscairn-delay-1 1 2 .3\n","oscairn-delay-1 1 .2 .3 junk","oscairn-delay-2 1 .2 .3\n"}) {
        {std::ofstream stream(temp/"state.clap",std::ios::binary);stream<<bad;}
        bool rejected=false;try{restored->loadState(temp.string());}catch(const std::runtime_error&){rejected=true;}
        check(rejected && restored->parameterValue(1)==.125 && restored->parameterValue(2)==0,"malformed state rejected without partial parameter mutation");
    }
    std::filesystem::remove_all(temp);
    std::vector<std::shared_ptr<PluginHost>> extra;while(root->effectResources()->prepared<initial->capacity)extra.push_back(root->createEffectContext());
    bool exhausted=false;try{root->createEffectContext();}catch(const std::runtime_error&){exhausted=true;}check(exhausted,"bounded capacity");
    process(*child,a,nullptr,127);extra.clear();root.reset();check(child->effectResources()->prepared==1,"child outlives originating interface");
    process(*child,a,inputs,127);
    // Prepare and retire siblings while the existing context renders.
    std::atomic<bool> done=false;std::thread audio([&]{Audio local;while(!done.load())process(*child,local,nullptr,127);});
    for(int i=0;i<40;++i){auto sibling=child->createEffectContext();check(sibling->effectResources()->prepared==2,"concurrent preparation");}
    done=true;audio.join();

    for(bool serial:{false,true}) {
        Graph shared(48000,127,ProcessingMode::Realtime,{},{{{0,"Input",2,0}},{{0,"Main",2,0}}});
        Graph separate(48000,127,ProcessingMode::Realtime,{},{{{0,"Input",2,0}},{{0,"Main",2,0}}});
        const auto x=shared.add(argv[1]),y=shared.addEffectContext(x),rx=separate.add(argv[1]),ry=separate.add(argv[1]);
        for(auto* g:{&shared,&separate}) {const auto first=g==&shared?x:rx,second=g==&shared?y:ry;
            g->host(first).setParameter(1,.125);g->host(second).setParameter(1,.25);g->host(second).setParameter(3,.9);
            g->connectAudio(graphInput,first);g->connectAudio(serial?first:graphInput,second);
            if(!serial)g->connectAudio(first,graphOutput);g->connectAudio(second,graphOutput);g->commit();}
        for(int k=0;k<600;++k){input.fill(0);if(k<3)input[k]=.5;
            forbidAllocation=true;const bool ok=shared.process(inputs,2,a.out.data(),2,127,nullptr)&&separate.process(inputs,2,b.out.data(),2,127,nullptr);forbidAllocation=false;check(ok,"serial/parallel graph");
            for(int c=0;c<2;++c)check(a.data[c]==b.data[c],"graph contexts equal separate references");}
        shared.remove(x);shared.commit();check(shared.process(a.out.data(),127,nullptr),"remove parent while child survives audio plan");shared.collect();
        check(shared.host(y).effectResources()->prepared==1,"retired plans release originating context");
    }
    // Two sound patches use separate buses; a later selection/parameter edit cannot move held voices.
    auto synth=loadPlugin(argv[2],"",48000,127);
    auto first=synth->prepareSound("saws",{{4,1},{8,1}}),second=synth->prepareSound("square",{{4,2},{6,18},{7,70},{8,2}});
    check(synth->selectSound(first),"select first bus");auto note=Event::note(true,60,.8,0,0,0,1);process(*synth,a,nullptr,127,&note,1);
    check(synth->selectSound(second),"select second bus");note=Event::note(true,67,.8,0,0,0,2);process(*synth,a,nullptr,127,&note,1);
    check(std::all_of(a.data[0].begin(),a.data[0].end(),[](float x){return x==0;}),"separate sounds absent from main");
    auto energy=[](const auto& v){double x=0;for(float f:v)x+=std::abs(f);return x;};
    check(energy(a.data[2])>0 && energy(a.data[4])>0,"old/new sounds overlap on distinct buses");
    check(synth->setParameter(8,3),"change next-note output");process(*synth,a,nullptr,127);
    check(energy(a.data[2])>0 && energy(a.data[4])>0 && energy(a.data[6])==0,"held voices retain bus ownership");
    std::array<Event,3> release{Event::midi(0xb0,64,127),Event::note(false,60,0,0,0,0,1),Event::note(false,67,0,0,0,0,2)};
    process(*synth,a,nullptr,127,release.data(),release.size());for(int i=0;i<100;++i)process(*synth,a,nullptr,127);
    check(energy(a.data[2])>0 && energy(a.data[4])>0,"sustain keeps both bus owners");
    note=Event::midi(0xb0,64,0);process(*synth,a,nullptr,127,&note,1);for(int i=0;i<1500;++i)process(*synth,a,nullptr,127);
    check(energy(a.data[2])==0 && energy(a.data[4])==0,"release drains both buses");

    // Full transition: one multitimbral synth and shared delays equal two independent chains.
    Graph combined(48000,127),leftChain(48000,127),rightChain(48000,127);
    const auto instrument=combined.add(argv[2]),fx=combined.add(argv[1]),fx2=combined.addEffectContext(fx);
    const auto li=leftChain.add(argv[2]),lf=leftChain.add(argv[1]),ri=rightChain.add(argv[2]),rf=rightChain.add(argv[1]);
    auto warm=combined.host(instrument).prepareSound("Warm",{{4,1},{8,1}});
    auto bright=combined.host(instrument).prepareSound("Bright",{{4,2},{6,18},{7,70},{8,2}});
    combined.host(instrument).selectSound(warm);
    auto lp=leftChain.host(li).prepareSound("Warm",{{4,1}}),rp=rightChain.host(ri).prepareSound("Bright",{{4,2},{6,18},{7,70}});
    leftChain.host(li).selectSound(lp);rightChain.host(ri).selectSound(rp);
    combined.connectNotes(graphInput,instrument);combined.connectAudio({{instrument,1,0},{fx,0,0},2});combined.connectAudio({{instrument,2,0},{fx2,0,0},2});
    combined.connectAudio(fx,graphOutput);combined.connectAudio(fx2,graphOutput);combined.host(fx).setParameter(1,.125);combined.host(fx2).setParameter(1,.5);combined.commit();
    for(auto* g:{&leftChain,&rightChain}){const auto i=g==&leftChain?li:ri,f=g==&leftChain?lf:rf;g->connectNotes(graphInput,i);g->connectAudio(i,f);g->connectAudio(f,graphOutput);g->host(f).setParameter(1,g==&leftChain?.125:.5);g->commit();}
    Audio c;double tailEnergy=0;
    for(int k=0;k<1600;++k) {
        const uint32_t n=k%2?37:127;
        Event ev;uint32_t count=0;
        if(k==0){ev=Event::note(true,60,.8,3);count=1;}
        if(k==80){combined.host(instrument).selectSound(bright);ev=Event::note(true,67,.8,5);count=1;}
        if(k==180){ev=Event::note(false,60,0,7);count=1;}
        if(k==240){ev=Event::note(false,67,0,11);count=1;}
        const bool forLeft=k==0 || k==180;
        forbidAllocation=true;
        const bool ok=combined.process(a.out.data(),n,nullptr,count?&ev:nullptr,count) && leftChain.process(b.out.data(),n,nullptr,count&&forLeft?&ev:nullptr,count&&forLeft?1:0) && rightChain.process(c.out.data(),n,nullptr,count&&!forLeft?&ev:nullptr,count&&!forLeft?1:0);
        forbidAllocation=false;check(ok,"sound-chain transition render");
        for(uint32_t ch=0;ch<2;++ch)for(uint32_t j=0;j<n;++j){check(std::abs(a.data[ch][j]-(b.data[ch][j]+c.data[ch][j]))<1e-7,"held voices and tails equal independent reference chains");if(k>1000)tailEnergy+=std::abs(a.data[ch][j]);}
    }
    check(tailEnergy>0,"tails overlap after voices finish");
    // A shared send deliberately mixes inputs before processing, unlike separate contexts.
    Graph send(48000,127,ProcessingMode::Realtime,{},{{{0,"A",2,0},{1,"B",2,0}},{{0,"Main",2,0}}});
    auto sendNode=send.add(argv[1]);send.connectAudio({{graphInput,0,0},{sendNode,0,0},2});send.connectAudio({{graphInput,1,0},{sendNode,0,0},2});send.connectAudio(sendNode,graphOutput);send.commit();
    auto summed=loadPlugin(argv[1],"",48000,127);std::array<float,127> sum{};const float* four[]{input.data(),input.data(),input.data(),input.data()};const float* twice[]{sum.data(),sum.data()};
    for(int k=0;k<400;++k){input.fill(0);if(!k)input[0]=.5;for(size_t j=0;j<127;++j)sum[j]=2*input[j];
        forbidAllocation=true;const bool ok=send.process(four,4,a.out.data(),2,127,nullptr);forbidAllocation=false;check(ok,"shared send render");process(*summed,b,twice,127);check(a.data[0]==b.data[0] && a.data[1]==b.data[1],"shared send equals one summed input");}

    // Report plugin-owned memory and comparative wall time; never assert a CPU speedup.
    auto engine=loadPlugin(argv[1],"",48000,127),context=engine->createEffectContext();
    auto r1=loadPlugin(argv[1],"",48000,127),r2=loadPlugin(argv[1],"",48000,127);
    auto measure=[&](PluginHost& x,PluginHost& y){const auto start=std::chrono::steady_clock::now();for(int i=0;i<2000;++i){process(x,a,inputs,127);process(y,b,inputs,127);}return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();};
    const auto memory=engine->effectResources();
    std::printf("plugin memory: one=%llu bytes, two shared=%llu, two separate=%llu (host/allocator overhead excluded)\n",(unsigned long long)(memory->shared_bytes+memory->context_bytes),(unsigned long long)(memory->shared_bytes+2*memory->context_bytes),(unsigned long long)(2*(memory->shared_bytes+memory->context_bytes)));
    std::printf("two-context render wall time: shared %.3f ms, separate %.3f ms (508000 context-frames total per run)\n",measure(*engine,*context),measure(*r1,*r2));
    std::puts("ok: independent effect contexts, lifecycle, routing, state, tails and sound buses");
}
