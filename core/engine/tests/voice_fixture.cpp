// SPDX-License-Identifier: AGPL-3.0-or-later
// A polyphonic instrument whose every voice costs a fixed time, for RFC 0018's budget tests:
// 128 voices, a sine per voice at its velocity and per-note volume, a 1 s release and a 3 ms
// choke fade, a note ending with NOTE_END when its last voice does. OSCAIRN_FIXTURE_VOICE_US: the
// microseconds each sounding voice spends per 128 frames (default 0). OSCAIRN_FIXTURE_LAYERS: the
// voices each note plays, as an AWM2-class sound's elements, each half as loud as the one before
// (default 1). OSCAIRN_FIXTURE_VOICE_BUDGET=1: it offers oscairn.voice-budget, giving up its own
// voices within the host's limit through the SDK's voice-budget-plugin.hpp, as an engine outside
// the sound-contexts helper (a sampler playing regions) would.
#include <clap/clap.h>
#include <oscairn-clap/voice-budget-plugin.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {
const char* features[]{CLAP_PLUGIN_FEATURE_INSTRUMENT,nullptr};
const clap_plugin_descriptor_t descriptor{CLAP_VERSION,"org.oscairn.test.voices","Costly voices","Oscairn","","","","1","",features};
struct Voice { bool active=false,held=false; int32_t id=-1; int16_t channel=0,key=0; double velocity=0,volume=1,phase=0,level=1,layer=1; uint32_t chokeLeft=0; bool choking=false; uint64_t started=0; };
struct Plugin {
    clap_plugin_t api{};const clap_host_t* host;
    double rate=48000,microseconds=0;uint32_t chokeFrames=144,layers=1,frames=128;
    std::array<Voice,128> voices{};uint64_t started=0;
    oscairn::voices::Limits limits; // oscairn.voice-budget's, audio thread
    double loudness(const Voice& v) const {return v.velocity*v.volume*v.level*v.layer*(v.choking?double(v.chokeLeft)/chokeFrames:1);}
    uint32_t sounding() const {uint32_t n=0;for(const auto& v:voices)n+=v.active && !v.choking;return n;}
    // The voice to give up by the extension's order (one context, 0, which a channel selects), or -1.
    int victim(uint32_t incoming)
    {
        std::array<oscairn::voices::Candidate,128> candidates{};
        for(size_t i=0;i<voices.size();++i)if(voices[i].active)candidates[i]={0,true,voices[i].held,voices[i].choking,false,loudness(voices[i]),voices[i].started};
        return oscairn::voices::victim(voices.size(),[&](size_t i){return voices[i].active?&candidates[i]:nullptr;},limits,incoming);
    }
    void fadeOut(int i){if(i<0)return;auto& v=voices[size_t(i)];v.held=false;if(!v.choking){v.choking=true;v.chokeLeft=chokeFrames;}}
    static Plugin& self(const clap_plugin_t* p){return *static_cast<Plugin*>(p->plugin_data);}
    void end(Voice& v,uint32_t at,const clap_output_events_t* out)
    {
        clap_event_note_t e{};e.header={sizeof(e),at,CLAP_CORE_EVENT_SPACE_ID,CLAP_EVENT_NOTE_END,0};
        e.note_id=v.id;e.port_index=0;e.channel=v.channel;e.key=v.key;v.active=false;
        for(const auto& o:voices)if(o.active && o.id==v.id && o.channel==v.channel && o.key==v.key)return; // the note sounds on in another layer
        out->try_push(out,&e.header);
    }
    bool matches(const Voice& v,int32_t id,int16_t channel,int16_t key) const
    {return v.active && (id<0 || v.id==id) && (channel<0 || v.channel==channel) && (key<0 || v.key==key);}
    void event(const clap_event_header_t* h,uint32_t at,const clap_output_events_t* out)
    {
        if(h->space_id!=CLAP_CORE_EVENT_SPACE_ID)return;
        if(h->type==CLAP_EVENT_NOTE_ON || h->type==CLAP_EVENT_NOTE_OFF || h->type==CLAP_EVENT_NOTE_CHOKE)
        {
            const auto& n=*reinterpret_cast<const clap_event_note_t*>(h);
            if(h->type==CLAP_EVENT_NOTE_ON)
            {
                for(uint32_t l=0;l<layers;++l)
                {
                    if(limits.limit && sounding()>=limits.limit)fadeOut(victim(0)); // within the host's limit
                    Voice* free=nullptr;for(auto& v:voices)if(!v.active){free=&v;break;}
                    if(!free){if(!l){Voice none;none.id=n.note_id;none.channel=n.channel;none.key=n.key;end(none,at,out);}return;}
                    *free=Voice{};free->active=free->held=true;free->id=n.note_id;free->channel=n.channel;free->key=n.key;free->velocity=n.velocity;
                    free->layer=std::ldexp(1.0,-int(l));free->started=++started;
                }
                return;
            }
            for(auto& v:voices)if(matches(v,n.note_id,n.channel,n.key))
            {
                v.held=false;
                if(h->type==CLAP_EVENT_NOTE_CHOKE && !v.choking){v.choking=true;v.chokeLeft=chokeFrames;}
            }
        }
        else if(h->type==CLAP_EVENT_NOTE_EXPRESSION)
        {
            const auto& x=*reinterpret_cast<const clap_event_note_expression_t*>(h);
            if(x.expression_id==CLAP_NOTE_EXPRESSION_VOLUME)for(auto& v:voices)if(matches(v,x.note_id,x.channel,x.key))v.volume=std::clamp(x.value,0.0,4.0);
        }
    }
    void render(float* left,float* right,uint32_t from,uint32_t to,const clap_output_events_t* out)
    {
        const double release=std::exp(std::log(1e-4)/(rate*1.0));
        for(auto& v:voices)if(v.active)
        {
            const double step=440*std::pow(2,(v.key-69)/12.0)/rate;
            for(uint32_t i=from;i<to;++i)
            {
                double gain=v.velocity*v.volume*v.level*v.layer*.1;
                if(v.choking){if(!v.chokeLeft){end(v,i,out);break;}gain*=double(v.chokeLeft--)/chokeFrames;}
                else if(!v.held && (v.level*=release)<1e-4){end(v,i,out);break;}
                const float s=float(std::sin(v.phase*2*M_PI)*gain);v.phase+=step;v.phase-=std::floor(v.phase);
                left[i]+=s;right[i]+=s;
            }
        }
    }
    explicit Plugin(const clap_host_t* h):host(h){
        if(const char* us=std::getenv("OSCAIRN_FIXTURE_VOICE_US"))microseconds=std::atof(us);
        if(const char* l=std::getenv("OSCAIRN_FIXTURE_LAYERS"))layers=std::clamp(std::atoi(l),1,16);
        api.desc=&descriptor;api.plugin_data=this;
        api.init=[](const clap_plugin_t*){return true;};
        api.destroy=[](const clap_plugin_t* p){delete &self(p);};
        api.activate=[](const clap_plugin_t* p,double rate,uint32_t,uint32_t){auto& s=self(p);s.rate=rate;s.chokeFrames=std::max<uint32_t>(1,uint32_t(rate*.003));return true;};
        api.deactivate=[](const clap_plugin_t*){};
        api.start_processing=[](const clap_plugin_t*){return true;};api.stop_processing=[](const clap_plugin_t*){};api.on_main_thread=[](const clap_plugin_t*){};
        api.reset=[](const clap_plugin_t* p){for(auto& v:self(p).voices)v.active=false;};
        api.process=[](const clap_plugin_t* p,const clap_process_t* b)->clap_process_status{
            auto& s=*static_cast<Plugin*>(p->plugin_data);
            float* left=b->audio_outputs[0].data32[0];float* right=b->audio_outputs[0].data32[1];
            s.frames=b->frames_count;
            while(s.limits.limit && s.sounding()>s.limits.limit){const int i=s.victim(oscairn::voices::none);if(i<0)break;s.fadeOut(i);}
            std::fill_n(left,b->frames_count,0.f);std::fill_n(right,b->frames_count,0.f);
            uint32_t sounding=0;for(const auto& v:s.voices)sounding+=v.active;
            const auto began=std::chrono::steady_clock::now();
            const auto* in=b->in_events;uint32_t done=0;
            for(uint32_t e=0;e<in->size(in);++e){
                const auto* h=in->get(in,e);const uint32_t at=std::min(h->time,b->frames_count);
                if(at>done){s.render(left,right,done,at,b->out_events);done=at;}
                s.event(h,at,b->out_events);
            }
            s.render(left,right,done,b->frames_count,b->out_events);
            // The voices' cost: busy, as DSP is, for their time.
            const auto spend=std::chrono::duration<double,std::micro>(s.microseconds*sounding*b->frames_count/128.0);
            while(std::chrono::steady_clock::now()-began<spend){}
            return CLAP_PROCESS_CONTINUE;
        };
        api.get_extension=[](const clap_plugin_t*,const char* id)->const void*{
            static const clap_plugin_audio_ports_t audio{
                [](const clap_plugin_t*,bool input)->uint32_t{return input?0:1;},
                [](const clap_plugin_t*,uint32_t i,bool input,clap_audio_port_info_t* p){if(input || i)return false;*p={};p->id=0;p->channel_count=2;p->flags=CLAP_AUDIO_PORT_IS_MAIN;p->port_type=CLAP_PORT_STEREO;p->in_place_pair=CLAP_INVALID_ID;std::strcpy(p->name,"Main");return true;}};
            static const clap_plugin_note_ports_t notes{
                [](const clap_plugin_t*,bool input)->uint32_t{return input?1:0;},
                [](const clap_plugin_t*,uint32_t i,bool input,clap_note_port_info_t* p){if(!input || i)return false;*p={};p->id=0;p->supported_dialects=CLAP_NOTE_DIALECT_CLAP;p->preferred_dialect=CLAP_NOTE_DIALECT_CLAP;std::strcpy(p->name,"Notes");return true;}};
            if(!std::strcmp(id,CLAP_EXT_AUDIO_PORTS))return &audio;
            static const oscairn_plugin_voice_budget_t budget{
                [](const clap_plugin_t*){return true;},
                [](const clap_plugin_t* p,uint32_t limit,const oscairn_voice_budget_context_t* c,uint32_t n){self(p).limits.set(limit,c,n);},
                [](const clap_plugin_t* p,double* cost)->uint32_t{auto& s=self(p);if(cost)*cost=s.microseconds*s.frames/128.0;uint32_t n=0;for(const auto& v:s.voices)n+=v.active;return n;},
                [](const clap_plugin_t* p,oscairn_voice_budget_note_t* out,uint32_t capacity)->uint32_t{
                    auto& s=self(p);oscairn::voices::Report<128> report;
                    for(const auto& v:s.voices)if(v.active)report.add(v.id,0,v.channel,v.key,s.loudness(v));
                    return report.write(out,capacity);}};
            if(!std::strcmp(id,CLAP_EXT_NOTE_PORTS))return &notes;
            if(!std::strcmp(id,OSCAIRN_EXT_VOICE_BUDGET) && std::getenv("OSCAIRN_FIXTURE_VOICE_BUDGET"))return &budget;
            return nullptr;
        };
    }
};
const clap_plugin_factory_t factory{
    [](const clap_plugin_factory_t*)->uint32_t{return 1;},
    [](const clap_plugin_factory_t*,uint32_t i)->const clap_plugin_descriptor_t*{return i?nullptr:&descriptor;},
    [](const clap_plugin_factory_t*,const clap_host_t* host,const char* id)->const clap_plugin_t*{return std::strcmp(id,descriptor.id)?nullptr:&(new Plugin(host))->api;}};
}
extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry{CLAP_VERSION,[](const char*){return true;},[]{},
    [](const char* id)->const void*{return std::strcmp(id,CLAP_PLUGIN_FACTORY_ID)?nullptr:&factory;}};
