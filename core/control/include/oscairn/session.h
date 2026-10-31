// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "oscairn/control.h"
#include "oscairn/assets.h"
#include "oscairn/parts.h"

#include <array>
#include <atomic>
#include <map>
#include <deque>
#include <span>

namespace oscairn
{
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
        Control& control() noexcept { return *this; }
        const Control& control() const noexcept { return *this; }

        // Edits are staged; a failed commit leaves the live plan unchanged and the
        // staging area available for correction. Empty names address nodes by ID only.
        void savePerformance(const std::string& directory) override;
        void loadPerformance(const std::string& directory) override;
        bool supportsState(NodeId node) const override {return graph_.host(node).supportsState();}
        void saveState(NodeId node,const std::string& directory) override;
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
        uint32_t addSplit(uint32_t part, size_t index, ChainPlace place = {}) override;
        void removeSplit(uint32_t part, uint32_t split) override;
        uint32_t addLane(uint32_t part, uint32_t split) override;
        void removeLane(uint32_t part, uint32_t split, uint32_t lane) override;
        void setLane(uint32_t part, uint32_t split, uint32_t lane, const Lane& settings) override;
        void setPartInput(uint32_t part, const PartInput& input) override;
        void setPartOutput(uint32_t part, uint32_t bus, std::optional<uint32_t> port = {}) override; // bus 0: a graph output port
        void setSend(uint32_t part, uint32_t bus, std::optional<double> level) override;            // no level: remove the send
        void setPartLevel(uint32_t part, double level, bool mute) override;
        void setPartName(uint32_t part, const std::string& name) override;
        const std::vector<Part>& parts() const { return parts_; }
        const std::vector<Wire>& wires() const { return wires_; }
        // A user wire between audio ports (channel rules or explicit pairs) or note endpoints.
        uint32_t connectPorts(const Wire& wire) override;
        void disconnectWire(uint32_t wire) override;
        // A source's level, pan, mute and solo in its destination input's implicit Mix.
        void setMix(uint32_t wire, double level, double pan, bool mute, bool solo) override;
        void setZones(uint32_t wire, const std::vector<NoteZone>& zones) override;
        uint32_t addScene(const std::string& name) override;
        void storeScene(uint32_t scene) override;
        void selectScene(uint32_t scene) override;
        void removeScene(uint32_t scene) override;
        void setSceneName(uint32_t scene, const std::string& name) override;
        void setSceneParameters(const std::vector<SceneParameter>& parameters) override;
        uint32_t addMacro(const std::string& name) override;
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
        bool hasNode( NodeId node ) const override { return graph_.contains( node ); }
        bool hasParameter( NodeId node, uint32_t param ) const override;
        std::vector<ParamInfo> parameters( NodeId node ) const override;
        ParamInfo parameterInfo( NodeId node, uint32_t param ) const override;
        double parameter( NodeId node, uint32_t param ) const override;
        std::string parameterText( NodeId node, uint32_t param, double value ) const override;
        bool setParameter( NodeId node, uint32_t param, double value ) override;
        bool parameterGesture( NodeId node, uint32_t param, bool begin ) override;

        void setPlaying( bool playing ) override { playing_.store( playing ); }
        void setTempo( double bpm ) override;
        void setClockSource( Transport::ClockSource source ) override { clockSource_.store( source ); }
        Transport::ClockSource clockSource() const override { return clockSource_.load(); }
        TransportState transportState() const override;
        bool note( bool on, int key, double velocity, int channel = 0 ) override;
        PadMap padMap() const override { return padMap_; }
        void setPadMap( const PadMap& map ) override;
        bool pad( uint32_t pad, bool on, double velocity, int channel = 0 ) override;
        bool padPressure( uint32_t pad, double pressure ) override;

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

        void panicInput() noexcept { graph_.panicInput(); } // audio thread only
        uint64_t droppedNotes() const noexcept { return droppedNotes_.load(); }
        double sampleRate() const noexcept { return sampleRate_; }
        uint32_t maxFrames() const noexcept { return maxFrames_; }

    private:
        static ParamInfo describe( const kick::clap::Parameter& param );
        static void compileWires(Graph& graph, std::vector<Wire>& wires);
        bool enqueue(const kick::clap::Event& event);
        void syncWires();
        void dropNode(NodeId node);
        template<class Edit> void editWires(Edit&& edit);
        void buildPart(const Part& part);
        void buildParts();
        struct BusEntry { NodeId node = 0; uint32_t port = 0; double level = 1, pan = 0; bool mute = false; uint32_t split = 0, lane = 0; };
        std::vector<BusEntry> busEntries(uint32_t bus);
        bool feeds(uint32_t from, uint32_t to);
        Part& findBus(uint32_t id);
        Scene& findScene(uint32_t id);
        Macro& findMacro(uint32_t id);
        double currentFraction(const MacroTarget& target) const;
        void driveMacro(uint32_t id, double value, bool& faders); // sets it and its targets
        void capture(Scene& scene) const;
        Part& findPart(uint32_t id);
        void removePairs(const AudioConnection& c);
        void remapPorts(NodeId node, const std::vector<kick::clap::Port>& oldInputs, const std::vector<kick::clap::Port>& oldOutputs,
                        const std::vector<uint32_t>& oldMainIn, const std::vector<uint32_t>& oldMainOut);
        const kick::clap::Parameter& findParameter( NodeId node, uint32_t param ) const;
        bool queue( NodeId node, uint32_t param, ChangeKind kind, double value );
        void publish( ChangeKind kind, NodeId node = 0, uint32_t param = 0, double value = 0, bool plugin = false );

        double sampleRate_;
        uint32_t maxFrames_;
        assets::Store assets_; // outlives the graph whose plugins resolve through it
        Graph graph_;
        SessionStatus hostStatus_;
        std::map<NodeId,std::string> nodes_;
        PadMap padMap_;
        struct HeldPad { int16_t key = -1; int8_t channel = 0; PadAssignment::Pressure pressure = PadAssignment::Pressure::None; };
        std::array<HeldPad, PadMap::pads> heldPads_{};
        std::vector<Part> parts_;
        std::vector<Wire> wires_;
        uint32_t nextPart_ = 1, nextWire_ = 1;
        std::vector<Scene> scenes_;
        std::vector<SceneParameter> sceneParameters_;
        uint32_t scene_ = 0, nextScene_ = 1;
        std::vector<Macro> macros_;
        uint32_t nextMacro_ = 1;
        bool staged_ = false;
        uint64_t revision_ = 0, sequence_ = 0;
        using Address = std::pair<NodeId,uint32_t>;
        struct Command { ChangeKind kind; double value; };
        std::map<Address,std::deque<Command>> pending_;
        size_t commandCount_ = 0;
        std::deque<Change> history_;
        std::map<Address,double> observed_;
        std::map<NodeId,uint64_t> outputDrops_;
        Transport transport_; // audio thread after construction
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
        std::array<kick::clap::Event, noteQueueCapacity + maxInputEvents + 1> merged_{};
        std::array<uint8_t, noteQueueCapacity + maxInputEvents + 1> mergedSources_{};
    };
}
