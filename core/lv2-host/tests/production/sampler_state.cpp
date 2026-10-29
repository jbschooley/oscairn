// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/plugin_host.h"
#include "oscairn/wav_file.h"
#include "../../../plugin-host/src/StateFiles.h"
#include <array>
#include <cmath>
#include <iostream>

using namespace oscairn;
namespace fs=std::filesystem;
static void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(int argc,char** argv)
{
    try
    {
        if(argc!=2)return 2;
        state_files::TemporaryDirectory temp;
        const auto sample=(temp.path/"sample with spaces.wav").string();
        std::vector<float> wave(48000);for(size_t i=0;i<wave.size();++i)wave[i]=.5f*std::sin(i*6.283185307179586*220/48000);
        writeWavFile(sample,wave,1,48000);
        auto make=[&]{return loadPlugin(argv[1],"http://samplv1.sourceforge.net/lv2",48000,256,ProcessingMode::Offline);};
        auto host=make();check(host->supportsState(),"samplv1 state support");
        const std::string property="http://samplv1.sourceforge.net/lv2#P101_SAMPLE_FILE";
        std::array<float,256> left{},right{};float* outputs[]{left.data(),right.data()};
        auto run=[&](PluginHost& h,bool note)
        {
            const auto on=kick::clap::Event::note(true,60,1);
            check(h.process(nullptr,0,outputs,2,left.size(),note?&on:nullptr,note?1:0,nullptr),"samplv1 render");h.service();
            double energy=0;for(size_t i=0;i<left.size();++i){check(std::isfinite(left[i]) && std::isfinite(right[i]),"finite sampler output");energy+=left[i]*left[i]+right[i]*right[i];}return energy;
        };
        check(host->setProperty(property,FilePath{sample}),"samplv1 accepts sample property");run(*host,false);
        check(host->requestProperty(property),"samplv1 patch Get accepted");run(*host,false);
        auto confirmed=host->propertyValue(property);
        check(confirmed && std::get<FilePath>(*confirmed).value==sample,"samplv1 confirms sample path");
        // A real control-port value must survive along with the opaque sample state.
        const auto& parameter=host->parameters().front();
        check(host->setParameter(parameter.id,parameter.maximum),"queued sampler control change");
        const auto state=temp.path/"state";host->saveState(state.string());
        const double savedValue=host->parameterValue(parameter.id);
        double before=run(*host,true);for(int i=0;i<12;++i)before+=run(*host,false);
        check(before>1e-5,"samplv1 produces sample audio");
        const auto moved=temp.path/"moved";fs::rename(state,moved);fs::remove(sample);
        for(const auto& f:fs::recursive_directory_iterator(moved))check(!f.is_symlink(),"production state owns sample assets");
        auto restored=make();restored->loadState(moved.string());
        check(restored->parameterValue(parameter.id)==savedValue,"samplv1 control-port state restored");
        double after=run(*restored,true);for(int i=0;i<12;++i)after+=run(*restored,false);
        check(after>1e-5,"samplv1 relocated sample produces audio without original file");
        check(!host->statistics().processErrors && !host->statistics().droppedEvents && !restored->statistics().processErrors && !restored->statistics().droppedEvents,"production sampler no errors or dropped events");
        std::cout<<"ok: samplv1 patch Set/Get, audio, queued controls, portable state; energy "<<before<<" -> "<<after<<'\n';return 0;
    }
    catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
