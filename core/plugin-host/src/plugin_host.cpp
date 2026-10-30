// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/plugin_host.h"
#include "StateFiles.h"
#include <kick_clap/Discovery.h>
#include <algorithm>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <locale>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
namespace oscairn
{
#ifdef OSCAIRN_LV2
    std::shared_ptr<PluginHost> loadLv2Plugin(const std::string&,const std::string&,double,uint32_t,ProcessingMode,Lv2BufferLimits);
#endif
    namespace
    {
        class ClapHost final : public PluginHost
        {
        public:
            kick::clap::Host host;
            std::shared_ptr<PluginHost> createEffectContext() override {
                auto child=std::make_shared<ClapHost>();
                if(!child->host.loadEffectContext(host))throw std::runtime_error(child->host.error());
                child->host.lockAudioLayout();return child;
            }
            std::optional<oscairn_effect_resources_t> effectResources() const override {
                oscairn_effect_resources_t r{};if(host.effectResources(r))return r;return {};
            }
            uint32_t soundContextCapacity() const noexcept override {return host.soundContextCapacity();}
            std::vector<oscairn_sound_context_info_t> soundContexts() const override {return host.soundContexts();}
            uint32_t prepareSound(const std::string& name,const std::vector<oscairn_sound_parameter_t>& p,const std::string& state,const std::string& preset,const std::string& sfz,const std::string& library,const std::string& directory) override {if(!state.empty() || !preset.empty())throw std::invalid_argument("Native contexts accept parameter or SFZ patches only");return host.prepareSound(name,p,sfz,library,directory);}
            bool selectSound(uint32_t id,int16_t channel) override {return host.selectSound(id,channel);}
            bool discardSound(uint32_t id,bool) override {return host.discardSound(id);}
            bool supportsState() const noexcept override {return host.supportsState();}
            void setAssets(kick::clap::AssetResolver* assets) noexcept override {host.setAssets(assets);}
            std::vector<kick::clap::AssetReference> assetDependencies() const override {return host.assetDependencies();}
            void saveSnapshot(const std::string& directory) override {
                if(soundContextCapacity() && !host.supportsSoundSnapshot())throw std::runtime_error("Native engine has no complete sound-context snapshot interface");
                PluginHost::saveSnapshot(directory);
            }
            void saveState(const std::string& directory) override
            {
                state_files::Bundle bundle(directory);std::vector<uint8_t> bytes;
                if(!host.saveState(bytes))throw std::runtime_error(host.error());
                const auto& id=host.descriptor().id;
                state_files::write(bundle.temporary.path/"plugin.id",id.data(),id.size());
                state_files::write(bundle.temporary.path/"state.clap",bytes.data(),bytes.size());
                bundle.publish();
            }
            void loadState(const std::string& directory) override
            {
                const auto path=std::filesystem::canonical(directory);
                const auto id=state_files::read(path/"plugin.id",65536);
                if(std::string(id.begin(),id.end())!=host.descriptor().id)throw std::runtime_error("State belongs to a different CLAP plugin");
                const auto bytes=state_files::read(path/"state.clap",kick::clap::Host::maxStateBytes);
                if(!host.loadState(bytes))throw std::runtime_error("CLAP state restore failed: "+host.error());
            }
            void service() override { host.service(); }
            const kick::clap::Descriptor& descriptor() const noexcept override { return host.descriptor(); }
            const std::vector<kick::clap::Parameter>& parameters() const noexcept override { return host.parameters(); }
            const std::vector<kick::clap::Port>& inputPorts() const noexcept override { return host.inputPorts(); }
            const std::vector<kick::clap::Port>& outputPorts() const noexcept override { return host.outputPorts(); }
            bool usesClapNotes(uint32_t port) const noexcept override { return host.usesClapNotes(port); }
            void panic() noexcept override { host.panic(); }
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
    void PluginHost::saveSnapshot(const std::string& directory)
    {
        if(soundContextCapacity() && !supportsState())throw std::runtime_error("Plugin cannot snapshot all native sounds");
        state_files::Bundle bundle(directory);
        std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(17);
        if(supportsState()){saveState((bundle.temporary.path/"state").string());out<<"state 1\n";}
        else {
            // shortcut: parameter-only restore fits one queue; add offline flushing for larger plugins.
            if(std::count_if(parameters().begin(),parameters().end(),[](const auto& p){return !(p.flags&CLAP_PARAM_IS_READONLY);})>kick::clap::Host::maxEvents)
                throw std::runtime_error("Parameter-only snapshots support at most 2048 writable parameters");
            out<<"parameters 1\n";
            for(const auto& p:parameters())if(!(p.flags&CLAP_PARAM_IS_READONLY)) {
                const double value=parameterValue(p.id);if(!std::isfinite(value))throw std::runtime_error("Nonfinite plugin parameter");
                out<<p.id<<' '<<value<<'\n';
            }
        }
        const auto text=out.str();state_files::write(bundle.temporary.path/"snapshot",text.data(),text.size());bundle.publish();
    }
    void PluginHost::loadSnapshot(const std::string& directory)
    {
        const auto data=state_files::read(std::filesystem::path(directory)/"snapshot",2*1024*1024);
        std::istringstream input(std::string(data.begin(),data.end()));input.imbue(std::locale::classic());
        std::string kind;unsigned version;if(!(input>>kind>>version) || version!=1)throw std::runtime_error("Invalid plugin snapshot version");
        if(kind=="state") {input>>std::ws;if(!input.eof())throw std::runtime_error("Trailing snapshot data");loadState((std::filesystem::path(directory)/"state").string());return;}
        if(kind!="parameters" || soundContextCapacity())throw std::runtime_error("Unsupported parameter-only snapshot");
        std::vector<oscairn_sound_parameter_t> values;uint32_t id;double value;
        while(input>>std::ws && !input.eof()) {
            if(values.size()>=kick::clap::Host::maxEvents || !(input>>id>>value) || !std::isfinite(value))throw std::runtime_error("Invalid parameter snapshot");
            const auto& all=parameters();const auto p=std::find_if(all.begin(),all.end(),[&](const auto& p){return p.id==id;});
            if(p==all.end() || (p->flags&CLAP_PARAM_IS_READONLY) || value<p->minimum || value>p->maximum || ((p->flags&CLAP_PARAM_IS_STEPPED) && value!=std::trunc(value)) ||
               std::any_of(values.begin(),values.end(),[&](const auto& v){return v.id==id;}))throw std::runtime_error("Snapshot parameter does not match plugin");
            values.push_back({id,value});
        }
        if(values.size()!=static_cast<size_t>(std::count_if(parameters().begin(),parameters().end(),[](const auto& p){return !(p.flags&CLAP_PARAM_IS_READONLY);})))throw std::runtime_error("Incomplete parameter snapshot");
        for(const auto& v:values)if(!setParameter(v.id,v.value))throw std::runtime_error("Snapshot parameter queue full");
    }

    std::vector<uint32_t> PluginHost::mainAudioPorts(bool input) const
    {
        const auto& ports=input?inputPorts():outputPorts();
        const auto main=std::find_if(ports.begin(),ports.end(),[](const auto& p){return p.flags&CLAP_AUDIO_PORT_IS_MAIN;});
        if(main!=ports.end())return {main->id};
        if(ports.size()==1)return {ports.front().id};
        throw std::invalid_argument("No unambiguous main audio bus; specify port/channel routes");
    }
    std::shared_ptr<PluginHost> loadPlugin(const std::string& path,const std::string& id,double rate,uint32_t frames,ProcessingMode mode,Lv2BufferLimits lv2Buffers)
    {
        (void)lv2Buffers; // LV2-only resource policy.
        (void)mode; // CLAP behavior is unchanged; LV2 uses the mode for Worker.
        if(path.find('\0')!=std::string::npos || id.find('\0')!=std::string::npos)
            throw std::invalid_argument("Plugin path and ID must not contain NUL bytes");
        auto source=std::filesystem::path(path).lexically_normal();
        if(source.filename().empty())source=source.parent_path();
        if(source.extension()==".lv2")
        {
#ifdef OSCAIRN_LV2
            return loadLv2Plugin(path,id,rate,frames,mode,lv2Buffers);
#else
            throw std::runtime_error("LV2 support is disabled; build with -DOSCAIRN_LV2=ON");
#endif
        }
        if(const char* worker=std::getenv("OSCAIRN_PLUGIN_WORKER"); worker && *worker)
            if(auto host=loadPluginInWorker(worker,path,id,rate,frames,mode))return host;
        const auto scan=kick::clap::inspect(path);
        const auto found=std::find_if(scan.plugins.begin(),scan.plugins.end(),[&](const auto& p){return id.empty() || p.id==id;});
        if(found==scan.plugins.end())
            throw std::runtime_error(scan.failures.empty()?"No matching CLAP plugin in "+path:scan.failures.front().message);
        auto host=std::make_shared<ClapHost>();
        if(!host->host.load(*found,rate,frames))throw std::runtime_error("Cannot load "+found->id+": "+host->host.error());
        host->host.lockAudioLayout();
        return host;
    }
}
