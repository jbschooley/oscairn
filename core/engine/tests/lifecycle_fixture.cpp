// SPDX-License-Identifier: AGPL-3.0-or-later
#include <clap/clap.h>
#include <clap/ext/audio-ports.h>
#include <clap/ext/note-ports.h>
#include <clap/ext/params.h>
#include <atomic>
#include <clap/ext/event-registry.h>
#include <oscairn-clap/param-target.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    const char* features[]{CLAP_PLUGIN_FEATURE_INSTRUMENT,nullptr};
    const clap_plugin_descriptor_t descriptor{CLAP_VERSION,"org.oscairn.test.lifecycle","Graph lifecycle fixture","Oscairn","","","","1","",features};
    struct Plugin
    {
        clap_plugin_t api{};
        std::atomic<double> value{0.5};
        bool gate = false;
        const clap_host_t* host;
        uint16_t laneSpace=0;
        std::atomic<int> mode{0},callbacks{0};
        static Plugin& get(const clap_plugin_t* p) { return *static_cast<Plugin*>(p->plugin_data); }
        static void events(const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t* out)
        {
            auto& s = get(p);
            for(uint32_t i=0; i<in->size(in); ++i)
            {
                const auto* e = in->get(in,i);
                if(e->type == CLAP_EVENT_NOTE_ON) s.gate = true;
                if(e->type == CLAP_EVENT_NOTE_OFF) s.gate = false;
                if(e->type == CLAP_EVENT_PARAM_VALUE)
                {
                    const auto& v=*reinterpret_cast<const clap_event_param_value_t*>(e);
                    if(v.param_id==1)s.mode=static_cast<int>(v.value);else s.value=v.value;
                }
                if(e->type == CLAP_EVENT_PARAM_GESTURE_BEGIN || e->type == CLAP_EVENT_PARAM_GESTURE_END) out->try_push(out,e);
            }
        }
        explicit Plugin(const clap_host_t* h):host(h)
        {
            api.desc=&descriptor; api.plugin_data=this;
            api.init=[](const clap_plugin_t* p){auto& s=get(p);
                auto* registry=static_cast<const clap_host_event_registry_t*>(s.host->get_extension(s.host,CLAP_EXT_EVENT_REGISTRY));
                return registry && registry->query(s.host,OSCAIRN_EVENT_SPACE_PARAM_TARGET,&s.laneSpace);};
            api.destroy=[](const clap_plugin_t* p){delete &get(p);};
            api.activate=[](const clap_plugin_t*,double,uint32_t,uint32_t){return true;};
            api.deactivate=[](const clap_plugin_t*){};
            api.start_processing=[](const clap_plugin_t*){return true;};
            api.stop_processing=[](const clap_plugin_t*){};
            api.reset=[](const clap_plugin_t* p){get(p).gate=false;};
            api.on_main_thread=[](const clap_plugin_t* p){++get(p).callbacks;};
            api.process=[](const clap_plugin_t* p,const clap_process_t* b)->clap_process_status{
                events(p,b->in_events,b->out_events);
                auto& s=get(p);
                s.host->request_callback(s.host);
                const int mode=s.mode.exchange(0);
                for(int i=0;i<(mode==-1?1:std::abs(mode));++i)
                {
                    if(mode==-512)
                    {
                        oscairn_event_param_target_t e{};e.header={sizeof(e),0,s.laneSpace,OSCAIRN_EVENT_PARAM_TARGET,0};
                        e.note_id=e.port_index=e.channel=e.key=-1;e.value=.5;
                        b->out_events->try_push(b->out_events,&e.header);
                    }
                    else
                    {
                        clap_event_note_t e{};e.header={sizeof(e),0,CLAP_CORE_EVENT_SPACE_ID,static_cast<uint16_t>(mode==-1?CLAP_EVENT_NOTE_OFF:CLAP_EVENT_NOTE_ON),0};
                        e.note_id=-1;e.key=60;e.velocity=1;b->out_events->try_push(b->out_events,&e.header);
                    }
                }
                for(uint32_t i=0;i<b->frames_count;++i){b->audio_outputs[0].data32[0][i]=s.gate?1.f:0.f;b->audio_outputs[0].data32[1][i]=static_cast<float>(s.callbacks.load());}
                return CLAP_PROCESS_CONTINUE;
            };
            api.get_extension=[](const clap_plugin_t*,const char* id)->const void* {
                static const clap_plugin_audio_ports_t ports{
                    [](const clap_plugin_t*,bool)->uint32_t{return 1;},
                    [](const clap_plugin_t*,uint32_t i,bool,clap_audio_port_info_t* p){
                        if(i) return false; *p={}; p->id=0; std::strcpy(p->name,"stereo"); p->channel_count=2;
                        p->flags=CLAP_AUDIO_PORT_IS_MAIN; p->port_type=CLAP_PORT_STEREO; p->in_place_pair=CLAP_INVALID_ID; return true;
                    }};
                static const clap_plugin_note_ports_t notes{
                    [](const clap_plugin_t*,bool)->uint32_t{return 1;},
                    [](const clap_plugin_t*,uint32_t i,bool in,clap_note_port_info_t* p){
                        if(i) return false; *p={}; p->id=0; p->supported_dialects=p->preferred_dialect=CLAP_NOTE_DIALECT_CLAP; return true;
                    }};
                static const clap_plugin_params_t params{
                    [](const clap_plugin_t*)->uint32_t{return 2;},
                    [](const clap_plugin_t*,uint32_t i,clap_param_info_t* p){
                        if(i>1) return false; *p={}; p->id=i+1; std::strcpy(p->name,"Value"); p->flags=CLAP_PARAM_IS_AUTOMATABLE;
                        p->min_value=i?0:-512; p->max_value=i?1:512; p->default_value=i?.5:0; return true;
                    },
                    [](const clap_plugin_t* p,clap_id id,double* v){if(id<1 || id>2)return false; *v=id==1?double(get(p).mode.load()):get(p).value.load(); return true;},
                    [](const clap_plugin_t*,clap_id,double v,char* out,uint32_t n){std::snprintf(out,n,"%.3f",v);return true;},
                    nullptr,events};
                if(!std::strcmp(id,CLAP_EXT_AUDIO_PORTS))return &ports;
                if(!std::strcmp(id,CLAP_EXT_NOTE_PORTS))return &notes;
                if(!std::strcmp(id,CLAP_EXT_PARAMS))return &params;
                return nullptr;
            };
        }
    };
    const clap_plugin_factory_t factory{
        [](const clap_plugin_factory_t*)->uint32_t{return 1;},
        [](const clap_plugin_factory_t*,uint32_t i){return i ? nullptr : &descriptor;},
        [](const clap_plugin_factory_t*,const clap_host_t* h,const char* id)->const clap_plugin_t*{
            return std::strcmp(id,descriptor.id) ? nullptr : &(new Plugin(h))->api;
        }};
}
extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry{
    CLAP_VERSION,[](const char*){return true;},[](){},
    [](const char* id)->const void*{return !std::strcmp(id,CLAP_PLUGIN_FACTORY_ID) ? &factory : nullptr;}};
