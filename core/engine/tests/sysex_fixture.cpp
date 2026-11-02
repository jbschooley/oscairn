// SPDX-License-Identifier: AGPL-3.0-or-later
// SysEx between plugins (RFC 0032 step 1). Two note effects, CLAP and MIDI dialects, no audio:
//   org.oscairn.test.sysex-echo   sends on every event it receives, in order, at its sample;
//   org.oscairn.test.sysex-source echoes too, and sends this schedule, by samples since it
//                                 started (sysexBytes() gives each message's bytes):
//     37    note on 60, message 1 (60 KiB), note on 61, message 2 (16 bytes)
//     600   message 3, one byte over the 64 KiB limit: the host refuses it
//     1000  messages 10..14, 60 KiB each: the fifth passes the 256 KiB a block holds
#include <clap/clap.h>
#include <cstring>
#include <vector>
#include "sysex_fixture.h"

namespace {
const char* features[]{CLAP_PLUGIN_FEATURE_NOTE_EFFECT,nullptr};
const clap_plugin_descriptor_t source{CLAP_VERSION,"org.oscairn.test.sysex-source","SysEx source","Oscairn","","","","1","",features};
const clap_plugin_descriptor_t echo{CLAP_VERSION,"org.oscairn.test.sysex-echo","SysEx echo","Oscairn","","","","1","",features};

struct Plugin {
    clap_plugin_t api{};
    bool generate;
    std::vector<uint8_t> scratch=std::vector<uint8_t>(sysexFixture::limit+1); // made here, never on the audio thread
    explicit Plugin(bool generate):generate(generate){
        api.desc=generate?&source:&echo;api.plugin_data=this;
        api.init=[](const clap_plugin_t*){return true;};api.destroy=[](const clap_plugin_t* p){delete static_cast<Plugin*>(p->plugin_data);};
        api.activate=[](const clap_plugin_t*,double,uint32_t,uint32_t){return true;};api.deactivate=[](const clap_plugin_t*){};
        api.start_processing=[](const clap_plugin_t*){return true;};api.stop_processing=[](const clap_plugin_t*){};api.on_main_thread=[](const clap_plugin_t*){};
        api.reset=[](const clap_plugin_t*){};
        api.process=[](const clap_plugin_t* p,const clap_process_t* b)->clap_process_status{
            auto& s=*static_cast<Plugin*>(p->plugin_data);
            const auto* in=b->in_events;const auto* out=b->out_events;
            for(uint32_t i=0;i<in->size(in);++i){const auto* h=in->get(in,i);if(h->space_id==CLAP_CORE_EVENT_SPACE_ID)out->try_push(out,h);}
            if(!s.generate)return CLAP_PROCESS_CONTINUE;
            const auto start=b->steady_time,end=start+b->frames_count;
            auto at=[&](int64_t t){return t>=start && t<end;};
            auto note=[&](int64_t t,int16_t key){clap_event_note_t e{};e.header={sizeof(e),uint32_t(t-start),CLAP_CORE_EVENT_SPACE_ID,CLAP_EVENT_NOTE_ON,0};e.note_id=-1;e.port_index=0;e.channel=0;e.key=key;e.velocity=.5;out->try_push(out,&e.header);};
            auto sysex=[&](int64_t t,uint32_t seed,uint32_t size){
                sysexFixture::fill(s.scratch.data(),size,seed);
                clap_event_midi_sysex_t e{};e.header={sizeof(e),uint32_t(t-start),CLAP_CORE_EVENT_SPACE_ID,CLAP_EVENT_MIDI_SYSEX,0};e.port_index=0;e.buffer=s.scratch.data();e.size=size;
                out->try_push(out,&e.header);}; // the host copies the bytes before it returns
            if(at(37)){note(37,60);sysex(37,1,sysexFixture::big);note(37,61);sysex(37,2,16);}
            if(at(600))sysex(600,3,sysexFixture::limit+1);
            if(at(1000))for(uint32_t seed=10;seed<15;++seed)sysex(1000,seed,sysexFixture::big);
            return CLAP_PROCESS_CONTINUE;
        };
        api.get_extension=[](const clap_plugin_t*,const char* id)->const void*{
            static const clap_plugin_note_ports_t notes{
                [](const clap_plugin_t*,bool)->uint32_t{return 1;},
                [](const clap_plugin_t*,uint32_t i,bool,clap_note_port_info_t* p){if(i)return false;*p={};p->id=0;p->supported_dialects=CLAP_NOTE_DIALECT_CLAP|CLAP_NOTE_DIALECT_MIDI;p->preferred_dialect=CLAP_NOTE_DIALECT_CLAP;std::strcpy(p->name,"Notes");return true;}};
            return std::strcmp(id,CLAP_EXT_NOTE_PORTS)?nullptr:&notes;
        };
    }
};
const clap_plugin_factory_t factory{[](const clap_plugin_factory_t*)->uint32_t{return 2;},[](const clap_plugin_factory_t*,uint32_t i){return i==0?&source:i==1?&echo:nullptr;},
    [](const clap_plugin_factory_t*,const clap_host_t*,const char* id)->const clap_plugin_t*{
        if(!std::strcmp(id,source.id))return &(new Plugin(true))->api;
        if(!std::strcmp(id,echo.id))return &(new Plugin(false))->api;
        return nullptr;}};
}
extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry{CLAP_VERSION,[](const char*){return true;},[](){},[](const char* id)->const void*{return std::strcmp(id,CLAP_PLUGIN_FACTORY_ID)?nullptr:&factory;}};
