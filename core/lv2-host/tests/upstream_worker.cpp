// SPDX-License-Identifier: AGPL-3.0-or-later
// Direct ABI harness for the unmodified upstream eg-sampler Worker implementation.
// State/patch setup here is test-only, not support advertised by Oscairn's adapter.
#include "../Worker.h"
#include "../Urids.h"
#include "oscairn/wav_file.h"
#include <lv2/atom/forge.h>
#include <lv2/midi/midi.h>
#include <lv2/patch/patch.h>
#include <dlfcn.h>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace
{
void check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Sampler
{
    oscairn::lv2::Urids urids;
    oscairn::lv2::Worker worker;
    LV2_URID_Map map{&urids,oscairn::lv2::Urids::map};
    LV2_Feature mapFeature{LV2_URID__map,&map},workerFeature{LV2_WORKER__schedule,worker.feature()};
    const LV2_Descriptor* descriptor;
    LV2_Handle instance=nullptr;
    alignas(8) std::array<uint8_t,16384> input{},output{};
    std::array<float,512> audio{};
    LV2_Atom_Forge forge{};
    LV2_Atom_Forge_Frame sequence{};
    Sampler(const LV2_Descriptor* d,bool offline):worker(offline),descriptor(d)
    {
        const LV2_Feature* features[]{&mapFeature,&workerFeature,nullptr};
        instance=d->instantiate(d,48000,"",features);check(instance,"upstream sampler instantiates");
        worker.attach(instance,static_cast<const LV2_Worker_Interface*>(d->extension_data(LV2_WORKER__interface)));
        d->connect_port(instance,0,input.data());d->connect_port(instance,1,output.data());d->connect_port(instance,2,audio.data());
        lv2_atom_forge_init(&forge,&map);d->activate(instance);
    }
    ~Sampler(){worker.finish();descriptor->deactivate(instance);descriptor->cleanup(instance);}
    void begin()
    {
        lv2_atom_forge_set_buffer(&forge,input.data(),input.size());lv2_atom_forge_sequence_head(&forge,&sequence,0);
    }
    void load(const std::string& path,uint32_t time)
    {
        LV2_Atom_Forge_Frame object;
        lv2_atom_forge_frame_time(&forge,time);lv2_atom_forge_object(&forge,&object,0,urids(LV2_PATCH__Set));
        lv2_atom_forge_key(&forge,urids(LV2_PATCH__property));lv2_atom_forge_urid(&forge,urids("http://lv2plug.in/plugins/eg-sampler#sample"));
        lv2_atom_forge_key(&forge,urids(LV2_PATCH__value));lv2_atom_forge_path(&forge,path.c_str(),path.size());lv2_atom_forge_pop(&forge,&object);
    }
    void note(uint32_t time)
    {
        const uint8_t data[]{0x90,60,127};lv2_atom_forge_frame_time(&forge,time);
        lv2_atom_forge_atom(&forge,sizeof(data),urids(LV2_MIDI__MidiEvent));lv2_atom_forge_write(&forge,data,sizeof(data));
    }
    void run(uint32_t frames)
    {
        lv2_atom_forge_pop(&forge,&sequence);
        auto* atom=reinterpret_cast<LV2_Atom*>(output.data());*atom={static_cast<uint32_t>(output.size()-sizeof(LV2_Atom)),urids(LV2_ATOM__Chunk)};
        descriptor->run(instance,frames);worker.emitResponses();worker.endRun();
    }
};
}
int main(int argc,char** argv)
{
    try
    {
        if(argc!=3)return 2;
        const auto close=[](void* handle){dlclose(handle);};
        std::unique_ptr<void,decltype(close)> library(dlopen(argv[1],RTLD_NOW|RTLD_LOCAL),close);
        check(bool(library),"open upstream sampler module");
        const auto entry=reinterpret_cast<const LV2_Descriptor*(*)(uint32_t)>(dlsym(library.get(),"lv2_descriptor"));
        check(entry,"upstream descriptor entry");const auto* descriptor=entry(0);
        const auto a=(std::filesystem::path(argv[2])/"worker-a.wav").string(),b=(std::filesystem::path(argv[2])/"worker-b.wav").string();
        oscairn::writeWavFile(a,std::vector<float>(4096,.25f),1,48000);oscairn::writeWavFile(b,std::vector<float>(4096,.75f),1,48000);
        std::vector<float> reference;
        for(uint32_t block:{37u,512u,37u})
        {
            Sampler sampler(descriptor,true);std::vector<float> rendered;
            for(uint32_t offset=0;offset<2048;offset+=block)
            {
                const auto frames=std::min(block,2048-offset);sampler.begin();
                for(const auto sample:{7u,700u})if(sample>=offset && sample<offset+frames){sampler.load(sample==7?a:b,sample-offset);sampler.note(sample-offset);}
                sampler.run(frames);rendered.insert(rendered.end(),sampler.audio.begin(),sampler.audio.begin()+frames);
            }
            for(uint32_t i=0;i<rendered.size();++i)check(rendered[i]==(i<7?0.f:i<700?.25f:.75f),"upstream file load and replacement affect the exact requested sample");
            if(reference.empty())reference=rendered;else check(reference==rendered,"upstream Worker renders repeatably across block sizes");
            check(!sampler.worker.errors() && !sampler.worker.drops(),"upstream worker has no errors or overflow");
        }
        {
            Sampler sampler(descriptor,false);sampler.begin();sampler.load(a,0);sampler.run(64);
            bool heard=false;const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
            while(!heard && std::chrono::steady_clock::now()<deadline)
            {
                sampler.begin();sampler.note(0);sampler.run(64);heard=sampler.audio[0]==.25f;std::this_thread::yield();
            }
            check(heard,"upstream sampler receives asynchronously loaded sample");
            sampler.begin();sampler.load(b,0);sampler.run(64); // destroy with sample replacement/free work pending
        }
        std::cout<<"ok: unmodified eg-sampler Worker, live load, exact offline replacement and shutdown\n";return 0;
    }
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
