// SPDX-License-Identifier: AGPL-3.0-or-later
// A note processor owns its input-to-output mapping until release delivery.
#include <clap/clap.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace {
#ifdef OSCAIRN_MIDI_DELAY
constexpr bool delayed=true;
constexpr const char* id="org.oscairn.midi-delay";
constexpr const char* name="Oscairn MIDI Delay";
constexpr int defaultValue=250,minimum=0,maximum=2000;
#else
constexpr bool delayed=false;
constexpr const char* id="org.oscairn.transpose";
constexpr const char* name="Oscairn Transpose";
constexpr int defaultValue=0,minimum=-48,maximum=48;
#endif
const char* features[]{CLAP_PLUGIN_FEATURE_NOTE_EFFECT,nullptr};
const clap_plugin_descriptor_t descriptor{CLAP_VERSION,id,name,"Oscairn","https://github.com/jbschooley/oscairn","","","0.1.0","Sample-timed note processor",features};
struct Processor {
    struct Voice {
        clap_event_note_t input{};
        int32_t outputId=0;
        int16_t outputKey=0;
        bool used=false,held=false,started=false,offDelivered=false,midi=false;
    };
    struct Packet {
        alignas(8) std::array<uint8_t,64> bytes{};
        uint64_t due=0,order=0;
        int voice=-1;
        const clap_event_header_t* header() const {return reinterpret_cast<const clap_event_header_t*>(bytes.data());}
    };
    clap_plugin_t api{};
    std::atomic<int> value{defaultValue};
    double rate=48000;
    std::array<Voice,128> voices{};
    std::array<Packet,1024> queue{};
    size_t queued=0;
    std::array<bool,16> pedal{};
    uint64_t clock=0,order=0,delay=0;
    int32_t nextId=0;
    bool panic=false,playing=false,transportSeen=false;
    static Processor& get(const clap_plugin_t* p){return *static_cast<Processor*>(p->plugin_data);}
    static bool later(const Packet& a,const Packet& b){return a.due>b.due || (a.due==b.due && a.order>b.order);}
    bool send(const clap_output_events_t* out,const clap_event_header_t& event,uint32_t time) {
        Packet p;std::memcpy(p.bytes.data(),&event,event.size);reinterpret_cast<clap_event_header_t*>(p.bytes.data())->time=time;
        if(!out || !out->try_push(out,p.header())){panic=true;return false;}return true;
    }
    void end(size_t i,uint32_t time,const clap_output_events_t* out) {
        auto e=voices[i].input;e.header.type=CLAP_EVENT_NOTE_END;send(out,e.header,time);voices[i].used=false;
        auto last=std::remove_if(queue.begin(),queue.begin()+queued,[&](const auto& p){return p.voice==static_cast<int>(i);});
        queued=last-queue.begin();std::make_heap(queue.begin(),queue.begin()+queued,later);
    }
    void recover(uint32_t time,const clap_output_events_t* out) {
        queued=0;for(auto& v:voices)v.used=false;pedal.fill(false);nextId=0;panic=false;
        clap_event_note_t e{};e.header={sizeof(e),time,CLAP_CORE_EVENT_SPACE_ID,CLAP_EVENT_NOTE_CHOKE,0};
        e.note_id=e.channel=e.key=-1;e.port_index=0;send(out,e.header,time);
        e.header.type=CLAP_EVENT_NOTE_END;send(out,e.header,time);
        // A dropped pedal-up must not leave the next phrase sustained downstream.
        for(uint8_t channel=0;channel<16;++channel){
            clap_event_midi_t pedalUp{};pedalUp.header={sizeof(pedalUp),time,0,CLAP_EVENT_MIDI,0};
            pedalUp.data[0]=0xb0|channel;pedalUp.data[1]=64;send(out,pedalUp.header,time);
        }
    }
    bool schedule(const clap_event_header_t& event,uint64_t due,int voice=-1) {
        if(queued==queue.size()){panic=true;return false;}
        auto& p=queue[queued++];std::memcpy(p.bytes.data(),&event,event.size);p.due=due;p.order=order++;p.voice=voice;
        std::push_heap(queue.begin(),queue.begin()+queued,later);return true;
    }
    static bool matches(const Voice& v,int32_t note,int16_t channel,int16_t key) {
        return v.used && (note<0 || note==v.input.note_id) && (channel<0 || channel==v.input.channel) && (key<0 || key==v.input.key);
    }
    void note(clap_event_note_t event,uint32_t time,const clap_output_events_t* out,bool midi=false) {
        if(event.port_index < -1 || event.port_index>0 || event.channel < -1 || event.channel>15 || event.key < -1 || event.key>127 || !std::isfinite(event.velocity))return;
        if(event.header.type==CLAP_EVENT_NOTE_ON){
            if(event.channel<0 || event.key<0)return;
            const int key=event.key+(delayed?0:value.load());
            if(key<0 || key>127){event.header.type=CLAP_EVENT_NOTE_END;send(out,event.header,time);return;}
            for(size_t i=0;i<voices.size();++i)if(!voices[i].used){
                if(nextId==INT32_MAX){panic=true;return;}
                voices[i]={event,nextId++,static_cast<int16_t>(key),true,true,false,false,midi};
                event.key=key;event.note_id=voices[i].outputId;schedule(event.header,clock+time+delay,i);return;
            }
            panic=true;return;
        }
        int oldest=-1;
        if(midi)for(size_t i=0;i<voices.size();++i)if(voices[i].midi && voices[i].held && matches(voices[i],-1,event.channel,event.key))
            if(oldest<0 || voices[i].outputId<voices[oldest].outputId)oldest=i;
        for(size_t i=0;i<voices.size();++i){auto& v=voices[i];
            if(!matches(v,event.note_id,event.channel,event.key) || (midi && static_cast<int>(i)!=oldest))continue;
            auto e=event;e.port_index=0;e.key=v.outputKey;e.channel=v.input.channel;e.note_id=v.outputId;
            if(event.header.type==CLAP_EVENT_NOTE_CHOKE){if(v.started)send(out,e.header,time);end(i,time,out);}
            else if(v.held){v.held=false;schedule(e.header,clock+time+delay,i);}
        }
    }
    void expression(const clap_event_note_expression_t& event,uint32_t time) {
        if(event.port_index>0)return;
        for(size_t i=0;i<voices.size();++i)if(matches(voices[i],event.note_id,event.channel,event.key)){
            auto e=event;e.port_index=0;e.key=voices[i].outputKey;e.channel=voices[i].input.channel;e.note_id=voices[i].outputId;
            schedule(e.header,clock+time+delay,i);
        }
    }
    void parameter(const clap_event_header_t* h) {
        if(!h || h->space_id || h->type!=CLAP_EVENT_PARAM_VALUE || h->size!=sizeof(clap_event_param_value_t))return;
        const auto& e=*reinterpret_cast<const clap_event_param_value_t*>(h);
        if(e.param_id==1 && e.note_id<0 && e.key<0 && std::isfinite(e.value))value=std::clamp(static_cast<int>(std::clamp(e.value,double(minimum),double(maximum))),minimum,maximum);
    }
    void input(const clap_event_header_t* h,uint32_t time,const clap_output_events_t* out) {
        if(!h || h->space_id!=CLAP_CORE_EVENT_SPACE_ID || h->size>64)return;
        if(h->type==CLAP_EVENT_PARAM_VALUE){parameter(h);return;}
        if((h->type==CLAP_EVENT_NOTE_ON || h->type==CLAP_EVENT_NOTE_OFF || h->type==CLAP_EVENT_NOTE_CHOKE) && h->size==sizeof(clap_event_note_t))
            return note(*reinterpret_cast<const clap_event_note_t*>(h),time,out);
        if(h->type==CLAP_EVENT_NOTE_EXPRESSION && h->size==sizeof(clap_event_note_expression_t))
            return expression(*reinterpret_cast<const clap_event_note_expression_t*>(h),time);
        if(h->type!=CLAP_EVENT_MIDI || h->size!=sizeof(clap_event_midi_t))return;
        const auto& m=*reinterpret_cast<const clap_event_midi_t*>(h);if(m.port_index || m.data[1]>127 || m.data[2]>127)return;
        const int kind=m.data[0]&0xf0,channel=m.data[0]&15;
        if(kind==0x90 || kind==0x80 || (kind==0xb0 && (m.data[1]==120 || m.data[1]==123))){
            clap_event_note_t e{};const bool on=kind==0x90 && m.data[2];
            e.header={sizeof(e),time,CLAP_CORE_EVENT_SPACE_ID,static_cast<uint16_t>(on?CLAP_EVENT_NOTE_ON:kind==0xb0 && m.data[1]==120?CLAP_EVENT_NOTE_CHOKE:CLAP_EVENT_NOTE_OFF),h->flags};
            e.note_id=-1;e.channel=channel;e.key=kind==0xb0?-1:m.data[1];e.velocity=m.data[2]/127.0;note(e,time,out,kind!=0xb0);return;
        }
        if(kind==0xa0 || kind==0xd0){clap_event_note_expression_t e{};e.header={sizeof(e),time,CLAP_CORE_EVENT_SPACE_ID,CLAP_EVENT_NOTE_EXPRESSION,h->flags};
            e.expression_id=CLAP_NOTE_EXPRESSION_PRESSURE;e.note_id=-1;e.channel=channel;e.key=kind==0xa0?m.data[1]:-1;e.value=(kind==0xa0?m.data[2]:m.data[1])/127.0;expression(e,time);return;}
        schedule(*h,clock+time+delay);
    }
    void deliver(uint32_t time,const clap_output_events_t* out) {
        while(queued && queue[0].due<=clock+time && !panic){
            std::pop_heap(queue.begin(),queue.begin()+queued,later);const auto p=queue[--queued];
            if(!send(out,*p.header(),time))return;
            if(p.voice>=0){auto& v=voices[p.voice];
                if(p.header()->type==CLAP_EVENT_NOTE_ON)v.started=true;
                if(p.header()->type==CLAP_EVENT_NOTE_OFF){v.offDelivered=true;if(!pedal[v.input.channel])end(p.voice,time,out);}
            }else if(p.header()->type==CLAP_EVENT_MIDI){const auto& m=*reinterpret_cast<const clap_event_midi_t*>(p.header());
                if((m.data[0]&0xf0)==0xb0 && m.data[1]==64){const auto c=m.data[0]&15;pedal[c]=m.data[2]>=64;
                    if(!pedal[c])for(size_t i=0;i<voices.size();++i)if(voices[i].used && voices[i].offDelivered && voices[i].input.channel==c)end(i,time,out);}
            }
        }
    }
    explicit Processor(const clap_host_t*) {
        api.desc=&descriptor;api.plugin_data=this;
        api.init=[](const clap_plugin_t*){return true;};api.destroy=[](const clap_plugin_t* p){delete &get(p);};
        api.activate=[](const clap_plugin_t* p,double r,uint32_t,uint32_t){get(p).rate=r;return std::isfinite(r) && r>0;};
        api.deactivate=[](const clap_plugin_t* p){get(p).panic=true;};api.start_processing=[](const clap_plugin_t*){return true;};api.stop_processing=[](const clap_plugin_t*){};
        api.reset=[](const clap_plugin_t* p){get(p).panic=true;};api.on_main_thread=[](const clap_plugin_t*){};
        api.process=[](const clap_plugin_t* p,const clap_process_t* b)->clap_process_status {
            auto& s=get(p);
            if(b->transport){const bool playing=b->transport->flags&CLAP_TRANSPORT_IS_PLAYING;if(s.transportSeen && s.playing && !playing)s.panic=true;s.playing=playing;s.transportSeen=true;}
            if(s.panic)s.recover(0,b->out_events);
            uint32_t event=0;const auto count=b->in_events->size(b->in_events);
            for(uint32_t frame=0;frame<b->frames_count;++frame){
                s.deliver(frame,b->out_events);
                while(event<count){const auto* h=b->in_events->get(b->in_events,event);if(!h){++event;continue;}if(h->time>frame)break;
                    // Delay changes wait for an idle stream so releases/controllers cannot overtake notes.
                    if(!s.queued && std::none_of(s.voices.begin(),s.voices.end(),[](const auto& v){return v.used;}))s.delay=delayed?static_cast<uint64_t>(std::llround(s.value.load()*s.rate/1000)):0;
                    s.input(h,frame,b->out_events);++event;if(s.panic)break;s.deliver(frame,b->out_events);
                }
                if(s.panic){s.recover(frame,b->out_events);break;}
            }
            s.clock+=b->frames_count;return CLAP_PROCESS_CONTINUE;
        };
        api.get_extension=[](const clap_plugin_t*,const char* id)->const void* {
            static const clap_plugin_audio_ports_t audio{[](const clap_plugin_t*,bool)->uint32_t{return 0;},[](const clap_plugin_t*,uint32_t,bool,clap_audio_port_info_t*){return false;}};
            static const clap_plugin_note_ports_t notes{[](const clap_plugin_t*,bool)->uint32_t{return 1;},[](const clap_plugin_t*,uint32_t i,bool in,clap_note_port_info_t* p){if(i)return false;*p={};p->id=0;p->supported_dialects=CLAP_NOTE_DIALECT_CLAP|CLAP_NOTE_DIALECT_MIDI;p->preferred_dialect=CLAP_NOTE_DIALECT_CLAP;std::snprintf(p->name,sizeof(p->name),"%s",in?"Input":"Output");return true;}};
            static const clap_plugin_params_t params{
                [](const clap_plugin_t*)->uint32_t{return 1;},
                [](const clap_plugin_t*,uint32_t i,clap_param_info_t* p){if(i)return false;*p={};p->id=1;p->flags=CLAP_PARAM_IS_AUTOMATABLE|CLAP_PARAM_IS_STEPPED;p->min_value=minimum;p->max_value=maximum;p->default_value=defaultValue;std::snprintf(p->name,sizeof(p->name),"%s",delayed?"Delay (ms)":"Transpose");return true;},
                [](const clap_plugin_t* p,clap_id id,double* v){if(id!=1)return false;*v=get(p).value.load();return true;},
                [](const clap_plugin_t*,clap_id id,double v,char* out,uint32_t n){if(id!=1)return false;std::snprintf(out,n,delayed?"%.0f ms":"%+.0f st",v);return true;},nullptr,
                [](const clap_plugin_t* p,const clap_input_events_t* in,const clap_output_events_t*){for(uint32_t i=0;i<in->size(in);++i)get(p).parameter(in->get(in,i));}};
            static const clap_plugin_state_t state{
                [](const clap_plugin_t* p,const clap_ostream_t* stream){const auto v=static_cast<unsigned>(get(p).value.load()-minimum);const uint8_t data[]{'O','M','F','X',1,delayed?uint8_t(1):uint8_t(0),static_cast<uint8_t>(v),static_cast<uint8_t>(v>>8)};size_t offset=0;while(offset<sizeof(data)){const auto n=stream->write(stream,reinterpret_cast<const char*>(data)+offset,sizeof(data)-offset);if(n<=0 || static_cast<uint64_t>(n)>sizeof(data)-offset)return false;offset+=n;}return true;},
                [](const clap_plugin_t* p,const clap_istream_t* stream){uint8_t data[8]{};size_t offset=0;while(offset<sizeof(data)){const auto n=stream->read(stream,reinterpret_cast<char*>(data)+offset,sizeof(data)-offset);if(n<=0 || static_cast<uint64_t>(n)>sizeof(data)-offset)return false;offset+=n;}const int v=minimum+data[6]+(unsigned(data[7])<<8);if(std::memcmp(data,"OMFX",4) || data[4]!=1 || data[5]!=(delayed?1:0) || v>maximum)return false;get(p).value=v;return true;}};
            if(!std::strcmp(id,CLAP_EXT_AUDIO_PORTS))return &audio;
            if(!std::strcmp(id,CLAP_EXT_NOTE_PORTS))return &notes;
            if(!std::strcmp(id,CLAP_EXT_PARAMS))return &params;
            if(!std::strcmp(id,CLAP_EXT_STATE))return &state;
            return nullptr;
        };
    }
};
const clap_plugin_factory_t factory{
    [](const clap_plugin_factory_t*)->uint32_t{return 1;},
    [](const clap_plugin_factory_t*,uint32_t i)->const clap_plugin_descriptor_t*{return i?nullptr:&descriptor;},
    [](const clap_plugin_factory_t*,const clap_host_t* h,const char* id)->const clap_plugin_t*{return std::strcmp(id,descriptor.id)?nullptr:&(new Processor(h))->api;}};
}
extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry{CLAP_VERSION,[](const char*){return true;},[](){},[](const char* id)->const void*{return !std::strcmp(id,CLAP_PLUGIN_FACTORY_ID)?&factory:nullptr;}};
