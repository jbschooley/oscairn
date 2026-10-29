// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
namespace oscairn::host
{
struct AudioConfiguration
{
    std::string inputDevice,outputDevice;
    std::vector<uint32_t> inputs,outputs{0,1}; // logical order -> physical channel (zero based)
    bool explicitOutputs=false;
    static bool isOption(std::string_view name)
    {return name=="--audio-inputs" || name=="--audio-outputs" || name=="--input-device" || name=="--output-device";}
    void setOption(std::string_view name,const std::string& value)
    {
        if(value.empty() || value.find('\0')!=std::string::npos)throw std::invalid_argument("Empty or invalid audio option");
        if(name=="--audio-inputs")inputs=parseChannels(value);
        else if(name=="--audio-outputs"){outputs=parseChannels(value);explicitOutputs=true;}
        else if(name=="--input-device")inputDevice=value;
        else if(name=="--output-device")outputDevice=value;
        else throw std::invalid_argument("Unknown audio option");
    }
    void validate() const
    {
        for(const auto* list:{&inputs,&outputs})
        {
            std::vector<uint32_t> seen;
            for(auto c:*list){if(c>=256 || std::find(seen.begin(),seen.end(),c)!=seen.end())throw std::invalid_argument("Invalid or duplicate device channel");seen.push_back(c);}
        }
        if(outputs.empty())throw std::invalid_argument("Audio device needs at least one output");
        if(!explicitOutputs && outputs!=std::vector<uint32_t>{0,1})throw std::invalid_argument("Custom outputs require explicit selection");
        if(inputDevice.find('\0')!=std::string::npos || outputDevice.find('\0')!=std::string::npos)throw std::invalid_argument("Invalid audio device name");
    }
    std::vector<std::string> arguments() const
    {
        validate();
        auto list=[](const auto& channels){std::string result;for(auto c:channels){if(!result.empty())result+=',';result+=std::to_string(c);}return result;};
        std::vector<std::string> args;
        if(!inputDevice.empty()){args.push_back("--input-device");args.push_back(inputDevice);}
        if(!outputDevice.empty()){args.push_back("--output-device");args.push_back(outputDevice);}
        if(!inputs.empty()){args.push_back("--audio-inputs");args.push_back(list(inputs));}
        if(explicitOutputs){args.push_back("--audio-outputs");args.push_back(list(outputs));}
        return args;
    }
    static std::vector<uint32_t> parseChannels(std::string_view text)
    {
        if(text=="none")return {};
        std::vector<uint32_t> result;
        while(!text.empty())
        {
            const auto comma=text.find(',');const auto item=text.substr(0,comma);uint32_t n=0;
            const auto parsed=std::from_chars(item.data(),item.data()+item.size(),n);
            if(parsed.ec!=std::errc{} || parsed.ptr!=item.data()+item.size() || n>=256 ||
               std::find(result.begin(),result.end(),n)!=result.end())throw std::invalid_argument("Audio channels must be unique zero-based indices 0..255");
            result.push_back(n);
            if(comma==std::string_view::npos)return result;
            text.remove_prefix(comma+1);
        }
        throw std::invalid_argument("Empty or incomplete audio channel list (use none to disable inputs)");
    }
};
}
