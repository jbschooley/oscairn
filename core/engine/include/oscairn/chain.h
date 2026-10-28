// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <kick_clap/Host.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace oscairn
{
    /// A chain of CLAP plugins processed in order within each block: the first
    /// step of Oscairn's graph. Each plugin receives the note output of the plugin
    /// before it (so a sequencer can play a synth in the same block) and the audio
    /// of the last plugin that produced audio (so effects follow instruments).
    /// The chain renders stereo.
    ///
    /// Building the chain (add) is a main-thread operation; process() is the
    /// audio-thread call. They must not overlap.
    class Chain
    {
    public:
        static constexpr uint32_t channels = 2;

        Chain( double sampleRate, uint32_t maxFrames );

        /// Loads a plugin and appends it. `pluginId` empty means the module's first
        /// plugin. Throws std::runtime_error on failure.
        void add( const std::string& path, const std::string& pluginId = {} );

        size_t                      size() const noexcept { return nodes_.size(); }
        const kick::clap::Descriptor& descriptor( size_t index ) const noexcept { return nodes_[index]->descriptor(); }

        /// Renders `frames` (at most maxFrames) into `outputs[0..1]`. `events` go to the
        /// first plugin, with times relative to the start of the block.
        bool process( float* const* outputs, uint32_t frames, const clap_event_transport_t* transport,
                      const kick::clap::Event* events = nullptr, uint32_t eventCount = 0 ) noexcept;

    private:
        double                                         sampleRate_;
        uint32_t                                       maxFrames_;
        std::vector<std::unique_ptr<kick::clap::Host>> nodes_;
        std::vector<float>                             bufferA_, bufferB_;
    };
}
