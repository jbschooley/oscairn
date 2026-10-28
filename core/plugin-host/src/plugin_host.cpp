// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/plugin_host.h"
#include <kick_clap/Discovery.h>
#include <algorithm>
#include <filesystem>
#include <stdexcept>
namespace oscairn
{
#ifdef OSCAIRN_LV2
    std::shared_ptr<PluginHost> loadLv2Plugin(const std::string&,const std::string&,double,uint32_t);
#endif
    namespace
    {
        class ClapHost final : public PluginHost
        {
        public:
            kick::clap::Host host;
            void service() override { host.service(); }
            const kick::clap::Descriptor& descriptor() const noexcept override { return host.descriptor(); }
            const std::vector<kick::clap::Parameter>& parameters() const noexcept override { return host.parameters(); }
            const std::vector<kick::clap::Port>& inputPorts() const noexcept override { return host.inputPorts(); }
            const std::vector<kick::clap::Port>& outputPorts() const noexcept override { return host.outputPorts(); }
            uint32_t noteInputPorts() const noexcept override { return host.noteInputPorts(); }
            uint32_t noteOutputPorts() const noexcept override { return host.noteOutputPorts(); }
            double parameterValue(clap_id id) const noexcept override { return host.parameterValue(id); }
            std::string parameterText(clap_id id, double value) const override { return host.parameterText(id,value); }
            bool setParameter(clap_id id, double value) noexcept override { return host.setParameter(id,value); }
            bool parameterGesture(clap_id id, bool begin) noexcept override { return host.parameterGesture(id,begin); }
            bool popOutput(kick::clap::Event& event) noexcept override { return host.popOutput(event); }
            uint64_t outputEventDrops() const noexcept override { return host.outputEventDrops(); }
            const kick::clap::Event* outputEvents(uint32_t& count) const noexcept override { return host.outputEvents(count); }
            kick::clap::Statistics statistics() const noexcept override { return host.statistics(); }
            bool process(const float* const* inputs, uint32_t ni, float* const* outputs, uint32_t no, uint32_t frames, const kick::clap::Event* events, uint32_t count, const clap_event_transport_t* transport) noexcept override { return host.process(inputs,ni,outputs,no,frames,events,count,transport); }
        };
    }
    std::shared_ptr<PluginHost> loadPlugin(const std::string& path,const std::string& id,double rate,uint32_t frames)
    {
        if(path.find('\0')!=std::string::npos || id.find('\0')!=std::string::npos)
            throw std::invalid_argument("Plugin path and ID must not contain NUL bytes");
        auto source=std::filesystem::path(path).lexically_normal();
        if(source.filename().empty())source=source.parent_path();
        if(source.extension()==".lv2")
        {
#ifdef OSCAIRN_LV2
            return loadLv2Plugin(path,id,rate,frames);
#else
            throw std::runtime_error("LV2 support is disabled; build with -DOSCAIRN_LV2=ON");
#endif
        }
        const auto scan=kick::clap::inspect(path);
        const auto found=std::find_if(scan.plugins.begin(),scan.plugins.end(),[&](const auto& p){return id.empty() || p.id==id;});
        if(found==scan.plugins.end())
            throw std::runtime_error(scan.failures.empty()?"No matching CLAP plugin in "+path:scan.failures.front().message);
        auto host=std::make_shared<ClapHost>();
        if(!host->host.load(*found,rate,frames))throw std::runtime_error("Cannot load "+found->id+": "+host->host.error());
        return host;
    }
}
