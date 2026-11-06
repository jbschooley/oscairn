// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <clap/clap.h>
#include <oscairn-clap/sound-contexts.h>
#include <oscairn-clap/effect-contexts.h>
#include <oscairn-clap/assets.h>
#include <oscairn-clap/memory.h>
#include <oscairn-clap/voice-budget.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace kick::clap {

// A preset a plugin's library lists for it through CLAP preset discovery: inside the plugin
// (kind 1, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN) or in a file (kind 0, location its path).
struct Preset { std::string name; uint32_t kind = 0; std::string location, loadKey; };

struct Descriptor {
    std::string path, id, name, vendor, version, description;
    std::vector<std::string> features;
    bool presetDiscovery = false; // its library offers CLAP preset discovery
    std::vector<Preset> presets;  // what a scan indexed (RFC 0005)
};

struct Parameter {
    clap_id id = CLAP_INVALID_ID;
    std::string name, module;
    double minimum = 0, maximum = 1, defaultValue = 0;
    uint32_t flags = 0;
    // The plugin's oscairn.ui hint, checked against the parameter: the control it wants
    // (enum oscairn_widget), its priority (0: none) and, for a range, its high parameter.
    uint32_t widget = 0, priority = 0;
    clap_id rangeHigh = CLAP_INVALID_ID;
    uint32_t group = 0; // drawn joined with the module's other parameters of this group; 0: none
    uint32_t role = 0;  // enum oscairn_role: what it does on every instrument (cutoff, attack); 0: none
};

struct Port {
    clap_id id = CLAP_INVALID_ID;
    std::string name;
    uint32_t channels = 0, flags = 0;
};

// Owning, fixed-capacity event. A SysEx event (CLAP_EVENT_MIDI_SYSEX) points at bytes held
// in a block's SysexPool, valid until that block's consumers have run; the command queue
// refuses SysEx, whose bytes could not outlive the caller there.
struct Event {
    // 64 bytes holds every event type the host accepts (the largest, parameter
    // value and modulation events, are 56 bytes).
    static constexpr size_t capacity = 64;
    alignas(8) std::array<uint8_t, capacity> bytes{};
    const clap_event_header_t* header() const noexcept {
        return reinterpret_cast<const clap_event_header_t*>(bytes.data());
    }
    bool valid() const noexcept;
    template<class T> static Event from(const T& source) noexcept {
        static_assert(sizeof(T) <= capacity, "event too large");
        Event result;
        std::memcpy(result.bytes.data(), &source, sizeof(T));
        return result;
    }
    static Event midi(uint8_t status, uint8_t a, uint8_t b, uint32_t frame = 0,
                      uint16_t port = 0) noexcept;
    static Event note(bool on, int key, double velocity, uint32_t frame = 0,
                      int channel = 0, int port = 0, int noteId = -1) noexcept;
};

// The bytes of one block's SysEx events (RFC 0032): preallocated, emptied each block, never
// grown, so an event's pointer stays valid until the block's consumers have run. A message
// over maxMessage, or one that would take the block past maxBlock, is dropped, never truncated.
struct SysexPool {
    static constexpr uint32_t maxMessage = 64 * 1024, maxBlock = 256 * 1024;
    uint32_t used = 0;
    alignas(8) uint8_t bytes[maxBlock];
    // Copies a SysEx event's bytes in and points the event at the copy. False when they do
    // not fit, and for any other event. Audio thread: no allocation.
    bool hold(Event& event) noexcept;
};

// A sound asset reference (RFC 0009): library ID, library-relative path, SHA-256 hex or empty.
struct AssetReference { std::string library, path, sha256; bool optional = false; }; // optional: the plugin does without it (oscairn.asset-dependencies draft 2)
// Resolves asset references for plugins through the oscairn.assets host extension. Main thread.
struct AssetResolver {
    virtual ~AssetResolver() = default;
    // The local path of library/path. An empty sha256 is filled in; a given one must match.
    // Throws std::exception carrying the reason.
    virtual std::string resolve(const std::string& library, const std::string& path, std::string& sha256) = 0;
};

struct Statistics {
    uint64_t blocks = 0, droppedEvents = 0, processErrors = 0;
    double lastMicroseconds = 0, maximumMicroseconds = 0;
    // Cumulative per host. A missed DSP deadline is not an audio-device xrun.
    uint64_t deadlineMisses = 0, controlSilences = 0, idleBlocks = 0;
    uint64_t restarts = 0; // Oscairn plugin workers restarted after failing (RFC 0010)
};

// Control API belongs to ONE main/UI thread. process(), enqueue() and atomic
// telemetry are the only cross-thread API. Control operations exclude rendering
// with a gate: the callback NEVER waits and returns silence during a change.
// Native sound-context operations instead use the extension's concurrent contract;
// preparing/selecting an unused context does not gate the playing instance.
// CLAP permits the exclusively held main thread to act as the symbolic audio
// thread for stop/reset/flush; thread-check reports this scope accurately.
class Host {
public:
    static constexpr uint32_t maxEvents = 2048;     // capacity of the control-thread queues
    static constexpr uint32_t maxAudioChannels = 64; // in each direction, across all buses
    static constexpr uint32_t maxBlockEvents = 512; // direct graph events per process() call; queued commands have a separate budget
    // A destination's block also carries the graph's own oscairn.beat event and its part's
    // harmony changes, which do not count against maxBlockEvents; hosts must accept up to this
    // many events per process() call.
    static constexpr uint32_t maxProcessEvents = maxBlockEvents + 1 + 32;
    // The event space this host assigns to oscairn.param-target (oscairn-clap/param-target.h)
    // through CLAP's event-registry. Plugins may send these events; the host passes them
    // to the caller in outputEvents() and never delivers them to a plugin.
    static constexpr uint16_t paramTargetSpace = 1000;
    // oscairn.beat (oscairn-clap/beat.h): the caller sends these events to every host each block;
    // the host delivers them only to a plugin that queried the space, and never counts the rest
    // as dropped.
    static constexpr uint16_t beatSpace = 1001;
    // oscairn.harmony (oscairn-clap/harmony.h): the same, and a harmony equal to the last one
    // delivered is not delivered again. The plugin reads the last on the main thread through
    // the host extension.
    static constexpr uint16_t harmonySpace = 1002;
    static constexpr uint32_t maxStateBytes = 64 * 1024 * 1024;
    static constexpr size_t maxPatchBytes = 1024 * 1024;
    Host();
    ~Host();
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;

    bool load(const Descriptor&, double sampleRate, uint32_t maxFrames,
              const std::vector<uint8_t>& state = {});
    bool loadEffectContext(const Host& source);
    bool effectResources(oscairn_effect_resources_t&) const;
    // oscairn.memory (RFC 0025). Main thread. The bytes the plugin may declare when it next
    // activates (a load, a state load, an effect context made from this one, which inherits it):
    // one declaring more is refused before it activates; 0, the default, sets no limit. An effect
    // context counts its context and asset bytes, an instance its shared bytes too.
    void setMemoryRoom(uint64_t bytes) noexcept;
    // CLAP render (clap.render). Main thread, before a load: true tells a plugin with the extension
    // CLAP_RENDER_OFFLINE when it next initializes (an effect context made from this one inherits
    // it), unless it has a hard realtime requirement; otherwise it is told CLAP_RENDER_REALTIME.
    void setOfflineRendering(bool offline) noexcept;
    // What the plugin declared at its last activation; false where it declares nothing.
    bool declaredMemory(oscairn_memory_t&) const;
    // What it holds now, as it says; false where it does not say or is not active.
    bool currentMemory(oscairn_memory_t&) const;
    // oscairn.memory (RFC 0025). Called on the main thread at every activation (a load, a state
    // load, a reconfigure, an effect context, a restart the plugin asked for), after the plugin
    // declares and before it activates and allocates, with what it declared (nullptr: nothing),
    // so the process hosting it can make room first: a worker's engine refits its limit. Throwing
    // refuses the activation. Process-wide; set once, before any plugin loads. Null: none.
    using ActivationObserver = void (*)(void* context, const oscairn_memory_t* declared);
    static void observeActivations(ActivationObserver, void* context) noexcept;
    void unload();
    bool reconfigure(double sampleRate, uint32_t maxFrames);
    // Main thread: reject later port ID/order/channel-count changes under the render gate.
    void lockAudioLayout();
    void service(); // main-loop callbacks, restart, timers, fd polling, rescans
    bool saveState(std::vector<uint8_t>& destination);
    bool loadState(const std::vector<uint8_t>& source);
    bool supportsState() const noexcept;
    // Main thread. The presets this plugin's library offers for it (CLAP preset discovery):
    // those inside the plugin, and preset files of the declared types in the declared folders,
    // within indexPresets' limits (Discovery.h). The provider runs in this process, on this
    // thread, as the plugin does; a host with a scan helper lists through it instead
    // (oscairn::scanPluginPresets, RFC 0005).
    std::vector<Preset> presets();
    // Main thread. Loads a preset (CLAP preset-load), never during process(); false with error().
    bool loadPreset(const Preset&);
    bool supportsSoundSnapshot() const noexcept;
    // CLAP gui: the plugin's own window. Main thread. Embedded in a window of the host's own where
    // the host makes them (macOS), else floating where the plugin floats one (RFC 0019).
    bool hasGui() const noexcept;
    bool showGui(const std::string& title); // opens it, or titles and raises it; false with error()
    void closeGui();                        // destroys the editor and its window; also on unload
    bool guiOpen() const noexcept;
    // Main thread. Where plugins resolve sound assets; null rejects every resolution.
    void setAssets(AssetResolver*) noexcept;
    // The assets the plugin's state uses (oscairn.asset-dependencies); empty if not declared.
    std::vector<AssetReference> assetDependencies() const;
    bool declaresAssets() const noexcept;
    uint32_t soundContextCapacity() const noexcept;
    std::vector<oscairn_sound_context_info_t> soundContexts() const;
    // patch: instrument-defined text (SFZ for the Sampler) whose files are in directory of asset library.
    uint32_t prepareSound(const std::string& name,const std::vector<oscairn_sound_parameter_t>& parameters,const std::string& patch={},const std::string& library={},const std::string& directory={});
    // A native context from one of presets() (draft 5's prepare_preset).
    uint32_t prepareSound(const std::string& name,const Preset& preset);
    bool selectSound(uint32_t id,int16_t channel=-1);
    bool discardSound(uint32_t id);
    // oscairn.sound-snapshots: a whole snapshot's sounds prepared beside the playing ones, returning
    // the new ID each MIDI channel selects in it; nothing is selected. Main thread, while playing.
    bool preparesSoundSnapshots() const noexcept;
    std::array<uint32_t,16> prepareSoundSnapshot(const std::vector<uint8_t>& bytes);
    // sound-snapshots' ring_out (RFC 0005): process() given four output channels after the ports'
    // passes them to the plugin as its ring-out buffer, where it renders its retiring sounds.
    bool ringsOut() const noexcept;
    // Its choke_retiring, queued before the selections made after it: the sounds retiring then fade
    // out into the buffer's last two channels. False when the plugin does not ring out or its queue is full.
    bool chokeRetiringSounds();
    // oscairn.voice-budget (RFC 0018): the plugin gives up its own voices within a limit; and
    // whether it said every voice follows CLAP's volume expression. Any thread.
    bool voiceBudget() const noexcept;
    bool honorsVolume() const noexcept;
    // Audio thread, between process() calls: the limit and contexts the next process() gives the
    // plugin first (set_limits), copied; up to OSCAIRN_VOICE_BUDGET_MAX_CONTEXTS.
    void setVoiceLimits(uint32_t limit, const oscairn_voice_budget_context_t* contexts, uint32_t count) noexcept;
    // Audio thread, after process(): the voices the plugin said were sounding, its estimate of one
    // voice's cost, and its notes' loudness, as its last process() left them; 0 without the extension.
    uint32_t voiceReport(double& voiceCost, const oscairn_voice_budget_note_t*& notes, uint32_t& count) const noexcept;
    bool loaded() const noexcept;
    bool faulted() const noexcept;
    const std::string& error() const noexcept;
    const Descriptor& descriptor() const noexcept;
    const std::vector<Parameter>& parameters() const noexcept;
    uint64_t metadataRevision() const noexcept;
    const std::vector<Port>& inputPorts() const noexcept;
    const std::vector<Port>& outputPorts() const noexcept;
    uint32_t noteInputPorts() const noexcept;
    bool usesClapNotes(uint32_t port) const noexcept;
    uint32_t noteOutputPorts() const noexcept;
    double parameterValue(clap_id) const noexcept;
    std::string parameterText(clap_id, double value) const;
    bool setParameter(clap_id, double plainValue) noexcept;
    bool parameterGesture(clap_id, bool begin) noexcept;
    bool stateDirty() const noexcept;
    // Counts parameter values the plugin reported while processing; any thread.
    uint64_t parameterReports() const noexcept;
    uint32_t latency() const noexcept;
    Statistics statistics() const noexcept;
    // Audio thread: the plugin's last process() asked to sleep (CLAP_PROCESS_SLEEP): it makes no
    // sound until it gets input or events.
    bool asleep() const noexcept;

    bool enqueue(const Event&) noexcept; // bounded multi-producer queue
    bool popOutput(Event&) noexcept; // main/control-thread consumer of parameter output
    uint64_t outputEventDrops() const noexcept; // detects gaps in plugin control output
    // Note, MIDI and expression events the plugin emitted during the last
    // process() call, sorted as emitted, with times relative to the start of that
    // call. Audio thread only; valid until the next process() on this host.
    const Event* outputEvents(uint32_t& count) const noexcept;
    void panic() noexcept;
    void reset(); // main-thread, exclusive audio scope

    // Planar channels are flattened in port order. Missing inputs are silent;
    // surplus plugin outputs render into preallocated scratch storage. All
    // supplied outputs are overwritten. Frames may exceed negotiated maxFrames:
    // the host splits them and rebases event and transport timestamps.
    bool process(const float* const* inputs, uint32_t inputChannels,
                 float* const* outputs, uint32_t outputChannels, uint32_t frames,
                 const Event* events = nullptr, uint32_t count = 0,
                 const clap_event_transport_t* transport = nullptr) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

// For a process with no event loop of its own (a plugin worker) once a plugin window is open:
// runs the platform's window events on the main thread for up to `milliseconds`, returning
// early when `descriptor` becomes readable. False, at once, where there are none to run here.
bool runWindowEvents(int descriptor, int milliseconds);

} // namespace kick::clap
