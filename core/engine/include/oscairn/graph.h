// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "oscairn/plugin_host.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
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
    /// ports/channels. Node connections resolve the main bus and centre mono into stereo.
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
        /// Throws std::runtime_error on failure.
        NodeId add( const std::string& path, const std::string& pluginId = {} );
        /// Validates and commits replacement atomically; failure preserves the current graph. The
        /// node's routes and note edges are removed; rewire, called with the new plugin in place,
        /// connects what it wants before the commit (the Session moves its wires, RFC 0011).
        void replace(NodeId node,const std::string& path,const std::string& pluginId,const std::string& preset,const std::function<void()>& rewire);
        void   remove( NodeId node );
        NodeId addEffectContext(NodeId source);
        NodeId effectSource(NodeId node) const;
        std::unique_ptr<Graph> prepareReplacement() const;
        // Where plugins added from now on resolve sound assets (RFC 0009); must outlive the graph.
        void setAssets(kick::clap::AssetResolver* assets) noexcept { assets_ = assets; }
        // onPublish must not throw; called after successful compilation/allocation, before publication.
        void publishReplacement(Graph& prepared,size_t noteCutoff,const std::function<void()>& onPublish);
        // Audio thread: adopt once before Session computes transport for this block.
        std::optional<size_t> beginBlock() noexcept;

        /// Connections take effect at the next commit(). Throw std::invalid_argument if
        /// the source cannot send or the destination cannot receive that kind of signal.
        void connectAudio( NodeId from, NodeId to );
        /// Explicit port IDs and channel ranges; gains sum without normalization.
        /// Ranges expand to unique channel pairs. Reconnecting updates gain.
        /// Disconnect removes the selected pairs regardless of gain.
        void connectAudio( const AudioConnection& connection );
        void disconnectAudio( const AudioConnection& connection );
        void connectNotes( NodeId from, NodeId to );
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
            std::vector<Mapping> mappings;
            std::vector<AudioConnection> audioRoutes;
            AudioLayout audioLayout;
        };
        /// Control-thread copy of staged topology; audio changes only at commit().
        Description describe() const;

        /// Compiles the current nodes and connections and hands them to the audio thread.
        /// Throws std::runtime_error if the connections form a cycle (nothing changes then).
        void commit();

        /// Frees plans the audio thread has finished with.
        void collect();
        /// Service staged nodes and removed nodes still owned by an audio plan.
        void service();
        uint64_t eventDrops() const noexcept { return eventDrops_.load(std::memory_order_relaxed); }

        // Audio thread ------------------------------------------------------------

        /// Renders `frames` (at most maxFrames) into `outputs[0..1]`. `events` enter at
        /// graphInput, with times relative to the start of the block.
        bool process( float* const* outputs, uint32_t frames, const clap_event_transport_t* transport,
                      const kick::clap::Event* events = nullptr, uint32_t eventCount = 0 ) noexcept;

        /// Planar channels in external port order. Missing input channels are silent;
        /// all configured outputs must have storage. Extra outputs are cleared.
        /// Buffers may alias: external inputs are copied before any output is cleared.
        bool process( const float* const* inputs, uint32_t inputChannels,
                      float* const* outputs, uint32_t outputChannels, uint32_t frames,
                      const clap_event_transport_t* transport,
                      const kick::clap::Event* events = nullptr, uint32_t eventCount = 0, bool adopt = true ) noexcept;
        void panicInput() noexcept { panicInput_=true; } // audio thread only
        uint32_t inputChannels() const noexcept { return inputChannels_; }
        uint32_t outputChannels() const noexcept { return outputChannels_; }

    private:
        void connectAudioBatch(const std::vector<AudioConnection>&);
        uint32_t resolve( const AudioEndpoint&, bool source, uint32_t count ) const;
        struct Node
        {
            std::shared_ptr<PluginHost> host;
            std::shared_ptr<char> effectGroup=std::make_shared<char>();
            uint32_t                          audioInputs = 0, audioOutputs = 0; // channels
        };
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
        uint32_t inputChannels_=0, outputChannels_=0;
        std::vector<AudioConnection> audioRoutes_;
        NodeId   nextId_ = 1;
        uint64_t replacementSerial_=0;
        size_t replacementNoteCutoff_=0;

        std::map<NodeId, Node>                nodes_;
        std::map<std::pair<NodeId, NodeId>,std::shared_ptr<NoteRoute>> noteEdges_;
        // shortcut: after 2^31 notes recreate the graph; add safe ID recycling for longer lifetimes.
        uint32_t nextNoteId_=0; // audio thread; IDs are never reused within this graph
        std::map<std::pair<NodeId, uint32_t>, std::pair<NodeId, clap_id>> paramTargets_; // (source, lane) -> (target, param)

        std::atomic<Plan*> pending_{ nullptr };
        bool panicInput_=false;
        Plan*              current_ = nullptr; // audio thread only
        Retired            retired_;
    };
}
