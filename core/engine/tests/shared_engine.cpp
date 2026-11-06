// SPDX-License-Identifier: AGPL-3.0-or-later
// RFC 0021 slice 3: parts sharing one engine, in the graph. Usage: shared-engine basic-synth.
//
// Two parts on one Basic Synth instance, each a node playing its own sound on its own MIDI channel
// and output bus (SoundContext), render what two instances of their own render: notes, a pitch
// bend sent on the keyboard's channel to both, and a parameter edit on one part's node, which
// changes only that part's sound. Each node meters its own bus. With a shared budget, the instance
// is told each part's reserve and priority under the sound its channel selects, an entry per part
// (the recording host stands between the graph and the real plugin and keeps what it was given).
#include "oscairn/graph.h"
#include "oscairn/sound_context.h"
#include "AllocationGuard.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
using namespace oscairn;
using kick::clap::Event;
static void check(bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);}

namespace {
constexpr uint32_t frames=64;
// The plugin as it is, but keeping the voice limits the graph gives it.
struct Recording final : PluginHost
{
    std::shared_ptr<PluginHost> p;std::vector<oscairn_voice_budget_context_t> last;uint32_t calls=0;
    explicit Recording(std::shared_ptr<PluginHost> plugin):p(std::move(plugin)){last.reserve(OSCAIRN_VOICE_BUDGET_MAX_CONTEXTS);}
    void setVoiceLimits(uint32_t limit,const oscairn_voice_budget_context_t* contexts,uint32_t count) noexcept override {last.assign(contexts,contexts+count);++calls;p->setVoiceLimits(limit,contexts,count);}
    void service() override {p->service();}
    void saveSnapshot(const std::string& d) override {p->saveSnapshot(d);}
    void loadSnapshot(const std::string& d) override {p->loadSnapshot(d);}
    const char* soundContextMode() const noexcept override {return p->soundContextMode();}
    uint32_t soundContextCapacity() const noexcept override {return p->soundContextCapacity();}
    std::vector<oscairn_sound_context_info_t> soundContexts() const override {return p->soundContexts();}
    uint32_t prepareSound(const std::string& n,const std::vector<oscairn_sound_parameter_t>& v,const std::string& s,const std::string& r,const std::string& z,const std::string& l,const std::string& d) override {return p->prepareSound(n,v,s,r,z,l,d);}
    bool selectSound(uint32_t id,int16_t c) override {return p->selectSound(id,c);}
    bool discardSound(uint32_t id,bool cut) override {return p->discardSound(id,cut);}
    bool voiceBudget() const noexcept override {return p->voiceBudget();}
    bool honorsVolume() const noexcept override {return p->honorsVolume();}
    uint32_t voiceReport(double& c,const oscairn_voice_budget_note_t*& n,uint32_t& k) const noexcept override {return p->voiceReport(c,n,k);}
    bool supportsState() const noexcept override {return p->supportsState();}
    bool savesSounds() const noexcept override {return p->savesSounds();}
    std::vector<oscairn_sound_parameter_t> soundValues(uint32_t id) const override {return p->soundValues(id);}
    const kick::clap::Descriptor& descriptor() const noexcept override {return p->descriptor();}
    const std::vector<kick::clap::Parameter>& parameters() const noexcept override {return p->parameters();}
    const std::vector<kick::clap::Port>& inputPorts() const noexcept override {return p->inputPorts();}
    const std::vector<kick::clap::Port>& outputPorts() const noexcept override {return p->outputPorts();}
    uint32_t noteInputPorts() const noexcept override {return p->noteInputPorts();}
    bool usesClapNotes(uint32_t port) const noexcept override {return p->usesClapNotes(port);}
    void panic() noexcept override {p->panic();}
    uint32_t noteOutputPorts() const noexcept override {return p->noteOutputPorts();}
    double parameterValue(clap_id id) const noexcept override {return p->parameterValue(id);}
    std::string parameterText(clap_id id,double v) const override {return p->parameterText(id,v);}
    bool setParameter(clap_id id,double v) noexcept override {return p->setParameter(id,v);}
    bool parameterGesture(clap_id id,bool b) noexcept override {return p->parameterGesture(id,b);}
    bool popOutput(Event& e) noexcept override {return p->popOutput(e);}
    uint64_t outputEventDrops() const noexcept override {return p->outputEventDrops();}
    const Event* outputEvents(uint32_t& n) const noexcept override {return p->outputEvents(n);}
    kick::clap::Statistics statistics() const noexcept override {return p->statistics();}
    bool process(const float* const* i,uint32_t ni,float* const* o,uint32_t no,uint32_t f,const Event* e,uint32_t n,const clap_event_transport_t* t) noexcept override {return p->process(i,ni,o,no,f,e,n,t);}
    bool asleep() const noexcept override {return p->asleep();}
};
// The note-ons each node hears, as recording's MIDI taps do (Graph::Listener).
struct Heard final : Graph::Listener
{
    std::array<int,8> ons{};
    void events(NodeId node,const Event* list,uint32_t count,bool emitted) noexcept override
    {for(uint32_t i=0;i<count;++i)if(!emitted && node<ons.size() && list[i].header()->space_id==CLAP_CORE_EVENT_SPACE_ID && list[i].header()->type==CLAP_EVENT_NOTE_ON)++ons[node];}
};
NoteZone keys(uint8_t low,uint8_t high,uint16_t reserve=0,uint8_t priority=0,uint32_t part=0){NoteZone z;z.lowKey=low;z.highKey=high;z.reserve=reserve;z.priority=priority;z.part=part;return z;}
const std::vector<oscairn_sound_parameter_t> first{{1,.4},{4,1},{5,9}},second{{1,.8},{4,2},{5,3}};
}

int main(int argc,char** argv){try{
    if(argc!=2)return 2;
    std::array<float,frames> left{},right{};float* out[]{left.data(),right.data()};
    // The timeline: notes in each part's keys, a pitch bend on channel 1 (the keyboard's), an edit.
    auto events=[&](int block,std::vector<Event>& e){
        e.clear();
        if(block==2){e.push_back(Event::note(true,40,.8));e.push_back(Event::note(true,90,.6,5));}
        if(block==20)e.push_back(Event::midi(0xe0,0,96,3)); // a bend up, on channel 1
        if(block==60){e.push_back(Event::note(false,40,0,7));e.push_back(Event::note(true,43,1,9));}
        if(block==90){e.push_back(Event::note(false,43,0));e.push_back(Event::note(false,90,0,1));}
    };
    constexpr int blocks=400;
    std::vector<float> separate,shared;
    std::vector<Event> e;
    {
        Graph g(48000,frames);const auto a=g.add(argv[1]),b=g.add(argv[1]);
        g.connectNotes(graphInput,a,{keys(0,63)});g.connectNotes(graphInput,b,{keys(64,127)});g.connectAudio(a,graphOutput);g.connectAudio(b,graphOutput);g.commit();
        check(g.host(a).selectSound(g.host(a).prepareSound("A",first)) && g.host(b).selectSound(g.host(b).prepareSound("B",second)),"each instance its sound");
        for(int i=0;i<blocks;++i){
            if(i==40)check(g.host(b).setParameter(1,.2),"the second instance's brightness");
            events(i,e);forbidAllocation=true;const bool ok=g.process(out,frames,nullptr,e.data(),uint32_t(e.size()));forbidAllocation=false;check(ok,"rendered");
            separate.insert(separate.end(),left.begin(),left.end());separate.insert(separate.end(),right.begin(),right.end());}
    }
    {
        Graph g(48000,frames);
        auto plugin=std::make_shared<Recording>(loadPlugin(argv[1],"",48000,frames));
        const auto bus=SoundContext::busParameter(*plugin);
        check(bus==8,"the Basic Synth can share: its Output bus is parameter 8");
        auto engine=std::make_shared<SoundContext::Engine>();engine->host=plugin;engine->busParameter=bus;
        check(SoundContext::room(*engine)==7,"seven more sounds beside its Default");
        const auto sa=plugin->prepareSound("A",first,{},{},{},{},{}),sb=plugin->prepareSound("B",second,{},{},{},{},{});
        auto ca=std::make_shared<SoundContext>(engine,0,1,sa),cb=std::make_shared<SoundContext>(engine,1,2,sb);
        check(SoundContext::room(*engine)==5,"two channels, two buses and two sounds taken");
        bool refused=false;try{SoundContext taken(engine,1,3,sb);}catch(const std::invalid_argument&){refused=true;}
        check(refused,"a channel taken is refused");
        const auto a=g.add(ca),b=g.add(cb);
        check(g.effectSource(b)==a,"the two nodes are one engine, as effect contexts are");
        check(g.host(a).outputPorts().size()==1 && g.host(a).outputPorts()[0].id==0 && g.host(b).outputPorts()[0].id==0 && ca->bus()==1 && cb->bus()==2,
              "each node's one output is its bus, under the main port's ID");
        g.connectNotes(graphInput,a,{keys(0,63,2,3,1)});g.connectNotes(graphInput,b,{keys(64,127,1,7,2)});g.connectAudio(a,graphOutput);g.connectAudio(b,graphOutput);g.commit();
        g.setVoiceBudget(1e6); // so the plugin is told its voices
        Heard heard;g.setListener(&heard);
        float peakA=0,peakB=0;const double shownA=g.host(a).parameterValue(1);
        for(int i=0;i<blocks;++i){
            if(i==40)check(g.host(b).setParameter(1,.2),"the second part's brightness");
            events(i,e);forbidAllocation=true;const bool ok=g.process(out,frames,nullptr,e.data(),uint32_t(e.size()));forbidAllocation=false;check(ok,"rendered");
            shared.insert(shared.end(),left.begin(),left.end());shared.insert(shared.end(),right.begin(),right.end());
            if(i==30){peakA=g.takePeaks(a)[0];peakB=g.takePeaks(b)[0];}
            if(i==100)g.takePeaks(a);
        }
        check(peakA>0 && peakB>0,"each node meters its own bus");
        g.setListener(nullptr);
        check(heard.ons[a]==2 && heard.ons[b]==1,"each node hears its own notes: "+std::to_string(heard.ons[a])+" and "+std::to_string(heard.ons[b]));
        check(g.host(b).parameterValue(1)==.2 && g.host(a).parameterValue(1)==shownA,"each node shows its own sound's values");
        check(!g.host(b).setParameter(8,4) && g.host(b).setParameter(8,2),"a node's bus is its place in the engine");
        // Per-part voice budget entries: each part's reserve and priority under its own sound.
        check(plugin->calls>0 && plugin->last.size()==2,"an entry per part: "+std::to_string(plugin->last.size()));
        check(plugin->last[0].context==sa && plugin->last[0].reserve==2 && plugin->last[0].priority==3,"the first part's sound, reserve and priority");
        check(plugin->last[1].context==sb && plugin->last[1].reserve==1 && plugin->last[1].priority==7,"the second part's");
        // A sound selected on a node is its channel's, and goes on its bus.
        const auto next=g.host(a).prepareSound("A2",{{1,.5}});
        check(g.host(a).selectSound(next,-1),"a sound selected on the first node");
        bool onlyFirst=false;
        for(int i=0;i<2;++i)g.process(out,frames,nullptr);
        for(const auto& c:plugin->soundContexts())if(c.id==next)onlyFirst=c.selected_channels==1;
        check(onlyFirst,"selected on its channel only");
        for(int i=0;i<2;++i)g.process(out,frames,nullptr);
        check(plugin->last.size()==2 && plugin->last[0].context==next,"the budget names the sound its channel now selects");
    }
    check(separate.size()==shared.size(),"same length");
    double worst=0;bool sounded=false;
    for(size_t i=0;i<separate.size();++i){worst=std::max(worst,double(std::abs(separate[i]-shared[i])));sounded=sounded||separate[i]!=0;}
    std::printf("shared engine: largest difference from two instances %.3g\n",worst);
    check(sounded,"the reference sounds");
    check(worst<1e-6,"two parts on one engine sound as two instances, bend and edit included");
    std::puts("ok: parts sharing one engine");
    return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
