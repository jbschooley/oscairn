// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "oscairn/graph.h"
#include "oscairn/parts.h"
#include "oscairn/pads.h"
#include "oscairn/recorder.h"
#include "oscairn/transport.h"
#include <oscairn-clap/ui.h>

#include <string>
#include <map>
#include <vector>

namespace oscairn
{
    // oscairn.ui's widgets by name, as the control protocol writes them ("knob"); unknown: auto.
    inline const char* widgetName(uint32_t widget)
    {
        static const char* const names[] = {"auto", "slider", "knob", "buttons", "switch", "list", "range"};
        return widget < std::size(names) ? names[widget] : "auto";
    }
    inline uint32_t widgetNamed(const std::string& name)
    {
        for(uint32_t w = 0; w <= OSCAIRN_WIDGET_RANGE; ++w) if(name == widgetName(w)) return w;
        return OSCAIRN_WIDGET_AUTO;
    }
    // oscairn.ui's roles by name, as the role banks (physical_controls.h) and the control
    // protocol write them ("cutoff"); none or unknown: empty.
    inline const char* roleName(uint32_t role)
    {
        static const char* const names[] = {"", "cutoff", "resonance", "filterEnvelope", "keyTracking", "filterAttack", "filterDecay", "filterSustain", "filterRelease",
                                            "attack", "decay", "sustain", "release", "hold", "velocity", "ampKeyTracking", "level",
                                            "lowGain", "midGain", "highGain", "midFrequency", "lowFrequency", "highFrequency", "midQ", "eqOutput",
                                            "level1", "level2", "level3", "level4", "level5", "level6", "level7", "level8", "mix", "pan"};
        static_assert(std::size(names) == OSCAIRN_ROLE_PAN + 1);
        return role < std::size(names) ? names[role] : "";
    }
    inline uint32_t roleNamed(const std::string& name)
    {
        for(uint32_t r = 1; r <= OSCAIRN_ROLE_PAN; ++r) if(name == roleName(r)) return r;
        return OSCAIRN_ROLE_NONE;
    }

    /// In-process control boundary. All calls belong to one control/main thread;
    /// audio and hardware input do not call this interface. No device or plugin handles
    /// escape it. The socket/JSON adapter will implement the same control operations.
    class Control
    {
    public:
        virtual ~Control() = default;

        struct ParamInfo
        {
            uint32_t    id = 0;
            std::string name;
            double      minimum = 0, maximum = 1, defaultValue = 0;
            bool        stepped = false;
            uint32_t    flags = 0;
            std::string module; // CLAP's module path ("Oscillator 1/Pitch"): where editors group it
            // The plugin's oscairn.ui hint: enum oscairn_widget, a priority (0: none), and for a
            // range its high parameter.
            uint32_t widget = 0, priority = 0, rangeHigh = CLAP_INVALID_ID;
            uint32_t group = 0; // drawn joined with the module's other parameters of this group
            uint32_t role = 0;  // enum oscairn_role (roleName); 0: none declared
        };
        struct TransportState
        {
            bool   playing = false;
            double tempo = 120.0;
            double beat = 0.0;
            std::vector<DeviceRule> clockOutputs; // RFC 0012 step 2: the MIDI outputs clock goes to (setClockOutputs)
        };
        struct DeviceChannel { uint32_t deviceChannel=0; std::string name; bool available=false; };
        struct SessionStatus
        {
            std::string audioDevice;
            std::vector<MidiEndpoint> midiInputs; // the host's MIDI inputs, for device selectors
            double sampleRate = 0, cpuLoad = 0;
            uint32_t bufferSize = 0;
            uint64_t deadlineMisses = 0;
            std::vector<DeviceChannel> audioInputs, audioOutputs; // logical order -> device channel
            bool audioReady=true;
            uint64_t graphEventDrops=0,midiEventDrops=0;
            // Plugins that have failed and are silent: a worker that crashed, hung or was given up.
            struct PluginFailure { NodeId node=0; std::string name, reason; bool operator==(const PluginFailure&) const = default; };
            std::vector<PluginFailure> failures;
            // Plugins in their own workers (RFC 0010): CPU time used in seconds, restarts included, and memory in bytes.
            struct WorkerUsage { NodeId node=0; std::string name; double cpu=0; uint64_t memory=0; bool operator==(const WorkerUsage&) const = default; };
            std::vector<WorkerUsage> workers;
            std::vector<MidiEndpoint> midiOutputs; // its MIDI outputs, numbered from 1 apart from the inputs, for parts' MIDI out and clock
        };
        virtual SessionStatus sessionStatus() const = 0;
        struct GraphState
        {
            Graph::Description topology;
            std::map<NodeId,std::string> names;
            uint64_t revision = 0;
            bool staged = false;
            std::vector<Part> parts; // RFC 0011: the part views' model, staged with the graph
            std::vector<Wire> wires;
            std::vector<Scene> scenes;
            std::vector<SceneParameter> untouched; // the parameters scenes never touch
            uint32_t scene = 0; // the scene last selected or stored, or 0
            std::vector<Macro> macros;
            std::set<NodeId> disabled; // plugins switched off (setNodeEnabled)
            std::set<NodeId> ringing;  // those of them still sounding out: notes held, a tail
            double masterLevel = 1, masterPan = 0; // the Performance's main output (setMaster)
        };
        enum class ChangeKind { Value, GestureBegin, GestureEnd, Graph, Resync };
        struct Change
        {
            uint64_t sequence = 0;
            ChangeKind kind = ChangeKind::Value;
            NodeId node = 0;
            uint32_t param = 0;
            double value = 0;
            bool plugin = false;
        };
        struct Changes { uint64_t latest = 0; bool lost = false; std::vector<Change> events; };

        // Control thread, engine-local paths. Performance save includes all prepared sounds
        // in a NEW folder; load validates before replacing the current graph.
        virtual void savePerformance(const std::string& directory) = 0;
        // Resource budgets (RFC 0025). The device's limits, set by the app from its profile: `cpu`
        // is the share of the audio thread's time (each block's duration) the plugins may take,
        // after the reliability level's margin; `memory` in bytes (0: not checked). Until a budget
        // is set nothing is checked and nothing is measured.
        // Memory classes (RFC 0025), in bytes; with `device` 0 they are off and `memory` is the
        // ceiling. With them, the ceiling for committed memory (the open Performance, what rings
        // out, the prepared next entry) is the committed share: device less what is reserved
        // (`system`: the OS, the engine's base and the shell; the running and always-available
        // apps' reservations) and the margin. `floor` (the profile's performanceMemory) is the
        // committed memory no app launch may take.
        struct Budget { double cpu = 0; uint64_t memory = 0; uint64_t device = 0, system = 0, floor = 0, margin = 0; };
        virtual void setBudget(const Budget& budget) = 0;
        // An app's memory reservation (RFC 0025): what it needs to open and do its job.
        struct AppMemory { std::string app; uint64_t reserve = 0; };
        // The always-available apps, reserved whether they run or not, so they always open.
        // Checked as a launch of the apps added; std::invalid_argument with the reason when they
        // do not fit beside the floor or what is committed.
        virtual void setAlwaysAvailable(const std::vector<AppMemory>& apps) = 0;
        // Asked before an app starts. Admitted: its reservation counts while it runs, after
        // opportunistic data is let go to make room. Refused: reason says what it needs, what is
        // free for apps and which running apps' closing would free enough; nothing running is
        // closed and nothing committed let go. offerNext: refused only because of the committed
        // next entry, and asking again with dropNext lets that entry go and admits it. An
        // always-available app is always admitted. Without memory classes, everything is admitted.
        struct Launch { bool admitted = false, offerNext = false; std::string reason; };
        virtual Launch launchApp(const AppMemory& app, bool dropNext = false) = 0;
        virtual void appClosed(const std::string& app) = 0;
        // The memory classes now: reserved, the committed share, committed in use (open, ringing
        // and a committed next entry), opportunistic held (a next entry prepared to cut).
        struct MemoryState
        {
            uint64_t device = 0, reserved = 0, share = 0, floor = 0, margin = 0, committed = 0, opportunistic = 0;
            std::vector<AppMemory> running, alwaysAvailable;
            bool nextPrepared = false, nextCommitted = false;
        };
        virtual MemoryState memoryState() = 0;
        // What a Performance needs: the open one (directory empty) or one in a folder, each plugin
        // at its cost measured on this device or at a default (measured false). fits: within the
        // budget; seamless: within it together with what plays now, so a change to it rings out.
        // For the open one, what plays now is the last Performance still ringing out: it never
        // makes the open one not fit, so adding a plugin right after a change is not refused.
        struct Cost { std::string name, plugin; double cpu = 0; uint64_t memory = 0; bool measured = false; };
        struct Estimate { double cpu = 0; uint64_t memory = 0; std::vector<Cost> plugins; Budget budget; bool fits = true, seamless = true; };
        virtual Estimate estimate(const std::string& directory = {}) = 0;
        // An export (RFC 0021): one archive file (package.h) holding this Performance, every sample
        // its plugins use, and the library Sound of each part still that Sound; with editedSounds,
        // also the Sound of each part edited since, as it is in the library. Refuses a file that exists.
        // Returns notes on what could not go: plugins not here, formats not bundled yet.
        // sign (RFC 0006, the exporter's choice): with the signing key named, sign each item, the
        // whole archive, or both. Plugin builds carry their publishers' signatures either way.
        struct ExportSigning { std::string key; bool items, archive; }; // no member initializers, so it can be a default argument here; {} is all false
        virtual std::vector<std::string> exportPerformance(const std::string& file, bool editedSounds = false, const ExportSigning& sign = {}) = 0;
        // A library item as an archive, as it is saved: a Performance with its parts' Sounds (an
        // edited part's when editedSounds), or a Sound; every sample they use. Refuses a file that exists.
        virtual std::vector<std::string> exportLibraryItem(const std::string& id, const std::string& file, bool editedSounds = false, const ExportSigning& sign = {}) = 0;
        // Signing keys (RFC 0006): private keys in the data folder, Keys/<name>.key, each readable
        // only by the user; listed by name, ID and public key, never the private key. Make: a new
        // one; a name is letters, digits, '.', '-' and '_', at most 64, not starting with '.'.
        struct SigningKey { std::string name, id, key; };
        virtual std::vector<SigningKey> signingKeys() = 0;
        virtual SigningKey makeSigningKey(const std::string& name) = 0;
        virtual void loadPerformance(const std::string& directory) = 0;
        // The next Performance, read and its plugins loaded off the audio thread while this one
        // plays (RFC 0021), so loading it then only publishes it. False: it would not fit beside
        // what plays now (RFC 0025), and loads when loaded, as a cut; with memory classes it is
        // still prepared, as opportunistic data let go first, when free memory allows. Empty: lets go of the
        // prepared one; preparing another or loading any other Performance does too.
        virtual bool preparePerformance(const std::string& directory) = 0;
        // The Performance's own details (format 3's info block, RFC 0021): saved with it, read
        // by the library. The ID is made on the first save and kept.
        // color: "#rrggbb", or empty for none; how lists and setlists show it.
        // revision: this save's, made at each save; parent: the one it was saved over. hash: of
        // its contents (contentHash in library.h), not an identifier: equal hashes, equal contents.
        // origin: for an item imported into the user's own storage (RFC 0021), the library it came
        // from, its ID there and the content hash of this copy as imported; empty otherwise.
        struct Origin { std::string library, item, hash; bool operator==(const Origin&) const = default; };
        struct PerformanceInfo { std::string id, name, description; std::vector<std::string> tags; std::string color, revision, parent, hash; Origin origin; };
        virtual PerformanceInfo performanceInfo() const = 0;
        virtual void setPerformanceInfo(const PerformanceInfo& info) = 0; // the ID is kept as it is
        // The library (RFC 0021): the device's Performances, by tags, text, favorites and
        // recent use. Performances save into it by name; loading by ID marks one used.
        // library: the installed library it is in (its ID), locked; empty for the user's own.
        struct LibraryItem { std::string id, kind = "performance", name, description, path; std::vector<std::string> tags; bool favorite = false; uint32_t recent = 0; std::string color, revision, hash, library; Origin origin; };
        // kind: "performance" or "sound"; empty for both. source: "mine" for the user's own items, an
        // installed library's ID for its items, empty for all. id: that item only, empty for any.
        struct LibraryQuery { std::string text; std::vector<std::string> tags; bool favorites = false, recent = false; uint32_t limit = 200; std::string kind, source, id; };
        // tags: each tag over every match with its count, but those the user hid from the tag list;
        // tagColors: the colors the user gave tags; hiddenTags: every tag the user hid.
        struct LibraryResult { std::vector<LibraryItem> items; std::vector<std::pair<std::string, uint32_t>> tags; uint32_t total = 0;
                               std::vector<std::pair<std::string, std::string>> tagColors; std::vector<std::string> hiddenTags; };
        virtual LibraryResult libraryQuery(const LibraryQuery& query) = 0;
        virtual void libraryFavorite(const std::string& id, bool favorite) = 0;
        // A tag as the user sees it (RFC 0021): hidden from the tag list (a search typed in full still
        // finds it) and its color ("#rrggbb", or empty for none). The user's, not any item's.
        virtual void libraryTag(const std::string& tag, bool hidden, const std::string& color) = 0;
        virtual uint32_t libraryRescan() = 0;                          // items found
        // An item's name, description, tags and color, written into its file (the rest of info is
        // ignored); a locked library's items refuse. Copy: a new item of the user's own with the
        // same contents and a new ID ("<name> copy" for one of theirs); returns its ID.
        virtual void libraryEdit(const std::string& id, const PerformanceInfo& info) = 0;
        virtual std::string libraryCopy(const std::string& id) = 0;
        // Save: over the open library Performance (renamed when name is given), else a new one.
        // asNew: always a new one, with its own ID. Returns its ID.
        virtual std::string savePerformanceToLibrary(const std::string& name, bool asNew = false) = 0;
        virtual void loadPerformanceFromLibrary(const std::string& id) = 0;
        // Libraries (RFC 0021), from archives (package.h): a library or an exported Performance.
        // Who signed a part of an archive (RFC 0006), as this device's trust store sees it: kind
        // "plugin" (name, version and platform: its build's), "app" or "archive"; status "trusted"
        // (by a key trusted for it), "unknown" (signed, by no key trusted for it), "unsigned" or
        // "revoked"; key: the ID of the key that decided it (empty when unsigned) and keyName
        // its name in the trust store.
        struct Signer { std::string kind, name, version, platform, status, key, keyName; };
        // code: each plugin build's signer (each app's, when archives carry apps); signer: the
        // whole archive's. Signatures that do not verify refuse the archive.
        struct Archive { std::string id, name, kind; std::vector<LibraryItem> items; std::vector<Signer> code; Signer signer; }; // items: kind, id, name
        virtual Archive archiveContents(const std::string& file) = 0;
        // Installs an archive, all of it or the chosen items (IDs), with the samples they use.
        // asLibrary: locked, under its name and its items' IDs; installing again puts back only
        // what is missing. Else into the user's own Performances and Sounds, under new IDs, a
        // Performance's parts pointing at the Sounds that came with it; an item whose copy is
        // still as imported is skipped, one edited or deleted since comes again as a new copy.
        // Code (RFC 0006): when a plugin build the items use is unsigned or by an unknown key, the
        // install is refused, listing them, unless acceptUnsignedCode (the user said yes, having
        // seen archiveContents' code); a build by a revoked key is never installed (what uses it
        // opens with a stand-in) and is noted. Data installs whether signed or not. code: the
        // signer of each build installed or refused.
        struct Installed { uint32_t added = 0, skipped = 0; std::vector<std::string> notes; std::vector<Signer> code; };
        virtual Installed installArchive(const std::string& file, bool asLibrary, const std::vector<std::string>& items = {}, bool acceptUnsignedCode = false) = 0;
        struct InstalledLibrary { std::string id, name; uint32_t items = 0; };
        virtual std::vector<InstalledLibrary> libraries() = 0;
        virtual void uninstallLibrary(const std::string& library) = 0; // its items go; Performances keep their parts, copies, and samples
        virtual void deleteLibraryItem(const std::string& item) = 0;   // a locked library's comes back when it is installed again
        // The plugin store (RFC 0006): the plugin builds installed from archives or added, by
        // platform, with their size on disk, the library items that use the plugin (names), and
        // whether the open Performance plays this device's build from the store.
        struct InstalledPlugin { std::string format, id, version, name; std::vector<std::string> platforms; uint64_t bytes = 0; std::vector<std::string> usedBy; bool open = false; };
        virtual std::vector<InstalledPlugin> installedPlugins() = 0;
        // Removes a plugin's builds from the store, one platform's or (empty) all. Refused while the
        // open Performance plays the build; items that use it open with a stand-in until it is back.
        virtual void removePlugin(const std::string& format, const std::string& id, const std::string& version, const std::string& platform = {}) = 0;
        // Adds another platform's build (such as linux-arm64) from a file or bundle here, so
        // exports carry it, replacing that platform's build in the store. It cannot be loaded here
        // to check: the device it is for checks its ID and version when it loads it.
        virtual void addPluginBuild(const std::string& format, const std::string& id, const std::string& version, const std::string& platform, const std::string& path) = 0;
        // The trust store (RFC 0006), trust.json in the data folder: the keys whose signatures this
        // device trusts, each by its public key (64 hex digits) and ID (its fingerprint), with a
        // name, what it is trusted for (scope: any of "plugins", "apps" and "content") and whether
        // it is revoked. Setting one adds it or replaces the entry for that key (id is ignored);
        // removing takes its ID.
        struct TrustedKey { std::string id, key, name; std::vector<std::string> scope; bool revoked = false; };
        virtual std::vector<TrustedKey> trustedKeys() = 0;
        virtual void setTrustedKey(const TrustedKey& key) = 0;
        virtual void removeTrustedKey(const std::string& id) = 0;
        // Sounds (RFC 0021): a part saved to the library, without its seat. Wires that cross the
        // part's edge are not saved and are listed in `left`. Save writes a new revision of the
        // Sound the part came from (renamed when name is given); asNew, or a part from no Sound,
        // makes a new Sound. Either way the part then comes from what was saved.
        struct SoundSaved { std::string id, revision; std::vector<std::string> left; };
        virtual SoundSaved savePartAsSound(uint32_t part, const std::string& name, bool asNew = false) = 0;
        // Replaces the part's chain with the Sound's, keeping its seat; one step to undo. Returns
        // what could not be kept (wires and control targets on the old plugins).
        virtual std::vector<std::string> loadSound(uint32_t part, const std::string& id) = 0;
        virtual uint32_t partFromSound(const std::string& id) = 0; // a new part, playing to Main
        // Audition (RFC 0021): a library item tried without keeping it. A Sound plays in part's
        // place, its chain set aside; a Performance opens in place of this one, which stays loaded
        // and silent (its held notes and tails ring out). Stop puts back what was there, the same
        // plugins with their state, and the tried one's tails ring out; keep makes it the change
        // (a Sound's load one step to undo). Edits made meanwhile go with the tried one. Starting
        // another stops this one; opening a Performance keeps it. A Performance is refused when
        // the two together would not fit the budget (RFC 0025).
        struct Audition { std::string id, kind; uint32_t part = 0; }; // id empty: none
        virtual void auditionStart(const std::string& id, uint32_t part = 0) = 0;
        virtual void auditionStop(bool keep = false) = 0;
        virtual Audition audition() const = 0;
        // Importing a Performance (RFC 0021): all of it, or chosen parts and scenes, added to the
        // open one in one step. performanceContents lists what a Performance folder holds, by
        // its own IDs.
        struct PerformancePart { uint32_t id = 0; std::string name; bool bus = false; };
        struct PerformanceScene { uint32_t id = 0; std::string name; };
        struct PerformanceContents { std::vector<PerformancePart> parts; std::vector<PerformanceScene> scenes; };
        virtual PerformanceContents performanceContents(const std::string& directory) = 0;
        // parts, scenes: IDs in that file; both empty for all of it. Returns the new parts' IDs,
        // and notes on what changed on the way (renamed scenes and controls, missing ports).
        struct Imported { std::vector<uint32_t> parts; std::vector<std::string> notes; };
        virtual Imported importPerformance(const std::string& directory, const std::vector<uint32_t>& parts = {}, const std::vector<uint32_t>& scenes = {}) = 0;
        // Per-node export saves the selected channel-0 instance of a hosted pool.
        virtual bool supportsState(NodeId node) const = 0;
        virtual void saveState(NodeId node,const std::string& directory) = 0;
        // The plugin's own window (RFC 0019), shown by the process hosting the plugin: whether it
        // has one, and opening it titled (or titling and raising it) or closing it.
        virtual bool hasPluginWindow(NodeId node) const = 0;
        virtual void showPluginWindow(NodeId node,bool show,const std::string& title = {}) = 0;
        virtual std::vector<PresetInfo> presets(NodeId node) = 0;
        virtual std::vector<PropertyInfo> properties(NodeId node) const = 0;
        // Acceptance queues a request; propertyValue is last plugin-confirmed state.
        virtual bool setProperty(NodeId node,const std::string& uri,const PropertyValue& value) = 0;
        virtual bool requestProperty(NodeId node,const std::string& uri) = 0;
        virtual std::optional<PropertyValue> propertyValue(NodeId node,const std::string& uri) const = 0;

        struct SoundInfo { uint32_t id;std::string name;uint32_t selectedChannels,activeVoices;bool idle; };
        struct Sounds { uint32_t capacity;std::vector<SoundInfo> contexts;std::string mode; };
        virtual Sounds sounds(NodeId node) const = 0;
        // sfz patches name files in directory of asset library (see the asset calls below).
        virtual uint32_t prepareSound(NodeId node,const std::string& name,const std::vector<oscairn_sound_parameter_t>& parameters,const std::string& state={},const std::string& preset={},const std::string& sfz={},const std::string& library={},const std::string& directory={}) = 0;
        virtual bool selectSound(NodeId node,uint32_t id,int16_t channel=-1) = 0;
        virtual bool discardSound(NodeId node,uint32_t id,bool cutTail=false) = 0;

        // Sound assets (docs/rfcs/0009): libraries of files that sounds use without copying them.
        struct AssetFolder { std::string library,directory; };      // a folder's place in a library
        struct AssetLibrary { std::string id,name,path;size_t files; };
        // The library containing folder (registering folder as a new library if none does).
        virtual AssetFolder registerAssetFolder(const std::string& folder) = 0;
        // Copies one file into the user library and returns its reference.
        virtual kick::clap::AssetReference importAsset(const std::string& file) = 0;
        virtual std::vector<AssetLibrary> assetLibraries() const = 0;

        virtual GraphState graphState() const = 0;
        virtual NodeId add( const std::string& name, const std::string& path, const std::string& pluginId = {} ) = 0;
        virtual NodeId addEffectContext(const std::string& name,NodeId source) = 0;
        virtual std::optional<oscairn_effect_resources_t> effectResources(NodeId node) const = 0;
        virtual void replace(NodeId node,const std::string& path,const std::string& pluginId,const std::string& preset = {}) = 0;
        virtual void remove( NodeId node ) = 0;
        virtual void connectAudio( NodeId from, NodeId to ) = 0;
        virtual void connectAudio( const AudioConnection& ) = 0;
        virtual void disconnectAudio( const AudioConnection& ) = 0;
        virtual void connectNotes( NodeId from, NodeId to ) = 0;
        virtual void disconnectAudio( NodeId from, NodeId to ) = 0;
        virtual void disconnectNotes( NodeId from, NodeId to ) = 0;
        // RFC 0011 parts and wires (parts.h). Part edits change only their part's wires.
        virtual uint32_t addPart( const std::string& name, PartInput input = {}, std::optional<uint32_t> output = {} ) = 0;
        virtual void removePart( uint32_t part ) = 0;
        virtual void insertIntoPart( uint32_t part, size_t index, NodeId node, ChainPlace place = {} ) = 0;
        virtual void removeFromPart( uint32_t part, NodeId node ) = 0; // wherever it is in the part
        virtual void movePartNode( uint32_t part, size_t from, size_t to, ChainPlace place = {} ) = 0; // a node or split entry
        // Splits (parts.h): a new one has two empty lanes. Kinds other than copy take their Split
        // (and, for left/right and mid/side, Join) node, already added; the split sets their mode
        // and removes them with itself. Removing one keeps its first lane's chain in its place;
        // other lanes' nodes leave the part. Removing the last lane removes the split. setLane
        // sets a lane's zones and Mix, not its chain.
        virtual uint32_t addSplit( uint32_t part, size_t index, ChainPlace place = {}, Split::Kind kind = Split::Kind::Copy, NodeId splitter = 0, NodeId joiner = 0 ) = 0;
        virtual void removeSplit( uint32_t part, uint32_t split ) = 0;
        virtual uint32_t addLane( uint32_t part, uint32_t split ) = 0; // its index
        virtual void removeLane( uint32_t part, uint32_t split, uint32_t lane ) = 0;
        virtual void setLane( uint32_t part, uint32_t split, uint32_t lane, const Lane& settings ) = 0;
        virtual void setPartInput( uint32_t part, const PartInput& input ) = 0;
        virtual void setPartMidiOut( uint32_t part, const PartMidiOut& midiOut ) = 0; // RFC 0012 step 1 (parts.h)
        // RFC 0033 (parts.h): the Performance's key and scale, and a part's own (none: the
        // Performance's). Plugins in a part read its key with the chord it holds (oscairn.harmony).
        virtual MusicalKey key() const = 0;
        virtual void setKey( const MusicalKey& key ) = 0;
        virtual void setPartKey( uint32_t part, const std::optional<MusicalKey>& key ) = 0;
        virtual void setPartOutput( uint32_t part, uint32_t bus, std::optional<uint32_t> port = {} ) = 0; // through a bus, or (bus 0) to a port
        virtual void setSend( uint32_t part, uint32_t bus, std::optional<double> level ) = 0;            // without a level, removes the send
        virtual void setPartLevel( uint32_t part, double level, bool mute ) = 0; // its fader, post-fader sends included
        virtual void setPartName( uint32_t part, const std::string& name ) = 0;
        virtual void setPartSolo( uint32_t part, bool solo ) = 0;
        virtual void setPartPan( uint32_t part, double pan ) = 0; // -1 left .. 1 right
        // The Performance's level (0..2, a gain) and pan (-1 left .. 1 right, a balance) on its main
        // output, the first audio output port: after every part, bus and user wire that plays there.
        virtual void setMaster( double level, double pan ) = 0;
        // Undo (RFC 0011): edits to parts, wires, splits, Mix settings, scenes, controls and pads,
        // nodes added, removed or replaced, parameter mappings, and parameter values set through
        // this interface. Playing, switching scenes, moving controls and choosing sounds are not
        // edits. An edit is everything up to a commit, or one change that needs none; repeated
        // changes to the same thing within a moment are one edit. The labels say what undo and
        // redo would do; empty: nothing to undo or redo. Opening a Performance clears them. The
        // history keeps at most 8 removed or replaced plugins alive; older steps go.
        struct History { std::string undo, redo; };
        virtual History history() const = 0;
        virtual bool undo() = 0; // false: nothing to undo
        virtual bool redo() = 0;
        // Each part's and bus's recent peak after its fader, held and falling, for the Mixer.
        virtual std::vector<PartMeter> partMeters() const = 0;
        // Each audio wire's recent peak after its level and pan, held and falling, for Routing.
        virtual std::vector<WireMeter> wireMeters() const = 0;
        virtual uint32_t connectPorts( const Wire& wire ) = 0; // a user wire; returns its ID
        virtual void disconnectWire( uint32_t wire ) = 0;
        // Switching plugins and wires off and on, as scenes do (RFC 0011). An off plugin takes no
        // new notes and no sound: notes it holds play on until released, with their releases and
        // pedal, and its sound rings out, then stops. In a part's chain, what reached an off plugin
        // also passes around it, so an effect off is bypassed. An off note wire takes no new notes;
        // an off audio wire is silent. Edits, like a level; staged until commit.
        virtual void setNodeEnabled( NodeId node, bool enabled ) = 0;
        virtual void setWireEnabled( uint32_t wire, bool enabled ) = 0;
        virtual void setMix( uint32_t wire, double level, double pan, bool mute, bool solo ) = 0;
        virtual void setZones( uint32_t wire, const std::vector<NoteZone>& zones ) = 0; // a note wire's zones

        // Scenes (parts.h). Adding or storing captures all of it as it is now; selecting applies the
        // kinds in the scene's scope at once and commits, plugins switched off ringing out.
        virtual uint32_t addScene( const std::string& name ) = 0;
        virtual void storeScene( uint32_t scene ) = 0;
        virtual void selectScene( uint32_t scene ) = 0;
        virtual void removeScene( uint32_t scene ) = 0;
        virtual void setSceneName( uint32_t scene, const std::string& name ) = 0;
        virtual void setSceneScope( uint32_t scene, uint32_t scope ) = 0; // Scene::Kind bits
        // How selecting it moves there (parts.h): glide in milliseconds (at most a minute), curve, launch.
        virtual void setSceneTransition( uint32_t scene, uint32_t glide, Scene::Curve curve, Scene::Launch launch ) = 0;
        // The plugin parameters scenes never touch (a sample's tuning, a level set by ear); scenes
        // touch every other automatable one. One taken back gets its current value in every scene.
        virtual void setSceneUntouched( const std::vector<SceneParameter>& parameters ) = 0;

        // The control tree (parts.h). Setting a control's value sets its targets, and their
        // targets in turn; an assignment that would loop is refused.
        virtual uint32_t addMacro( const std::string& name, uint32_t part = 0 ) = 0; // part: its owner; 0 the Performance
        virtual void removeMacro( uint32_t macro ) = 0;
        virtual void setMacroName( uint32_t macro, const std::string& name ) = 0;
        // A target newly relative takes its current value as base; one already relative keeps its base.
        virtual void setMacroTargets( uint32_t macro, const std::vector<MacroTarget>& targets ) = 0;
        virtual void setMacro( uint32_t macro, double value ) = 0;
        virtual void remove( NodeId node, UserWires userWires ) = 0;
        virtual void mapParameter( NodeId from, uint32_t lane, NodeId to, uint32_t param ) = 0;
        virtual void unmapParameter( NodeId from, uint32_t lane ) = 0;
        virtual void commit() = 0;
        /// Bounded history, independent cursor for every consumer. lost requires resync.
        virtual uint64_t changeSequence() const = 0;
        virtual Changes changesAfter( uint64_t sequence ) const = 0;

        virtual NodeId nodeId( const std::string& name ) const = 0;
        virtual bool hasNode( NodeId node ) const = 0;
        virtual bool hasParameter( NodeId node, uint32_t param ) const = 0;
        virtual std::vector<ParamInfo> parameters( NodeId node ) const = 0;
        virtual ParamInfo parameterInfo( NodeId node, uint32_t param ) const = 0;
        virtual double parameter( NodeId node, uint32_t param ) const = 0;
        virtual std::string parameterText( NodeId node, uint32_t param, double value ) const = 0;
        /// Accepted values coalesce until service(), then retry if the plugin is busy.
        /// False means the bounded pending-command store refused this new command.
        virtual bool setParameter( NodeId node, uint32_t param, double value ) = 0;
        virtual bool parameterGesture( NodeId node, uint32_t param, bool begin ) = 0;

        virtual void setPlaying( bool playing ) = 0;
        virtual void setTempo( double bpm ) = 0;
        virtual void setClockSource( Transport::ClockSource source ) = 0;
        virtual Transport::ClockSource clockSource() const = 0;
        // RFC 0012 step 2: MIDI clock (24 a beat), Start, Stop, Continue and Song Position from the
        // transport to the outputs these rules choose (none: no clock), resolved against the
        // session status's midiOutputs. The device's cabling: not saved with a Performance.
        virtual void setClockOutputs( const std::vector<DeviceRule>& devices ) = 0;
        /// Last rendered block's transport. Fields are atomic telemetry, not a transaction.
        virtual TransportState transportState() const = 0;

        /// UI notes at the next block boundary; channel is zero-based. Physical MIDI
        /// bypasses this queue and enters Session::process with sample offsets.
        /// False means overflow; the session discards queued UI notes and broadcasts
        /// release on the next block. This may release other voices on shared destinations.
        virtual bool note( bool on, int key, double velocity, int channel = 0 ) = 0;
        // All notes off (RFC 0030): every note played from the keys, MIDI inputs and the UI is
        // released at the next block. Any thread; never waits.
        virtual void allNotesOff() = 0;
        // MIDI inputs whose messages are left out, by their names in the session status (RFC 0024:
        // a controller's port that duplicates another's). Replaces the last list; empty: none.
        virtual void ignoreMidiInputs( const std::vector<std::string>& names ) = 0;

        // Pads (pads.h): the Performance's pad map, and playing a pad through it. A pad press
        // plays its assigned note, on its channel or else `channel`; its release ends the note
        // it started, even if the map or bank changed meanwhile.
        virtual PadMap padMap() const = 0;
        virtual void setPadMap( const PadMap& map ) = 0;
        virtual bool pad( uint32_t pad, bool on, double velocity, int channel = 0 ) = 0;
        virtual bool padPressure( uint32_t pad, double pressure ) = 0; // 0..1, as the pad's assignment sends it

        // Physical controls (RFC 0011, physical_controls.h): the Performance's overrides, by
        // the control's logical identity. Assigning Kind::None removes one; saved with it.
        virtual std::vector<AssignmentOverride> assignments() const = 0;
        virtual void assign( const std::string& control, const Assignment& target, uint32_t part = 0 ) = 0; // part: that part's Sound's own assignment

        // Recording (RFC 0020): a take records taps, each to its own file in a folder of its own
        // under the data folder's recordings/, from the next block until it is stopped. Taps:
        // a graph output or input port (port), a part after or before its fader (part), a
        // plugin's audio output port (node, port), and notes: those entering the graph, those a
        // part receives (part) or a plugin sends (node), one track each in the take's MIDI file.
        // No taps: Main and the notes entering the graph. One take at a time: throws
        // std::invalid_argument for an unknown tap, std::logic_error while another records.
        // `when` (RFC 0020 step 3): from now, seconds ago or a bar line (next or last; now with
        // the transport stopped), for a length or until stopped. A start in the past reads the
        // retroactive buffer and ends now unless keepGoing; std::out_of_range for a tap it does
        // not hold. A take with an end (a capture, a length) records beside the one recording.
        struct RecordTap
        {
            enum class Kind { Output, Input, Part, Node, Notes } kind = Kind::Output;
            uint32_t port = 0, part = 0;
            NodeId node = 0;
            bool preFader = false; // parts
        };
        using Take = Recorder::Take;
        using RecordWhen = Recorder::Start;
        virtual uint32_t recordStart( const std::vector<RecordTap>& taps = {}, const std::string& name = {}, const RecordWhen& when = {} ) = 0;
        virtual void recordStop( uint32_t take ) = 0; // the take ends now; its files close after
        // This run's takes and the closed ones found in recordings/, oldest first; earlier runs'
        // are numbered as found.
        virtual std::vector<Take> recordTakes() const = 0;
        // The retroactive buffer (RFC 0020 step 3): these taps (none: Main and the notes entering
        // the graph) kept in memory for the last `seconds`, 0 for none. Its rings are committed
        // memory (RFC 0025): std::length_error when they do not fit the budget beside the
        // Performance, std::logic_error while a take recording reads them.
        virtual void recordRetro( double seconds, const std::vector<RecordTap>& taps = {} ) = 0;
        virtual double recordRetroSeconds() const = 0;
        // A closed take: deleted with its folder; renamed (its folder, keeping the time it starts
        // with); played back into the outputs from its first audio file, after the taps, so no
        // take records it (0 stops). std::invalid_argument for no such take, std::logic_error
        // while it is written, std::length_error when its audio does not fit the memory budget.
        virtual void recordDelete( uint32_t take ) = 0;
        virtual void recordRename( uint32_t take, const std::string& name ) = 0;
        virtual void recordPlay( uint32_t take ) = 0;
    };
}
