// SPDX-License-Identifier: AGPL-3.0-or-later
#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <lv2/time/time.h>
#include <lv2/urid/urid.h>
#include <lv2/options/options.h>
#include <lv2/parameters/parameters.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/worker/worker.h>
#include <lv2/state/state.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

namespace
{
struct Fixture
{
    std::array<void*,19> ports{};
    bool effect=false,audit=false,worker=false;
    LV2_Worker_Schedule* schedule=nullptr;
    float workValue=1,completed=0,endRuns=0;
    int32_t sequenceSize=0;
    std::array<std::array<bool,128>,16> held{};
    std::array<std::array<float,128>,16> pressure{};
    LV2_URID midi,sequence,object,position,bpm,frame,beat,bar,barBeat,beatUnit,beatsPerBar,speed,stateKey,atomFloat;
    float tempo=0,frameValue=0,beatValue=0;
    void message(const uint8_t* bytes)
    {
        const int c=bytes[0]&15,k=bytes[1];
        switch(bytes[0]&0xf0)
        {
        case 0x90:held[c][k]=bytes[2]!=0;break;
        case 0x80:held[c][k]=false;break;
        case 0xa0:pressure[c][k]=bytes[2]/127.f;break;
        case 0xd0:for(auto& p:pressure[c])p=bytes[1]/127.f;break;
        case 0xb0:if(k==120 || k==123)held[c].fill(false);break;
        }
    }
};
LV2_Handle instantiate(const LV2_Descriptor* descriptor,double rate,const char*,const LV2_Feature* const* features)
{
    LV2_URID_Map* map=nullptr;
    for(auto f=features;*f;++f)if(!std::strcmp((*f)->URI,LV2_URID__map))map=static_cast<LV2_URID_Map*>((*f)->data);
    if(!map)return nullptr;
    auto* s=new Fixture;s->audit=std::string_view(descriptor->URI).ends_with("audit");s->effect=std::string_view(descriptor->URI).ends_with("effect");
    s->worker=std::string_view(descriptor->URI)=="urn:oscairn:test:worker";
    for(auto f=features;*f;++f)if(!std::strcmp((*f)->URI,LV2_WORKER__schedule))s->schedule=static_cast<LV2_Worker_Schedule*>((*f)->data);
    if(s->worker && !s->schedule){delete s;return nullptr;}
    auto id=[&](const char* uri){return map->map(map->handle,uri);};
    for(auto f=features;*f;++f)if(!std::strcmp((*f)->URI,LV2_OPTIONS__options))
        for(auto o=static_cast<const LV2_Options_Option*>((*f)->data);o->key;++o)
            if(o->key==id(LV2_BUF_SIZE__sequenceSize) && o->type==id(LV2_ATOM__Int) && o->size==sizeof(int32_t))
                s->sequenceSize=*static_cast<const int32_t*>(o->value);
    s->stateKey=id("urn:oscairn:test:value");s->atomFloat=id(LV2_ATOM__Float);
    s->midi=id(LV2_MIDI__MidiEvent);s->sequence=id(LV2_ATOM__Sequence);s->object=id(LV2_ATOM__Object);s->position=id(LV2_TIME__Position);
    s->bpm=id(LV2_TIME__beatsPerMinute);s->frame=id(LV2_TIME__frame);s->beat=id(LV2_TIME__beat);
    s->bar=id(LV2_TIME__bar);s->barBeat=id(LV2_TIME__barBeat);s->beatUnit=id(LV2_TIME__beatUnit);
    s->beatsPerBar=id(LV2_TIME__beatsPerBar);s->speed=id(LV2_TIME__speed);
    if(s->audit)
    {
        bool sampleRate=false,maxBlock=false;
        for(auto f=features;*f;++f)if(!std::strcmp((*f)->URI,LV2_OPTIONS__options))
            for(auto o=static_cast<const LV2_Options_Option*>((*f)->data);o->key;++o)
            {
                if(o->key==id(LV2_PARAMETERS__sampleRate))sampleRate=o->type==id(LV2_ATOM__Float) && *static_cast<const float*>(o->value)==rate;
                if(o->key==id(LV2_BUF_SIZE__maxBlockLength))maxBlock=o->type==id(LV2_ATOM__Int) && *static_cast<const int32_t*>(o->value)==512;
            }
        if(!sampleRate || !maxBlock){delete s;return nullptr;}
    }
    return s;
}
void run(LV2_Handle instance,uint32_t frames)
{
    auto& s=*static_cast<Fixture*>(instance);
    const float gain=*static_cast<float*>(s.ports[0]);
    const auto* in=static_cast<float*>(s.ports[1]);auto* out=static_cast<float*>(s.ports[2]);
    auto* events=static_cast<LV2_Atom_Sequence*>(s.ports[3]);auto* echo=static_cast<LV2_Atom_Sequence*>(s.ports[4]);
    const auto capacity=echo->atom.size;echo->atom.type=s.sequence;echo->body={0,0};lv2_atom_sequence_clear(echo);
    uint32_t position=0;
    auto renderUntil=[&](uint32_t end){
        float level=0;for(size_t c=0;c<16;++c)for(size_t k=0;k<128;++k)if(s.held[c][k])level+=1+s.pressure[c][k];
        for(;position<end;++position)out[position]=(s.effect?in[position]:level)*gain*s.workValue;
    };
    LV2_ATOM_SEQUENCE_FOREACH(events,e)
    {
        renderUntil(std::min(frames,static_cast<uint32_t>(e->time.frames)));
        if(e->body.type==s.midi)
        {
            s.message(reinterpret_cast<const uint8_t*>(e+1));
            if(s.worker)
            {
                const float value=2;
                s.schedule->schedule_work(s.schedule->handle,sizeof(value),&value);
            }
            lv2_atom_sequence_append_event(echo,capacity,e);
        }
        else if(e->body.type==s.object)
        {
            const auto* obj=reinterpret_cast<const LV2_Atom_Object*>(&e->body);
            const LV2_Atom *bpm=nullptr,*frame=nullptr,*beat=nullptr;
            lv2_atom_object_get(obj,s.bpm,&bpm,s.frame,&frame,s.beat,&beat,0);
            if(bpm)s.tempo=reinterpret_cast<const LV2_Atom_Float*>(bpm)->body;
            if(frame)s.frameValue=static_cast<float>(reinterpret_cast<const LV2_Atom_Long*>(frame)->body);
            if(beat)s.beatValue=static_cast<float>(reinterpret_cast<const LV2_Atom_Double*>(beat)->body);
            if(s.audit)
            {
                const LV2_Atom *bar=nullptr,*barBeat=nullptr,*unit=nullptr,*perBar=nullptr,*speed=nullptr;
                lv2_atom_object_get(obj,s.bar,&bar,s.barBeat,&barBeat,s.beatUnit,&unit,s.beatsPerBar,&perBar,s.speed,&speed,0);
                if(bar)*static_cast<float*>(s.ports[13])=static_cast<float>(reinterpret_cast<const LV2_Atom_Long*>(bar)->body);
                if(barBeat)*static_cast<float*>(s.ports[14])=reinterpret_cast<const LV2_Atom_Float*>(barBeat)->body;
                if(unit)*static_cast<float*>(s.ports[15])=static_cast<float>(reinterpret_cast<const LV2_Atom_Int*>(unit)->body);
                if(perBar)*static_cast<float*>(s.ports[16])=reinterpret_cast<const LV2_Atom_Float*>(perBar)->body;
                if(speed)*static_cast<float*>(s.ports[17])=reinterpret_cast<const LV2_Atom_Float*>(speed)->body;
            }
        }
    }
    renderUntil(frames);
    *static_cast<float*>(s.ports[5])=s.tempo;*static_cast<float*>(s.ports[6])=s.frameValue;*static_cast<float*>(s.ports[7])=s.beatValue;
    if(s.audit && frames>=4)
    {
        out[0]=*static_cast<float*>(s.ports[9]);out[1]=*static_cast<float*>(s.ports[10]);out[2]=*static_cast<float*>(s.ports[11]);
        out[3]=(s.ports[12] || s.ports[18])?1.f:0.f;
    }
    const auto mode=*static_cast<float*>(s.ports[8]);
    if(mode==1)echo->atom.size=0xffffffff;
    else if(mode==2){echo->atom.size=sizeof(echo->body)+sizeof(LV2_Atom_Event);auto* e=lv2_atom_sequence_begin(&echo->body);e->body={0xffffffff,s.midi};}
    else if(mode==3 && echo->atom.size>sizeof(echo->body))lv2_atom_sequence_begin(&echo->body)->time.frames=frames;
}
const LV2_Descriptor synth{"urn:oscairn:test:synth",instantiate,[](LV2_Handle h,uint32_t i,void* p){if(i<19)static_cast<Fixture*>(h)->ports[i]=p;},nullptr,run,nullptr,[](LV2_Handle h){delete static_cast<Fixture*>(h);},nullptr};
const LV2_Descriptor effect{"urn:oscairn:test:effect",instantiate,synth.connect_port,nullptr,run,nullptr,synth.cleanup,nullptr};
LV2_Worker_Status work(LV2_Handle,LV2_Worker_Respond_Function respond,void* handle,uint32_t size,const void* data)
{return respond(handle,size,data);}
LV2_Worker_Status response(LV2_Handle h,uint32_t size,const void* data)
{
    auto& s=*static_cast<Fixture*>(h);
    if(size!=sizeof(float))return LV2_WORKER_ERR_UNKNOWN;
    std::memcpy(&s.workValue,data,size);++s.completed;
    *static_cast<float*>(s.ports[5])=s.completed;return LV2_WORKER_SUCCESS;
}
LV2_Worker_Status endRun(LV2_Handle h)
{
    auto& s=*static_cast<Fixture*>(h);*static_cast<float*>(s.ports[5])=s.completed;*static_cast<float*>(s.ports[6])=++s.endRuns;return LV2_WORKER_SUCCESS;
}
const LV2_Worker_Interface workerInterface{work,response,endRun};
const LV2_Descriptor worker{"urn:oscairn:test:worker",instantiate,synth.connect_port,nullptr,run,nullptr,synth.cleanup,
    [](const char* uri)->const void*{return !std::strcmp(uri,LV2_WORKER__interface)?&workerInterface:nullptr;}};
const LV2_Descriptor brokenWorker{"urn:oscairn:test:bad-worker",instantiate,synth.connect_port,nullptr,run,nullptr,synth.cleanup,nullptr};
const LV2_Descriptor audit{"urn:oscairn:test:audit",instantiate,synth.connect_port,nullptr,run,nullptr,synth.cleanup,nullptr};
void largeRun(LV2_Handle instance,uint32_t frames)
{
    auto& s=*static_cast<Fixture*>(instance);
    auto* out=static_cast<float*>(s.ports[2]);
    auto* echo=static_cast<LV2_Atom_Sequence*>(s.ports[4]);
    const auto capacity=echo->atom.size;
    std::fill_n(out,frames,0.f);
    if(frames>=3){out[0]=capacity+sizeof(LV2_Atom);out[1]=s.sequenceSize;out[2]=s.ports[0]?1:0;}
    echo->atom.type=s.sequence;echo->body={0,0};lv2_atom_sequence_clear(echo);
    // An opaque notification fills all but one MIDI event's space. A host must
    // validate/skip the large notification and still recover the trailing note.
    constexpr uint32_t payload=131376;
    if(capacity<sizeof(echo->body)+sizeof(LV2_Atom_Event)+payload+24)return;
    auto* e=lv2_atom_sequence_begin(&echo->body);
    e->time.frames=0;e->body={payload,s.object};std::memset(e+1,0,payload);
    echo->atom.size=sizeof(echo->body)+sizeof(LV2_Atom_Event)+payload;
    alignas(8) uint8_t bytes[24]{};
    auto* note=reinterpret_cast<LV2_Atom_Event*>(bytes);note->time.frames=0;note->body={3,s.midi};
    bytes[sizeof(LV2_Atom_Event)]=0x90;bytes[sizeof(LV2_Atom_Event)+1]=60;bytes[sizeof(LV2_Atom_Event)+2]=100;
    lv2_atom_sequence_append_event(echo,capacity,note);
    const auto bad=*static_cast<float*>(s.ports[1]);
    if(bad==1)echo->atom.size=capacity+1; // Just beyond this port's actual allocation.
    if(bad==2)e->body.size=0xffffffff; // Overflow-sized event within a valid outer size.
}
LV2_State_Status save(LV2_Handle h,LV2_State_Store_Function store,LV2_State_Handle context,uint32_t,const LV2_Feature* const*)
{
    auto& s=*static_cast<Fixture*>(h);
    if(*static_cast<float*>(s.ports[8])==3)return LV2_STATE_ERR_UNKNOWN;
    return store(context,s.stateKey,&s.workValue,sizeof(float),s.atomFloat,LV2_STATE_IS_POD|LV2_STATE_IS_PORTABLE);
}
LV2_State_Status restore(LV2_Handle h,LV2_State_Retrieve_Function retrieve,LV2_State_Handle context,uint32_t,const LV2_Feature* const*)
{
    auto& s=*static_cast<Fixture*>(h);size_t size=0;uint32_t type=0,flags=0;
    auto* value=retrieve(context,s.stateKey,&size,&type,&flags);
    if(!value){s.workValue=1;return LV2_STATE_SUCCESS;}
    if(size!=sizeof(float) || type!=s.atomFloat)return LV2_STATE_ERR_BAD_TYPE;
    std::memcpy(&s.workValue,value,size);
    return s.workValue<0?LV2_STATE_ERR_UNKNOWN:LV2_STATE_SUCCESS;
}
const LV2_State_Interface stateInterface{save,restore};
const LV2_Descriptor state{"urn:oscairn:test:state",instantiate,synth.connect_port,nullptr,run,nullptr,synth.cleanup,
    [](const char* uri)->const void*{return !std::strcmp(uri,LV2_STATE__interface)?&stateInterface:nullptr;}};
const LV2_Descriptor large{"urn:oscairn:test:large",instantiate,synth.connect_port,nullptr,largeRun,nullptr,synth.cleanup,nullptr};
}
LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index){return index==0?&synth:index==1?&effect:index==2?&audit:index==3?&worker:index==4?&brokenWorker:index==5?&large:index==6?&state:nullptr;}
