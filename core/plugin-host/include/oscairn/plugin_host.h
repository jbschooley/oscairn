// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <kick_clap/Host.h>
#include <array>
#include <map>
#include <optional>
#include <stdexcept>
#include <variant>

namespace oscairn
{
    // Selected before instantiation; Offline permits synchronous non-RT work.
    enum class ProcessingMode { Realtime, Offline };

    // Per LV2 instance, chosen before loading; no allocation/resizing in process().
    // Limits count aligned bytes including Atom headers, across input/output ports.
    struct Lv2BufferLimits
    {
        uint32_t defaultBytes = 16 * 1024;
        uint32_t maxPortBytes = 1024 * 1024;
        uint32_t maxTotalBytes = 4 * 1024 * 1024;
    };

    struct FilePath { std::string value; bool operator==(const FilePath&) const = default; };
    using PropertyValue = std::variant<bool, int32_t, int64_t, float, double, std::string, FilePath>;
    enum class PropertyType { Unknown, Bool, Int, Long, Float, Double, String, Path };
    struct PropertyInfo { std::string uri, name; PropertyType type=PropertyType::Unknown; bool readable=false, writable=false; };
    struct PresetInfo { std::string uri, name; };

    // Shared graph host contract. Events and metadata retain the existing CLAP-shaped
    // representation; format adapters translate at their boundary. Control methods
    // belong to one main thread; process/outputEvents belong to one audio thread.
    // Destruction happens only after the graph retires all audio plans using the host.
    class PluginHost
    {
    public:
        virtual ~PluginHost() = default;
        virtual void service() = 0;
        // Full Performance snapshot: native context state or all hosted pool instances.
        // Creates a new directory. Restore is for an unpublished replacement host only.
        virtual void saveSnapshot(const std::string& directory);
        virtual void loadSnapshot(const std::string& directory);
        virtual std::shared_ptr<PluginHost> createEffectContext() { throw std::runtime_error("Plugin has no native effect contexts"); }
        virtual std::optional<oscairn_effect_resources_t> effectResources() const { return {}; }
        // Memory declared before activating (oscairn.memory, RFC 0025). Control thread. What the
        // plugin said, at its last activation, activating would allocate; empty where it declares
        // nothing. setMemoryRoom: the bytes it may declare at its next activations (a state load,
        // an effect context made from it); one declaring more is refused before it activates. 0: no limit.
        virtual std::optional<oscairn_memory_t> declaredMemory() const { return {}; }
        virtual void setMemoryRoom(uint64_t) {}
        // Control thread. Save to a NEW directory; restore from a saved directory.
        // Raw adapters may silence/reset during restore. Graph-pooled instruments instead
        // prepare/select a spare; state export still snapshots the selected channel-0 instance.
        virtual const char* soundContextMode() const noexcept { return soundContextCapacity()?"native":"unsupported"; }
        virtual uint32_t soundContextCapacity() const noexcept { return 0; }
        virtual std::vector<oscairn_sound_context_info_t> soundContexts() const { return {}; }
        // sfz: patch text for native engines that define one; its files are in directory of asset library.
        virtual uint32_t prepareSound(const std::string& /*name*/,const std::vector<oscairn_sound_parameter_t>& /*parameters*/,const std::string& /*state*/ = {},const std::string& /*preset*/ = {},const std::string& /*sfz*/ = {},const std::string& /*library*/ = {},const std::string& /*directory*/ = {}) { throw std::invalid_argument("Plugin has no sound contexts"); }
        virtual bool selectSound(uint32_t,int16_t=-1) { throw std::invalid_argument("Plugin has no sound contexts"); }
        virtual bool discardSound(uint32_t,bool=false) { throw std::invalid_argument("Plugin has no sound contexts"); }
        // RFC 0021: the native snapshot saveSnapshot wrote to `directory` prepared beside the playing
        // sounds (oscairn.sound-snapshots), concurrently with rendering; returns the new sound each
        // MIDI channel selects in it, selecting none. Throws where the plugin cannot or refuses (too
        // few free contexts), having added nothing. Control thread.
        virtual bool preparesSnapshots() const noexcept { return false; }
        virtual std::array<uint32_t,16> prepareSnapshot(const std::string& /*directory*/) { throw std::invalid_argument("Plugin cannot prepare a snapshot beside its sounds"); }
        // RFC 0005: the plugin renders its retiring sounds into a hidden ring-out buffer, four output
        // channels after its ports' that process() passes when the caller gives them (no > the ports').
        virtual bool ringsOut() const noexcept { return false; }
        // RFC 0005 (choke_retiring): at the next block, before the selections made after this call, the
        // sounds retiring then fade out over the choke time into the buffer's last two channels. Control thread.
        virtual bool chokeRetiring() { return false; }
        virtual bool supportsState() const noexcept { return false; }
        // The plugin's own window (CLAP gui, RFC 0019), shown by the process hosting the plugin.
        // Control thread. showWindow opens it titled `title`, or titles and raises it; false closes
        // it. Throws where it cannot open. It also closes when the host goes.
        virtual bool hasWindow() const { return false; }
        virtual void showWindow(bool show, const std::string& /*title*/ = {}) { if(show) throw std::runtime_error("Plugin has no window"); }
        // Main thread. Sound assets (RFC 0009): where the plugin resolves them, and which it uses.
        virtual void setAssets(kick::clap::AssetResolver*) noexcept {}
        virtual std::vector<kick::clap::AssetReference> assetDependencies() const { return {}; }
        virtual void saveState(const std::string&) { throw std::runtime_error("Plugin state saving is unsupported"); }
        virtual void loadState(const std::string&) { throw std::runtime_error("Plugin state restoration is unsupported"); }
        virtual std::vector<PresetInfo> presets() { return {}; }
        virtual void loadPreset(const std::string&) { throw std::runtime_error("Plugin presets are unsupported"); }
        virtual std::vector<PropertyInfo> properties() const { return {}; }
        // Acceptance queues a next-block request, not confirmation of plugin success.
        virtual bool setProperty(const std::string&,const PropertyValue&) { return false; }
        virtual bool requestProperty(const std::string&) { return false; }
        // Last plugin-confirmed value, updated by service(); absent until reported.
        virtual std::optional<PropertyValue> propertyValue(const std::string&) const { return {}; }
        virtual const kick::clap::Descriptor& descriptor() const noexcept = 0;
        virtual const std::vector<kick::clap::Parameter>& parameters() const noexcept = 0;
        virtual const std::vector<kick::clap::Port>& inputPorts() const noexcept = 0;
        virtual const std::vector<kick::clap::Port>& outputPorts() const noexcept = 0;
        // Control thread: port IDs forming the unambiguous main audio bus.
        // Format adapters resolve their own bus metadata; ambiguous layouts require explicit routes.
        virtual std::vector<uint32_t> mainAudioPorts(bool input) const;
        virtual uint32_t noteInputPorts() const noexcept = 0;
        virtual bool usesClapNotes(uint32_t) const noexcept { return false; }
        virtual void panic() noexcept = 0;
        virtual uint32_t noteOutputPorts() const noexcept = 0;
        virtual double parameterValue(clap_id id) const noexcept = 0;
        // Changes when the plugin reports new parameter values while processing; audio thread.
        virtual uint64_t parameterReports() const noexcept { return 0; }
        // Changes when parameters() does (a rescan of their info); main thread. 0: never.
        virtual uint64_t parameterRevision() const noexcept { return 0; }
        virtual std::string parameterText(clap_id id, double value) const = 0;
        virtual bool setParameter(clap_id id, double value) noexcept = 0;
        virtual bool parameterGesture(clap_id id, bool begin) noexcept = 0;
        virtual bool popOutput(kick::clap::Event& event) noexcept = 0;
        virtual uint64_t outputEventDrops() const noexcept = 0;
        virtual const kick::clap::Event* outputEvents(uint32_t& count) const noexcept = 0;
        virtual kick::clap::Statistics statistics() const noexcept = 0;
        // Why a plugin in a worker is not playing (its worker crashed, hung or was too slow, and
        // is being restarted or was given up); empty while it plays. Any thread.
        virtual std::string failure() const { return {}; }
        virtual bool process(const float* const* inputs, uint32_t ni, float* const* outputs, uint32_t no, uint32_t frames, const kick::clap::Event* events, uint32_t count, const clap_event_transport_t* transport) noexcept = 0;
        // Audio thread: process() in two halves, for a plugin that renders elsewhere (in a worker),
        // so the caller renders others meanwhile (RFC 0034). startProcess hands the block over and
        // returns without waiting; finishProcess waits for it and returns what process() would have.
        // False from startProcess: nothing started; call process() instead. The outputs must stay
        // valid until finishProcess; inputs, events and transport are taken at the start.
        virtual bool startProcess(const float* const*, uint32_t, float* const*, uint32_t, uint32_t, const kick::clap::Event*, uint32_t, const clap_event_transport_t*) noexcept { return false; }
        virtual bool finishProcess() noexcept { return false; }
        // Audio thread, after process(): the plugin said it makes no more sound until it gets
        // input (CLAP_PROCESS_SLEEP). False where a host cannot tell.
        virtual bool asleep() const noexcept { return false; }
        // Control thread: the memory a plugin in its own worker holds (its share of the worker's
        // physical footprint on macOS, resident memory on Linux); 0 where the host cannot tell (a
        // plugin in the engine's process).
        virtual uint64_t residentBytes() const { return 0; }
        // Control thread: the CPU time, in seconds, a plugin in its own worker has used (its share of
        // the worker's, restarts included); 0 for a plugin in the engine's process.
        virtual double cpuSeconds() const { return 0; }
        // Whether the plugin runs in a worker process (RFC 0010); a free instance is taken over only
        // where the placement policy would put a fresh one (RFC 0021).
        virtual bool inWorker() const noexcept { return false; }
        // Control thread, on a host nothing plays: clears what processing left (CLAP reset: buffers,
        // voices, envelopes, modulation), as a fresh activation would. False where it cannot.
        virtual bool reset() { return false; }
    };
    // A process's resident memory in bytes (0: this one); 0 when it cannot be read.
    uint64_t residentBytes(long pid = 0);
    // RFC 0025, Linux only: what the reliability level in OSCAIRN_RELIABILITY (set by the app from
    // its profile; workers inherit it) asks of an Oscairn process at its start. At instrument and
    // pass-through: every page locked (mlockall; future pages too only under an unlimited
    // RLIMIT_MEMLOCK, since a finite one would turn later allocations into failures), and the process's place in
    // the out-of-memory killer's order (oom_score_adj: the engine -999, a worker 300). Returns what
    // could not be done, empty when everything asked was done or nothing was asked.
    std::string applyMemoryPolicy(int oomScoreAdj);
    // CLAP module or LV2 bundle directory; pluginId is a CLAP ID or an LV2 URI.
    std::shared_ptr<PluginHost> loadPlugin(const std::string& path, const std::string& pluginId,
                                           double sampleRate, uint32_t maxFrames, ProcessingMode mode = ProcessingMode::Realtime, Lv2BufferLimits lv2Buffers = {},
                                           uint64_t memoryRoom = 0); // memoryRoom: as setMemoryRoom, for its first activation
    // How many times loadPlugin has been asked for an instance in this process (RFC 0021 measures reuse by it).
    uint64_t pluginInstancesLoaded() noexcept;
    // RFC 0010: a CLAP plugin hosted by the worker executable `program` in its own process,
    // with the effect contexts later created from it. loadPlugin uses it as the placement
    // policy says.
    // RFC 0010: where loadPlugin runs a CLAP plugin. Main thread. LV2 plugins stay in the engine.
    enum class Placement { Engine, Worker };
    struct PlacementPolicy
    {
        std::string worker; // the oscairn-plugin-worker executable; empty runs every plugin in the engine
        Placement standard = Placement::Worker;
        std::map<std::string, Placement> plugins; // overrides by plugin ID, as loadPlugin is given it
        // The memory a worker may use before it is stopped, in bytes (0: no limit): its physical
        // footprint on macOS, resident memory on Linux, as its supervisor reads them.
        uint64_t memoryLimit = uint64_t(4) << 30;
        std::string scan; // the oscairn-plugin-scan helper for a fresh preset listing (RFC 0005); empty: none
    };
    // Starts as OSCAIRN_PLUGIN_WORKER says: every plugin in the worker executable it names, or none;
    // OSCAIRN_WORKER_MEMORY_MB sets the memory limit; OSCAIRN_PLUGIN_SCAN names the scan helper
    // a loaded plugin's preset listing reads through (empty: a worker's plugin lists none).
    void setPlacementPolicy(PlacementPolicy);
    const PlacementPolicy& placementPolicy();
    std::shared_ptr<PluginHost> loadPluginInWorker(const std::string& program, const std::string& path, const std::string& pluginId,
                                                   double sampleRate, uint32_t maxFrames, ProcessingMode mode = ProcessingMode::Realtime, uint64_t memoryLimit = 0,
                                                   uint64_t memoryRoom = 0);
    // RFC 0005: one plugin's presets, read by the isolated scan helper the placement policy names
    // (its `scan`). The plugin's preset-provider code runs in that helper process (timeout, crash
    // isolation, stderr), never in this one, and off the caller's thread. One listing per binary
    // and modification time, kept, failures included. Throws, saying why, where no helper is
    // configured or its listing failed, and "Presets are being listed; ask again" until it is in.
    std::vector<PresetInfo> scanPluginPresets(const std::string& path, const std::string& pluginId);
}
