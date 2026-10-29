// SPDX-License-Identifier: AGPL-3.0-or-later
// Uses the normal format adapter. Run each invocation in its own timed process.
#include "oscairn/plugin_host.h"
#include "oscairn/transport.h"
#include "oscairn/wav_file.h"
#include <clap/ext/params.h>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <vector>

int main(int argc,char** argv)
{
    try
    {
        if(argc!=3 && argc!=10)return 2;
        const bool inspect=argc==3;
        const double rate=inspect?48000:std::stod(argv[4]);
        const uint32_t block=inspect?512:std::stoul(argv[5]);
        const auto mode=inspect || std::string(argv[6])=="offline"?oscairn::ProcessingMode::Offline:oscairn::ProcessingMode::Realtime;
        auto host=oscairn::loadPlugin(argv[1],argv[2],rate,block,mode);
        if(inspect)
        {
            std::cout<<"name="<<host->descriptor().name<<"\nuri="<<host->descriptor().id
                <<"\ninputs="<<host->inputPorts().size()<<"\noutputs="<<host->outputPorts().size()<<"\nnotes="<<host->noteInputPorts()<<'\n';
            for(const auto& p:host->parameters())std::cout<<"param="<<p.id<<'\t'<<p.name<<'\t'<<p.minimum<<'\t'<<p.maximum<<'\t'<<p.defaultValue<<'\t'<<p.flags<<'\n';
            return 0;
        }
        const bool held=std::string(argv[3])=="held";
        const bool synth=held || std::string(argv[3])=="synth";
        const int param=std::stoi(argv[7]);const double value=std::stod(argv[8]);
        if(param>=0 && !host->setParameter(param,value))throw std::runtime_error("parameter write rejected");
        const auto ni=host->inputPorts().size(),no=host->outputPorts().size();
        if(!no || no>8 || ni>8 || (synth && !host->noteInputPorts()))throw std::runtime_error("unexpected port inventory");
        std::vector<std::vector<float>> input(ni,std::vector<float>(block)),output(no,std::vector<float>(block));
        std::vector<const float*> in;std::vector<float*> out;
        for(auto& channel:input)in.push_back(channel.data());
        for(auto& channel:output)out.push_back(channel.data());
        const uint32_t total=static_cast<uint32_t>(rate*2);
        std::vector<float> rendered;rendered.reserve(total*no);
        double power=0,tail=0,difference=0,peak=0;
        oscairn::Transport transport(rate,120);
        for(uint32_t offset=0;offset<total;offset+=block)
        {
            const auto frames=std::min(block,total-offset);
            for(uint32_t c=0;c<ni;++c)for(uint32_t i=0;i<frames;++i)
            {
                const double time=(offset+i)/rate;
                input[c][i]=time<1.2?static_cast<float>((.2*std::sin(6.283185307179586*(c?330:220)*time)+.1*std::sin(6.283185307179586*1700*time))*(time<.6?1:.25)):0;
            }
            std::vector<kick::clap::Event> events;
            if(synth)
            {
                const double times[]{.1,.6,.8,1.1};
                for(int i=0;i<4;++i)
                {
                    if(held && i%2)continue;
                    const auto at=static_cast<uint32_t>(std::llround(times[i]*rate));
                    if(at>=offset && at<offset+frames)events.push_back(kick::clap::Event::note(i%2==0,i<2?60:67,.8,at-offset));
                }
            }
            const auto time=transport.current();
            if(!host->process(in.data(),ni,out.data(),no,frames,events.data(),events.size(),&time))throw std::runtime_error("process returned false");
            for(uint32_t i=0;i<frames;++i)for(uint32_t c=0;c<no;++c)
            {
                const double sample=out[c][i];if(!std::isfinite(sample))throw std::runtime_error("nonfinite audio");
                peak=std::max(peak,std::abs(sample));power+=sample*sample;
                if(offset+i>rate*1.8)tail+=sample*sample;
                if(ni)difference+=std::pow(sample-in[c%ni][i],2);
                rendered.push_back(sample);
            }
            transport.advance(frames);host->service();
        }
        const auto stats=host->statistics();
        if(peak<1e-7)throw std::runtime_error("silent output");
        if(stats.processErrors || stats.droppedEvents)throw std::runtime_error("host reported process errors or dropped events");
        oscairn::writeWavFile(argv[9],rendered,no,rate);
        std::cout<<std::setprecision(10)<<"RESULT peak="<<peak<<" rms="<<std::sqrt(power/rendered.size())
            <<" tail_rms="<<std::sqrt(tail/(rate*.2*no))<<" dry_difference_rms="<<std::sqrt(difference/rendered.size())
            <<" channels="<<no<<" blocks="<<stats.blocks<<'\n';
        return 0;
    }
    catch(const std::exception& e){std::cerr<<"PROBE_ERROR "<<e.what()<<'\n';return 1;}
}
