// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "oscairn/plugin_host.h"
#include "oscairn/note_zone.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <set>
#include <span>
#include <memory>
#include <optional>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace oscairn
{
    struct NoteRoute;
    using NodeId = uint32_t;

    /// The graph's own endpoints: external audio and note events (keyboard, pads, MIDI) enter
    /// at graphInput; graphOutput exposes the configured playback ports.
    constexpr NodeId graphInput  = 0xFFFFFFFEu;
    constexpr NodeId graphOutput = 0xFFFFFFFFu;

    struct AudioEndpoint
    {
        NodeId node;
        uint32_t port = 0, channel = 0; // stable plugin port ID, channel within that port
        bool operator==(const AudioEndpoint&) const = default;
    };
    struct AudioConnection
    {
        AudioEndpoint from, to;
        uint32_t channels = 1; // contiguous range, contained in each selected port
        double gain = 1;
        // Live gains (Graph::setLiveGain) whose product scales it each block, by slot; 0: none.
        std::array<uint8_t, 4> live{};
        bool operator==(const AudioConnection&) const = default;
    };
    struct AudioLayout
    {
        std::vector<kick::clap::Port> inputs;
        std::vector<kick::clap::Port> outputs{{0,"Main",2,0}};
    };

    /// A graph of plugin nodes with audio and note connections, processed in
    /// dependency order within each block, so a sequencer's notes reach the instruments
    /// it plays in the same block. Audio connected into a node is summed; notes from
    /// several sources are merged in time order. Explicit audio routes preserve independent
    /// ports/channels. Node connections resolve the main bus and center mono into stereo.
    ///
    /// Editing (add, remove, connect, commit) happens on the main thread while the audio
    /// thread keeps processing. commit() compiles the edits into a plan and hands it to
    /// the audio thread without locking; the audio thread hands replaced plans back, and
    /// they are freed on the main thread (by commit() or collect()). Plugins therefore
    /// are always created and destroyed on the main thread, and keep their state when
    /// the graph around them changes.
    class Graph
    {
    public:
        static constexpr uint32_t channels = 2; // default stereo convenience API
        static constexpr uint32_t maxAudioChannels = 256, maxAudioConnections = 4096;

        Graph( double sampleRate, uint32_t maxFrames, ProcessingMode mode = ProcessingMode::Realtime, Lv2BufferLimits lv2Buffers = {}, AudioLayout audio = {} );
        ~Graph();

        Graph( const Graph& )            = delete;
        Graph& operator=( const Graph& ) = delete;

        // Main thread -------------------------------------------------------------

        /// Loads a CLAP module or LV2 bundle as a new node. Multiple-plugin LV2 bundles
        /// require pluginId (the URI); CLAP defaults to the module's first plugin.
        /// Throws std::runtime_error on failure. memoryRoom: the bytes the plugin may declare when
        /// it activates (oscairn.memory, RFC 0025); 0: no limit.
        NodeId add( const std::string& path, const std::string& pluginId = {}, uint64_t memoryRoom = 0 );
        /// Validates and commits replacement atomically; failure preserves the current graph. The
        /// node's routes and note edges are removed; rewire, called with the new plugin in place,
        /// connects what it wants before the commit (the Session moves its wires, RFC 0011).
        void replace(NodeId node,const std::string& path,const std::string& pluginId,const std::string& preset,const std::function<void()>& rewire);
        void   remove( NodeId node );
        /// As remove, but the caller keeps the plugin (an instance kept warm, RFC 0021); a plan
        /// still using it holds it too, and its last owner frees it on the main thread.
        std::shared_ptr<PluginHost> release( NodeId node );
        NodeId addEffectContext(NodeId source);
        /// A host already made, such as a plugin's stand-in when it is not here (RFC 0006).
        NodeId add( std::shared_ptr<PluginHost> host );
        /// A context of another graph's engine (a replacement taking over a playing engine, RFC 0021).
        NodeId addEffectContext(const Graph& graph, NodeId source);
        /// The first node sharing this one's engine, not counting `except`.
        NodeId effectSource(NodeId node, const std::set<NodeId>& except = {}) const;
        std::unique_ptr<Graph> prepareReplacement() const;
        /// Seamless Performance changes (RFC 0021), on a prepared replacement: `current`'s nodes
        /// `ids` come along under their IDs, retiring. The sound they send to the outputs and to
        /// each other stays; the note connections into them drain through the same routes, so
        /// held notes are still released; nothing else reaches them. The caller removes each once
        /// idle(). Their IDs are below the replacement's, so none is taken.
        void carryOver( const Graph& current, const std::set<NodeId>& ids );
        /// On a prepared replacement, after carryOver and its own note connections: `node` holds the
        /// instance `current`'s `from` holds (a native instrument whose next sounds were prepared in it,
        /// RFC 0021). The notes `from` holds go on through `node`: its connection from graphInput takes
        /// over the old one's ledger, so held notes keep their releases and pedal, and connections from
        /// nodes carried over come along draining.
        void takeNotes( const Graph& current, NodeId from, NodeId node );
        /// A replacement prepared ahead keeps its node IDs: nodes added here meanwhile take later ones.
        void reserveIds( const Graph& prepared ) { nextId_ = std::max( nextId_, prepared.nextId_ ); }
        // Where plugins added from now on resolve sound assets (RFC 0009); must outlive the graph.
        void setAssets(kick::clap::AssetResolver* assets) noexcept { assets_ = assets; }
        // onPublish must not throw; called after successful compilation/allocation, before publication.
        void publishReplacement(Graph& prepared,const std::function<void()>& onPublish);
        // Audio thread: adopt once before Session computes transport for this block.
        bool beginBlock() noexcept; // true: a replacement was adopted

        /// Connections take effect at the next commit(). Throw std::invalid_argument if
        /// the source cannot send or the destination cannot receive that kind of signal.
        void connectAudio( NodeId from, NodeId to );
        /// Explicit port IDs and channel ranges; gains sum without normalization.
        /// Ranges expand to unique channel pairs. Reconnecting updates gain.
        /// Disconnect removes the selected pairs regardless of gain.
        void connectAudio( const AudioConnection& connection );
        void disconnectAudio( const AudioConnection& connection );
        /// With zones (at most NoteZones::capacity), only the notes a zone accepts pass, changed
        /// as it says; reconnecting replaces the zones and keeps held notes' releases.
        void connectNotes( NodeId from, NodeId to, const std::vector<NoteZone>& zones = {} );
        /// A draining connection (seamless switching, RFC 0021) passes no new notes; releases,
        /// expressions and the pedal still reach the notes it holds. Takes effect at commit().
        void setDraining( NodeId from, NodeId to, bool draining );
        /// A retiring node is being switched away from: the audio thread counts the notes still
        /// sounding through its connections, how long its output has been silent (below -100 dB)
        /// and whether the plugin said it is asleep. idle() says when it may go: no note sounding,
        /// and asleep or silent for quietFrames; or no note for limitFrames, whatever it sounds
        /// like. Takes effect at commit().
        void setRetiring( NodeId node, bool retiring );
        bool idle( NodeId node, uint64_t quietFrames, uint64_t limitFrames ) const;
        /// Retiring or not: no note sounding through it, and asleep or silent for quietFrames
        /// (free to be taken over by the next Performance, RFC 0021).
        bool silent( NodeId node, uint64_t quietFrames ) const;
        /// A replacement not yet published: the node gets a freshly loaded plugin in place of its
        /// host, keeping its ID and connections (a node whose taken-over instance refused its state).
        void reload( NodeId node, const std::string& path, const std::string& pluginId = {}, uint64_t memoryRoom = 0 );
        /// The notes sounding through a node's note connections at its last block (held, or
        /// sustained by the pedal), for measuring its cost per voice (RFC 0025); 0 for a node that
        /// takes no notes, UINT32_MAX for one just set retiring and not yet counted.
        uint32_t sounding( NodeId node ) const;
        void disconnectAudio( NodeId from, NodeId to );
        void disconnectNotes( NodeId from, NodeId to );

        /// Routes a plugin's oscairn.param-target lane (oscairn-clap/param-target.h) to a
        /// parameter of another node, replacing any earlier mapping of that lane. The target
        /// is processed after the source, and the lane's events reach it in the same block
        /// as standard CLAP parameter events: global locks before notes, explicit per-note locks
        /// after notes at the same sample (through the corresponding note connection). Takes effect
        /// at the next commit(). Throws std::invalid_argument for unknown nodes, a parameter
        /// the target does not have or cannot change, or source == target.
        void mapParamTarget( NodeId source, uint32_t lane, NodeId target, clap_id param );
        void unmapParamTarget( NodeId source, uint32_t lane );

        /// RFC 0012 step 1: MIDI for external outputs. A tap copies what a node sends from its note
        /// outputs (notes, pressure and MIDI channel messages), or what enters at graphInput through
        /// `zones` (as a note connection's), to the outputs in `ports` (a bit per output number), on
        /// `channel` (1..16; 0 keeps each message's own). `key` is the caller's (a part's ID): a tap
        /// kept under the same key, source, ports and channel keeps its held notes; a tap removed,
        /// or one whose ports or channel moved, sends note offs for them (and a sustain pedal's
        /// release) with the old settings. Replaces every tap; takes effect at commit().
        /// Throws std::invalid_argument for a source that sends no notes, bad zones or a channel past 16.
        struct MidiTap
        {
            uint32_t key = 0;
            NodeId from = graphInput;
            std::vector<NoteZone> zones;
            uint64_t ports = 0;
            uint8_t channel = 0;
            // What the part chose (its rules and channel). While it stays the same, the tap keeps its
            // held notes even when its ports change, so a device renumbered under it gets their offs.
            uint64_t destination = 0;
            bool operator==(const MidiTap&) const = default;
        };
        void setMidiTaps( std::vector<MidiTap> taps );
        std::vector<MidiTap> midiTaps() const;
        /// RFC 0033: a part's harmony (oscairn-clap/harmony.h): its key and scale (`root`, and
        /// `scale` as pitch classes above it, bit 0 the root), and the chord held at its input:
        /// the keys that pass `zones` from graphInput, when `input` is set. Every node in `nodes`
        /// gets oscairn.harmony events: the harmony at sample 0 of each block, and each change at
        /// the sample of the note that made it, before that note (its host passes on only
        /// changes, to plugins that asked). `key` is the caller's (a part's ID): a harmony kept
        /// under the same key keeps the keys it holds, unless its `input` or `zones` changed —
        /// then its releases could be lost, and it starts with none. Replaces every harmony;
        /// takes effect at
        /// commit(). Throws std::invalid_argument for bad zones, a root past 11 or a scale without
        /// its root.
        struct Harmony
        {
            uint32_t key = 0;
            std::vector<NodeId> nodes;
            bool input = false;
            std::vector<NoteZone> zones;
            uint8_t root = 0;
            uint16_t scale = 1;
        };
        void setHarmonies( std::vector<Harmony> harmonies );
        /// One MIDI 1.0 message for external outputs, `time` samples into its block.
        struct MidiMessage { uint32_t time = 0; uint64_t ports = 0; uint8_t size = 0; std::array<uint8_t, 3> bytes{}; };
        static constexpr uint32_t maxMidiOut = 1024;
        /// Audio thread: the taps' messages since clearMidiOut(), each tap's in time order; past
        /// maxMidiOut they are dropped and counted in eventDrops().
        std::span<const MidiMessage> midiOut() const noexcept { return { midiOut_.data(), midiOutCount_ }; }
        void clearMidiOut() noexcept { midiOutCount_ = 0; }

        /// The node's plugin host. Its control API (parameters, service()) belongs to the
        /// main thread; setParameter is safe while the graph is processing.
        const PluginHost& host( NodeId node ) const;
        PluginHost&       host( NodeId node );
        bool                    contains( NodeId node ) const { return nodes_.count( node ) != 0; }

        struct NodeInfo
        {
            NodeId id;
            kick::clap::Descriptor plugin;
            std::vector<kick::clap::Port> inputs, outputs;
            uint32_t noteInputs, noteOutputs;
        };
        struct Mapping { NodeId source; uint32_t lane; NodeId target; uint32_t param; };
        struct Description
        {
            std::vector<NodeInfo> nodes;
            std::vector<std::pair<NodeId,NodeId>> notes;
            std::map<std::pair<NodeId,NodeId>,std::vector<NoteZone>> noteZones; // connections with zones
            std::set<std::pair<NodeId,NodeId>> draining; // note connections draining (setDraining)
            std::vector<Mapping> mappings;
            std::vector<AudioConnection> audioRoutes;
            AudioLayout audioLayout;
        };
        /// Control-thread copy of staged topology; audio changes only at commit().
        Description describe() const;

        /// Compiles the current nodes and connections and hands them to the audio thread.
        /// Throws std::runtime_error if the connections form a cycle (nothing changes then).
        /// rampGains: a route in both plans ramps to its new gain over 5 ms instead of stepping,
        /// for changes that are played (a control swept) rather than edited.
        void commit(bool rampGains = false);

        /// Plans compiled so far (commit() and publishReplacement()), for tests.
        uint64_t plansCompiled() const noexcept { return plansCompiled_; }

        /// Live gains: a fixed table of gains the audio thread reads once per block, so a fader
        /// moves without compiling a plan. A route naming slots (AudioConnection::live) is scaled
        /// by their product, ramped across each block from the last block's. Slot 0 is none;
        /// every slot starts at 1. Any thread; never waits. The table belongs to this graph and
        /// stays across publishReplacement().
        static constexpr uint32_t liveGains = 256;
        /// ramp false: the next block starts at the new gain (a slot just given to a part).
        void setLiveGain( uint32_t slot, float gain, bool ramp = true ) noexcept
        {
            if( !slot || slot >= liveGains ) return;
            liveTarget_[slot].store( gain, std::memory_order_relaxed );
            if( !ramp ) liveJump_[slot].store( true, std::memory_order_release );
        }
        float liveGain( uint32_t slot ) const noexcept { return slot < liveGains ? liveTarget_[slot].load( std::memory_order_relaxed ) : 1.f; }
        /// Audio thread, during or after process(): a slot's gain at this block's start and end, as
        /// routes ramp it (a recording's post-fader tap follows the same ramp).
        float liveGainFrom( uint32_t slot ) const noexcept { return slot < liveGains ? liveFrom_[slot] : 1.f; }
        float liveGainTo( uint32_t slot ) const noexcept { return slot < liveGains ? liveTo_[slot] : 1.f; }

        /// Frees plans the audio thread has finished with.
        void collect();
        /// Removed plugins not freed yet: those an audio plan or undo still owns (control thread).
        size_t heldRemoved() const noexcept { return removedHosts_.size(); }
        /// Service staged nodes and removed nodes still owned by an audio plan.
        void service();
        uint64_t eventDrops() const noexcept { return eventDrops_.load(std::memory_order_relaxed); }
        /// Meters measure a node's first `meterChannels` audio output channels, in port order.
        static constexpr uint32_t meterChannels = 16;
        using Peaks = std::array<float, meterChannels>;
        /// Control thread: the largest absolute sample on each of a node's audio output channels
        /// since the last call (for meters); zeros for an unknown node.
        Peaks takePeaks( NodeId node ) noexcept;

        // Audio thread ------------------------------------------------------------

        /// Renders `frames` (at most maxFrames) into `outputs[0..1]`. `events` enter at
        /// graphInput, with times relative to the start of the block. Every node also gets the
        /// free-running beat (oscairn-clap/beat.h, RFC 0032) at sample 0: it starts at 0 with
        /// the graph and moves on at the transport's tempo (120 BPM without one), playing or
        /// stopped, through replacements (publishReplacement).
        bool process( float* const* outputs, uint32_t frames, const clap_event_transport_t* transport,
                      const kick::clap::Event* events = nullptr, uint32_t eventCount = 0 ) noexcept;

        /// Planar channels in external port order. Missing input channels are silent;
        /// all configured outputs must have storage. Extra outputs are cleared.
        /// Buffers may alias: external inputs are copied before any output is cleared.
        /// `sources`, when given, says where each event came from (note_zone.h).
        bool process( const float* const* inputs, uint32_t inputChannels,
                      float* const* outputs, uint32_t outputChannels, uint32_t frames,
                      const clap_event_transport_t* transport,
                      const kick::clap::Event* events = nullptr, uint32_t eventCount = 0, bool adopt = true,
                      const uint8_t* sources = nullptr ) noexcept;
        void panicInput() noexcept { panicInput_=true; } // audio thread only
        /// Audio thread, after process(): this block's samples of one of a node's audio output
        /// channels (in port order), or of an external input channel for graphInput; null when the
        /// node is not in the plan or has no such channel. Recording reads its taps here (RFC 0020).
        const float* audioOutput( NodeId node, uint32_t channel ) const noexcept;
        /// Told, on the audio thread during process(), the events each node receives in this block
        /// (in time order, after zones) and those it sent (emitted). Must not block or allocate.
        struct Listener
        {
            virtual void events( NodeId node, const kick::clap::Event* events, uint32_t count, bool emitted ) noexcept = 0;
        protected:
            ~Listener() = default;
        };
        void setListener( Listener* listener ) noexcept { listener_.store( listener, std::memory_order_release ); } // control thread; null: none
        uint32_t inputChannels() const noexcept { return inputChannels_; }
        uint32_t outputChannels() const noexcept { return outputChannels_; }

        /// A node as the graph holds it. A copy keeps the plugin alive, with its state, so a
        /// removed or replaced node can be restored under its ID (for undo).
        struct Activity { std::atomic<uint32_t> sounding{0}; std::atomic<uint64_t> quiet{0}, free{0}; std::atomic<bool> asleep{false}; }; // see setRetiring
        struct Node
        {
            std::shared_ptr<PluginHost> host;
            bool retiring=false;
            std::shared_ptr<Activity> activity=std::make_shared<Activity>();
            std::shared_ptr<char> effectGroup=std::make_shared<char>();
            std::shared_ptr<std::array<std::atomic<float>,meterChannels>> peaks=std::make_shared<std::array<std::atomic<float>,meterChannels>>(); // see takePeaks
            uint32_t                          audioInputs = 0, audioOutputs = 0; // channels
        };
        Node node( NodeId id ) const { return nodes_.at( id ); }
        /// Staged like add: puts a held node back under its ID, replacing (and retiring) any node
        /// there now; the node's connections are the caller's to make again.
        void restore( NodeId id, const Node& node );

    private:
        void connectAudioBatch(const std::vector<AudioConnection>&);
        uint32_t resolve( const AudioEndpoint&, bool source, uint32_t count ) const;
        struct Plan;
        std::vector<std::shared_ptr<PluginHost>> removedHosts_; // control thread ownership until plan retirement
        std::atomic<uint64_t> eventDrops_{0};

        /// Plans retired by the audio thread, waiting to be freed on the main thread.
        /// Single producer (audio thread), single consumer (main thread).
        class Retired
        {
        public:
            bool  full() const noexcept { return head_.load( std::memory_order_acquire ) - tail_.load( std::memory_order_acquire ) == slots_.size(); }
            void  push( Plan* p ) noexcept;
            Plan* pop() noexcept;

        private:
            std::array<Plan*, 16> slots_{};
            std::atomic<size_t>   head_{ 0 }, tail_{ 0 };
        };

        Plan* compile() const;
        void  checkSource( NodeId from, bool audio ) const;
        void  checkDestination( NodeId to, bool audio ) const;

        double   sampleRate_;
        uint32_t maxFrames_;
        ProcessingMode mode_;
        Lv2BufferLimits lv2Buffers_;
        kick::clap::AssetResolver* assets_ = nullptr;
        AudioLayout audioLayout_;
    public:
        const AudioLayout& audioLayout() const noexcept { return audioLayout_; }
    private:
        uint32_t inputChannels_=0, outputChannels_=0;
        std::vector<AudioConnection> audioRoutes_;
        NodeId   nextId_ = 1;
        uint64_t replacementSerial_=0;

        std::map<NodeId, Node>                nodes_;
        std::map<std::pair<NodeId, NodeId>,std::shared_ptr<NoteRoute>> noteEdges_;
        std::map<std::pair<NodeId, NodeId>,std::vector<NoteZone>> noteZones_;
        std::set<std::pair<NodeId, NodeId>> draining_;
        // shortcut: after 2^31 notes recreate the graph; add safe ID recycling for longer lifetimes.
        uint32_t nextNoteId_=0; // audio thread; IDs are never reused within this graph
        std::map<std::pair<NodeId, uint32_t>, std::pair<NodeId, clap_id>> paramTargets_; // (source, lane) -> (target, param)

        std::vector<std::pair<MidiTap, std::shared_ptr<NoteRoute>>> taps_; // see setMidiTaps
        struct HeldKeys; // a harmony's keys held at its input, kept across plans
        std::vector<std::pair<Harmony, std::shared_ptr<HeldKeys>>> harmonies_; // see setHarmonies
        std::array<MidiMessage, maxMidiOut> midiOut_{}; // audio thread
        uint32_t midiOutCount_ = 0;
        std::atomic<Plan*> pending_{ nullptr };
        std::atomic<Listener*> listener_{ nullptr };
        bool panicInput_=false;
        double freeBeats_ = 0; // audio thread: oscairn.beat's position at the next block's start
        Plan*              current_ = nullptr; // audio thread only
        Retired            retired_;
        uint64_t plansCompiled_ = 0; // control thread
        std::array<std::atomic<float>, liveGains> liveTarget_; // see setLiveGain; set to 1 by the constructor
        std::array<std::atomic<bool>, liveGains> liveJump_{};  // see setLiveGain's ramp
        std::array<float, liveGains> liveFrom_{}, liveTo_{};    // audio thread: each slot at the last block's start and this one's
    };
}
