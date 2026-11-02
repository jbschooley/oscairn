// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "oscairn/control.h"
#include "oscairn/assets.h"
#include "oscairn/parts.h"
#include "oscairn/library.h"
#include "oscairn/signing.h"

#include <array>
#include <chrono>
#include <memory>
#include <atomic>
#include <map>
#include <deque>
#include <set>
#include <functional>
#include <span>
#include <tuple>

struct json_value_s; // sheredom/json.h: the document readers' parsed values

namespace oscairn
{
    struct Package; // package.h
    /// Device-independent graph session. Construct/configure/service/destroy on the
    /// control thread. One audio thread calls process(); stop it before destruction.
    /// The caller owns audio I/O, MIDI capture, clocks, output buffers and scheduling.
    class Session final : public Control
    {
    public:
        static constexpr uint32_t maxInputEvents = 256;
        static constexpr uint32_t maxClockEvents = 1024;
        static constexpr size_t noteQueueCapacity = 128;
        static constexpr size_t commandCapacity = 4096;
        static constexpr size_t historyCapacity = 4096;

        Session( double sampleRate, uint32_t maxFrames, Lv2BufferLimits lv2Buffers = {}, AudioLayout audio = {} );
        ~Session() override; // keeps this device's measurements (RFC 0025)
        Control& control() noexcept { return *this; }
        const Control& control() const noexcept { return *this; }

        // Edits are staged; a failed commit leaves the live plan unchanged and the
        // staging area available for correction. Empty names address nodes by ID only.
        void savePerformance(const std::string& directory) override;
        void setBudget(const Budget& budget) override;
        void setAlwaysAvailable(const std::vector<AppMemory>& apps) override;
        Launch launchApp(const AppMemory& app, bool dropNext = false) override;
        void appClosed(const std::string& app) override;
        MemoryState memoryState() override;
        Estimate estimate(const std::string& directory = {}) override;
        std::vector<std::string> exportPerformance(const std::string& file, bool editedSounds = false, const ExportSigning& sign = {}) override;
        std::vector<std::string> exportLibraryItem(const std::string& id, const std::string& file, bool editedSounds = false, const ExportSigning& sign = {}) override;
        std::vector<SigningKey> signingKeys() override;
        SigningKey makeSigningKey(const std::string& name) override;
        void loadPerformance(const std::string& directory) override;
        bool preparePerformance(const std::string& directory) override;
        PerformanceInfo performanceInfo() const override {return info_;}
        void setPerformanceInfo(const PerformanceInfo& info) override;
        LibraryResult libraryQuery(const LibraryQuery& query) override {return library().query(query);}
        void libraryFavorite(const std::string& id,bool favorite) override {library().setFavorite(id,favorite);}
        void libraryTag(const std::string& tag, bool hidden, const std::string& color) override {library().setTag(tag,hidden,color);}
        uint32_t libraryRescan() override {return library().rescan();}
        std::string savePerformanceToLibrary(const std::string& name, bool asNew = false) override;
        void loadPerformanceFromLibrary(const std::string& id) override;
        Archive archiveContents(const std::string& file) override;
        Installed installArchive(const std::string& file, bool asLibrary, const std::vector<std::string>& items = {}, bool acceptUnsignedCode = false) override;
        std::vector<InstalledLibrary> libraries() override;
        void uninstallLibrary(const std::string& library) override;
        void deleteLibraryItem(const std::string& item) override;
        std::vector<InstalledPlugin> installedPlugins() override;
        void removePlugin(const std::string& format, const std::string& id, const std::string& version, const std::string& platform = {}) override;
        void addPluginBuild(const std::string& format, const std::string& id, const std::string& version, const std::string& platform, const std::string& path) override;
        std::vector<TrustedKey> trustedKeys() override;
        void setTrustedKey(const TrustedKey& key) override;
        void removeTrustedKey(const std::string& id) override;
        void auditionStart(const std::string& id, uint32_t part = 0) override;
        void auditionStop(bool keep = false) override;
        Audition audition() const override { return audition_ ? audition_->what : Audition{}; }
        void libraryEdit(const std::string& id, const PerformanceInfo& info) override;
        std::string libraryCopy(const std::string& id) override;
        SoundSaved savePartAsSound(uint32_t part,const std::string& name,bool asNew = false) override;
        std::vector<std::string> loadSound(uint32_t part,const std::string& id) override;
        uint32_t partFromSound(const std::string& id) override;
        PerformanceContents performanceContents(const std::string& directory) override;
        Imported importPerformance(const std::string& directory, const std::vector<uint32_t>& parts = {}, const std::vector<uint32_t>& scenes = {}) override;
        bool supportsState(NodeId node) const override {return graph_.host(node).supportsState();}
        void saveState(NodeId node,const std::string& directory) override;
        bool hasPluginWindow(NodeId node) const override {return graph_.host(node).hasWindow();}
        void showPluginWindow(NodeId node,bool show,const std::string& title = {}) override {graph_.host(node).showWindow(show,title);}
        std::vector<PresetInfo> presets(NodeId node) override {return graph_.host(node).presets();}
        std::vector<PropertyInfo> properties(NodeId node) const override {return graph_.host(node).properties();}
        bool setProperty(NodeId node,const std::string& uri,const PropertyValue& value) override {return graph_.host(node).setProperty(uri,value);}
        bool requestProperty(NodeId node,const std::string& uri) override {return graph_.host(node).requestProperty(uri);}
        std::optional<PropertyValue> propertyValue(NodeId node,const std::string& uri) const override {return graph_.host(node).propertyValue(uri);}
        Sounds sounds(NodeId node) const override;
        assets::Store& assets() noexcept { return assets_; }
        uint32_t prepareSound(NodeId node,const std::string& name,const std::vector<oscairn_sound_parameter_t>& p,const std::string& state={},const std::string& preset={},const std::string& sfz={},const std::string& library={},const std::string& directory={}) override {return graph_.host(node).prepareSound(name,p,state,preset,sfz,library,directory);}
        AssetFolder registerAssetFolder(const std::string& folder) override {AssetFolder f;if(!assets_.locate(folder,f.library,f.directory))f.library=assets_.registerFolder(folder);return f;}
        kick::clap::AssetReference importAsset(const std::string& file) override {return assets_.import(file);}
        std::vector<AssetLibrary> assetLibraries() const override {std::vector<AssetLibrary> r;for(const auto& l:assets_.libraries())r.push_back({l.id,l.name,l.path,l.files});return r;}
        bool selectSound(NodeId node,uint32_t id,int16_t channel=-1) override {return graph_.host(node).selectSound(id,channel);}
        bool discardSound(NodeId node,uint32_t id,bool cutTail=false) override {return graph_.host(node).discardSound(id,cutTail);}
        GraphState graphState() const override;
        NodeId add( const std::string& name, const std::string& path, const std::string& pluginId = {} ) override;
        NodeId addEffectContext(const std::string& name,NodeId source) override;
        std::optional<oscairn_effect_resources_t> effectResources(NodeId node) const override {return graph_.host(node).effectResources();}
        void replace(NodeId node,const std::string& path,const std::string& pluginId,const std::string& preset = {}) override;
        void remove( NodeId node ) override { remove(node, UserWires::Remove); }
        // Audio and note connections are user wires (parts.h), compiled into graph routes.
        void connectAudio( NodeId from, NodeId to ) override;
        void connectAudio( const AudioConnection& c ) override;
        void disconnectAudio( const AudioConnection& c ) override;
        void connectNotes( NodeId from, NodeId to ) override;
        void disconnectAudio( NodeId from, NodeId to ) override;
        void disconnectNotes( NodeId from, NodeId to ) override;

        // RFC 0011 step 1: parts and wire ownership. Part edits change only their part's wires;
        // user wires stay until removed. Edits are staged, like graph edits.
        uint32_t addPart(const std::string& name, PartInput input = {}, std::optional<uint32_t> output = {}) override;
        void removePart(uint32_t part) override; // its wires; its nodes stay
        void insertIntoPart(uint32_t part, size_t index, NodeId node, ChainPlace place = {}) override;
        void removeFromPart(uint32_t part, NodeId node) override;
        void movePartNode(uint32_t part, size_t from, size_t to, ChainPlace place = {}) override;
        uint32_t addSplit(uint32_t part, size_t index, ChainPlace place = {}, Split::Kind kind = Split::Kind::Copy, NodeId splitter = 0, NodeId joiner = 0) override;
        void removeSplit(uint32_t part, uint32_t split) override;
        uint32_t addLane(uint32_t part, uint32_t split) override;
        void removeLane(uint32_t part, uint32_t split, uint32_t lane) override;
        void setLane(uint32_t part, uint32_t split, uint32_t lane, const Lane& settings) override;
        void setPartInput(uint32_t part, const PartInput& input) override;
        void setPartMidiOut(uint32_t part, const PartMidiOut& midiOut) override;
        MusicalKey key() const override { return key_; }
        void setKey(const MusicalKey& key) override;
        void setPartKey(uint32_t part, const std::optional<MusicalKey>& key) override;
        void setPartOutput(uint32_t part, uint32_t bus, std::optional<uint32_t> port = {}) override; // bus 0: a graph output port
        void setSend(uint32_t part, uint32_t bus, std::optional<double> level) override;            // no level: remove the send
        void setPartLevel(uint32_t part, double level, bool mute) override;
        void setPartName(uint32_t part, const std::string& name) override;
        void setPartSolo(uint32_t part, bool solo) override;
        void setPartPan(uint32_t part, double pan) override;
        void setMaster(double level, double pan) override;
        void setNodeRole(NodeId node, const std::string& role) override;
        History history() const override;
        bool undo() override;
        bool redo() override;
        /// Forgets every step, as opening a Performance does; a host calls it once its startup
        /// graph is built, so undo cannot take that apart.
        void clearHistory();
        std::vector<PartMeter> partMeters() const override;
        std::vector<WireMeter> wireMeters() const override;
        const std::vector<Part>& parts() const { return parts_; }
        const std::vector<Wire>& wires() const { return wires_; }
        // A user wire between audio ports (channel rules or explicit pairs) or note endpoints.
        uint32_t connectPorts(const Wire& wire) override;
        void disconnectWire(uint32_t wire) override;
        void setNodeEnabled(NodeId node, bool enabled) override;
        void setWireEnabled(uint32_t wire, bool enabled) override;
        // A source's level, pan, mute and solo in its destination input's implicit Mix.
        void setMix(uint32_t wire, double level, double pan, bool mute, bool solo) override;
        void setZones(uint32_t wire, const std::vector<NoteZone>& zones) override;
        uint32_t addScene(const std::string& name) override;
        void storeScene(uint32_t scene) override;
        void selectScene(uint32_t scene) override;
        void removeScene(uint32_t scene) override;
        void setSceneName(uint32_t scene, const std::string& name) override;
        void setSceneUntouched(const std::vector<SceneParameter>& parameters) override;
        void setSceneScope(uint32_t scene, uint32_t scope) override;
        void setSceneTransition(uint32_t scene, uint32_t glide, Scene::Curve curve, Scene::Launch launch) override;
        uint32_t addMacro(const std::string& name, uint32_t part = 0) override;
        void removeMacro(uint32_t macro) override;
        void setMacroName(uint32_t macro, const std::string& name) override;
        void setMacroTargets(uint32_t macro, const std::vector<MacroTarget>& targets) override;
        void setMacro(uint32_t macro, double value) override;
        void remove(NodeId node, UserWires userWires) override;
        void mapParameter( NodeId from, uint32_t lane, NodeId to, uint32_t param ) override { graph_.mapParamTarget( from, lane, to, param ); staged_ = true; }
        void unmapParameter( NodeId from, uint32_t lane ) override { graph_.unmapParamTarget( from, lane ); staged_ = true; }
        void commit() override;
        /// Call at each control tick (desktop: 5 ms), never on the audio thread.
        void service();
        SessionStatus sessionStatus() const override;
        /// From the host's main loop. A changed list of MIDI inputs re-resolves the parts'
        /// device selectors, applied at once unless edits are staged (then with them).
        void setHostStatus(SessionStatus status);
        uint64_t changeSequence() const override { return sequence_; }
        Changes changesAfter( uint64_t sequence ) const override;

        NodeId nodeId( const std::string& name ) const override;
        bool hasNode( NodeId node ) const override { return graph_.contains( node ) && !leaving_.count( node ); } // the last Performance's plugins ringing out are not this one's
        bool hasParameter( NodeId node, uint32_t param ) const override;
        std::vector<ParamInfo> parameters( NodeId node ) const override;
        ParamInfo parameterInfo( NodeId node, uint32_t param ) const override;
        double parameter( NodeId node, uint32_t param ) const override;
        std::string parameterText( NodeId node, uint32_t param, double value ) const override;
        bool setParameter( NodeId node, uint32_t param, double value ) override; // an edit (undo.cpp)
        bool parameterGesture( NodeId node, uint32_t param, bool begin ) override;

        void setPlaying( bool playing ) override { playing_.store( playing ); }
        void setTempo( double bpm ) override;
        void setClockSource( Transport::ClockSource source ) override { clockSource_.store( source ); }
        Transport::ClockSource clockSource() const override { return clockSource_.load(); }
        void setClockOutputs( const std::vector<DeviceRule>& devices ) override;
        TransportState transportState() const override;
        uint32_t recordStart( const std::vector<RecordTap>& taps = {}, const std::string& name = {}, const RecordWhen& when = {} ) override;
        void recordStop( uint32_t take ) override;
        std::vector<Take> recordTakes() const override;
        void recordRetro( double seconds, const std::vector<RecordTap>& taps = {} ) override;
        double recordRetroSeconds() const override { return recorder_.retroSeconds(); }
        void recordDelete( uint32_t take ) override;
        void recordRename( uint32_t take, const std::string& name ) override;
        void recordPlay( uint32_t take ) override;
        Recorder& recorder() noexcept { return recorder_; } // its ring length, disk reserve and test hooks
        bool note( bool on, int key, double velocity, int channel = 0 ) override;
        void allNotesOff() override { panic_.store( true, std::memory_order_release ); } // the queue overflow's emergency release
        void ignoreMidiInputs( const std::vector<std::string>& names ) override;
        PadMap padMap() const override { return padMap_; }
        void setPadMap( const PadMap& map ) override;
        bool pad( uint32_t pad, bool on, double velocity, int channel = 0 ) override;
        bool padPressure( uint32_t pad, double pressure ) override;
        std::vector<AssignmentOverride> assignments() const override;
        void assign( const std::string& control, const Assignment& target, uint32_t part = 0 ) override;

        struct MidiClockEvent
        {
            uint8_t status = 0;
            int64_t sampleTime = 0; // absolute sample time, in an already rendered block
            uint16_t songPosition = 0;
        };
        /// Audio thread only. Stereo planar output, 1..maxFrames frames. Direct events
        /// must be sorted, with offsets within this block (at most maxInputEvents).
        /// MIDI-clock events (at most maxClockEvents) describe the preceding interval,
        /// as Transport requires.
        /// UI notes precede direct events at sample 0. No wall clock, locks or allocation.
        /// `sources`, empty or one per direct event, says which MIDI input each came from
        /// (note_zone.h); UI notes are built in, direct events without one untagged.
        bool process( float* const* outputs, uint32_t frames,
                      std::span<const kick::clap::Event> events = {},
                      std::span<const MidiClockEvent> clocks = {},
                      std::span<const uint8_t> sources = {} ) noexcept;

        bool process(const float* const* inputs, uint32_t inputChannels, float* const* outputs, uint32_t outputChannels,
                     uint32_t frames, std::span<const kick::clap::Event> events = {},
                     std::span<const MidiClockEvent> clocks = {}, std::span<const uint8_t> sources = {}) noexcept;

        /// Audio thread, after process(): the block's MIDI for hardware outputs (RFC 0012), the
        /// parts', in time order; each message's ports are the output numbers in the session
        /// status's midiOutputs, a bit each.
        std::span<const Graph::MidiMessage> midiOutput() const noexcept { return { midiOut_.data(), midiOutCount_ }; }
        void panicInput() noexcept { graph_.panicInput(); } // audio thread only
        uint64_t droppedNotes() const noexcept { return droppedNotes_.load(); }
        double sampleRate() const noexcept { return sampleRate_; }
        size_t leaving() const noexcept { return leaving_.size(); } // the last Performance's plugins still ringing out
        // RFC 0025: memory runs short (Linux PSI, polled at each service with memory classes):
        // one more item let go in the eviction order, a next entry prepared to cut, then, since
        // the estimates were wrong, a committed one. False when nothing was left to let go.
        bool memoryPressure();
        size_t heldRemoved() const noexcept { return graph_.heldRemoved(); } // removed plugins not freed yet
        uint32_t maxFrames() const noexcept { return maxFrames_; }

    private:
        static ParamInfo describe( const kick::clap::Parameter& param );
        // off: plugins switched off; rungOut: those of them whose notes and sound have ended.
        // leaving: the last Performance's plugins ringing out (RFC 0021); their routes are left alone.
        static void compileWires(Graph& graph, std::vector<Wire>& wires, const std::set<NodeId>& off, const std::set<NodeId>& rungOut, const std::set<NodeId>& leaving, double masterLevel, double masterPan);
        void enable(NodeId node, bool enabled);  // the flags and the graph's retiring count; wires are the caller's
        void ringOut();                          // service(): an off plugin that has rung out stops sounding
        bool enqueue(const kick::clap::Event& event);
        void syncWires();
        std::vector<Graph::MidiTap> midiTaps(const Graph& graph, const std::vector<Part>& parts, const std::vector<Wire>& wires) const; // parts' MIDI out
        std::vector<Graph::Harmony> harmonies(const std::vector<Part>& parts, const MusicalKey& key) const; // parts' oscairn.harmony (RFC 0033)
        void clock(const clap_event_transport_t& t, uint32_t frames) noexcept; // process(): clock out
        void dropNode(NodeId node);
        template<class Edit> void editWires(Edit&& edit);
        void buildPart(const Part& part);
        void buildParts();
        struct BusEntry { NodeId node = 0; uint32_t port = 0; double level = 1, pan = 0; bool mute = false; uint32_t split = 0, lane = 0; bool fader = false; std::vector<WireVia> via{}; }; // fader: passes an empty bus's fader; via: the empty buses on the way
        std::vector<BusEntry> busEntries(uint32_t bus);
        bool splitNode(NodeId node) const;       // a split's Split or Join node
        bool heard(const Part& part);            // not silenced by another part's solo
        void meter();                            // service(): updates the meters from the graph's peaks
        // Resource budgets (budget.cpp, RFC 0025): this device's measurements of each plugin build,
        // kept in the data folder once a budget is set.
        // cpu: the busiest block seen; rest: with no note sounding; voice: added per sounding note;
        // voices: the most notes seen sounding at once. All shares of a block's time.
        struct Measurement { double cpu = 0, rest = 0, voice = 0; uint32_t voices = 0; uint64_t memory = 0; };
        Budget budget_;
        std::map<std::string, Measurement> measured_;          // by plugin ID, version and build digest, and what it declared (pluginKey)
        bool measurementsLoaded_ = false, measurementsChanged_ = false;
        std::chrono::steady_clock::time_point measurementsSaved_{};
        uint64_t measureTicks_ = 0;
        std::map<NodeId, uint64_t> loadedBytes_;               // this process's growth as an in-process plugin loaded
        std::atomic<uint32_t> blockFrames_{0};                 // the audio thread's last block
        void measure();                                        // service()
        void loadMeasurements();
        void saveMeasurements();
        std::string pluginKey(NodeId node) const { return pluginKey(graph_, node); }
        std::string pluginKey(const Graph& graph, NodeId node) const;
        // limit: the notes its part may play at once (RFC 0018), 0 for none.
        Cost costOf(const std::string& key, const std::string& name, const std::string& plugin, uint32_t limit = 0) const;
        // A loaded plugin's cost: its measurement, its memory the larger of that and what it
        // declared (oscairn.memory); an effect context made from another counts its own bytes.
        Cost nodeCost(const Graph& graph, NodeId node, const std::string& name = {}, uint32_t limit = 0) const;
        // What the memory budget leaves for one more plugin to declare beside these (0: no memory budget).
        uint64_t memoryRoom(const Graph& graph, const std::vector<NodeId>& beside) const;
        uint64_t memoryRoom() const;                            // beside the open Performance
        // Memory classes (RFC 0025): the apps reserved, a next entry prepared only to cut, and the arithmetic.
        std::vector<AppMemory> alwaysAvailable_, running_;
        bool preparedOpportunistic_ = false;
        bool memoryClasses() const { return budget_.cpu > 0 && budget_.device > 0; }
        uint64_t reservedMemory(bool closedAlwaysAvailable = true) const; // system and apps; with or without closed always-available ones
        uint64_t memoryCeiling() const;                                     // the committed share, or the flat memory budget
        uint64_t preparedMemory() const;                                    // the prepared next entry's plugins
        uint64_t committedMemory() const;                                   // open, ringing, a committed next entry
        void evictOpportunistic();                                          // until it fits what reservations and commitments leave
        int pressureFd_ = -1;                                               // Linux: a PSI trigger on memory pressure
        void armPressure(bool on);
        void pollPressure();
        uint32_t noteLimit(NodeId node) const; // its part's, or 0
        Estimate estimateOf(const std::vector<Cost>& plugins, const std::vector<Cost>& playing) const;
        std::string tooMuch(const Estimate& e) const;          // what does not fit, for a refusal
        std::map<uint32_t, std::vector<double>> meters_, wireMeters_; // per part and per wire, per channel

        // Undo (undo.cpp). The model: what an edit can change, with the nodes it holds.
        struct Model
        {
            std::vector<Part> parts;
            std::vector<Wire> wires;
            std::vector<Scene> scenes;
            std::vector<SceneParameter> untouched;
            std::vector<Macro> macros;
            PadMap pads;
            std::vector<AssignmentOverride> assignments;
            std::vector<Graph::Mapping> mappings;
            std::map<NodeId, std::pair<std::string, Graph::Node>> nodes;
            std::set<NodeId> disabled;
            MusicalKey key;
            std::pair<double, double> master{1, 0};
            std::map<NodeId, std::string> roles;
        };
        struct Step
        {
            std::string label, key; // key: the one thing it changed, for merging; empty when several
            std::chrono::steady_clock::time_point when;
            std::shared_ptr<const Model> before, after; // a model step; otherwise a parameter step:
            NodeId node = 0; uint32_t param = 0; double from = 0, to = 0;
        };
        // Changes made while one is alive are not edits (switching scenes, moving controls).
        struct Quiet
        {
            Session& s;
            explicit Quiet(Session& session);
            ~Quiet();
        };
        Model model() const;
        static std::pair<std::string, std::string> summarize(const Model& before, const Model& after);
        void record();                                         // a step for what changed since the baseline
        void markEdited(const Model& before);                  // parts from Sounds that the step since `before` changed
        void markEdited(NodeId node);                          // the part from a Sound holding this plugin, after a parameter step
        std::set<uint32_t> loading_;                           // parts a Sound is being loaded into: that step is no edit of it
        void push(Step step);                                  // merging it into the last when they change the same thing
        void apply(const Model& from, const Model& to);        // the change from one to the other, onto now
        bool applyParameter(NodeId node, uint32_t param, double value, double* applied = nullptr, bool bounded = true); // not an edit; bounded: see queue
        std::vector<Step> undo_, redo_;
        std::shared_ptr<const Model> baseline_;
        // An audition: what it set aside (the model before it, holding those plugins) and what
        // stopping puts back that the model does not hold.
        struct Auditioning
        {
            Audition what;
            std::shared_ptr<const Model> before;
            std::vector<Step> undo, redo;
            PerformanceInfo info;
            double tempo = 120;
            Transport::ClockSource clock = Transport::ClockSource::Internal;
            uint32_t scene = 0, mappingScene = 0;
        };
        std::optional<Auditioning> audition_;
        int quiet_ = 0;
        bool mergeable_ = false; // the last step may take in the next
        void dropSplitNodes(const Split& split); // removes them from the graph, with their wires
        bool feeds(uint32_t from, uint32_t to);
        Part& findBus(uint32_t id);
        Scene& findScene(uint32_t id);
        Macro& findMacro(uint32_t id);
        double currentFraction(const MacroTarget& target) const;
        // The summing rule (RFC 0011): every control's offset into t's destination, `macro` taken
        // at `value`; the destination's base; and noting where the controls left each destination.
        // The value is the base plus each relative target's offset plus each absolute target's point
        // less the base: `fixed` + (1 - `absolute`) x base.
        struct Sum
        {
            double fixed = 0; int absolute = 0;
            double at(double base) const { return fixed + (1 - absolute) * base; }
            // The base that gives `value`; with one absolute target the base does not count, and stays.
            double baseFor(double value, double base) const { return absolute == 1 ? base : (value - fixed) / (1 - absolute); }
        };
        Sum contributions(const MacroTarget& t, uint32_t macro, double value) const;
        void setBase(const MacroTarget& t, double base);
        void noteBases(); // after controls arrive (a load, a Sound, an import): one base per destination, as it plays now
        using Destination = std::tuple<int, NodeId, uint32_t, uint32_t>;
        static Destination destination(const MacroTarget& t) { return {int(t.kind), t.node, t.param, t.part}; }
        std::map<Destination, double> written_; // what the controls last set each destination to; anything else moving it since rebases it
        void driveMacro(uint32_t id, double value, bool& faders); // sets it and its targets
        void capture(Scene& scene) const;
        // Scenes moving (parts.cpp): applied at once, blended between two, a glide's steps and a morph.
        void applyScene(const Scene& scene, uint32_t scope);
        void blendScenes(const Scene& from, const Scene& to, double t, Scene::Curve curve, double switchAt, uint32_t scope);
        void startScene(const Scene& scene);
        void advanceScenes(); // service()
        void morphScenes(uint32_t from, uint32_t to, double t);
        std::vector<std::pair<double, bool>> faderLevels() const; // each part's level and mute, to notice a hand on a fader
        struct Transition { Scene from, to; uint64_t start = 0, frames = 1; std::vector<std::pair<double, bool>> applied; };
        std::optional<Transition> transition_;
        struct PendingScene { uint32_t scene = 0; double at = 0; }; // waiting for beat `at`
        std::optional<PendingScene> pendingScene_;
        bool applyingScene_ = false;
        std::atomic<uint64_t> framesProcessed_{0}; // the audio thread's count, for glides
        bool touched(NodeId node, const ParamInfo& param) const; // a parameter scenes touch
        Part& findPart(uint32_t id);
        void removePairs(const AudioConnection& c);
        void remapPorts(NodeId node, const std::vector<kick::clap::Port>& oldInputs, const std::vector<kick::clap::Port>& oldOutputs,
                        const std::vector<uint32_t>& oldMainIn, const std::vector<uint32_t>& oldMainOut);
        const kick::clap::Parameter& findParameter( NodeId node, uint32_t param ) const;
        // bounded: refused once commandCapacity commands wait. A scene's values are not: each parameter
        // keeps at most one waiting value, so they never outnumber the parameters.
        bool queue( NodeId node, uint32_t param, ChangeKind kind, double value, bool bounded = true );
        void publish( ChangeKind kind, NodeId node = 0, uint32_t param = 0, double value = 0, bool plugin = false );

        double sampleRate_;
        uint32_t maxFrames_;
        assets::Store assets_; // outlives the graph whose plugins resolve through it
        Graph graph_;
        SessionStatus hostStatus_;
        std::map<NodeId,std::string> nodes_;
        PadMap padMap_;
        MusicalKey key_; // the Performance's (RFC 0033)
        struct HeldPad { int16_t key = -1; int8_t channel = 0; PadAssignment::Pressure pressure = PadAssignment::Pressure::None; };
        std::array<HeldPad, PadMap::pads> heldPads_{};
        std::vector<Part> parts_;
        std::vector<Wire> wires_;
        std::set<NodeId> disabled_, rungOut_; // plugins switched off, and those of them done sounding
        std::set<NodeId> leaving_;            // the last Performance's plugins, ringing out after a change; not part of this one
        uint32_t nextPart_ = 1, nextWire_ = 1;
        std::vector<Scene> scenes_;
        std::vector<SceneParameter> untouched_; // parameters scenes never touch
        uint32_t scene_ = 0, nextScene_ = 1;
        uint32_t mappingScene_ = 0; // the scene last selected that set the mappings: those off in it are off
        std::vector<Macro> macros_;
        double masterLevel_ = 1, masterPan_ = 0; // the Performance's main output
        std::map<NodeId, std::string> roles_;   // RFC 0016's node roles
        std::vector<std::string> notices_;      // SessionStatus::notices
        std::vector<AssignmentOverride> assignments_; // physical controls' overrides (physical_controls.cpp)
        uint32_t nextMacro_ = 1;
        bool staged_ = false;
        PerformanceInfo info_; // the open Performance's; its ID is made at its first save
        std::unique_ptr<Library> library_; // made at first use, so a session without the library never reads the user's folders
        Library& library() {if(!library_)library_=std::make_unique<Library>();return *library_;}
        PerformanceInfo writeDocument(const std::string& directory,PerformanceInfo info,const Part* sound,std::vector<std::string>* left);
        std::vector<std::string> loadSoundInto(uint32_t part,const std::filesystem::path& folder);
        // A document's nodes (a Performance's or a Sound's), each its saved plugin or, not here, its stand-in.
        std::map<std::string,NodeId> addNodes(const ::json_value_s* document,const std::filesystem::path& root,std::vector<NodeId>& added,const std::set<std::string>* only); // only: these keys (and their engines); null: all
        NodeId adopt(const std::string& name, std::shared_ptr<PluginHost> host); // a stand-in (RFC 0006), with no budget check
    public:
        // A plugin build a document was saved with (RFC 0006), for one platform.
        struct LockEntry { std::string format, id, version, platform, sha256; bool operator==(const LockEntry&) const = default; };
    private:
        std::vector<LockEntry> lock_;                       // what the documents loaded said, merged
        // Each plugin build's digest by its path, as of its newest write, last looked at when checked.
        struct Digest { std::filesystem::file_time_type newest; std::string digest; std::chrono::steady_clock::time_point checked; };
        mutable std::map<std::string, Digest> digests_;
        // The digest (bundleDigest) of the build at path; empty where there is none. Its writes are
        // looked at again after a second, or now when fresh.
        std::string buildDigest(const std::string& path, bool fresh = false) const;
        void lockPlugin(std::vector<LockEntry>& lock, const std::string& format, const std::string& id, const std::string& version, const std::string& path, bool standIn);
        void mergeLock(const std::vector<LockEntry>& more);
        // A Performance read and its plugins loaded with their state, not playing yet (RFC 0021).
        struct Prepared
        {
            std::filesystem::path root;
            std::filesystem::file_time_type stamp;
            std::unique_ptr<Graph> next;
            PadMap pads;
            PerformanceInfo info;
            std::vector<LockEntry> lock;
            double tempo = 120;
            Transport::ClockSource clock = Transport::ClockSource::Internal;
            std::set<NodeId> disabled;
            std::map<NodeId,std::string> names;
            std::vector<Wire> wires;
            std::vector<Part> parts;
            uint32_t nextPart = 1, nextWire = 1;
            std::vector<Scene> scenes;
            std::vector<SceneParameter> untouched;
            uint32_t nextScene = 1, selectedScene = 0, mappingScene = 0;
            std::vector<Macro> macros;
            uint32_t nextMacro = 1;
            std::vector<AssignmentOverride> assignments;
            MusicalKey key;
            double masterLevel = 1, masterPan = 0;
            std::map<NodeId,std::string> roles;
            std::vector<std::string> notices;
        };
        std::unique_ptr<Prepared> read(const std::filesystem::path& root);
        void open(Prepared& prepared, bool seamless);
        std::unique_ptr<Prepared> prepared_; // the next one, prepared ahead; after graph_, so it goes first
        std::set<NodeId> nodesOf(const Part& part) const;
        static std::vector<assets::Ref> assetsOf(const std::filesystem::path& manifest);
        std::vector<std::string> exportArchive(const std::string& file, const std::string& kind, const std::function<std::vector<PartSource>(const std::filesystem::path&, Package&)>& write, bool editedSounds, const ExportSigning& sign);
        static signing::Key signingKey(const std::string& name); // Keys/<name>.key in the data folder
        static Signer judge(const std::string& kind, const std::vector<signing::Signature>& signatures); // against the trust store
        uint64_t revision_ = 0, sequence_ = 0;
        using Address = std::pair<NodeId,uint32_t>;
        struct Command { ChangeKind kind; double value; };
        std::map<Address,std::deque<Command>> pending_;
        size_t commandCount_ = 0;
        std::deque<Change> history_;
        std::map<Address,double> observed_;
        std::map<NodeId,uint64_t> outputDrops_;
        std::map<NodeId,uint64_t> parameterRevisions_; // each plugin's parameter info as last seen
        Transport transport_; // audio thread after construction
        // Recording (RFC 0020): the taps asked for, compiled again when the wires change.
        Recorder recorder_;
        std::vector<std::pair<uint32_t, std::vector<RecordTap>>> recordTaps_; // each live take's, by take
        std::vector<RecordTap> retroTaps_;
        std::vector<Wire> recordWires_;
        uint64_t recordRevision_ = 0;
        Recorder::Taps compileTaps( const std::vector<RecordTap>& taps, bool strict ); // parts.cpp; strict: an unknown tap throws
        std::vector<RecordTap> defaultTaps() const;
        void admitRecording( uint64_t adding, uint64_t freeing ) const; // std::length_error past the memory budget
        Take closedTake( uint32_t take ) const;
        // Takes found in recordings/, read again when the folder changes; numbers given to those found.
        mutable std::vector<Take> scanned_;
        mutable std::filesystem::file_time_type scannedAt_{};
        mutable std::map<std::string, uint32_t> takeIds_;
        mutable uint32_t nextFound_ = 1u << 24; // apart from this run's own numbers
        Transport::ClockSource following_ = Transport::ClockSource::Internal;
        std::atomic<bool> playing_{ false };
        std::atomic<double> tempo_{ 120.0 };
        std::atomic<Transport::ClockSource> clockSource_{ Transport::ClockSource::Internal };
        std::atomic<bool> statusPlaying_{ false };
        std::atomic<double> statusTempo_{ 120.0 }, statusBeat_{ 0.0 };

        // Single producer (control), single consumer (audio). A process call drains
        // only the published snapshot, so a busy producer cannot extend its deadline.
        std::array<kick::clap::Event, noteQueueCapacity> notes_{};
        std::atomic<size_t> noteHead_{ 0 }, noteTail_{ 0 };
        std::atomic<bool> panic_{ false };
        std::atomic<uint64_t> droppedNotes_{ 0 };
        std::vector<std::string> ignoredInputs_;      // control thread
        std::atomic<uint64_t> ignoredSources_{ 0 };   // their sources now, a bit each, for the audio thread
        // MIDI for hardware outputs (midiOutput), audio thread.
        std::array<Graph::MidiMessage, Graph::maxMidiOut + 8> midiOut_{};
        uint32_t midiOutCount_ = 0;
        // Clock out (setClockOutputs): its outputs, and on the audio thread whether the transport
        // played at the last block, where that block ended, the next pulse's number, and the
        // outputs the last block went to (one added is started, one dropped is stopped).
        std::vector<DeviceRule> clockOutputs_;          // control thread
        std::atomic<uint64_t> clockPorts_{ 0 };         // their outputs now, a bit each
        bool clockPlaying_ = false;
        uint64_t lastClockPorts_ = 0;
        double clockEnd_ = 0;
        int64_t nextPulse_ = 0;
        std::array<kick::clap::Event, noteQueueCapacity + maxInputEvents + 1> merged_{};
        std::array<uint8_t, noteQueueCapacity + maxInputEvents + 1> mergedSources_{};
    };
}
