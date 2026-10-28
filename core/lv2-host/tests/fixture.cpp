// SPDX-License-Identifier: AGPL-3.0-or-later
#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <lv2/time/time.h>
#include <lv2/urid/urid.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

namespace
{
struct Fixture
{
    std::array<void*,9> ports{};
    bool effect=false;
    std::array<std::array<bool,128>,16> held{};
    std::array<std::array<float,128>,16> pressure{};
    LV2_URID midi,sequence,object,position,bpm,frame,beat;
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
LV2_Handle instantiate(const LV2_Descriptor* descriptor,double,const char*,const LV2_Feature* const* features)
{
    LV2_URID_Map* map=nullptr;
    for(auto f=features;*f;++f)if(!std::strcmp((*f)->URI,LV2_URID__map))map=static_cast<LV2_URID_Map*>((*f)->data);
    if(!map)return nullptr;
    auto* s=new Fixture;s->effect=std::string_view(descriptor->URI).ends_with("effect");
    auto id=[&](const char* uri){return map->map(map->handle,uri);};
    s->midi=id(LV2_MIDI__MidiEvent);s->sequence=id(LV2_ATOM__Sequence);s->object=id(LV2_ATOM__Object);s->position=id(LV2_TIME__Position);
    s->bpm=id(LV2_TIME__beatsPerMinute);s->frame=id(LV2_TIME__frame);s->beat=id(LV2_TIME__beat);return s;
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
        for(;position<end;++position)out[position]=(s.effect?in[position]:level)*gain;
    };
    LV2_ATOM_SEQUENCE_FOREACH(events,e)
    {
        renderUntil(std::min(frames,static_cast<uint32_t>(e->time.frames)));
        if(e->body.type==s.midi)
        {
            s.message(reinterpret_cast<const uint8_t*>(e+1));
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
        }
    }
    renderUntil(frames);
    *static_cast<float*>(s.ports[5])=s.tempo;*static_cast<float*>(s.ports[6])=s.frameValue;*static_cast<float*>(s.ports[7])=s.beatValue;
    const auto mode=*static_cast<float*>(s.ports[8]);
    if(mode==1)echo->atom.size=0xffffffff;
    else if(mode==2){echo->atom.size=sizeof(echo->body)+sizeof(LV2_Atom_Event);auto* e=lv2_atom_sequence_begin(&echo->body);e->body={0xffffffff,s.midi};}
    else if(mode==3 && echo->atom.size>sizeof(echo->body))lv2_atom_sequence_begin(&echo->body)->time.frames=frames;
}
const LV2_Descriptor synth{"urn:oscairn:test:synth",instantiate,[](LV2_Handle h,uint32_t i,void* p){if(i<9)static_cast<Fixture*>(h)->ports[i]=p;},nullptr,run,nullptr,[](LV2_Handle h){delete static_cast<Fixture*>(h);},nullptr};
const LV2_Descriptor effect{"urn:oscairn:test:effect",instantiate,synth.connect_port,nullptr,run,nullptr,synth.cleanup,nullptr};
}
LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index){return index==0?&synth:index==1?&effect:nullptr;}
