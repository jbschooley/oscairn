// SPDX-License-Identifier: AGPL-3.0-or-later
#include <clap/clap.h>
#include <clap/ext/audio-ports.h>
#include <cstring>
namespace {
const char* features[]{CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,nullptr};
const clap_plugin_descriptor_t descriptor{CLAP_VERSION,"org.oscairn.test.audio-ports","Audio ports fixture","Oscairn","","","","1","Stereo main and mono sidechain/aux",features};
struct Plugin {
    clap_plugin_t api{};
    const clap_host_t* host;bool changed=false,requested=false;
    explicit Plugin(const clap_host_t* h):host(h){
        api.desc=&descriptor;api.plugin_data=this;
        api.init=[](const clap_plugin_t*){return true;};
        api.destroy=[](const clap_plugin_t* p){delete static_cast<Plugin*>(p->plugin_data);};
        api.activate=[](const clap_plugin_t*,double,uint32_t,uint32_t){return true;};
        api.deactivate=[](const clap_plugin_t*){};api.start_processing=[](const clap_plugin_t*){return true;};
        api.stop_processing=[](const clap_plugin_t*){};api.reset=[](const clap_plugin_t*){};
        api.process=[](const clap_plugin_t* p,const clap_process_t* b)->clap_process_status{
            if(b->audio_inputs_count!=2 || b->audio_outputs_count!=2)return CLAP_PROCESS_ERROR;
            auto& self=*static_cast<Plugin*>(p->plugin_data);
            if(!self.requested && b->frames_count && b->audio_inputs[1].data32[0][0]==99){self.requested=true;self.host->request_callback(self.host);}
            for(uint32_t i=0;i<b->frames_count;++i){
                const auto side=b->audio_inputs[1].data32[0][i];
                b->audio_outputs[0].data32[0][i]=b->audio_inputs[0].data32[0][i]+10*side;
                b->audio_outputs[0].data32[1][i]=b->audio_inputs[0].data32[1][i]+20*side;
                b->audio_outputs[1].data32[0][i]=side;
            }return CLAP_PROCESS_CONTINUE;
        };
        api.get_extension=[](const clap_plugin_t*,const char* id)->const void*{
            static const clap_plugin_audio_ports_t ports{
                [](const clap_plugin_t*,bool)->uint32_t{return 2;},
                [](const clap_plugin_t* plugin,uint32_t i,bool input,clap_audio_port_info_t* p)->bool{
                    if(i>1)return false;*p={};p->id=(input?10:30)+i*10;
                    if(!input && !i && static_cast<Plugin*>(plugin->plugin_data)->changed)++p->id;
                    std::strcpy(p->name,i?(input?"Sidechain":"Aux"):"Main");p->flags=i?0:CLAP_AUDIO_PORT_IS_MAIN;
                    p->channel_count=i?1:2;p->port_type=i?CLAP_PORT_MONO:CLAP_PORT_STEREO;p->in_place_pair=CLAP_INVALID_ID;return true;
                }};
            return std::strcmp(id,CLAP_EXT_AUDIO_PORTS)?nullptr:&ports;
        };api.on_main_thread=[](const clap_plugin_t* p){auto& self=*static_cast<Plugin*>(p->plugin_data);if(self.requested){self.changed=true;self.host->request_restart(self.host);}};
    }
};
const clap_plugin_factory_t factory{
    [](const clap_plugin_factory_t*)->uint32_t{return 1;},
    [](const clap_plugin_factory_t*,uint32_t i){return i?nullptr:&descriptor;},
    [](const clap_plugin_factory_t*,const clap_host_t* h,const char* id)->const clap_plugin_t*{return std::strcmp(id,descriptor.id)?nullptr:&(new Plugin(h))->api;}};
}
extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry{
    CLAP_VERSION,[](const char*){return true;},[](){},
    [](const char* id)->const void*{return std::strcmp(id,CLAP_PLUGIN_FACTORY_ID)?nullptr:&factory;}};
