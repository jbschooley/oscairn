// SPDX-License-Identifier: AGPL-3.0-or-later
// A threshold detector passes audio through while emitting note edges at exact samples.
#include <clap/clap.h>
#include <cstring>
namespace {
const char* features[]{CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,CLAP_PLUGIN_FEATURE_NOTE_EFFECT,nullptr};
const clap_plugin_descriptor_t descriptor{CLAP_VERSION,"org.oscairn.test.audio-midi","Audio to MIDI fixture","Oscairn","","","","1","",features};
struct Plugin {
    clap_plugin_t api{};bool high=false;
    explicit Plugin(){
        api.desc=&descriptor;api.plugin_data=this;
        api.init=[](const clap_plugin_t*){return true;};api.destroy=[](const clap_plugin_t* p){delete static_cast<Plugin*>(p->plugin_data);};
        api.activate=[](const clap_plugin_t*,double,uint32_t,uint32_t){return true;};api.deactivate=[](const clap_plugin_t*){};
        api.start_processing=[](const clap_plugin_t*){return true;};api.stop_processing=[](const clap_plugin_t*){};api.on_main_thread=[](const clap_plugin_t*){};
        api.reset=[](const clap_plugin_t* p){static_cast<Plugin*>(p->plugin_data)->high=false;};
        api.process=[](const clap_plugin_t* p,const clap_process_t* b)->clap_process_status{
            auto& s=*static_cast<Plugin*>(p->plugin_data);
            for(uint32_t i=0;i<b->frames_count;++i){
                const auto input=b->audio_inputs[0].data32[0][i];b->audio_outputs[0].data32[0][i]=input;
                const bool high=input>.5f;if(high!=s.high){
                    clap_event_note_t e{};e.header={sizeof(e),i,0,static_cast<uint16_t>(high?CLAP_EVENT_NOTE_ON:CLAP_EVENT_NOTE_OFF),0};e.note_id=-1;e.key=60;e.velocity=high?.8:0;
                    if(!b->out_events->try_push(b->out_events,&e.header))return CLAP_PROCESS_ERROR;s.high=high;
                }
            }return CLAP_PROCESS_CONTINUE;
        };
        api.get_extension=[](const clap_plugin_t*,const char* id)->const void*{
            static const clap_plugin_audio_ports_t audio{
                [](const clap_plugin_t*,bool)->uint32_t{return 1;},
                [](const clap_plugin_t*,uint32_t i,bool,clap_audio_port_info_t* p){if(i)return false;*p={};p->id=0;p->channel_count=1;p->flags=CLAP_AUDIO_PORT_IS_MAIN;p->port_type=CLAP_PORT_MONO;p->in_place_pair=CLAP_INVALID_ID;std::strcpy(p->name,"Main");return true;}};
            static const clap_plugin_note_ports_t notes{
                [](const clap_plugin_t*,bool input)->uint32_t{return input?0:1;},
                [](const clap_plugin_t*,uint32_t i,bool input,clap_note_port_info_t* p){if(input || i)return false;*p={};p->id=0;p->supported_dialects=p->preferred_dialect=CLAP_NOTE_DIALECT_CLAP;std::strcpy(p->name,"Detected notes");return true;}};
            if(!std::strcmp(id,CLAP_EXT_AUDIO_PORTS))return &audio;if(!std::strcmp(id,CLAP_EXT_NOTE_PORTS))return &notes;return nullptr;
        };
    }
};
const clap_plugin_factory_t factory{[](const clap_plugin_factory_t*)->uint32_t{return 1;},[](const clap_plugin_factory_t*,uint32_t i){return i?nullptr:&descriptor;},[](const clap_plugin_factory_t*,const clap_host_t*,const char* id)->const clap_plugin_t*{return std::strcmp(id,descriptor.id)?nullptr:&(new Plugin)->api;}};
}
extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry{CLAP_VERSION,[](const char*){return true;},[](){},[](const char* id)->const void*{return std::strcmp(id,CLAP_PLUGIN_FACTORY_ID)?nullptr:&factory;}};
