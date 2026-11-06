// SPDX-License-Identifier: AGPL-3.0-or-later
// A polyphonic instrument whose every voice costs a fixed time, for RFC 0018's budget tests:
// 128 voices, a sine per voice at its velocity and per-note volume, a 1 s release and a 3 ms
// choke fade, each voice ending with NOTE_END. OSCAIRN_FIXTURE_VOICE_US: the microseconds each
// sounding voice spends per 128 frames (default 0).
#include <clap/clap.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {
const char* features[]{CLAP_PLUGIN_FEATURE_INSTRUMENT,nullptr};
const clap_plugin_descriptor_t descriptor{CLAP_VERSION,"org.oscairn.test.voices","Costly voices","Oscairn","","","","1","",features};
struct Voice { bool active=false,held=false; int32_t id=-1; int16_t channel=0,key=0; double velocity=0,volume=1,phase=0,level=1; uint32_t chokeLeft=0; bool choking=false; };
struct Plugin {
    clap_plugin_t api{};const clap_host_t* host;
    double rate=48000,microseconds=0;uint32_t chokeFrames=144;
    std::array<Voice,128> voices{};
    static Plugin& self(const clap_plugin_t* p){return *static_cast<Plugin*>(p->plugin_data);}
    void end(Voice& v,uint32_t at,const clap_output_events_t* out)
    {
        clap_event_note_t e{};e.header={sizeof(e),at,CLAP_CORE_EVENT_SPACE_ID,CLAP_EVENT_NOTE_END,0};
        e.note_id=v.id;e.port_index=0;e.channel=v.channel;e.key=v.key;out->try_push(out,&e.header);v.active=false;
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
                Voice* free=nullptr;for(auto& v:voices)if(!v.active){free=&v;break;}
                if(!free){Voice none;none.id=n.note_id;none.channel=n.channel;none.key=n.key;end(none,at,out);return;}
                *free=Voice{};free->active=free->held=true;free->id=n.note_id;free->channel=n.channel;free->key=n.key;free->velocity=n.velocity;
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
                double gain=v.velocity*v.volume*v.level*.1;
                if(v.choking){if(!v.chokeLeft){end(v,i,out);break;}gain*=double(v.chokeLeft--)/chokeFrames;}
                else if(!v.held && (v.level*=release)<1e-4){end(v,i,out);break;}
                const float s=float(std::sin(v.phase*2*M_PI)*gain);v.phase+=step;v.phase-=std::floor(v.phase);
                left[i]+=s;right[i]+=s;
            }
        }
    }
    explicit Plugin(const clap_host_t* h):host(h){
        if(const char* us=std::getenv("OSCAIRN_FIXTURE_VOICE_US"))microseconds=std::atof(us);
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
            if(!std::strcmp(id,CLAP_EXT_NOTE_PORTS))return &notes;
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
