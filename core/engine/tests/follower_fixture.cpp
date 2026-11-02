// SPDX-License-Identifier: AGPL-3.0-or-later
// Follows oscairn.beat (RFC 0032), and shows what it got on its three audio outputs, every
// sample:
//   0, 1: -1 (unused)
//   2: the free-running beat, counted on at the tempo from the latest beat event, -1 before any
#include <clap/clap.h>
#include <oscairn-clap/beat.h>
#include <cstring>

namespace {
const char* features[]{CLAP_PLUGIN_FEATURE_INSTRUMENT,nullptr};
const clap_plugin_descriptor_t descriptor{CLAP_VERSION,"org.oscairn.test.follower","Beat follower","Oscairn","","","","1","",features};
struct Plugin {
    clap_plugin_t api{};const clap_host_t* host;
    uint16_t beatSpace=UINT16_MAX;
    double rate=48000,beat=-1,perSample=0;
    explicit Plugin(const clap_host_t* h):host(h){
        api.desc=&descriptor;api.plugin_data=this;
        api.init=[](const clap_plugin_t* p){
            auto& s=*static_cast<Plugin*>(p->plugin_data);
            const auto* registry=static_cast<const clap_host_event_registry_t*>(s.host->get_extension(s.host,CLAP_EXT_EVENT_REGISTRY));
            if(registry)registry->query(s.host,OSCAIRN_EVENT_SPACE_BEAT,&s.beatSpace);
            return true;};
        api.destroy=[](const clap_plugin_t* p){delete static_cast<Plugin*>(p->plugin_data);};
        api.activate=[](const clap_plugin_t* p,double rate,uint32_t,uint32_t){static_cast<Plugin*>(p->plugin_data)->rate=rate;return true;};api.deactivate=[](const clap_plugin_t*){};
        api.start_processing=[](const clap_plugin_t*){return true;};api.stop_processing=[](const clap_plugin_t*){};api.on_main_thread=[](const clap_plugin_t*){};
        api.reset=[](const clap_plugin_t*){};
        api.process=[](const clap_plugin_t* p,const clap_process_t* b)->clap_process_status{
            auto& s=*static_cast<Plugin*>(p->plugin_data);
            const auto* in=b->in_events;uint32_t next=0;const uint32_t count=in->size(in);
            float* const* out=b->audio_outputs[0].data32;
            for(uint32_t i=0;i<b->frames_count;++i){
                for(;next<count && in->get(in,next)->time<=i;++next){
                    const auto* h=in->get(in,next);
                    if(h->space_id==s.beatSpace && h->type==OSCAIRN_EVENT_BEAT){
                        const auto& e=*reinterpret_cast<const oscairn_event_beat_t*>(h);
                        s.beat=double(e.beats)/CLAP_BEATTIME_FACTOR;s.perSample=e.tempo/60./s.rate;
                    }
                }
                out[0][i]=-1;out[1][i]=-1;out[2][i]=float(s.beat);
                if(s.beat>=0)s.beat+=s.perSample;
            }
            return CLAP_PROCESS_CONTINUE;
        };
        api.get_extension=[](const clap_plugin_t*,const char* id)->const void*{
            static const clap_plugin_audio_ports_t audio{
                [](const clap_plugin_t*,bool input)->uint32_t{return input?0:1;},
                [](const clap_plugin_t*,uint32_t i,bool input,clap_audio_port_info_t* p){if(input || i)return false;*p={};p->id=0;p->channel_count=3;p->flags=CLAP_AUDIO_PORT_IS_MAIN;p->port_type=nullptr;p->in_place_pair=CLAP_INVALID_ID;std::strcpy(p->name,"Seen");return true;}};
            return std::strcmp(id,CLAP_EXT_AUDIO_PORTS)?nullptr:&audio;
        };
    }
};
const clap_plugin_factory_t factory{[](const clap_plugin_factory_t*)->uint32_t{return 1;},[](const clap_plugin_factory_t*,uint32_t i){return i?nullptr:&descriptor;},
    [](const clap_plugin_factory_t*,const clap_host_t* host,const char* id)->const clap_plugin_t*{return std::strcmp(id,descriptor.id)?nullptr:&(new Plugin(host))->api;}};
}
extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry{CLAP_VERSION,[](const char*){return true;},[](){},[](const char* id)->const void*{return std::strcmp(id,CLAP_PLUGIN_FACTORY_ID)?nullptr:&factory;}};
