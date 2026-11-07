// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/plugin_host.h"
#include "StateFiles.h"
#include <kick_clap/Discovery.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <locale>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <thread>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#endif
#ifdef __linux__
#include <sys/mman.h>
#include <sys/resource.h>
#endif
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <stdexcept>
namespace oscairn
{
#ifdef OSCAIRN_LV2
    std::shared_ptr<PluginHost> loadLv2Plugin(const std::string&,const std::string&,double,uint32_t,ProcessingMode,Lv2BufferLimits);
#endif
    namespace
    {
        // A CLAP preset as a URI: clap-preset:KIND:LOCATION#LOAD_KEY, the parts percent-encoded.
        std::string encodePart(const std::string& s)
        {
            static const char* hex="0123456789ABCDEF";std::string out;
            for(unsigned char c:s){if(c<0x20 || c=='%' || c==':' || c=='#' || c==0x7f){out+='%';out+=hex[c>>4];out+=hex[c&15];}else out+=char(c);}
            return out;
        }
        std::string decodePart(const std::string& s)
        {
            std::string out;
            for(size_t i=0;i<s.size();++i){
                if(s[i]!='%'){out+=s[i];continue;}
                if(i+2>=s.size() || !std::isxdigit(static_cast<unsigned char>(s[i+1])) || !std::isxdigit(static_cast<unsigned char>(s[i+2])))throw std::invalid_argument("Invalid preset URI");
                out+=char(std::stoi(s.substr(i+1,2),nullptr,16));i+=2;
            }
            return out;
        }
        std::string presetUri(const kick::clap::Preset& p){return "clap-preset:"+std::to_string(p.kind)+":"+encodePart(p.location)+"#"+encodePart(p.loadKey);}
        kick::clap::Preset presetFrom(const std::string& uri)
        {
            const std::string scheme="clap-preset:";
            const auto colon=uri.find(':',scheme.size()),hash=uri.find('#');
            if(uri.compare(0,scheme.size(),scheme)!=0 || colon==std::string::npos || hash==std::string::npos || hash<colon)throw std::invalid_argument("Not a CLAP preset URI");
            kick::clap::Preset p;const auto kind=uri.substr(scheme.size(),colon-scheme.size());
            if(kind!="0" && kind!="1")throw std::invalid_argument("Invalid preset location kind");
            p.kind=uint32_t(kind[0]-'0');p.location=decodePart(uri.substr(colon+1,hash-colon-1));p.loadKey=decodePart(uri.substr(hash+1));
            return p;
        }

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
            std::optional<oscairn_memory_t> declaredMemory() const override {oscairn_memory_t m{};if(host.declaredMemory(m))return m;return {};}
            void setMemoryRoom(uint64_t bytes) override {host.setMemoryRoom(bytes);}
            uint32_t soundContextCapacity() const noexcept override {return host.soundContextCapacity();}
            std::vector<oscairn_sound_context_info_t> soundContexts() const override {return host.soundContexts();}
            uint32_t prepareSound(const std::string& name,const std::vector<oscairn_sound_parameter_t>& p,const std::string& state,const std::string& preset,const std::string& sfz,const std::string& library,const std::string& directory) override {
                if(!preset.empty()){if(!state.empty() || !p.empty() || !sfz.empty() || !library.empty() || !directory.empty())throw std::invalid_argument("A native sound from a preset takes nothing else");return host.prepareSound(name,presetFrom(preset));}
                if(!state.empty())throw std::invalid_argument("Native contexts accept parameter, SFZ or preset patches only");return host.prepareSound(name,p,sfz,library,directory);}
            bool selectSound(uint32_t id,int16_t channel) override {return host.selectSound(id,channel);}
            bool discardSound(uint32_t id,bool) override {return host.discardSound(id);}
            bool preparesSnapshots() const noexcept override {return host.preparesSoundSnapshots();}
            bool ringsOut() const noexcept override {return host.ringsOut();}
            bool chokeRetiring() override {return host.chokeRetiringSounds();}
            bool voiceBudget() const noexcept override {return host.voiceBudget();}
            bool honorsVolume() const noexcept override {return host.honorsVolume();}
            void setVoiceLimits(uint32_t limit,const oscairn_voice_budget_context_t* contexts,uint32_t count) noexcept override {host.setVoiceLimits(limit,contexts,count);}
            uint32_t voiceReport(double& voiceCost,const oscairn_voice_budget_note_t*& notes,uint32_t& count) const noexcept override {return host.voiceReport(voiceCost,notes,count);}
            std::array<uint32_t,16> prepareSnapshot(const std::string& directory) override {
                // What saveSnapshot wrote for a native engine: "state 1" and its state bundle.
                const auto path=std::filesystem::path(directory);
                const auto head=state_files::read(path/"snapshot",64);
                if(std::string(head.begin(),head.end())!="state 1\n")throw std::runtime_error("Not a native sound snapshot");
                const auto id=state_files::read(path/"state"/"plugin.id",65536);
                if(std::string(id.begin(),id.end())!=host.descriptor().id)throw std::runtime_error("State belongs to a different CLAP plugin");
                return host.prepareSoundSnapshot(state_files::read(path/"state"/"state.clap",kick::clap::Host::maxStateBytes));
            }
            bool savesSounds() const noexcept override {return host.savesSounds();}
            void saveSound(uint32_t id,const std::string& directory) override {
                state_files::Bundle bundle(directory);
                const auto bytes=host.saveSoundState(id);const auto& plugin=host.descriptor().id;
                std::filesystem::create_directory(bundle.temporary.path/"state");
                state_files::write(bundle.temporary.path/"state"/"plugin.id",plugin.data(),plugin.size());
                state_files::write(bundle.temporary.path/"state"/"state.clap",bytes.data(),bytes.size());
                const std::string head="state 1\n";state_files::write(bundle.temporary.path/"snapshot",head.data(),head.size());
                bundle.publish();
            }
            std::vector<oscairn_sound_parameter_t> soundValues(uint32_t id) const override {return host.soundValues(id);}
            bool supportsState() const noexcept override {return host.supportsState();}
            bool hasWindow() const override {return host.hasGui();}
            void showWindow(bool show,const std::string& title) override {if(!show)host.closeGui();else if(!host.showGui(title))throw std::runtime_error(host.error());}
            // RFC 0005: through the isolated scan helper where one is configured; without one, this
            // plugin already runs in this process, and so does its index (within the index's budget).
            std::vector<PresetInfo> presets() override {
                if(!placementPolicy().scan.empty())return scanPluginPresets(host.descriptor().path,host.descriptor().id);
                std::vector<PresetInfo> r;for(const auto& p:host.presets())r.push_back({presetUri(p),p.name});return r;
            }
            void loadPreset(const std::string& uri) override {if(!host.loadPreset(presetFrom(uri)))throw std::runtime_error(host.error());}
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
            bool reset() override { host.reset(); return true; }
            const kick::clap::Descriptor& descriptor() const noexcept override { return host.descriptor(); }
            const std::vector<kick::clap::Parameter>& parameters() const noexcept override { return host.parameters(); }
            const std::vector<kick::clap::Port>& inputPorts() const noexcept override { return host.inputPorts(); }
            const std::vector<kick::clap::Port>& outputPorts() const noexcept override { return host.outputPorts(); }
            bool usesClapNotes(uint32_t port) const noexcept override { return host.usesClapNotes(port); }
            void panic() noexcept override { host.panic(); }
            uint32_t noteInputPorts() const noexcept override { return host.noteInputPorts(); }
            uint32_t noteOutputPorts() const noexcept override { return host.noteOutputPorts(); }
            double parameterValue(clap_id id) const noexcept override { return host.parameterValue(id); }
            uint64_t parameterReports() const noexcept override { return host.parameterReports(); }
            uint64_t parameterRevision() const noexcept override { return host.metadataRevision(); }
            std::string parameterText(clap_id id, double value) const override { return host.parameterText(id,value); }
            bool setParameter(clap_id id, double value) noexcept override { return host.setParameter(id,value); }
            bool parameterGesture(clap_id id, bool begin) noexcept override { return host.parameterGesture(id,begin); }
            bool popOutput(kick::clap::Event& event) noexcept override { return host.popOutput(event); }
            uint64_t outputEventDrops() const noexcept override { return host.outputEventDrops(); }
            const kick::clap::Event* outputEvents(uint32_t& count) const noexcept override { return host.outputEvents(count); }
            kick::clap::Statistics statistics() const noexcept override { return host.statistics(); }
            bool asleep() const noexcept override { return host.asleep(); }
            bool wasReset() const noexcept override { return host.wasReset(); }
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
    namespace
    {
        PlacementPolicy& policy()
        {
            static PlacementPolicy p=[]{
                PlacementPolicy initial;if(const char* w=std::getenv("OSCAIRN_PLUGIN_WORKER"))initial.worker=w;
                if(const char* mb=std::getenv("OSCAIRN_WORKER_MEMORY_MB"))initial.memoryLimit=std::strtoull(mb,nullptr,10)<<20;
                if(const char* s=std::getenv("OSCAIRN_PLUGIN_SCAN"))initial.scan=s;
                return initial;}();
            return p;
        }
    }
    void setPlacementPolicy(PlacementPolicy p) {policy()=std::move(p);}
    const PlacementPolicy& placementPolicy() {return policy();}
    uint64_t residentBytes(long pid)
    {
        if(pid == 0) pid = long(getpid());
#ifdef __APPLE__
        proc_taskinfo info{};
        if(proc_pidinfo(int(pid), PROC_PIDTASKINFO, 0, &info, sizeof(info)) != int(sizeof(info))) return 0;
        return info.pti_resident_size;
#else
        std::ifstream statm("/proc/" + std::to_string(pid) + "/statm");
        uint64_t pages = 0, resident = 0;
        if(!(statm >> pages >> resident)) return 0;
        return resident * uint64_t(sysconf(_SC_PAGESIZE));
#endif
    }

    std::string applyMemoryPolicy(int oomScoreAdj)
    {
        const char* level=std::getenv("OSCAIRN_RELIABILITY");
        if(!level || (std::string(level)!="instrument" && std::string(level)!="pass-through"))return {};
#ifdef __linux__
        std::string missed;
        // Faulted in when allocated, during preparation, never during a block; code pages stay resident too.
        // MCL_FUTURE past a finite RLIMIT_MEMLOCK does not fail here: every later mmap past the limit
        // fails, so a plugin load or a recorder ring throws instead of a degraded lock. Lock the
        // future only where the limit cannot be reached (the device image; RFC 0028).
        rlimit memlock{};
        const bool unlimited = getrlimit(RLIMIT_MEMLOCK, &memlock) == 0 && memlock.rlim_cur == RLIM_INFINITY;
        const int flags = MCL_CURRENT | (unlimited ? MCL_FUTURE : 0);
        if(mlockall(flags) != 0) missed += std::string("memory not locked (") + std::strerror(errno) + "; RLIMIT_MEMLOCK too low?)";
        else if(!unlimited) missed += "future memory not locked: RLIMIT_MEMLOCK is " + std::to_string(uint64_t(memlock.rlim_cur));
        // Lowering it below 0 needs CAP_SYS_RESOURCE; raising it does not.
        std::ofstream adj("/proc/self/oom_score_adj");adj<<oomScoreAdj<<std::flush;
        if(!adj)missed+=std::string(missed.empty()?"":"; ")+"oom_score_adj not set to "+std::to_string(oomScoreAdj);
        return missed;
#else
        (void)oomScoreAdj;
        return "memory locking and the out-of-memory order are Linux only";
#endif
    }

    std::vector<PresetInfo> scanPluginPresets(const std::string& path, const std::string& pluginId)
    {
        // One listing per binary as it is now (its path and newest modification), the failures of a
        // helper that ran included: a hung provider costs one helper timeout, not one per caller or
        // refresh. A helper that could not be started (no pipe, no process, no thread) is not kept.
        // shortcut: preset files added to a plugin's folders show once its binary changes or the engine restarts; key on the folders too if that matters.
        struct Listing { kick::clap::ScanResult result; std::string failure; bool started = true; };
        // Never destroyed: a listing's thread may still be running when the process exits.
        static auto& mutex = *new std::mutex;
        static auto& listings = *new std::map<std::pair<std::string, std::filesystem::file_time_type>, std::shared_future<Listing>>;
        const auto helper = placementPolicy().scan;
        if(helper.empty()) throw std::runtime_error("Presets not listed: no plugin scan helper is configured");
        std::error_code e;
        auto stamp = std::filesystem::last_write_time(path, e);
        if(std::filesystem::is_directory(path, e)) // a bundle: its binary may change inside it
            for(const auto& f : std::filesystem::recursive_directory_iterator(path, e)) stamp = std::max(stamp, f.last_write_time(e));
        const auto key = std::pair{path, stamp};
        std::promise<Listing> promise;
        std::shared_future<Listing> listing;
        bool run = false;
        {
            std::lock_guard lock(mutex);
            auto& slot = listings[key];
            if(!slot.valid()) { slot = promise.get_future().share(); run = true; }
            listing = slot;
        }
        if(run)
            // Off the caller's thread, which is the engine's control thread (presets.list) or the
            // desktop's message thread: neither waits on a provider. The scan's own 15 s bounds the
            // helper, and a hung provider dies with it.
            try
            {
                std::thread([helper, key, promise = std::move(promise)]() mutable {
                    Listing l;
                    const bool ran = kick::clap::indexPresetsWithHelper(helper, key.first, 15000, l.result, l.failure);
                    if(!ran && l.failure.empty()) l.failure = "Plugin scan failed";
                    l.started = ran || !l.failure.starts_with("Cannot "); // runHelper's "Cannot ...": the helper never started
                    promise.set_value(std::move(l));
                }).detach();
            }
            catch(const std::system_error& error)
            {
                { std::lock_guard lock(mutex); listings.erase(key); }
                throw std::runtime_error(std::string("Presets not listed: cannot start the listing: ") + error.what());
            }
        if(listing.wait_for(std::chrono::seconds(0)) != std::future_status::ready) throw std::runtime_error("Presets are being listed; ask again");
        const auto& l = listing.get();
        if(!l.started) { std::lock_guard lock(mutex); listings.erase(key); } // said once, then asked again
        if(!l.failure.empty()) throw std::runtime_error("Presets not listed: " + l.failure);
        std::vector<PresetInfo> presets;
        for(const auto& plugin : l.result.plugins)
            if(plugin.id == pluginId) for(const auto& preset : plugin.presets) presets.push_back({presetUri(preset), preset.name});
        return presets;
    }

    namespace { std::atomic<uint64_t> instancesLoaded{0}; }
    uint64_t pluginInstancesLoaded() noexcept { return instancesLoaded.load(std::memory_order_relaxed); }
    std::shared_ptr<PluginHost> loadPlugin(const std::string& path,const std::string& id,double rate,uint32_t frames,ProcessingMode mode,Lv2BufferLimits lv2Buffers,uint64_t memoryRoom)
    {
        (void)lv2Buffers; // LV2-only resource policy.
        instancesLoaded.fetch_add(1,std::memory_order_relaxed);
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
        const auto& policy=placementPolicy();
        const auto placement=policy.plugins.find(id);
        if(!policy.worker.empty() && (placement==policy.plugins.end()?policy.standard:placement->second)==Placement::Worker)
            return loadPluginInWorker(policy.worker,path,id,rate,frames,mode,policy.memoryLimit,memoryRoom);
        const auto scan=kick::clap::inspect(path);
        const auto found=std::find_if(scan.plugins.begin(),scan.plugins.end(),[&](const auto& p){return id.empty() || p.id==id;});
        if(found==scan.plugins.end())
            throw std::runtime_error(scan.failures.empty()?"No matching CLAP plugin in "+path:scan.failures.front().message);
        auto host=std::make_shared<ClapHost>();
        host->host.setMemoryRoom(memoryRoom);
        host->host.setOfflineRendering(mode==ProcessingMode::Offline);
        if(!host->host.load(*found,rate,frames))throw std::runtime_error("Cannot load "+found->id+": "+host->host.error());
        host->host.setMemoryRoom(0);
        host->host.lockAudioLayout();
        // The first service (the plugin's init callback, its parameter flush) holds the gate; run it
        // before any audio thread can see the plugin, or its first block is refused as a control silence.
        host->service();
        return host;
    }
}
