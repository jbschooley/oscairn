// SPDX-License-Identifier: AGPL-3.0-or-later
#include <clap/clap.h>
#include <clap/ext/audio-ports.h>
#include <cstring>
namespace {
const char* features[]{CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,nullptr};
const clap_plugin_descriptor_t descriptor{CLAP_VERSION,"org.oscairn.test.audio-ports","Audio ports fixture","Oscairn","","","","1","Stereo main and mono sidechain/aux",features};
const clap_plugin_descriptor_t manyDescriptor{CLAP_VERSION,"org.oscairn.test.many-ports","Many buses fixture","Oscairn","","","","1","Main plus 16 stereo auxiliary buses",features};
const clap_plugin_descriptor_t excessiveDescriptor{CLAP_VERSION,"org.oscairn.test.excessive-ports","Excessive channels fixture","Oscairn","","","","1","33 stereo buses exceeds channel budget",features};
struct Plugin {
    clap_plugin_t api{};
    const clap_host_t* host;bool changed=false,requested=false;uint32_t buses=0;
    explicit Plugin(const clap_host_t* h,uint32_t count=0):host(h),buses(count){
        api.desc=count?(count==17?&manyDescriptor:&excessiveDescriptor):&descriptor;api.plugin_data=this;
        api.init=[](const clap_plugin_t*){return true;};
        api.destroy=[](const clap_plugin_t* p){delete static_cast<Plugin*>(p->plugin_data);};
        api.activate=[](const clap_plugin_t*,double,uint32_t,uint32_t){return true;};
        api.deactivate=[](const clap_plugin_t*){};api.start_processing=[](const clap_plugin_t*){return true;};
        api.stop_processing=[](const clap_plugin_t*){};api.reset=[](const clap_plugin_t*){};
        api.process=[](const clap_plugin_t* p,const clap_process_t* b)->clap_process_status{
            const auto buses=static_cast<Plugin*>(p->plugin_data)->buses;
            if(buses) {
                if(b->audio_inputs_count!=buses || b->audio_outputs_count!=buses)return CLAP_PROCESS_ERROR;
                for(uint32_t port=0;port<buses;++port)for(uint32_t c=0;c<2;++c)for(uint32_t i=0;i<b->frames_count;++i)
                    b->audio_outputs[port].data32[c][i]=b->audio_inputs[port].data32[c][i];
                return CLAP_PROCESS_CONTINUE;
            }
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
                [](const clap_plugin_t* p,bool)->uint32_t{const auto buses=static_cast<Plugin*>(p->plugin_data)->buses;return buses?buses:2;},
                [](const clap_plugin_t* plugin,uint32_t i,bool input,clap_audio_port_info_t* p)->bool{
                    const auto buses=static_cast<Plugin*>(plugin->plugin_data)->buses;
                    if(buses){if(i>=buses)return false;*p={};p->id=(input?100:200)+i;p->channel_count=2;p->flags=i?0:CLAP_AUDIO_PORT_IS_MAIN;std::strcpy(p->name,i?"Aux":"Main");p->port_type=CLAP_PORT_STEREO;p->in_place_pair=CLAP_INVALID_ID;return true;}
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
    [](const clap_plugin_factory_t*)->uint32_t{return 3;},
    [](const clap_plugin_factory_t*,uint32_t i){return i==0?&descriptor:i==1?&manyDescriptor:i==2?&excessiveDescriptor:nullptr;},
    [](const clap_plugin_factory_t*,const clap_host_t* h,const char* id)->const clap_plugin_t*{if(!std::strcmp(id,manyDescriptor.id))return &(new Plugin(h,17))->api;if(!std::strcmp(id,excessiveDescriptor.id))return &(new Plugin(h,33))->api;return std::strcmp(id,descriptor.id)?nullptr:&(new Plugin(h))->api;}};
}
extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry{
    CLAP_VERSION,[](const char*){return true;},[](){},
    [](const char* id)->const void*{return std::strcmp(id,CLAP_PLUGIN_FACTORY_ID)?nullptr:&factory;}};
