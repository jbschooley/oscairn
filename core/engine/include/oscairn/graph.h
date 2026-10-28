// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <kick_clap/Host.h>

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

    /// A graph of CLAP plugin nodes with audio and note connections, processed in
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

        Graph( double sampleRate, uint32_t maxFrames );
        ~Graph();

        Graph( const Graph& )            = delete;
        Graph& operator=( const Graph& ) = delete;

        // Main thread -------------------------------------------------------------

        /// Loads a plugin as a new node. `pluginId` empty means the module's first plugin.
        /// Throws std::runtime_error on failure.
        NodeId add( const std::string& path, const std::string& pluginId = {} );
        void   remove( NodeId node );

        /// Connections take effect at the next commit(). Throw std::invalid_argument if
        /// the source cannot send or the destination cannot receive that kind of signal.
        void connectAudio( NodeId from, NodeId to );
        void connectNotes( NodeId from, NodeId to );
        void disconnectAudio( NodeId from, NodeId to );
        void disconnectNotes( NodeId from, NodeId to );

        const kick::clap::Host& host( NodeId node ) const;
        bool                    contains( NodeId node ) const { return nodes_.count( node ) != 0; }

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
            std::shared_ptr<kick::clap::Host> host;
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
        NodeId   nextId_ = 1;

        std::map<NodeId, Node>                nodes_;
        std::vector<std::pair<NodeId, NodeId>> audioEdges_, noteEdges_;

        std::atomic<Plan*> pending_{ nullptr };
        Plan*              current_ = nullptr; // audio thread only
        Retired            retired_;
    };
}
