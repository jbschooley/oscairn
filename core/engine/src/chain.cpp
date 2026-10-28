// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/chain.h"

#include <kick_clap/Discovery.h>

#include <algorithm>
#include <stdexcept>

namespace oscairn
{
    Chain::Chain( double sampleRate, uint32_t maxFrames )
        : sampleRate_( sampleRate )
        , maxFrames_( maxFrames )
        , bufferA_( channels * maxFrames, 0.0f )
        , bufferB_( channels * maxFrames, 0.0f )
    {
    }

    void Chain::add( const std::string& path, const std::string& pluginId )
    {
        // Reading the descriptors loads the module in this process; fine for a
        // trusted path. Untrusted folders go through the out-of-process scanner.
        const auto scan = kick::clap::inspect( path );
        const auto found = std::find_if( scan.plugins.begin(), scan.plugins.end(), [&]( const kick::clap::Descriptor& d ) {
            return pluginId.empty() || d.id == pluginId;
        } );
        if( found == scan.plugins.end() )
        {
            if( !scan.failures.empty() )
                throw std::runtime_error( scan.failures.front().message );
            throw std::runtime_error( pluginId.empty() ? "no plugins in " + path : "plugin " + pluginId + " not found in " + path );
        }

        auto host = std::make_unique<kick::clap::Host>();
        if( !host->load( *found, sampleRate_, maxFrames_ ) )
            throw std::runtime_error( "cannot load " + found->id + ": " + host->error() );
        nodes_.push_back( std::move( host ) );
    }

    bool Chain::process( float* const* outputs, uint32_t frames, const clap_event_transport_t* transport,
                         const kick::clap::Event* events, uint32_t eventCount ) noexcept
    {
        if( frames > maxFrames_ )
            return false;

        float* a[channels] = { bufferA_.data(), bufferA_.data() + maxFrames_ };
        float* b[channels] = { bufferB_.data(), bufferB_.data() + maxFrames_ };

        float* const*                audio      = nullptr; // the latest plugin audio, if any
        const kick::clap::Event*     nodeEvents = events;
        uint32_t                     nodeCount  = eventCount;
        bool                         ok         = true;

        for( auto& node : nodes_ )
        {
            if( !node->outputPorts().empty() )
            {
                float* const* target = ( audio == a ) ? b : a;
                ok &= node->process( audio, audio ? channels : 0, target, channels, frames, nodeEvents, nodeCount, transport );
                audio = target;
            }
            else
            {
                ok &= node->process( audio, audio ? channels : 0, nullptr, 0, frames, nodeEvents, nodeCount, transport );
            }

            // The next plugin plays what this one emitted. A plugin without note
            // outputs (an instrument or effect) passes no notes on.
            if( node->noteOutputPorts() > 0 )
                nodeEvents = node->outputEvents( nodeCount );
            else
            {
                nodeEvents = nullptr;
                nodeCount  = 0;
            }
        }

        for( uint32_t c = 0; c < channels; ++c )
        {
            if( audio )
                std::copy_n( audio[c], frames, outputs[c] );
            else
                std::fill_n( outputs[c], frames, 0.0f );
        }
        return ok;
    }
}
