// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/graph.h"

#include <oscairn-clap/param-target.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace oscairn
{
    /// A compiled graph: nodes in dependency order with everything the audio thread needs
    /// preallocated, so processing never allocates or locks.
    struct Graph::Plan
    {
        struct Step
        {
            std::shared_ptr<PluginHost> host; // keeps the plugin alive while any plan uses it
            uint32_t                          audioInputs = 0, audioOutputs = 0;
            std::vector<int>                  audioSources; // step indices, summed
            std::vector<int>                  noteSources;  // step indices; -1 is graphInput
            bool                              toOutput = false;
            float*                            out[channels]{};

            struct Route // where one of this step's param-target lanes goes
            {
                uint32_t lane;
                int      target;
                clap_id  param;
                double   minimum, maximum;
                bool     stepped;
            };
            std::vector<Route>             routes;
            std::vector<kick::clap::Event> paramIn; // converted events waiting for this step this block
            uint32_t                       paramCount = 0;
        };

        struct Key // orders merged notes: by time, then by source and position
        {
            uint32_t                 time;
            uint32_t                 order;
            const kick::clap::Event* event;
        };

        std::vector<Step>              steps;
        std::vector<float>             buffers; // channels * maxFrames per audio-producing step
        std::vector<float>             mix;     // the summed audio input of one step
        std::vector<Key>               keys;
        std::vector<kick::clap::Event> merged;
    };

    //==============================================================================
    void Graph::Retired::push( Plan* p ) noexcept
    {
        const auto head = head_.load( std::memory_order_relaxed );
        slots_[head % slots_.size()] = p;
        head_.store( head + 1, std::memory_order_release );
    }

    Graph::Plan* Graph::Retired::pop() noexcept
    {
        const auto tail = tail_.load( std::memory_order_relaxed );
        if( tail == head_.load( std::memory_order_acquire ) )
            return nullptr;
        Plan* p = slots_[tail % slots_.size()];
        tail_.store( tail + 1, std::memory_order_release );
        return p;
    }

    //==============================================================================
    Graph::Graph( double sampleRate, uint32_t maxFrames, ProcessingMode mode, Lv2BufferLimits lv2Buffers )
        : sampleRate_( sampleRate )
        , maxFrames_( maxFrames )
        , mode_( mode )
        , lv2Buffers_( lv2Buffers )
    {
    }

    Graph::~Graph()
    {
        // The audio thread must have stopped calling process().
        delete pending_.exchange( nullptr );
        delete current_;
        collect();
    }

    NodeId Graph::add( const std::string& path, const std::string& pluginId )
    {
        Node node;
        node.host = loadPlugin( path, pluginId, sampleRate_, maxFrames_, mode_, lv2Buffers_ );
        for( const auto& port : node.host->inputPorts() )
            node.audioInputs += port.channels;
        for( const auto& port : node.host->outputPorts() )
            node.audioOutputs += port.channels;

        const NodeId id = nextId_++;
        nodes_.emplace( id, std::move( node ) );
        return id;
    }

    void Graph::remove( NodeId node )
    {
        nodes_.erase( node );
        for( auto it = paramTargets_.begin(); it != paramTargets_.end(); )
            it = ( it->first.first == node || it->second.first == node ) ? paramTargets_.erase( it ) : std::next( it );
        auto touches = [&]( const std::pair<NodeId, NodeId>& e ) { return e.first == node || e.second == node; };
        audioEdges_.erase( std::remove_if( audioEdges_.begin(), audioEdges_.end(), touches ), audioEdges_.end() );
        noteEdges_.erase( std::remove_if( noteEdges_.begin(), noteEdges_.end(), touches ), noteEdges_.end() );
    }

    void Graph::checkSource( NodeId from, bool audio ) const
    {
        if( from == graphInput && !audio )
            return;
        const auto it = nodes_.find( from );
        if( it == nodes_.end() )
            throw std::invalid_argument( "no such source node" );
        if( audio ? it->second.audioOutputs == 0 : it->second.host->noteOutputPorts() == 0 )
            throw std::invalid_argument( audio ? "the source has no audio outputs" : "the source has no note outputs" );
    }

    void Graph::checkDestination( NodeId to, bool audio ) const
    {
        if( to == graphOutput && audio )
            return;
        const auto it = nodes_.find( to );
        if( it == nodes_.end() )
            throw std::invalid_argument( "no such destination node" );
        if( audio ? it->second.audioInputs == 0 : it->second.host->noteInputPorts() == 0 )
            throw std::invalid_argument( audio ? "the destination has no audio inputs" : "the destination has no note inputs" );
    }

    void Graph::connectAudio( NodeId from, NodeId to )
    {
        checkSource( from, true );
        checkDestination( to, true );
        if( std::find( audioEdges_.begin(), audioEdges_.end(), std::make_pair( from, to ) ) == audioEdges_.end() )
            audioEdges_.emplace_back( from, to );
    }

    void Graph::connectNotes( NodeId from, NodeId to )
    {
        checkSource( from, false );
        checkDestination( to, false );
        if( std::find( noteEdges_.begin(), noteEdges_.end(), std::make_pair( from, to ) ) == noteEdges_.end() )
            noteEdges_.emplace_back( from, to );
    }

    void Graph::disconnectAudio( NodeId from, NodeId to )
    {
        audioEdges_.erase( std::remove( audioEdges_.begin(), audioEdges_.end(), std::make_pair( from, to ) ), audioEdges_.end() );
    }

    void Graph::disconnectNotes( NodeId from, NodeId to )
    {
        noteEdges_.erase( std::remove( noteEdges_.begin(), noteEdges_.end(), std::make_pair( from, to ) ), noteEdges_.end() );
    }

    void Graph::mapParamTarget( NodeId source, uint32_t lane, NodeId target, clap_id param )
    {
        if( !nodes_.count( source ) || !nodes_.count( target ) || source == target )
            throw std::invalid_argument( "a parameter mapping needs two different nodes" );
        const auto& params = nodes_.at( target ).host->parameters();
        const auto  found  = std::find_if( params.begin(), params.end(), [&]( const kick::clap::Parameter& p ) { return p.id == param; } );
        if( found == params.end() || ( found->flags & CLAP_PARAM_IS_READONLY ) )
            throw std::invalid_argument( "the target has no such parameter, or it is read-only" );
        paramTargets_[{ source, lane }] = { target, param };
    }

    void Graph::unmapParamTarget( NodeId source, uint32_t lane )
    {
        paramTargets_.erase( { source, lane } );
    }

    const PluginHost& Graph::host( NodeId node ) const
    {
        return *nodes_.at( node ).host;
    }

    PluginHost& Graph::host( NodeId node )
    {
        return *nodes_.at( node ).host;
    }

    Graph::Description Graph::describe() const
    {
        Description result;
        for( const auto& [id,node] : nodes_ )
            result.nodes.push_back({id,node.host->descriptor(),node.host->inputPorts(),node.host->outputPorts(),
                                    node.host->noteInputPorts(),node.host->noteOutputPorts()});
        result.audio = audioEdges_;
        result.notes = noteEdges_;
        for( const auto& [from,to] : paramTargets_ )
            result.mappings.push_back({from.first,from.second,to.first,to.second});
        return result;
    }

    Graph::Plan* Graph::compile() const
    {
        // Dependency order (Kahn's algorithm), lowest node ID first among ready nodes so
        // the order is stable.
        std::map<NodeId, int> indegree;
        for( const auto& [id, node] : nodes_ )
            indegree[id] = 0;
        auto internal = [&]( const std::pair<NodeId, NodeId>& e ) { return nodes_.count( e.first ) && nodes_.count( e.second ); };
        std::vector<std::pair<NodeId, NodeId>> mappingEdges;
        for( const auto& [from, to] : paramTargets_ )
            mappingEdges.emplace_back( from.first, to.first );
        for( const auto* edges : { &audioEdges_, &noteEdges_, &std::as_const( mappingEdges ) } )
            for( const auto& e : *edges )
                if( internal( e ) )
                    ++indegree[e.second];

        std::vector<NodeId> order;
        while( order.size() < nodes_.size() )
        {
            auto ready = std::find_if( indegree.begin(), indegree.end(), []( const auto& d ) { return d.second == 0; } );
            if( ready == indegree.end() )
                throw std::runtime_error( "the graph's connections form a cycle" );
            const NodeId id = ready->first;
            ready->second   = -1; // done
            order.push_back( id );
            for( const auto* edges : { &audioEdges_, &noteEdges_, &std::as_const( mappingEdges ) } )
                for( const auto& e : *edges )
                    if( e.first == id && internal( e ) )
                        --indegree[e.second];
        }

        auto plan = std::make_unique<Plan>();
        std::map<NodeId, int> stepOf;
        for( const NodeId id : order )
        {
            const auto& node = nodes_.at( id );
            Plan::Step  step;
            step.host         = node.host;
            step.audioInputs  = node.audioInputs;
            step.audioOutputs = node.audioOutputs;
            stepOf[id]        = (int)plan->steps.size();
            plan->steps.push_back( std::move( step ) );
        }
        for( const auto& [from, to] : audioEdges_ )
        {
            if( !nodes_.count( from ) )
                continue;
            if( to == graphOutput )
                plan->steps[(size_t)stepOf[from]].toOutput = true;
            else if( nodes_.count( to ) )
                plan->steps[(size_t)stepOf[to]].audioSources.push_back( stepOf[from] );
        }
        for( const auto& [from, to] : noteEdges_ )
        {
            if( !nodes_.count( to ) || ( from != graphInput && !nodes_.count( from ) ) )
                continue;
            plan->steps[(size_t)stepOf[to]].noteSources.push_back( from == graphInput ? -1 : stepOf[from] );
        }

        for( const auto& [from, to] : paramTargets_ )
        {
            const auto& params = nodes_.at( to.first ).host->parameters();
            const auto  p      = std::find_if( params.begin(), params.end(), [&]( const kick::clap::Parameter& q ) { return q.id == to.second; } );
            if( p == params.end() )
                continue; // the plugin no longer has it
            auto& source = plan->steps[(size_t)stepOf[from.first]];
            auto& target = plan->steps[(size_t)stepOf[to.first]];
            source.routes.push_back( { from.second, stepOf[to.first], to.second, p->minimum, p->maximum, ( p->flags & CLAP_PARAM_IS_STEPPED ) != 0 } );
            target.paramIn.resize( kick::clap::Host::maxBlockEvents );
        }

        size_t producing = 0;
        for( const auto& step : plan->steps )
            producing += step.audioOutputs > 0;
        plan->buffers.assign( producing * channels * maxFrames_, 0.0f );
        size_t next = 0;
        for( auto& step : plan->steps )
            if( step.audioOutputs > 0 )
            {
                for( uint32_t c = 0; c < channels; ++c )
                    step.out[c] = plan->buffers.data() + ( next * channels + c ) * maxFrames_;
                ++next;
            }
        plan->mix.assign( channels * maxFrames_, 0.0f );
        plan->keys.reserve( kick::clap::Host::maxBlockEvents );
        plan->merged.resize( kick::clap::Host::maxBlockEvents );
        return plan.release();
    }

    void Graph::commit()
    {
        Plan* plan = compile(); // throws on a cycle, before anything changes
        collect();
        delete pending_.exchange( plan, std::memory_order_acq_rel ); // a plan the audio thread never took
    }

    void Graph::collect()
    {
        while( Plan* p = retired_.pop() )
            delete p;
    }

    bool Graph::process( float* const* outputs, uint32_t frames, const clap_event_transport_t* transport,
                         const kick::clap::Event* events, uint32_t eventCount ) noexcept
    {
        // Adopt the newest plan, retiring the current one, if there is room to retire it.
        if( !current_ || !retired_.full() )
            if( Plan* plan = pending_.exchange( nullptr, std::memory_order_acq_rel ) )
            {
                if( current_ )
                    retired_.push( current_ );
                current_ = plan;
            }

        for( uint32_t c = 0; c < channels; ++c )
            std::fill_n( outputs[c], frames, 0.0f );
        Plan* plan = current_;
        if( !plan )
            return true;
        if( frames > maxFrames_ )
            return false;

        bool ok = true;
        for( auto& step : plan->steps )
        {
            // Audio in: the sum of the sources.
            float*        mix[channels] = { plan->mix.data(), plan->mix.data() + maxFrames_ };
            const float* const* inputs  = nullptr;
            if( !step.audioSources.empty() && step.audioInputs > 0 )
            {
                for( uint32_t c = 0; c < channels; ++c )
                {
                    std::fill_n( mix[c], frames, 0.0f );
                    for( int source : step.audioSources )
                    {
                        const float* from = plan->steps[(size_t)source].out[c];
                        for( uint32_t i = 0; i < frames; ++i )
                            mix[c][i] += from[i];
                    }
                }
                inputs = mix;
            }

            // Notes in: every source's events this block, merged in time order.
            plan->keys.clear();
            uint32_t order = 0;
            for( uint32_t i = 0; i < step.paramCount; ++i ) // before notes at the same sample
                plan->keys.push_back( { step.paramIn[i].header()->time, order++, &step.paramIn[i] } );
            for( int source : step.noteSources )
            {
                const kick::clap::Event* list  = events;
                uint32_t                 count = eventCount;
                if( source >= 0 )
                    list = plan->steps[(size_t)source].host->outputEvents( count );
                for( uint32_t i = 0; i < count && plan->keys.size() < kick::clap::Host::maxBlockEvents; ++i )
                    if( list[i].header()->space_id == CLAP_CORE_EVENT_SPACE_ID ) // lane events are routed, not passed on
                        plan->keys.push_back( { list[i].header()->time, order++, &list[i] } );
            }
            std::sort( plan->keys.begin(), plan->keys.end(), []( const Plan::Key& a, const Plan::Key& b ) {
                return a.time != b.time ? a.time < b.time : a.order < b.order;
            } );
            for( size_t i = 0; i < plan->keys.size(); ++i )
                plan->merged[i] = *plan->keys[i].event;
            const auto merged = (uint32_t)plan->keys.size();

            if( step.audioOutputs > 0 )
            {
                ok &= step.host->process( inputs, inputs ? channels : 0, step.out, channels, frames, plan->merged.data(), merged, transport );
                if( step.audioOutputs == 1 ) // a mono plugin plays in the centre
                    std::copy_n( step.out[0], frames, step.out[1] );
            }
            else
                ok &= step.host->process( inputs, inputs ? channels : 0, nullptr, 0, frames, plan->merged.data(), merged, transport );
            step.paramCount = 0;

            // This step's param-target lane events become parameter events for their
            // targets, which come later in this block.
            if( !step.routes.empty() )
            {
                uint32_t    count  = 0;
                const auto* output = step.host->outputEvents( count );
                for( uint32_t i = 0; i < count; ++i )
                {
                    const auto* h = output[i].header();
                    if( h->space_id != kick::clap::Host::paramTargetSpace || h->type != OSCAIRN_EVENT_PARAM_TARGET )
                        continue;
                    const auto& e     = *reinterpret_cast<const oscairn_event_param_target_t*>( h );
                    const auto  route = std::find_if( step.routes.begin(), step.routes.end(), [&]( const Plan::Step::Route& r ) { return r.lane == e.lane; } );
                    if( route == step.routes.end() )
                        continue; // lane not mapped
                    auto& target = plan->steps[(size_t)route->target];
                    if( target.paramCount == target.paramIn.size() )
                        continue; // full for this block
                    const double range = route->maximum - route->minimum;
                    if( e.mode == OSCAIRN_PARAM_TARGET_SET )
                    {
                        clap_event_param_value_t v{};
                        v.header     = { sizeof( v ), h->time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 };
                        v.param_id   = route->param;
                        v.note_id    = e.note_id;
                        v.port_index = e.port_index;
                        v.channel    = e.channel;
                        v.key        = e.key;
                        v.value      = route->minimum + std::clamp( e.value, 0.0, 1.0 ) * range;
                        if( route->stepped )
                            v.value = std::round( v.value );
                        target.paramIn[target.paramCount++] = kick::clap::Event::from( v );
                    }
                    else
                    {
                        clap_event_param_mod_t m{};
                        m.header     = { sizeof( m ), h->time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_MOD, 0 };
                        m.param_id   = route->param;
                        m.note_id    = e.note_id;
                        m.port_index = e.port_index;
                        m.channel    = e.channel;
                        m.key        = e.key;
                        m.amount     = std::clamp( e.value, -1.0, 1.0 ) * range;
                        target.paramIn[target.paramCount++] = kick::clap::Event::from( m );
                    }
                }
            }
        }

        for( const auto& step : plan->steps )
            if( step.toOutput )
                for( uint32_t c = 0; c < channels; ++c )
                    for( uint32_t i = 0; i < frames; ++i )
                        outputs[c][i] += step.out[c][i];
        return ok;
    }
}
