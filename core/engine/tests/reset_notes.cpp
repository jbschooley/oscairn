// SPDX-License-Identifier: AGPL-3.0-or-later
// A host that resets its plugin on its own (a panic, an event overflow) ends every note it sent:
// CLAP's reset has no output events, so the plugin sends no NOTE_END for them. The graph must
// forget them, or a held note it kept would keep a retiring node from going, hold a place under
// the note limit, and count as a live note in the shared budget. The tail fixture ends its voice
// on reset without NOTE_END. Its host is reset behind the graph's back: in the engine, through
// a prepared instrument in front of it, in a worker, and a Basic Synth two nodes share.
//   oscairn-reset-notes-test WORKER FIXTURE.clap BASIC-SYNTH.clap
#include "oscairn/graph.h"
#include "oscairn/sound_context.h"
#include "../src/PreparedInstrument.h"
#include "AllocationGuard.h"
#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
using namespace oscairn;
using kick::clap::Event;
static void check(bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);}

namespace {
constexpr uint32_t frames=128;
constexpr uint64_t never=uint64_t(1)<<40;
// What a node was sent: chokes and volume expressions, the graph giving a note up.
struct Heard : Graph::Listener {
    uint32_t chokes=0,fades=0;
    void events(NodeId,const Event* list,uint32_t n,bool emitted) noexcept override {
        if(emitted)return;
        for(uint32_t i=0;i<n;++i){
            const auto* h=list[i].header();if(h->space_id!=CLAP_CORE_EVENT_SPACE_ID)continue;
            if(h->type==CLAP_EVENT_NOTE_CHOKE)++chokes;
            if(h->type==CLAP_EVENT_NOTE_EXPRESSION && reinterpret_cast<const clap_event_note_expression_t*>(h)->expression_id==CLAP_NOTE_EXPRESSION_VOLUME)++fades;
        }
    }
};
bool render(Graph& g,std::vector<Event> e={}){
    std::array<float,frames> l{},r{};float* out[]{l.data(),r.data()};
    forbidAllocation=true;const bool ok=g.process(out,frames,nullptr,e.data(),uint32_t(e.size()));forbidAllocation=false;return ok;
}

// node plays `host`; reset is the plugin's own host, which the graph does not know.
void single(const std::string& name,std::shared_ptr<PluginHost> node,PluginHost& reset){
    Graph g(48000,frames);
    const auto n=g.add(node);
    NoteZone z;z.maxNotes=1;
    g.connectNotes(graphInput,n,{z});g.connectAudio(n,graphOutput);g.setRetiring(n,true);g.commit();
    g.setVoiceBudget(1e6); // far above what it takes: nothing is given up, but its notes are counted
    for(int i=0;i<10;++i)check(render(g),name+": rendered");
    check(render(g,{Event::note(true,60,.8)}),name+": rendered");
    for(int i=0;i<10;++i)check(render(g),name+": rendered");
    check(g.sounding(n)==1,name+": the held note sounds: "+std::to_string(g.sounding(n)));
    const auto measured=g.noteCost(n).perNote;
    check(measured>=0,name+": its cost per note is measured");
    reset.panic();
    for(int i=0;i<20;++i)check(render(g),name+": rendered");
    check(g.sounding(n)==0,name+": the reset note no longer sounds: "+std::to_string(g.sounding(n)));
    check(g.idle(n,1024,never,never),name+": the node may go, silent with no note sounding");
    check(g.noteCost(n).perNote==measured,name+": no block counted a live note since the reset");
    Heard heard;g.setListener(&heard);
    check(render(g,{Event::note(true,64,.8)}),name+": rendered");
    g.setListener(nullptr);
    check(!heard.chokes && !heard.fades,name+": the next note takes no place a reset note held");
    std::printf("ok: %s\n",name.c_str());
}
}

int main(int argc,char** argv){
    if(argc!=4)return 2;
    const std::string worker=argv[1],fixture=argv[2],synth=argv[3],tail="org.kick.test.tail";
    int failures=0;
    auto run=[&](auto&& scenario){try{scenario();}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());++failures;}};
    run([&]{auto host=loadPlugin(fixture,tail,48000,frames);single("in the engine",host,*host);});
    run([&]{auto inner=loadPlugin(fixture,tail,48000,frames);single("through a prepared instrument",preparedInstrument(inner,48000,frames,ProcessingMode::Realtime,{}),*inner);});
    run([&]{
        auto host=loadPluginInWorker(worker,fixture,tail,48000,frames);
        check(host->inWorker(),"the fixture in a worker");
        single("in a worker",host,*host);});
    run([&]{auto inner=loadPluginInWorker(worker,fixture,tail,48000,frames);single("through a prepared instrument, in a worker",preparedInstrument(inner,48000,frames,ProcessingMode::Realtime,{}),*inner);});
    run([&]{
        // Two nodes on one Basic Synth: a reset of the instance ends both nodes' notes.
        Graph g(48000,frames);
        auto plugin=loadPlugin(synth,"",48000,frames);
        auto engine=SoundContext::lead(plugin);
        check(engine!=nullptr,"the Basic Synth can share");
        const auto sa=plugin->prepareSound("A",{}),sb=plugin->prepareSound("B",{});
        const auto a=g.add(std::make_shared<SoundContext>(engine,0,1,sa)),b=g.add(std::make_shared<SoundContext>(engine,1,2,sb));
        NoteZone low;low.highKey=63;NoteZone high;high.lowKey=64;
        g.connectNotes(graphInput,a,{low});g.connectNotes(graphInput,b,{high});g.connectAudio(a,graphOutput);g.connectAudio(b,graphOutput);
        g.setRetiring(a,true);g.setRetiring(b,true);g.commit();
        for(int i=0;i<4;++i)check(render(g),"shared: rendered");
        check(render(g,{Event::note(true,40,.8),Event::note(true,90,.8)}),"shared: rendered");
        for(int i=0;i<10;++i)check(render(g),"shared: rendered");
        check(g.sounding(a)==1 && g.sounding(b)==1,"shared: each node's held note sounds");
        plugin->panic();
        for(int i=0;i<40;++i)check(render(g),"shared: rendered");
        check(g.sounding(a)==0 && g.sounding(b)==0,"shared: neither node's reset note sounds: "+std::to_string(g.sounding(a))+" and "+std::to_string(g.sounding(b)));
        check(g.idle(a,1024,never,never) && g.idle(b,1024,never,never),"shared: both nodes may go");
        std::puts("ok: a shared instance");
    });
    return failures?1:0;
}
