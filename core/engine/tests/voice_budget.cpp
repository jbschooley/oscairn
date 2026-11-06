// SPDX-License-Identifier: AGPL-3.0-or-later
// RFC 0018 step 3: the shared budget, rendered offline. Three parts play a chart that asks for
// more than the budget, a note every other block: first B's loud notes (priority 5, reserving 8),
// the oldest; then A's bass and melody, notes A releases, its quiet notes and loud ones, and C's
// loud ones. Released notes are newer and as loud as the held ones, so neither age nor velocity
// alone puts them first. Without the budget the plugins' time passes it in most blocks;
// with it, they stay under, and the notes given up (faded, or choked at the hard limit) are the released and quiet ones before any
// loud held note (A's bass and melody spared), B's last. Usage: plugin [notes per group [share]],
// the budget being the plugins at rest and that share of what the notes add (default .5); the voice fixture
// (OSCAIRN_FIXTURE_VOICE_US) costs a fixed time per voice, a real instrument what it costs.
#include "oscairn/graph.h"
#include "AllocationGuard.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
using namespace oscairn;
using kick::clap::Event;
static void check(bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);}

namespace {
constexpr uint32_t frames=64,first=150,last=230; // the measured window: every note sounding
struct Steal { NodeId node; int32_t id; bool hard; uint32_t block=0; };
// What each node receives: note IDs to keys and velocities, which were released, and the notes given up in order.
struct Recorder : Graph::Listener {
    struct Note { int16_t key=0; double velocity=0; bool released=false; uint32_t block=0; }; // block: when it started
    std::map<std::pair<NodeId,int32_t>,Note> notes;std::map<std::pair<NodeId,int32_t>,bool> fading;std::vector<Steal> steals;
    std::array<Steal,256> pending{};size_t count=0; // audio thread: no allocation; drained after each block
    std::array<std::pair<NodeId,clap_event_note_t>,256> seen{};size_t seenCount=0;std::array<bool,256> released{};
    void events(NodeId node,const Event* list,uint32_t n,bool emitted) noexcept override {
        if(emitted)return;
        for(uint32_t i=0;i<n;++i){
            const auto* h=list[i].header();if(h->space_id!=CLAP_CORE_EVENT_SPACE_ID)continue;
            if((h->type==CLAP_EVENT_NOTE_ON || h->type==CLAP_EVENT_NOTE_OFF) && seenCount<seen.size()){released[seenCount]=h->type==CLAP_EVENT_NOTE_OFF;seen[seenCount++]={node,*reinterpret_cast<const clap_event_note_t*>(h)};}
            if(h->type==CLAP_EVENT_NOTE_EXPRESSION && count<pending.size()){const auto& x=*reinterpret_cast<const clap_event_note_expression_t*>(h);if(x.expression_id==CLAP_NOTE_EXPRESSION_VOLUME)pending[count++]={node,x.note_id,false};}
            if(h->type==CLAP_EVENT_NOTE_CHOKE && count<pending.size())pending[count++]={node,reinterpret_cast<const clap_event_note_t*>(h)->note_id,true};
        }
    }
    void drain(uint32_t block){
        for(size_t i=0;i<seenCount;++i){auto& note=notes[{seen[i].first,seen[i].second.note_id}];if(released[i])note.released=true;else{note.key=seen[i].second.key;note.velocity=seen[i].second.velocity;note.block=block;}}
        seenCount=0;
        // A fade's first step is a steal; a choke not after a fade a hard one.
        for(size_t i=0;i<count;++i){pending[i].block=block;auto& f=fading[{pending[i].node,pending[i].id}];if(!pending[i].hard){if(!f)steals.push_back(pending[i]);f=true;}else if(!f){steals.push_back(pending[i]);f=true;}}
        count=0;
    }
};
struct Run { std::array<NodeId,3> nodes{};std::vector<float> totals,resting;uint64_t misses=0;std::array<Graph::NoteCost,3> costs{};Recorder recorder; };

void play(const char* plugin,uint32_t n,double budget,Run& run){
    Graph g(48000,frames);
    std::array<NodeId,3> node{};
    for(uint32_t p=0;p<3;++p){
        node[p]=g.add(plugin);
        NoteZone z;z.channels=uint16_t(1u<<p);z.part=p+1;if(p==1){z.priority=5;z.reserve=8;} // each part on its own channel
        g.connectNotes(graphInput,node[p],{z});g.connectAudio(node[p],graphOutput);
    }
    run.nodes=node;g.commit();g.setVoiceBudget(budget);g.setListener(&run.recorder);
    std::array<float,frames> left{},right{};float* out[]{left.data(),right.data()};
    // The chart: (block, on, key, velocity, part), a note every other block.
    struct Played { uint32_t block; bool on; int key; double velocity; int part; };
    std::vector<Played> chart;uint32_t at=20; // after 20 blocks at rest, measured
    auto add=[&](int key,double velocity,int part){chart.push_back({at,true,key,velocity,part});at+=2;};
    for(uint32_t i=0;i<2*n;++i)add(60+int(i),1,1);                 // B's loud notes
    add(24,1,0);add(108,1,0);                                        // A's bass and melody
    for(uint32_t i=0;i<n;++i){chart.push_back({at+1,false,36+int(i),0,0});add(36+int(i),1,0);} // A's notes released, each a block after it starts
    for(uint32_t i=0;i<n;++i)add(48+int(i),.2,0);                    // A's quiet notes
    for(uint32_t i=0;i<3*n;++i)add(60+int(i/3),1,i%3==0?0:2);        // A's loud notes, and twice as many of C's
    check(at<first,"the chart is played before the measured window");
    for(uint32_t block=0;block<=last;++block){
        std::vector<Event> events; // built before rendering; the block itself allocates nothing
        for(const auto& p:chart)if(p.block==block)events.push_back(Event::note(p.on,p.key,p.velocity,0,p.part));
        forbidAllocation=true;
        const bool ok=g.process(out,frames,nullptr,events.data(),uint32_t(events.size()));
        forbidAllocation=false;
        check(ok,"rendered");
        run.recorder.drain(block);
        float total=0;for(const auto id:node)total+=float(g.host(id).statistics().lastMicroseconds);
        if(block>=first)run.totals.push_back(total);
        if(block>=5 && block<20)run.resting.push_back(total);
    }
    for(uint32_t p=0;p<3;++p){run.costs[p]=g.noteCost(node[p]);run.misses+=g.host(node[p]).statistics().deadlineMisses;}
    g.setListener(nullptr);
}
}

int main(int argc,char** argv){try{
    if(argc<2)return 2;
    const uint32_t n=argc>2?uint32_t(std::atoi(argv[2])):8;
    Run open;play(argv[1],n,0,open);
    // The plugins at rest, and a share of what the chart's notes add, by the median block (a loaded
    // machine's spikes aside).
    auto median=[](std::vector<float> v){std::sort(v.begin(),v.end());return v[v.size()/2];};
    const float mean=median(open.totals),rest=median(open.resting);
    const double share=argc>3?std::atof(argv[3]):.5,budget=rest+(mean-rest)*share;
    Run held;play(argv[1],n,budget,held);
    auto over=[&](const Run& r){size_t k=0;for(const auto t:r.totals)k+=t>budget;return k;};
    const double block=frames/48000.0*1e6;
    auto late=[&](const Run& r){size_t k=0;for(const auto t:r.totals)k+=t>block;return k;};
    std::printf("budget %.0f us a %u-frame block (%.0f us); without it the plugins took %.0f us in the median block, %.0f us at rest\n",budget,frames,block,mean,rest);
    for(const auto* r:{&open,&held})
        std::printf("%s: %zu of %zu blocks over the budget, %zu past the block's time, %llu plugin deadline misses\n",r==&open?"without the budget":"with the budget",over(*r),r->totals.size(),late(*r),(unsigned long long)r->misses);
    for(uint32_t p=0;p<3;++p)std::printf("part %c: %.1f us at rest, %.1f us per note, %llu stolen, %llu hard chokes\n","ABC"[p],held.costs[p].rest,held.costs[p].perNote,(unsigned long long)held.costs[p].stolen,(unsigned long long)held.costs[p].hardChokes);
    // Most blocks over the budget without it; hardly any with it.
    check(over(open)*10>=open.totals.size()*8,"without the budget the chart passes it in most blocks");
    // A loaded machine holds blocks up whatever plays; the budget still clears most of them.
    check(over(held)*4<=over(open),"with the budget the plugins stay under it, but for a quarter of the blocks at most");
    // The notes given up: released and quiet ones before any loud held note; B, reserving 8 at a
    // higher priority, loses the fewest and keeps its reserve.
    int lastQuiet=-1,firstLoud=-1;std::array<size_t,3> lost{};size_t cut=0;std::string order;
    uint32_t loudAt=UINT32_MAX;bool passedOver=false; // a released or quiet note sounding when a loud held one was given up, given up only later
    for(size_t i=0;i<held.recorder.steals.size();++i){
        const auto& s=held.recorder.steals[i];
        const auto note=held.recorder.notes.at({s.node,s.id});
        const bool loud=!note.released && note.velocity>=.5;
        cut+=s.hard;order+=s.hard?(loud?'L':'Q'):(loud?'l':'q');for(size_t p=0;p<3;++p)if(s.node==held.nodes[p])order+="abc"[p];order+=' ';
        for(size_t p=0;p<3;++p)lost[p]+=s.node==held.nodes[p];
        if(loud){if(firstLoud<0)firstLoud=int(i);loudAt=std::min(loudAt,s.block);}
        else{lastQuiet=int(i);passedOver|=note.block<loudAt && s.block>loudAt;}
    }
    std::printf("given up: %zu (A %zu, B %zu, C %zu), %zu of them choked before any fade; the last released or quiet one at %d, the first loud held one at %d\n",held.recorder.steals.size(),lost[0],lost[1],lost[2],cut,lastQuiet,firstLoud);
    std::printf("in order (q released or quiet, l loud, then the part; capitals choked before any fade): %s\n",order.c_str());
    check(lastQuiet>=0,"released and quiet notes were given up");
    check(!passedOver,"released and quiet notes before any loud held note (of those sounding then)");
    check(lost[1]<=lost[0] && lost[1]<=lost[2] && lost[1]<=2*n-std::min(2*n,8u),"the part of higher priority loses the fewest, never its reserve");
    std::puts("ok: the shared budget keeps the chart under it, giving up released and quiet notes first");
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}}
