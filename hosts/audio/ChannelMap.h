// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "oscairn/graph.h"
#include "AudioConfiguration.h"
#include <algorithm>
#include <stdexcept>

namespace oscairn::host
{
// Independent of JUCE so channel ordering and isolation can be tested without hardware.
// Open a contiguous prefix of physical device channels: callback indices then agree
// with physical indices even on backends which compact active-channel arrays.
class ChannelMap
{
    AudioConfiguration config_;
    uint32_t maxFrames_;
    std::vector<float> outputBuffer_;
    std::vector<bool> mappedOutputs_;
    std::vector<const float*> inputs_;
    std::vector<float*> outputs_;
public:
    explicit ChannelMap(AudioConfiguration config,uint32_t maxFrames):config_(std::move(config)),maxFrames_(maxFrames)
    {
        config_.validate();
        if(!maxFrames)throw std::invalid_argument("Audio device needs a positive block size");
        inputs_.resize(config_.inputs.size());outputBuffer_.resize(config_.outputs.size()*maxFrames);
        mappedOutputs_.resize(Graph::maxAudioChannels);
        for(auto c:config_.outputs)mappedOutputs_[c]=true;
        for(size_t c=0;c<config_.outputs.size();++c)outputs_.push_back(outputBuffer_.data()+c*maxFrames);
    }
    const AudioConfiguration& configuration()const{return config_;}
    static uint32_t required(const std::vector<uint32_t>& channels){return channels.empty()?0:*std::max_element(channels.begin(),channels.end())+1;}
    AudioLayout layout()const
    {
        AudioLayout result;
        if(!config_.inputs.empty())result.inputs={{0,"Device inputs",static_cast<uint32_t>(config_.inputs.size()),0}};
        result.outputs={{0,"Device outputs",static_cast<uint32_t>(config_.outputs.size()),0}};return result;
    }
    // Render must overwrite every logical output, including silence on failure.
    // Capture points into the device buffers; playback is staged separately until
    // render returns, preserving duplex aliasing without a second capture copy.
    template<class Render> bool process(const float* const* in,uint32_t ni,float* const* out,uint32_t no,uint32_t frames,Render&& render) noexcept
    {
        const bool monoDefault=!config_.explicitOutputs && no==1;
        auto silence=[&]{if(out)for(uint32_t c=0;c<no;++c)if(out[c])std::fill_n(out[c],frames,0.f);};
        if(!out || !no || !frames || ni>Graph::maxAudioChannels || no>Graph::maxAudioChannels || (ni && !in) ||
           ni<required(config_.inputs) || (!monoDefault && no<required(config_.outputs))) {silence();return false;}
        for(auto c:config_.inputs)if(!in[c]){silence();return false;}
        for(auto c:config_.outputs)if(c<no && !out[c]){silence();return false;}
        bool ok=true;
        for(uint32_t offset=0;offset<frames;)
        {
            const auto n=std::min(maxFrames_,frames-offset);
            for(size_t c=0;c<inputs_.size();++c)inputs_[c]=in[config_.inputs[c]]+offset;
            const bool rendered=render(inputs_.data(),static_cast<uint32_t>(inputs_.size()),outputs_.data(),static_cast<uint32_t>(outputs_.size()),n,offset);
            ok=ok && rendered;
            for(uint32_t c=0;c<no;++c)if(!mappedOutputs_[c] && out[c])std::fill_n(out[c]+offset,n,0.f);
            // Mapped channels are overwritten below; only unused hardware needs clearing.
            for(size_t c=0;c<outputs_.size();++c)if(config_.outputs[c]<no)std::copy_n(outputs_[c],n,out[config_.outputs[c]]+offset);
            offset+=n;
        }
        return ok;
    }
};
}
