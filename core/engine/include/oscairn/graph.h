// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "oscairn/plugin_host.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace oscairn
{
    using NodeId = uint32_t;

    /// The graph's own endpoints: external note events (keyboard, pads, MIDI) enter at
    /// graphInput, and audio connected to graphOutput is mixed into the main output.
    constexpr NodeId graphInput  = 0xFFFFFFFEu;
    constexpr NodeId graphOutput = 0xFFFFFFFFu;

    /// A graph of plugin nodes with audio and note connections, processed in
    /// dependency order within each block, so a sequencer's notes reach the instruments
    /// it plays in the same block. Audio connected into a node is summed; notes from
    /// several sources are merged in time order. Renders stereo; a mono plugin output
    /// plays in the centre.
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
        static constexpr uint32_t channels = 2;

        Graph( double sampleRate, uint32_t maxFrames, ProcessingMode mode = ProcessingMode::Realtime, Lv2BufferLimits lv2Buffers = {} );
        ~Graph();

        Graph( const Graph& )            = delete;
        Graph& operator=( const Graph& ) = delete;

        // Main thread -------------------------------------------------------------

        /// Loads a CLAP module or LV2 bundle as a new node. Multiple-plugin LV2 bundles
        /// require pluginId (the URI); CLAP defaults to the module's first plugin.
        /// Throws std::runtime_error on failure.
        NodeId add( const std::string& path, const std::string& pluginId = {} );
        void   remove( NodeId node );

        /// Connections take effect at the next commit(). Throw std::invalid_argument if
        /// the source cannot send or the destination cannot receive that kind of signal.
        void connectAudio( NodeId from, NodeId to );
        void connectNotes( NodeId from, NodeId to );
        void disconnectAudio( NodeId from, NodeId to );
        void disconnectNotes( NodeId from, NodeId to );

        /// Routes a plugin's oscairn.param-target lane (oscairn-clap/param-target.h) to a
        /// parameter of another node, replacing any earlier mapping of that lane. The target
        /// is processed after the source, and the lane's events reach it in the same block
        /// as standard CLAP parameter events, before notes at the same sample. Takes effect
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
            std::vector<std::pair<NodeId,NodeId>> audio, notes;
            std::vector<Mapping> mappings;
        };
        /// Control-thread copy of staged topology; audio changes only at commit().
        Description describe() const;

        /// Compiles the current nodes and connections and hands them to the audio thread.
        /// Throws std::runtime_error if the connections form a cycle (nothing changes then).
        void commit();

        /// Frees plans the audio thread has finished with.
        void collect();

        // Audio thread ------------------------------------------------------------

        /// Renders `frames` (at most maxFrames) into `outputs[0..1]`. `events` enter at
        /// graphInput, with times relative to the start of the block.
        bool process( float* const* outputs, uint32_t frames, const clap_event_transport_t* transport,
                      const kick::clap::Event* events = nullptr, uint32_t eventCount = 0 ) noexcept;

    private:
        struct Node
        {
            std::shared_ptr<PluginHost> host;
            uint32_t                          audioInputs = 0, audioOutputs = 0; // channels
        };
        struct Plan;

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
        NodeId   nextId_ = 1;

        std::map<NodeId, Node>                nodes_;
        std::vector<std::pair<NodeId, NodeId>> audioEdges_, noteEdges_;
        std::map<std::pair<NodeId, uint32_t>, std::pair<NodeId, clap_id>> paramTargets_; // (source, lane) -> (target, param)

        std::atomic<Plan*> pending_{ nullptr };
        Plan*              current_ = nullptr; // audio thread only
        Retired            retired_;
    };
}
