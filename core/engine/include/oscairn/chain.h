// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "oscairn/graph.h"

#include <cstdint>
#include <string>
#include <vector>

namespace oscairn
{
    /// A straight line of plugins, built on Graph: each plugin plays the note output
    /// of the plugin before it (the first plays the events passed to process()) and
    /// processes the audio of the last plugin that produced audio, which goes to the
    /// output.
    ///
    /// Building the chain (add) is a main-thread operation; process() is the audio-thread call.
    class Chain
    {
    public:
        static constexpr uint32_t channels = Graph::channels;

        Chain( double sampleRate, uint32_t maxFrames, ProcessingMode mode = ProcessingMode::Realtime, Lv2BufferLimits lv2Buffers = {} )
            : graph_( sampleRate, maxFrames, mode, lv2Buffers )
        {
        }

        /// Loads a plugin and appends it. An LV2 bundle with multiple plugins requires
        /// its URI in pluginId; a CLAP module defaults to its first plugin.
        /// Throws std::runtime_error on failure.
        void add( const std::string& path, const std::string& pluginId = {} );

        size_t                        size() const noexcept { return nodes_.size(); }
        NodeId                        node( size_t index ) const noexcept { return nodes_[index]; }
        const kick::clap::Descriptor& descriptor( size_t index ) const noexcept { return graph_.host( nodes_[index] ).descriptor(); }
        Graph&                        graph() noexcept { return graph_; }

        bool process( float* const* outputs, uint32_t frames, const clap_event_transport_t* transport,
                      const kick::clap::Event* events = nullptr, uint32_t eventCount = 0 ) noexcept
        {
            return graph_.process( outputs, frames, transport, events, eventCount );
        }

    private:
        Graph               graph_;
        std::vector<NodeId> nodes_;
        NodeId              lastAudio_ = 0; // 0: none yet
    };
}
