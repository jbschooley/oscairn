// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/plugin_host.h"
#include "Urids.h"
#include "Worker.h"
#include "StateBridge.h"
#include "Patch.h"
#include "../plugin-host/src/StateFiles.h"
#include <lilv/lilv.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/midi/midi.h>
#include <lv2/options/options.h>
#include <lv2/parameters/parameters.h>
#include <lv2/state/state.h>
#include <lv2/presets/presets.h>
#include <lv2/port-props/port-props.h>
#include <lv2/resize-port/resize-port.h>
#include <lv2/time/time.h>
#include <lv2/urid/urid.h>
#include <clap/ext/params.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <limits>
#include <stdexcept>

namespace oscairn
{
namespace
{
using Event = kick::clap::Event;
using Parameter = kick::clap::Parameter;
using PortInfo = kick::clap::Port;
constexpr uint32_t maxEvents = kick::clap::Host::maxBlockEvents;
using Node = std::unique_ptr<LilvNode, decltype(&lilv_node_free)>;
Node own(LilvNode* n) { return Node(n, lilv_node_free); }
std::string text(const LilvNode* n) { return n ? lilv_node_as_string(n) : ""; }

using lv2::Urids;

class Lv2Host final : public PluginHost
{
    enum class Kind { Ignored, Audio, Control, Atom };
    struct Port
    {
        Kind kind=Kind::Ignored;
        bool input=false, midi=false, time=false, toggle=false, optional=false, patch=false;
        uint32_t requestedBytes=0;
        uint32_t channel=0, midiIndex=0;
        float control=0;
        std::vector<float> audio;
        std::vector<uint64_t> atom; // 8-byte alignment
        std::string symbol;
        uint32_t atomBytes() const {return static_cast<uint32_t>(atom.size()*sizeof(uint64_t));}
        LV2_Atom_Sequence* sequence(){return reinterpret_cast<LV2_Atom_Sequence*>(atom.data());}
    };
    struct Timed { Event event; uint32_t order; };
    std::unique_ptr<LilvWorld,decltype(&lilv_world_free)> world_{lilv_world_new(),lilv_world_free};
    std::unique_ptr<LilvInstance,decltype(&lilv_instance_free)> instance_{nullptr,lilv_instance_free};
    bool active_=false;
    const LilvPlugin* plugin_=nullptr;
    const LV2_State_Interface* state_=nullptr;
    bool statePortError_=false;
    state_files::TemporaryDirectory scratch_;
    std::unique_ptr<lv2::Patch> patch_;
    uint32_t patchInput_=UINT32_MAX;
    LV2_URID atomObject_=0,atomBlank_=0;
    std::atomic_flag gate_=ATOMIC_FLAG_INIT;
    std::atomic<bool> stateBusy_{false},resumePanic_{false},faulted_{false};
    std::atomic<uint64_t> stateSilences_{0};
    struct Quiescent
    {
        Lv2Host& host;
        explicit Quiescent(Lv2Host& h):host(h)
        {
            host.stateBusy_.store(true);
            while(host.gate_.test_and_set(std::memory_order_acquire))std::this_thread::sleep_for(std::chrono::microseconds(50));
            host.worker_.quiesce();
        }
        ~Quiescent()
        {
            try{host.worker_.resume();}catch(...){host.faulted_=true;++host.errors_;}
            host.gate_.clear(std::memory_order_release);host.stateBusy_.store(false);
        }
    };
    kick::clap::Descriptor descriptor_;
    std::vector<Parameter> parameters_;
    std::vector<PortInfo> inputs_,outputs_;
    std::vector<Port> ports_;
    std::unique_ptr<std::atomic<double>[]> values_;
    uint32_t noteInputs_=0,noteOutputs_=0;
    ProcessingMode mode_;
    double rate_;
    uint32_t maxFrames_;
    Urids urids_;
    lv2::Worker worker_;
    LV2_URID_Map map_{&urids_,Urids::map};
    LV2_URID_Unmap unmap_{&urids_,Urids::unmap};
    LV2_Atom_Forge forge_{};
    LV2_URID sequence_,chunk_,frameTime_,midi_,position_,frame_,speed_,bpm_,beat_,bar_,barBeat_,beatUnit_,beatsPerBar_;
    int32_t minimum_=1, maximum_, sequenceSize_=0;
    float sampleRate_;
    std::array<LV2_Options_Option,5> options_{};
    LV2_State_Make_Path makePath_{this,makePath};
    LV2_State_Free_Path freePath_{nullptr,[](void*,char* path){std::free(path);}};
    std::array<LV2_Feature,9> features_{};
    std::array<const LV2_Feature*,10> featurePointers_{};
    // One control producer, one audio consumer; graph callers share this contract.
    std::array<Event,kick::clap::Host::maxEvents> commands_{};
    std::atomic<size_t> head_{0},tail_{0};
    std::array<Timed,2*maxEvents> ordered_{}; // Separate budgets for queued controls and graph events.
    std::array<Event,maxEvents> emitted_{};
    uint32_t emittedCount_=0;
    bool inputPanic_=false,outputPanic_=false;
    std::atomic<uint64_t> blocks_{0},drops_{0},errors_{0},deadlines_{0};
    std::atomic<double> lastUs_{0},maxUs_{0};

    static char* makePath(void* handle,const char* relative) noexcept
    {
        try
        {
            if(!relative)return nullptr;
            const auto path=std::filesystem::path(relative).lexically_normal();
            if(path.is_absolute())return nullptr;
            for(const auto& part:path)if(part=="..")return nullptr;
            const auto result=static_cast<Lv2Host*>(handle)->scratch_.path/path;
            std::filesystem::create_directories(result.parent_path());return ::strdup(result.c_str());
        }
        catch(...){return nullptr;}
    }
    static const void* getPort(const char* symbol,void* data,uint32_t* size,uint32_t* type)
    {
        auto& host=*static_cast<Lv2Host*>(data);
        for(auto& port:host.ports_)if(port.symbol==symbol && port.kind==Kind::Control && port.input)
        {*size=sizeof(float);*type=host.urids_(LV2_ATOM__Float);return &port.control;}
        *size=0;*type=0;return nullptr;
    }
    static void setPort(const char* symbol,void* data,const void* value,uint32_t size,uint32_t type)
    {
        auto& host=*static_cast<Lv2Host*>(data);
        double number=0;bool valid=true;
        if(type==host.urids_(LV2_ATOM__Float) && size==sizeof(float)){float v;std::memcpy(&v,value,size);number=v;}
        else if(type==host.urids_(LV2_ATOM__Double) && size==sizeof(double))std::memcpy(&number,value,size);
        else if(type==host.urids_(LV2_ATOM__Int) && size==sizeof(int32_t)){int32_t v;std::memcpy(&v,value,size);number=v;}
        else valid=false;
        for(uint32_t i=0;i<host.ports_.size();++i)if(host.ports_[i].symbol==symbol && host.writable(i))
        {
            if(!valid || !std::isfinite(number)){host.statePortError_=true;return;}
            clap_event_param_value_t e{};e.param_id=i;e.value=number;e.note_id=e.port_index=e.channel=e.key=-1;
            host.apply(Event::from(e));return;
        }
    }
    void restore(const LilvState* state)
    {
        if(!state || text(lilv_state_get_plugin_uri(state))!=descriptor_.id)throw std::runtime_error("State belongs to a different LV2 plugin");
        if(lilv_state_get_num_properties(state) && !state_)throw std::runtime_error("LV2 plugin has no state interface");
        Quiescent guard(*this);
        const bool wasActive=active_;statePortError_=false;
        if(active_){lilv_instance_deactivate(instance_.get());active_=false;}
        try
        {
            lv2::StateBridge bridge(instance_.get(),state_);
            lilv_state_restore(state,state_?&bridge.proxy:nullptr,setPort,this,0,featurePointers_.data());
            if(bridge.status!=LV2_STATE_SUCCESS || bridge.missingPath || statePortError_)throw std::runtime_error(bridge.missingPath?"Missing LV2 state asset":"LV2 plugin rejected state");
            tail_.store(head_.load());
            if(patch_)patch_->clear();
            lilv_instance_activate(instance_.get());active_=true;faulted_=false;resumePanic_=wasActive;
        }
        catch(...){faulted_=true;throw;}
    }

    Node uri(const char* value) const { return own(lilv_new_uri(world_.get(),value)); }
    bool is(const LilvPlugin* plugin,const LilvPort* port,const char* klass) const
    {auto n=uri(klass);return lilv_port_is_a(plugin,port,n.get());}
    bool property(const LilvPlugin* plugin,const LilvPort* port,const char* name) const
    {auto n=uri(name);return lilv_port_has_property(plugin,port,n.get());}
    bool supports(const LilvPlugin* plugin,const LilvPort* port,const char* type) const
    {auto n=uri(type);return lilv_port_supports_event(plugin,port,n.get());}
    const Parameter* parameter(clap_id id) const noexcept
    {
        const auto it=std::lower_bound(parameters_.begin(),parameters_.end(),id,[](const auto& p,clap_id key){return p.id<key;});
        return it!=parameters_.end() && it->id==id ? &*it : nullptr;
    }
    bool writable(clap_id id) const noexcept
    {const auto* p=parameter(id);return p && !(p->flags&CLAP_PARAM_IS_READONLY);}
    bool queue(const Event& e) noexcept
    {
        const auto head=head_.load(std::memory_order_relaxed);
        if(head-tail_.load(std::memory_order_acquire)==commands_.size())return false;
        commands_[head%commands_.size()]=e;
        head_.store(head+1,std::memory_order_release);
        return true;
    }
    void apply(const Event& event) noexcept
    {
        const auto& e=*reinterpret_cast<const clap_event_param_value_t*>(event.bytes.data());
        const auto* p=parameter(e.param_id);
        if(!p || (p->flags&CLAP_PARAM_IS_READONLY) || !std::isfinite(e.value) || e.note_id!=-1 ||
           e.port_index!=-1 || e.channel!=-1 || e.key!=-1){++drops_;return;}
        auto value=std::clamp(e.value,p->minimum,p->maximum);
        if(ports_[p->id].toggle)value=value>0?1:0;
        else if(p->flags&CLAP_PARAM_IS_STEPPED)value=std::clamp(std::trunc(value),p->minimum,p->maximum);
        ports_[e.param_id].control=static_cast<float>(value);
        values_[e.param_id].store(value,std::memory_order_relaxed);
    }
    static uint32_t midiSize(uint8_t status) noexcept
    {
        if(status<0x80)return 0;
        if(status<0xf0)return (status&0xe0)==0xc0?2:3;
        switch(status){case 0xf1:case 0xf3:return 2;case 0xf2:return 3;case 0xf6:case 0xf8:case 0xfa:case 0xfb:case 0xfc:case 0xfe:case 0xff:return 1;default:return 0;}
    }
    void append(Port& port,uint32_t time,LV2_URID type,const void* data,uint32_t size) noexcept
    {
        auto* sequence=port.sequence();
        const auto padded=(sizeof(LV2_Atom_Event)+size+7u)&~size_t(7);
        if(size>port.atomBytes() || sizeof(LV2_Atom)+sequence->atom.size+padded>port.atomBytes())
        {++drops_;if(type==midi_)inputPanic_=true;return;}
        auto* event=reinterpret_cast<LV2_Atom_Event*>(reinterpret_cast<uint8_t*>(sequence)+sizeof(LV2_Atom)+sequence->atom.size);
        event->time.frames=time;event->body={size,type};
        std::memcpy(event+1,data,size);
        std::memset(reinterpret_cast<uint8_t*>(event+1)+size,0,padded-size-sizeof(LV2_Atom_Event));
        sequence->atom.size+=static_cast<uint32_t>(padded);
    }
    void sendMidi(int portIndex,uint32_t time,uint8_t status,uint8_t a,uint8_t b) noexcept
    {
        const uint8_t bytes[]{status,a,b};
        for(auto& port:ports_)if(port.kind==Kind::Atom && port.input && port.midi &&
            (portIndex==-1 || static_cast<uint32_t>(portIndex)==port.midiIndex))append(port,time,midi_,bytes,midiSize(status));
    }
    void musical(const Event& event,uint32_t start) noexcept
    {
        const auto* h=event.header();const auto time=h->time-start;
        if(h->type==CLAP_EVENT_MIDI)
        {
            const auto& e=*reinterpret_cast<const clap_event_midi_t*>(h);
            const auto length=midiSize(e.data[0]);
            if(!length || (length>1 && e.data[1]>127) || (length>2 && e.data[2]>127) || e.port_index>=noteInputs_){++drops_;return;}
            sendMidi(e.port_index,time,e.data[0],e.data[1],e.data[2]);return;
        }
        int port=-1,channel=-1,key=-1;double value=0;bool pressure=false,on=false;
        if(h->type==CLAP_EVENT_NOTE_EXPRESSION)
        {
            const auto& e=*reinterpret_cast<const clap_event_note_expression_t*>(h);
            if(e.expression_id!=CLAP_NOTE_EXPRESSION_PRESSURE || e.note_id!=-1){++drops_;return;}
            port=e.port_index;channel=e.channel;key=e.key;value=e.value;pressure=true;
        }
        else
        {
            const auto& e=*reinterpret_cast<const clap_event_note_t*>(h);
            if(e.note_id!=-1){++drops_;return;}
            port=e.port_index;channel=e.channel;key=e.key;value=e.velocity;on=h->type==CLAP_EVENT_NOTE_ON;
        }
        if(port < -1 || (port>=0 && static_cast<uint32_t>(port)>=noteInputs_) || channel < -1 || channel>15 || key < -1 || key>127 ||
           !std::isfinite(value) || value<0 || value>1 || (on && (key<0 || channel<0 || port<0))){++drops_;return;}
        const auto v=static_cast<uint8_t>(std::lround(value*127));
        for(int c=channel<0?0:channel;c<=(channel<0?15:channel);++c)
        {
            if(pressure)sendMidi(port,time,static_cast<uint8_t>((key<0?0xd0:0xa0)|c),key<0?v:static_cast<uint8_t>(key),key<0?0:v);
            else if(key<0)sendMidi(port,time,static_cast<uint8_t>(0xb0|c),123,0);
            else sendMidi(port,time,static_cast<uint8_t>((on?0x90:0x80)|c),static_cast<uint8_t>(key),v);
        }
    }
    bool valid(const Event& event,uint32_t frames) noexcept
    {
        const auto* h=event.header();if(h->space_id!=CLAP_CORE_EVENT_SPACE_ID || h->time>=frames)return false;
        uint32_t expected=0;
        switch(h->type)
        {
        case CLAP_EVENT_NOTE_ON:case CLAP_EVENT_NOTE_OFF:case CLAP_EVENT_NOTE_CHOKE:expected=sizeof(clap_event_note_t);break;
        case CLAP_EVENT_NOTE_EXPRESSION:expected=sizeof(clap_event_note_expression_t);break;
        case CLAP_EVENT_MIDI:expected=sizeof(clap_event_midi_t);break;
        case CLAP_EVENT_PARAM_VALUE:expected=sizeof(clap_event_param_value_t);break;
        case CLAP_EVENT_PARAM_GESTURE_BEGIN:case CLAP_EVENT_PARAM_GESTURE_END:expected=sizeof(clap_event_param_gesture_t);break;
        default:return false;
        }
        return h->size==expected;
    }
    void transport(Port& port,const clap_event_transport_t* t,uint32_t start) noexcept
    {
        if(!t)return;
        alignas(8) uint8_t buffer[512]{};
        auto forge=forge_;lv2_atom_forge_set_buffer(&forge,buffer,sizeof(buffer));
        LV2_Atom_Forge_Frame object;lv2_atom_forge_object(&forge,&object,0,position_);
        const bool playing=t->flags&CLAP_TRANSPORT_IS_PLAYING;
        const bool meter=(t->flags&CLAP_TRANSPORT_HAS_TIME_SIGNATURE) && t->tsig_num && t->tsig_denom;
        const double beatScale=meter?t->tsig_denom/4.0:1.0;
        const bool hasTempo=(t->flags&CLAP_TRANSPORT_HAS_TEMPO) && std::isfinite(t->tempo) && t->tempo>0 && std::isfinite(t->tempo_inc);
        const double elapsed=playing?start:0;
        auto key=[&](LV2_URID id){lv2_atom_forge_key(&forge,id);};
        key(speed_);lv2_atom_forge_float(&forge,playing?1.f:0.f);
        if(hasTempo){key(bpm_);lv2_atom_forge_float(&forge,static_cast<float>((t->tempo+elapsed*t->tempo_inc)*beatScale));}
        if(meter)
        {
            key(beatUnit_);lv2_atom_forge_int(&forge,t->tsig_denom);
            key(beatsPerBar_);lv2_atom_forge_float(&forge,t->tsig_num);
        }
        if(t->flags&CLAP_TRANSPORT_HAS_SECONDS_TIMELINE)
        {
            key(frame_);lv2_atom_forge_long(&forge,static_cast<int64_t>(std::llround(static_cast<double>(t->song_pos_seconds)/CLAP_SECTIME_FACTOR*rate_))+(playing?start:0));
        }
        // Without tempo, only the block-start beat is known while playing.
        if((t->flags&CLAP_TRANSPORT_HAS_BEATS_TIMELINE) && (!elapsed || hasTempo))
        {
            const double quarter=static_cast<double>(t->song_pos_beats)/CLAP_BEATTIME_FACTOR+
                (hasTempo?(elapsed*t->tempo+elapsed*(elapsed-1)*.5*t->tempo_inc)/rate_/60:0);
            const double beat=quarter*beatScale;
            key(beat_);lv2_atom_forge_double(&forge,beat);
            if(meter)
            {
                // Respect the host's bar origin: previous bars may use another meter.
                const double relative=(quarter-static_cast<double>(t->bar_start)/CLAP_BEATTIME_FACTOR)*beatScale;
                const auto bars=static_cast<int64_t>(std::floor(relative/t->tsig_num));
                key(bar_);lv2_atom_forge_long(&forge,static_cast<int64_t>(t->bar_number)+bars);
                key(barBeat_);lv2_atom_forge_float(&forge,static_cast<float>(relative-bars*t->tsig_num));
            }
        }
        lv2_atom_forge_pop(&forge,&object);
        const auto* atom=reinterpret_cast<const LV2_Atom*>(buffer);
        append(port,0,atom->type,atom+1,atom->size);
    }
    void collect(Port& port,uint32_t start,uint32_t frames) noexcept
    {
        const auto* seq=port.sequence();
        if(seq->atom.type==chunk_)return; // plugin wrote nothing
        if(seq->atom.type!=sequence_ || seq->atom.size<sizeof(seq->body) || seq->atom.size>port.atomBytes()-sizeof(LV2_Atom) ||
           (seq->body.unit!=0 && seq->body.unit!=frameTime_)){++errors_;outputPanic_|=port.midi;return;}
        size_t offset=sizeof(LV2_Atom_Sequence),end=sizeof(LV2_Atom)+seq->atom.size;
        while(offset<end)
        {
            if(end-offset<sizeof(LV2_Atom_Event)){++errors_;outputPanic_|=port.midi;break;}
            const auto* e=reinterpret_cast<const LV2_Atom_Event*>(reinterpret_cast<const uint8_t*>(seq)+offset);
            const uint64_t size=(uint64_t(sizeof(*e))+e->body.size+7)&~uint64_t(7);
            if(size>end-offset){++errors_;outputPanic_|=port.midi;break;}
            if(port.patch && patch_ && (e->body.type==atomObject_ || e->body.type==atomBlank_))
                if(!patch_->collect(&e->body))++drops_;
            if(port.midi && e->body.type==midi_)
            {
                const auto* bytes=reinterpret_cast<const uint8_t*>(e+1);
                if(e->time.frames<0 || e->time.frames>=frames || e->body.size<1 || e->body.size>3 ||
                   midiSize(bytes[0])!=e->body.size || (e->body.size>1 && bytes[1]>127) || (e->body.size>2 && bytes[2]>127)){++drops_;outputPanic_=true;}
                else if(emittedCount_==emitted_.size()){++drops_;outputPanic_=true;}
                else
                {
                    const auto time=start+static_cast<uint32_t>(e->time.frames);
                    const auto kind=bytes[0]&0xf0;
                    if(kind==0x80 || kind==0x90)emitted_[emittedCount_++]=Event::note(kind==0x90 && bytes[2],bytes[1],bytes[2]/127.0,time,bytes[0]&15,port.midiIndex);
                    else if(kind==0xa0 || kind==0xd0)
                    {
                        clap_event_note_expression_t pressure{};
                        pressure.header={sizeof(pressure),time,CLAP_CORE_EVENT_SPACE_ID,CLAP_EVENT_NOTE_EXPRESSION,0};
                        pressure.expression_id=CLAP_NOTE_EXPRESSION_PRESSURE;pressure.note_id=-1;pressure.port_index=static_cast<int16_t>(port.midiIndex);
                        pressure.channel=bytes[0]&15;pressure.key=kind==0xa0?bytes[1]:-1;pressure.value=(kind==0xa0?bytes[2]:bytes[1])/127.0;
                        emitted_[emittedCount_++]=Event::from(pressure);
                    }
                    else emitted_[emittedCount_++]=Event::midi(bytes[0],e->body.size>1?bytes[1]:0,e->body.size>2?bytes[2]:0,time,static_cast<uint16_t>(port.midiIndex));
                }
            }
            offset+=static_cast<size_t>(size);
        }
    }
public:
    Lv2Host(const std::string& bundle,const std::string& id,double rate,uint32_t frames,ProcessingMode mode,Lv2BufferLimits limits):mode_(mode),rate_(rate),maxFrames_(frames),worker_(mode==ProcessingMode::Offline),maximum_(static_cast<int32_t>(frames)),sampleRate_(static_cast<float>(rate))
    {
        if(!world_ || !std::isfinite(rate) || rate<=0 || !frames || frames>1048576)throw std::invalid_argument("Invalid LV2 host configuration");
        // sequenceSize is an Atom Int. Round in 64 bits before comparing limits.
        const auto aligned=[](uint32_t n){return (uint64_t(n)+7u)&~uint64_t(7);};
        if(limits.defaultBytes<sizeof(LV2_Atom_Sequence) || limits.maxPortBytes>INT32_MAX ||
           aligned(limits.defaultBytes)>limits.maxPortBytes || aligned(limits.defaultBytes)>limits.maxTotalBytes)
            throw std::invalid_argument("Invalid LV2 Atom buffer limits");
        sequenceSize_=static_cast<int32_t>(aligned(limits.defaultBytes));
        const auto path=std::filesystem::canonical(bundle).string()+"/";
        auto bundleUri=own(lilv_new_file_uri(world_.get(),nullptr,path.c_str()));
        lilv_world_load_bundle(world_.get(),bundleUri.get());
        const auto* plugins=lilv_world_get_all_plugins(world_.get());const LilvPlugin* plugin=nullptr;
        if(id.empty())
        {
            if(lilv_plugins_size(plugins)!=1)throw std::runtime_error("LV2 bundle must contain one plugin, or supply pluginId as its URI: "+bundle);
            plugin=lilv_plugins_get(plugins,lilv_plugins_begin(plugins));
        }
        else {auto key=uri(id.c_str());plugin=lilv_plugins_get_by_uri(plugins,key.get());}
        if(!plugin || !lilv_plugin_verify(plugin))throw std::runtime_error("No valid matching LV2 plugin in "+bundle);
        plugin_=plugin;
        descriptor_.path=std::filesystem::canonical(bundle).string();descriptor_.id=text(lilv_plugin_get_uri(plugin));
        auto name=own(lilv_plugin_get_name(plugin));descriptor_.name=text(name.get());
        auto author=own(lilv_plugin_get_author_name(plugin));descriptor_.vendor=text(author.get());descriptor_.features={"lv2"};
        sequence_=urids_(LV2_ATOM__Sequence);chunk_=urids_(LV2_ATOM__Chunk);frameTime_=urids_(LV2_ATOM__frameTime);midi_=urids_(LV2_MIDI__MidiEvent);
        position_=urids_(LV2_TIME__Position);frame_=urids_(LV2_TIME__frame);speed_=urids_(LV2_TIME__speed);bpm_=urids_(LV2_TIME__beatsPerMinute);
        beat_=urids_(LV2_TIME__beat);bar_=urids_(LV2_TIME__bar);barBeat_=urids_(LV2_TIME__barBeat);beatUnit_=urids_(LV2_TIME__beatUnit);beatsPerBar_=urids_(LV2_TIME__beatsPerBar);
        lv2_atom_forge_init(&forge_,&map_);
        options_[0]={LV2_OPTIONS_INSTANCE,0,urids_(LV2_BUF_SIZE__minBlockLength),sizeof(minimum_),urids_(LV2_ATOM__Int),&minimum_};
        options_[1]={LV2_OPTIONS_INSTANCE,0,urids_(LV2_BUF_SIZE__maxBlockLength),sizeof(maximum_),urids_(LV2_ATOM__Int),&maximum_};
        options_[2]={LV2_OPTIONS_INSTANCE,0,urids_(LV2_BUF_SIZE__sequenceSize),sizeof(sequenceSize_),urids_(LV2_ATOM__Int),&sequenceSize_};
        options_[3]={LV2_OPTIONS_INSTANCE,0,urids_(LV2_PARAMETERS__sampleRate),sizeof(sampleRate_),urids_(LV2_ATOM__Float),&sampleRate_};
        features_={LV2_Feature{LV2_URID__map,&map_},{LV2_URID__unmap,&unmap_},{LV2_OPTIONS__options,options_.data()},
                   {LV2_BUF_SIZE__boundedBlockLength,nullptr},{LV2_CORE__inPlaceBroken,nullptr},{LV2_WORKER__schedule,worker_.feature()},
                   {LV2_STATE__loadDefaultState,nullptr},{LV2_STATE__makePath,&makePath_},{LV2_STATE__freePath,&freePath_}};
        for(size_t i=0;i<features_.size();++i)featurePointers_[i]=&features_[i];
        std::unique_ptr<LilvNodes,decltype(&lilv_nodes_free)> required(lilv_plugin_get_required_features(plugin),lilv_nodes_free);
        LILV_FOREACH(nodes,i,required.get())
        {
            const auto feature=text(lilv_nodes_get(required.get(),i));
            if(std::none_of(features_.begin(),features_.end(),[&](const auto& f){return feature==f.URI;}))
                throw std::runtime_error("Unsupported required LV2 feature: "+feature);
        }
        auto requiredOption=uri(LV2_OPTIONS__requiredOption);
        std::unique_ptr<LilvNodes,decltype(&lilv_nodes_free)> requiredOptions(lilv_plugin_get_value(plugin,requiredOption.get()),lilv_nodes_free);
        LILV_FOREACH(nodes,i,requiredOptions.get())
        {
            const auto* option=lilv_nodes_get(requiredOptions.get(),i);
            const auto name=text(option);
            if(!lilv_node_is_uri(option) || std::none_of(options_.begin(),options_.end(),[&](const auto& o){
                const auto* key=Urids::unmap(&urids_,o.key);return key && name==key;}))
                throw std::runtime_error("Unsupported required LV2 option: "+name);
        }
        const auto count=lilv_plugin_get_num_ports(plugin);
        if(count>256)throw std::runtime_error("LV2 port limit exceeded (256)");
        ports_.resize(count);values_=std::make_unique<std::atomic<double>[]>(count);
        for(uint32_t i=0;i<count;++i)
        {
            const auto* source=lilv_plugin_get_port_by_index(plugin,i);auto& port=ports_[i];values_[i].store(0);
            port.symbol=text(lilv_port_get_symbol(plugin,source));
            port.input=is(plugin,source,LV2_CORE__InputPort);
            if(port.input==is(plugin,source,LV2_CORE__OutputPort))throw std::runtime_error("LV2 port has ambiguous direction");
            auto label=own(lilv_port_get_name(plugin,source));
            if(is(plugin,source,LV2_CORE__AudioPort))
            {
                port.kind=Kind::Audio;auto& list=port.input?inputs_:outputs_;port.channel=static_cast<uint32_t>(list.size());
                list.push_back({i,text(label.get()),1,0});port.audio.resize(frames);
            }
            else if(is(plugin,source,LV2_CORE__ControlPort))
            {
                port.kind=Kind::Control;LilvNode *d=nullptr,*lo=nullptr,*hi=nullptr;lilv_port_get_range(plugin,source,&d,&lo,&hi);
                auto defaultNode=own(d),minNode=own(lo),maxNode=own(hi);
                auto number=[](LilvNode* n,double fallback){return n && (lilv_node_is_float(n)||lilv_node_is_int(n))?static_cast<double>(lilv_node_as_float(n)):fallback;};
                const auto scale=property(plugin,source,LV2_CORE__sampleRate)?rate:1.0;
                double minimum=number(lo,0)*scale,maximum=number(hi,1)*scale,initial=number(d,0);
                if(!std::isfinite(minimum)||!std::isfinite(maximum)||!std::isfinite(initial)||minimum>maximum ||
                   std::abs(minimum)>std::numeric_limits<float>::max()||std::abs(maximum)>std::numeric_limits<float>::max())throw std::runtime_error("Invalid LV2 control range");
                uint32_t flags=port.input?CLAP_PARAM_IS_AUTOMATABLE:CLAP_PARAM_IS_READONLY;
                port.toggle=property(plugin,source,LV2_CORE__toggled);
                if(port.toggle){minimum=0;maximum=1;initial=initial>0?1:0;flags|=CLAP_PARAM_IS_STEPPED;}
                else if(property(plugin,source,LV2_CORE__integer))
                {
                    minimum=std::ceil(minimum);maximum=std::floor(maximum);
                    if(minimum>maximum)throw std::runtime_error("LV2 integer range contains no integer");
                    initial=std::trunc(initial);flags|=CLAP_PARAM_IS_STEPPED;
                }
                initial=std::clamp(initial,minimum,maximum);port.control=static_cast<float>(initial);values_[i].store(initial);
                parameters_.push_back({i,text(label.get()),"",minimum,maximum,initial,flags});
            }
            else if(is(plugin,source,LV2_ATOM__AtomPort))
            {
                auto type=uri(LV2_ATOM__bufferType),sequenceType=uri(LV2_ATOM__Sequence);
                std::unique_ptr<LilvNodes,decltype(&lilv_nodes_free)> types(lilv_port_get_value(plugin,source,type.get()),lilv_nodes_free);
                if(!lilv_nodes_contains(types.get(),sequenceType.get()))
                {
                    if(property(plugin,source,LV2_CORE__connectionOptional))continue;
                    throw std::runtime_error("Unsupported LV2 atom buffer type (requires Sequence)");
                }
                auto minimumKey=uri(LV2_RESIZE_PORT__minimumSize);auto minimum=own(lilv_port_get(plugin,source,minimumKey.get()));
                port.optional=property(plugin,source,LV2_CORE__connectionOptional);
                // Parse the full lexical integer: narrowing through Lilv's int32
                // accessor can wrap a hostile or simply oversized requirement.
                uint64_t requested=0;bool validMinimum=true;
                if(minimum)
                {
                    auto lexical=text(minimum.get());if(!lexical.empty() && lexical[0]=='+')lexical.erase(0,1);
                    const auto parsed=std::from_chars(lexical.data(),lexical.data()+lexical.size(),requested);
                    validMinimum=lilv_node_is_int(minimum.get()) && parsed.ec==std::errc{} && parsed.ptr==lexical.data()+lexical.size();
                }
                if(!validMinimum)
                {
                    if(port.optional)continue;
                    throw std::runtime_error("Invalid LV2 Atom minimumSize on port "+std::to_string(i));
                }
                // Check before alignment, so even UINT64_MAX cannot wrap.
                const auto bytes=requested>limits.maxPortBytes?requested:aligned(std::max(limits.defaultBytes,static_cast<uint32_t>(requested)));
                if(bytes>limits.maxPortBytes)
                {
                    if(port.optional)continue;
                    throw std::runtime_error("LV2 Atom port "+std::to_string(i)+" requires "+std::to_string(bytes)+
                        " bytes; per-port limit is "+std::to_string(limits.maxPortBytes));
                }
                port.kind=Kind::Atom;port.requestedBytes=static_cast<uint32_t>(bytes);
                port.midi=supports(plugin,source,LV2_MIDI__MidiEvent);port.time=supports(plugin,source,LV2_TIME__Position);
                port.patch=supports(plugin,source,LV2_PATCH__Message) || supports(plugin,source,LV2_PATCH__Set) || supports(plugin,source,LV2_PATCH__Get);
            }
            else if(!property(plugin,source,LV2_CORE__connectionOptional))throw std::runtime_error("Unsupported required LV2 port: "+text(label.get()));
        }
        // Required ports get the budget first regardless of port order. Optional
        // ports that do not fit stay disconnected. All allocation precedes instantiate.
        uint64_t totalBytes=0;
        for(bool optional : {false,true})for(auto& port:ports_)if(port.kind==Kind::Atom && port.optional==optional)
        {
            if(totalBytes+port.requestedBytes>limits.maxTotalBytes)
            {
                if(optional){port.kind=Kind::Ignored;continue;}
                throw std::runtime_error("LV2 Atom buffers require more than total per-instance limit of "+std::to_string(limits.maxTotalBytes)+" bytes");
            }
            totalBytes+=port.requestedBytes;
            port.atom.resize(port.requestedBytes/sizeof(uint64_t));
            sequenceSize_=std::max(sequenceSize_,static_cast<int32_t>(port.atomBytes()));
        }
        for(auto& port:ports_)if(port.kind==Kind::Atom && port.midi)port.midiIndex=port.input?noteInputs_++:noteOutputs_++;
        for(uint32_t i=0;i<ports_.size();++i)if(ports_[i].kind==Kind::Atom && ports_[i].patch)
        {
            if(!patch_){patch_=std::make_unique<lv2::Patch>(world_.get(),plugin,urids_,&map_);atomObject_=urids_(LV2_ATOM__Object);atomBlank_=urids_(LV2_ATOM__Blank);}
            if(ports_[i].input && patchInput_==UINT32_MAX)patchInput_=i;
        }
        instance_.reset(lilv_plugin_instantiate(plugin,rate,featurePointers_.data()));
        if(!instance_)throw std::runtime_error("LV2 instantiation failed: "+descriptor_.id);
        try
        {
            state_=static_cast<const LV2_State_Interface*>(lilv_instance_get_extension_data(instance_.get(),LV2_STATE__interface));
            if(state_ && (!state_->save || !state_->restore))throw std::runtime_error("Invalid LV2 state interface");
            const auto* worker=static_cast<const LV2_Worker_Interface*>(lilv_instance_get_extension_data(instance_.get(),LV2_WORKER__interface));
            auto workerFeature=uri(LV2_WORKER__schedule);
            if(worker)worker_.attach(lilv_instance_get_handle(instance_.get()),worker);
            else if(lilv_nodes_contains(required.get(),workerFeature.get()))throw std::runtime_error("Required LV2 Worker interface is missing");
            for(uint32_t i=0;i<ports_.size();++i)
            {
                auto& p=ports_[i];void* data=nullptr;
                if(p.kind==Kind::Audio)data=p.audio.data();else if(p.kind==Kind::Control)data=&p.control;else if(p.kind==Kind::Atom)data=p.atom.data();
                lilv_instance_connect_port(instance_.get(),i,data);
            }
            auto stateKey=uri(LV2_STATE__state);
            std::unique_ptr<LilvNodes,decltype(&lilv_nodes_free)> defaults(lilv_plugin_get_value(plugin,stateKey.get()),lilv_nodes_free);
            if(lilv_nodes_size(defaults.get()))
            {
                std::unique_ptr<LilvState,decltype(&lilv_state_free)> initial(lilv_state_new_from_world(world_.get(),&map_,lilv_plugin_get_uri(plugin)),lilv_state_free);
                if(!initial)throw std::runtime_error("Cannot read LV2 default state");
                restore(initial.get());
            }
            else if(auto feature=uri(LV2_STATE__loadDefaultState);lilv_nodes_contains(required.get(),feature.get()))throw std::runtime_error("Required LV2 default state is missing");
            else {lilv_instance_activate(instance_.get());active_=true;}
        }
        catch(...){worker_.finish();instance_.reset();throw;}
    }
    ~Lv2Host() override
    {
        worker_.finish(); // join/drain while the instance and its buffers still exist
        if(active_)lilv_instance_deactivate(instance_.get());
        instance_.reset(); // cleanup may still use connected buffers and host features
    }
    void service() override {if(patch_)errors_+=patch_->service();}
    std::vector<PropertyInfo> properties() const override{return patch_?patch_->properties():std::vector<PropertyInfo>{};}
    bool setProperty(const std::string& uri,const PropertyValue& value) override
    {return patch_ && patchInput_!=UINT32_MAX && patch_->request(uri,&value,ports_[patchInput_].atomBytes());}
    bool requestProperty(const std::string& uri) override
    {return patch_ && patchInput_!=UINT32_MAX && patch_->request(uri,nullptr,ports_[patchInput_].atomBytes());}
    std::optional<PropertyValue> propertyValue(const std::string& uri) const override{return patch_?patch_->value(uri):std::optional<PropertyValue>{};}
    bool supportsState() const noexcept override{return state_!=nullptr;}
    void saveState(const std::string& directory) override
    {
        if(!supportsState())throw std::runtime_error("LV2 plugin has no complete state interface");
        state_files::Bundle bundle(directory);Quiescent guard(*this);
        if(faulted_)throw std::runtime_error("Cannot save a failed LV2 instance");
        if(patch_ && patch_->pending())throw std::runtime_error("Process queued LV2 property requests before saving state");
        auto tail=tail_.load();const auto head=head_.load();
        while(tail!=head){apply(commands_[tail%commands_.size()]);++tail;}tail_=tail;
        lv2::StateBridge bridge(instance_.get(),state_);
        const auto dir=bundle.temporary.path.string();
        std::unique_ptr<LilvState,decltype(&lilv_state_free)> state(lilv_state_new_from_instance(plugin_,&bridge.proxy,&map_,scratch_.path.c_str(),dir.c_str(),dir.c_str(),dir.c_str(),getPort,this,LV2_STATE_IS_POD|LV2_STATE_IS_PORTABLE,featurePointers_.data()),lilv_state_free);
        if(!state || bridge.status!=LV2_STATE_SUCCESS)throw std::runtime_error("LV2 plugin state saving failed");
        if(lilv_state_save(world_.get(),&map_,&unmap_,state.get(),nullptr,dir.c_str(),"state.ttl"))throw std::runtime_error("Cannot write LV2 state");
        state_files::materialize(bundle.temporary.path);bundle.publish();
    }
    void loadState(const std::string& directory) override
    {
        const auto file=std::filesystem::canonical(directory)/"state.ttl";
        if(std::filesystem::file_size(file)>kick::clap::Host::maxStateBytes)throw std::runtime_error("LV2 state file exceeds size limit");
        std::unique_ptr<LilvState,decltype(&lilv_state_free)> state(lilv_state_new_from_file(world_.get(),&map_,nullptr,file.c_str()),lilv_state_free);
        if(!state)throw std::runtime_error("Cannot read LV2 state");
        restore(state.get());
    }
    std::vector<PresetInfo> presets() override
    {
        auto type=uri(LV2_PRESETS__Preset),label=uri("http://www.w3.org/2000/01/rdf-schema#label");
        std::unique_ptr<LilvNodes,decltype(&lilv_nodes_free)> related(lilv_plugin_get_related(plugin_,type.get()),lilv_nodes_free);
        std::vector<PresetInfo> result;
        LILV_FOREACH(nodes,i,related.get())
        {
            auto* node=lilv_nodes_get(related.get(),i);lilv_world_load_resource(world_.get(),node);
            auto name=own(lilv_world_get(world_.get(),node,label.get(),nullptr));result.push_back({text(node),text(name.get())});
        }
        return result;
    }
    void loadPreset(const std::string& id) override
    {
        const auto available=presets();if(std::none_of(available.begin(),available.end(),[&](const auto& p){return p.uri==id;}))throw std::invalid_argument("Unknown LV2 preset");
        auto node=uri(id.c_str());std::unique_ptr<LilvState,decltype(&lilv_state_free)> state(lilv_state_new_from_world(world_.get(),&map_,node.get()),lilv_state_free);
        if(!state)throw std::runtime_error("Cannot read LV2 preset");
        restore(state.get());
    }
    const kick::clap::Descriptor& descriptor() const noexcept override{return descriptor_;}
    const std::vector<Parameter>& parameters() const noexcept override{return parameters_;}
    const std::vector<PortInfo>& inputPorts() const noexcept override{return inputs_;}
    const std::vector<PortInfo>& outputPorts() const noexcept override{return outputs_;}
    uint32_t noteInputPorts() const noexcept override{return noteInputs_;}
    uint32_t noteOutputPorts() const noexcept override{return noteOutputs_;}
    double parameterValue(clap_id id) const noexcept override{return parameter(id)?values_[id].load():0;}
    std::string parameterText(clap_id,double value) const override{return std::to_string(value);}
    bool setParameter(clap_id id,double value) noexcept override
    {
        if(!writable(id)||!std::isfinite(value))return false;
        const auto* p=parameter(id);value=std::clamp(value,p->minimum,p->maximum);if(ports_[p->id].toggle)value=value>0?1:0;
        else if(p->flags&CLAP_PARAM_IS_STEPPED)value=std::clamp(std::trunc(value),p->minimum,p->maximum);
        clap_event_param_value_t e{};e.header={sizeof(e),0,CLAP_CORE_EVENT_SPACE_ID,CLAP_EVENT_PARAM_VALUE,0};
        e.param_id=id;e.note_id=-1;e.port_index=e.channel=e.key=-1;e.value=value;
        return queue(Event::from(e));
    }
    bool parameterGesture(clap_id id,bool) noexcept override{return writable(id);} // LV2 control ports have no gesture message
    bool popOutput(Event&) noexcept override{return false;} // output control values use atomic telemetry
    uint64_t outputEventDrops() const noexcept override{return 0;}
    const Event* outputEvents(uint32_t& count) const noexcept override{count=emittedCount_;return emitted_.data();}
    kick::clap::Statistics statistics() const noexcept override{return {blocks_.load(),drops_.load()+worker_.drops(),errors_.load()+worker_.errors(),lastUs_.load(),maxUs_.load(),deadlines_.load(),stateSilences_.load(),0};}
    bool process(const float* const* inputs,uint32_t ni,float* const* outputs,uint32_t no,uint32_t frames,const Event* events,uint32_t count,const clap_event_transport_t* t) noexcept override
    {
        emittedCount_=0;outputPanic_=false;
        auto silence=[&]{if(outputs)for(uint32_t c=0;c<no;++c)if(outputs[c])std::fill_n(outputs[c],frames,0.f);};
        if(!frames)return true;
        if(frames>maxFrames_ || (no && !outputs) || (count && !events) || count>maxEvents){silence();++errors_;return false;}
        auto releaseOutputs=[&]{if(noteOutputs_)emitted_[emittedCount_++]=Event::note(false,-1,0,0,-1,-1);};
        if(stateBusy_.load() || gate_.test_and_set(std::memory_order_acquire)){silence();releaseOutputs();resumePanic_=true;++stateSilences_;return true;}
        struct Release {std::atomic_flag& gate;~Release(){gate.clear(std::memory_order_release);}} release{gate_};
        if(faulted_){silence();releaseOutputs();++errors_;return false;}
        if(resumePanic_.exchange(false)){inputPanic_=true;releaseOutputs();}
        const auto began=std::chrono::steady_clock::now();
        uint32_t total=0;auto tail=tail_.load(std::memory_order_relaxed);const auto head=head_.load(std::memory_order_acquire);
        while(tail!=head && total<maxEvents){ordered_[total]={commands_[tail%commands_.size()],total};++total;++tail;}
        tail_.store(tail,std::memory_order_release);
        for(uint32_t i=0;i<count;++i)if(valid(events[i],frames)){ordered_[total]={events[i],total};++total;}else ++drops_;
        std::sort(ordered_.begin(),ordered_.begin()+total,[](const auto& a,const auto& b){return a.event.header()->time!=b.event.header()->time?a.event.header()->time<b.event.header()->time:a.order<b.order;});
        for(auto& p:ports_)if(p.kind==Kind::Audio)
        {
            if(p.input && inputs && p.channel<ni && inputs[p.channel])std::copy_n(inputs[p.channel],frames,p.audio.data());
            else std::fill_n(p.audio.data(),frames,0.f);
        }
        const bool releaseInputs=inputPanic_;inputPanic_=false;
        uint32_t next=0,start=0;
        while(start<frames)
        {
            // Control-port edits split runs at their exact sample; MIDI stays in the
            // atom sequence, with timestamps relative to this run's audio pointers.
            for(uint32_t i=next;i<total && ordered_[i].event.header()->time==start;++i)
                if(ordered_[i].event.header()->type==CLAP_EVENT_PARAM_VALUE)apply(ordered_[i].event);
            uint32_t end=frames;
            for(uint32_t i=next;i<total;++i)if(ordered_[i].event.header()->type==CLAP_EVENT_PARAM_VALUE && ordered_[i].event.header()->time>start){end=ordered_[i].event.header()->time;break;}
            for(uint32_t i=0;i<ports_.size();++i)
            {
                auto& p=ports_[i];
                if(p.kind==Kind::Audio)lilv_instance_connect_port(instance_.get(),i,p.audio.data()+start);
                if(p.kind==Kind::Atom)
                {
                    auto* seq=p.sequence();
                    seq->atom={p.input?static_cast<uint32_t>(sizeof(seq->body)):p.atomBytes()-static_cast<uint32_t>(sizeof(LV2_Atom)),p.input?sequence_:chunk_};
                    seq->body={0,0};if(p.input && p.time)transport(p,t,start);
                }
            }
            if(start==0 && releaseInputs)
                for(int c=0;c<16;++c)
                {sendMidi(-1,0,static_cast<uint8_t>(0xb0|c),120,0);sendMidi(-1,0,static_cast<uint8_t>(0xb0|c),123,0);}
            if(start==0 && patch_ && patchInput_!=UINT32_MAX)
            {
                const auto pending=patch_->pending();lv2::Patch::Packet packet;
                for(uint32_t i=0;i<pending && patch_->pop(packet);++i)
                {
                    const auto* atom=reinterpret_cast<const LV2_Atom*>(packet.bytes.data());
                    append(ports_[patchInput_],0,atom->type,atom+1,atom->size);
                }
            }
            while(next<total && ordered_[next].event.header()->time<end)
            {
                const auto& event=ordered_[next++].event;const auto type=event.header()->type;
                if(type!=CLAP_EVENT_PARAM_VALUE && type!=CLAP_EVENT_PARAM_GESTURE_BEGIN && type!=CLAP_EVENT_PARAM_GESTURE_END)musical(event,start);
            }
            lilv_instance_run(instance_.get(),end-start);
            worker_.emitResponses();worker_.endRun();
            for(auto& p:ports_)if(p.kind==Kind::Atom && !p.input)collect(p,start,end-start);
            start=end;
        }
        silence();
        for(const auto& p:ports_)if(p.kind==Kind::Audio && !p.input && p.channel<no && outputs[p.channel])std::copy_n(p.audio.data(),frames,outputs[p.channel]);
        for(const auto& p:parameters_)if(p.flags&CLAP_PARAM_IS_READONLY)
        {const auto value=ports_[p.id].control;if(std::isfinite(value))values_[p.id].store(value);else ++errors_;}
        if(outputPanic_)
        {
            const auto index=emittedCount_<emitted_.size()?emittedCount_++:emittedCount_-1;
            emitted_[index]=Event::note(false,-1,0,frames-1,-1,-1);
        }
        // Stable, bounded merge across multiple MIDI output ports.
        for(uint32_t i=1;i<emittedCount_;++i){auto e=emitted_[i];auto j=i;while(j && emitted_[j-1].header()->time>e.header()->time){emitted_[j]=emitted_[j-1];--j;}emitted_[j]=e;}
        const auto us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-began).count();
        lastUs_.store(us);if(us>maxUs_.load())maxUs_.store(us);if(mode_==ProcessingMode::Realtime && us>frames/rate_*1e6)++deadlines_;++blocks_;
        return true;
    }
};
}
std::shared_ptr<PluginHost> loadLv2Plugin(const std::string& path,const std::string& id,double rate,uint32_t frames,ProcessingMode mode,Lv2BufferLimits limits)
{return std::make_shared<Lv2Host>(path,id,rate,frames,mode,limits);}
}
