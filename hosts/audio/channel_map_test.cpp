// SPDX-License-Identifier: AGPL-3.0-or-later
#include "ChannelMap.h"
#include "AllocationGuard.h"
#include <array>
#include <iostream>
using namespace oscairn::host;
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(){try{
    for(auto text:{"", "1,", "1,1", "-1", "256", "1.5", "1,,2"}){bool bad=false;try{AudioConfiguration::parseChannels(text);}catch(const std::invalid_argument&){bad=true;}check(bad,"invalid device map rejected");}
    check(AudioConfiguration::parseChannels("4,1,0")==std::vector<uint32_t>({4,1,0}),"map preserves selected order");
    ChannelMap map({{}, {}, {4,1}, {5,2}, true},37);
    std::array<std::array<float,100>,6> buffers;std::array<const float*,6> in;std::array<float*,6> out;
    for(size_t i=0;i<6;++i){buffers[i].fill(float(i+1));in[i]=buffers[i].data();out[i]=buffers[i].data();}
    uint32_t calls=0;
    auto render=[&](const float* const* input,uint32_t ni,float* const* output,uint32_t no,uint32_t frames,uint32_t offset){
        if(ni!=2 || no!=2 || frames>37 || offset!=calls*37 || input[0]!=in[4]+offset || input[1]!=in[1]+offset)return false;++calls;
        for(uint32_t i=0;i<frames;++i){output[0][i]=input[0][i];output[1][i]=input[1][i];}return true;};
    forbidAllocation=true;bool ok=map.process(in.data(),6,out.data(),6,100,render);forbidAllocation=false;
    check(ok && calls==3,"mapped rendering splits callback without allocation");
    for(size_t c=0;c<6;++c)for(auto value:buffers[c])check(value==(c==5?5.f:c==2?2.f:0.f),"sparse physical outputs isolated and aliased inputs preserved");
    calls=0;check(!map.process(in.data(),6,out.data(),2,100,render) && !calls,"missing cue hardware rejected before rendering");
    for(auto value:buffers[0])check(value==0,"missing cue is never folded into master");
    auto partial=[](const float* const*,uint32_t,float* const* output,uint32_t,uint32_t n,uint32_t){std::fill_n(output[0],n,.5f);std::fill_n(output[1],n,0.f);return false;};
    check(!map.process(in.data(),6,out.data(),6,100,partial),"renderer error is reported");
    for(size_t c=0;c<6;++c)for(auto v:buffers[c])check(v==(c==5?.5f:0.f),"renderer error preserves valid independent bus and preserves renderer-provided silence");
    ChannelMap normal({},37);calls=0;
    auto stereo=[](const float* const*,uint32_t,float* const* output,uint32_t,uint32_t n,uint32_t){std::fill_n(output[0],n,.25f);std::fill_n(output[1],n,.75f);return true;};
    check(normal.process(nullptr,0,out.data(),1,100,stereo),"default retains mono-device support");for(auto value:buffers[0])check(value==.25f,"default mono policy keeps left");
    std::cout<<"ok: physical channel order, sparse maps, aliasing, callback splitting and unavailable-output isolation\n";return 0;
}catch(const std::exception& e){forbidAllocation=false;std::cerr<<e.what()<<'\n';return 1;}}
