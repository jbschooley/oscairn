// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "oscairn/graph.h"
#include <algorithm>
#include <charconv>
#include <stdexcept>
#include <string_view>

namespace oscairn::host
{
struct AudioConfiguration
{
    std::string inputDevice,outputDevice;
    std::vector<uint32_t> inputs,outputs{0,1}; // logical order -> physical channel (zero based)
    bool explicitOutputs=false;
    static std::vector<uint32_t> parseChannels(std::string_view text)
    {
        if(text=="none")return {};
        std::vector<uint32_t> result;
        while(!text.empty())
        {
            const auto comma=text.find(',');const auto item=text.substr(0,comma);uint32_t n=0;
            const auto parsed=std::from_chars(item.data(),item.data()+item.size(),n);
            if(parsed.ec!=std::errc{} || parsed.ptr!=item.data()+item.size() || n>=Graph::maxAudioChannels ||
               std::find(result.begin(),result.end(),n)!=result.end())throw std::invalid_argument("Audio channels must be unique zero-based indices 0..255");
            result.push_back(n);
            if(comma==std::string_view::npos)return result;
            text.remove_prefix(comma+1);
        }
        throw std::invalid_argument("Empty or incomplete audio channel list (use none to disable inputs)");
    }
};
// Independent of JUCE so channel ordering and isolation can be tested without hardware.
// Open a contiguous prefix of physical device channels: callback indices then agree
// with physical indices even on backends which compact active-channel arrays.
class ChannelMap
{
    AudioConfiguration config_;
    uint32_t maxFrames_;
    std::vector<float> inputBuffer_,outputBuffer_;
    std::vector<const float*> inputs_;
    std::vector<float*> outputs_;
    static void validate(const std::vector<uint32_t>& channels)
    {
        std::vector<uint32_t> seen;
        for(auto c:channels){if(c>=Graph::maxAudioChannels || std::find(seen.begin(),seen.end(),c)!=seen.end())throw std::invalid_argument("Invalid or duplicate device channel");seen.push_back(c);}
    }
public:
    explicit ChannelMap(AudioConfiguration config,uint32_t maxFrames):config_(std::move(config)),maxFrames_(maxFrames)
    {
        validate(config_.inputs);validate(config_.outputs);
        if(config_.outputs.empty() || !maxFrames)throw std::invalid_argument("Audio device needs at least one output and a positive block size");
        if(!config_.explicitOutputs && config_.outputs!=std::vector<uint32_t>{0,1})throw std::invalid_argument("Custom outputs require explicit selection");
        inputBuffer_.resize(config_.inputs.size()*maxFrames);outputBuffer_.resize(config_.outputs.size()*maxFrames);
        for(size_t c=0;c<config_.inputs.size();++c)inputs_.push_back(inputBuffer_.data()+c*maxFrames);
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
    // All buffers are preallocated. Copy input first to support duplex aliasing.
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
            for(size_t c=0;c<inputs_.size();++c)std::copy_n(in[config_.inputs[c]]+offset,n,inputBuffer_.data()+c*maxFrames_);
            for(auto* channel:outputs_)std::fill_n(channel,n,0.f);
            const bool rendered=render(inputs_.data(),static_cast<uint32_t>(inputs_.size()),outputs_.data(),static_cast<uint32_t>(outputs_.size()),n,offset);
            ok=ok && rendered;
            for(uint32_t c=0;c<no;++c)if(out[c])std::fill_n(out[c]+offset,n,0.f);
            // Graph failures can leave valid audio on unrelated buses. Preserve it;
            // pre-clearing above prevents stale data if the renderer writes nothing.
            for(size_t c=0;c<outputs_.size();++c)if(config_.outputs[c]<no)std::copy_n(outputs_[c],n,out[config_.outputs[c]]+offset);
            offset+=n;
        }
        return ok;
    }
};
}
