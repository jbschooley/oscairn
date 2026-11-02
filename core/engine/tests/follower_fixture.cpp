// SPDX-License-Identifier: AGPL-3.0-or-later
// Follows oscairn.harmony and oscairn.beat (RFC 0032, RFC 0033), and shows what it got on its
// audio outputs, every sample (a stereo main port, then a mono one):
//   0: the latest harmony's chord_type + 100 * chord_root + 10000 * chord_bass, -1 before any
//   1: its scale + 10000 * key_root, -1 before any
//   2: the free-running beat, counted on at the tempo from the latest beat event, -1 before any
// Its state is the harmony the host extension gives on the main thread (empty when it gives none).
#include <clap/clap.h>
#include <oscairn-clap/beat.h>
#include <oscairn-clap/harmony.h>
#include <cstring>

namespace {
const char* features[]{CLAP_PLUGIN_FEATURE_INSTRUMENT,nullptr};
const clap_plugin_descriptor_t descriptor{CLAP_VERSION,"org.oscairn.test.follower","Harmony and beat follower","Oscairn","","","","1","",features};
struct Plugin {
    clap_plugin_t api{};const clap_host_t* host;
    uint16_t harmonySpace=UINT16_MAX,beatSpace=UINT16_MAX;
    double rate=48000,chord=-1,scale=-1,beat=-1,perSample=0;
    explicit Plugin(const clap_host_t* h):host(h){
        api.desc=&descriptor;api.plugin_data=this;
        api.init=[](const clap_plugin_t* p){
            auto& s=*static_cast<Plugin*>(p->plugin_data);
            const auto* registry=static_cast<const clap_host_event_registry_t*>(s.host->get_extension(s.host,CLAP_EXT_EVENT_REGISTRY));
            if(registry){registry->query(s.host,OSCAIRN_EVENT_SPACE_HARMONY,&s.harmonySpace);registry->query(s.host,OSCAIRN_EVENT_SPACE_BEAT,&s.beatSpace);}
            return true;};
        api.destroy=[](const clap_plugin_t* p){delete static_cast<Plugin*>(p->plugin_data);};
        api.activate=[](const clap_plugin_t* p,double rate,uint32_t,uint32_t){static_cast<Plugin*>(p->plugin_data)->rate=rate;return true;};api.deactivate=[](const clap_plugin_t*){};
        api.start_processing=[](const clap_plugin_t*){return true;};api.stop_processing=[](const clap_plugin_t*){};api.on_main_thread=[](const clap_plugin_t*){};
        api.reset=[](const clap_plugin_t*){};
        api.process=[](const clap_plugin_t* p,const clap_process_t* b)->clap_process_status{
            auto& s=*static_cast<Plugin*>(p->plugin_data);
            const auto* in=b->in_events;uint32_t next=0;const uint32_t count=in->size(in);
            float* out[3]{b->audio_outputs[0].data32[0],b->audio_outputs[0].data32[1],b->audio_outputs[1].data32[0]};
            for(uint32_t i=0;i<b->frames_count;++i){
                for(;next<count && in->get(in,next)->time<=i;++next){
                    const auto* h=in->get(in,next);
                    if(h->space_id==s.harmonySpace && h->type==OSCAIRN_EVENT_HARMONY){
                        const auto& e=reinterpret_cast<const oscairn_event_harmony_t*>(h)->harmony;
                        s.chord=e.chord_type+100.*e.chord_root+10000.*e.chord_bass;s.scale=e.scale+10000.*e.key_root;
                    }
                    if(h->space_id==s.beatSpace && h->type==OSCAIRN_EVENT_BEAT){
                        const auto& e=*reinterpret_cast<const oscairn_event_beat_t*>(h);
                        s.beat=double(e.beats)/CLAP_BEATTIME_FACTOR;s.perSample=e.tempo/60./s.rate;
                    }
                }
                out[0][i]=float(s.chord);out[1][i]=float(s.scale);out[2][i]=float(s.beat);
                if(s.beat>=0)s.beat+=s.perSample;
            }
            return CLAP_PROCESS_CONTINUE;
        };
        api.get_extension=[](const clap_plugin_t*,const char* id)->const void*{
            static const clap_plugin_audio_ports_t audio{
                [](const clap_plugin_t*,bool input)->uint32_t{return input?0:2;},
                [](const clap_plugin_t*,uint32_t i,bool input,clap_audio_port_info_t* p){if(input || i>1)return false;*p={};p->id=i;p->channel_count=2-i;p->flags=i?0:CLAP_AUDIO_PORT_IS_MAIN;p->port_type=i?CLAP_PORT_MONO:CLAP_PORT_STEREO;p->in_place_pair=CLAP_INVALID_ID;std::strcpy(p->name,i?"Beat":"Harmony");return true;}};
            static const clap_plugin_state_t state{
                [](const clap_plugin_t* p,const clap_ostream_t* stream){
                    auto& s=*static_cast<Plugin*>(p->plugin_data);
                    const auto* ext=static_cast<const oscairn_host_harmony_t*>(s.host->get_extension(s.host,OSCAIRN_EXT_HARMONY));
                    oscairn_harmony_t h{};
                    if(!ext || !ext->get(s.host,&h))return true;
                    return stream->write(stream,&h,sizeof h)==int64_t(sizeof h);},
                [](const clap_plugin_t*,const clap_istream_t*){return true;}};
            if(!std::strcmp(id,CLAP_EXT_AUDIO_PORTS))return &audio;if(!std::strcmp(id,CLAP_EXT_STATE))return &state;return nullptr;
        };
    }
};
const clap_plugin_factory_t factory{[](const clap_plugin_factory_t*)->uint32_t{return 1;},[](const clap_plugin_factory_t*,uint32_t i){return i?nullptr:&descriptor;},
    [](const clap_plugin_factory_t*,const clap_host_t* host,const char* id)->const clap_plugin_t*{return std::strcmp(id,descriptor.id)?nullptr:&(new Plugin(host))->api;}};
}
extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry{CLAP_VERSION,[](const char*){return true;},[](){},[](const char* id)->const void*{return std::strcmp(id,CLAP_PLUGIN_FACTORY_ID)?nullptr:&factory;}};
