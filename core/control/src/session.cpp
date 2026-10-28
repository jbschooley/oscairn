// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/session.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace oscairn
{
    Session::Session( double sampleRate, uint32_t maxFrames )
        : sampleRate_( sampleRate ), maxFrames_( maxFrames ), graph_( sampleRate, maxFrames ), transport_( sampleRate )
    {
        if( !std::isfinite( sampleRate ) || sampleRate <= 0 || maxFrames == 0 )
            throw std::invalid_argument( "session needs a positive sample rate and block size" );
        transport_.setPlaying( false );
    }

    NodeId Session::add( const std::string& name, const std::string& path, const std::string& pluginId )
    {
        if( !name.empty() && std::any_of(nodes_.begin(),nodes_.end(),[&](const auto& n){ return n.second == name; }) )
            throw std::invalid_argument( "duplicate node name: " + name );
        const auto id = graph_.add( path, pluginId );
        nodes_.emplace( id, name );
        for( const auto& p : graph_.host(id).parameters() ) observed_[{id,p.id}] = graph_.host(id).parameterValue(p.id);
        staged_ = true;
        return id;
    }

    NodeId Session::nodeId( const std::string& name ) const
    {
        const auto it = std::find_if(nodes_.begin(),nodes_.end(),[&](const auto& n){ return !name.empty() && n.second == name; });
        if( it == nodes_.end() )
            throw std::invalid_argument( "no node " + name );
        return it->first;
    }

    bool Session::hasParameter( NodeId node, uint32_t param ) const
    {
        if( !graph_.contains( node ) )
            return false;
        const auto& all = graph_.host( node ).parameters();
        return std::any_of( all.begin(), all.end(), [param]( const auto& p ) { return p.id == param; } );
    }

    const kick::clap::Parameter& Session::findParameter( NodeId node, uint32_t param ) const
    {
        if( !graph_.contains( node ) )
            throw std::invalid_argument( "no node " + std::to_string( node ) );
        for( const auto& p : graph_.host( node ).parameters() )
            if( p.id == param )
                return p;
        throw std::invalid_argument( "no parameter " + std::to_string( param ) + " on node " + std::to_string( node ) );
    }

    Control::ParamInfo Session::describe( const kick::clap::Parameter& p )
    {
        return { p.id, p.name, p.minimum, p.maximum, p.defaultValue, ( p.flags & CLAP_PARAM_IS_STEPPED ) != 0, p.flags };
    }

    std::vector<Control::ParamInfo> Session::parameters( NodeId node ) const
    {
        if( !graph_.contains( node ) )
            throw std::invalid_argument( "no node " + std::to_string( node ) );
        std::vector<ParamInfo> result;
        for( const auto& p : graph_.host( node ).parameters() )
            result.push_back( describe( p ) );
        return result;
    }

    Control::ParamInfo Session::parameterInfo( NodeId node, uint32_t param ) const
    {
        return describe( findParameter( node, param ) );
    }

    double Session::parameter( NodeId node, uint32_t param ) const
    {
        findParameter( node, param );
        if( const auto it = pending_.find({node,param}); it != pending_.end() )
            for( auto command = it->second.rbegin(); command != it->second.rend(); ++command )
                if( command->kind == ChangeKind::Value ) return command->value;
        return graph_.host( node ).parameterValue( param );
    }

    std::string Session::parameterText( NodeId node, uint32_t param, double value ) const
    {
        findParameter( node, param );
        if( !std::isfinite( value ) )
            throw std::invalid_argument( "parameter value must be finite" );
        return graph_.host( node ).parameterText( param, value );
    }

    bool Session::setParameter( NodeId node, uint32_t param, double value )
    {
        const auto& p = findParameter( node, param );
        if( !std::isfinite( value ) || ( p.flags & CLAP_PARAM_IS_READONLY ) != 0 )
            throw std::invalid_argument( "parameter value must be finite and parameter writable" );
        value = std::clamp(value,p.minimum,p.maximum);
        if( p.flags & CLAP_PARAM_IS_STEPPED ) value = std::trunc(value);
        return queue(node,param,ChangeKind::Value,value);
    }

    void Session::setTempo( double bpm )
    {
        if( !std::isfinite( bpm ) || bpm < 1.0 || bpm > 1000.0 )
            throw std::invalid_argument( "tempo must be between 1 and 1000 BPM" );
        tempo_.store( bpm );
    }

    Control::TransportState Session::transportState() const
    {
        return { statusPlaying_.load(), statusTempo_.load(), statusBeat_.load() };
    }

    bool Session::note( bool on, int key, double velocity, int channel )
    {
        if( key < 0 || key > 127 || channel < 0 || channel > 15 || !std::isfinite( velocity ) || velocity < 0 || velocity > 1 )
            throw std::invalid_argument( "invalid note key, channel or velocity" );
        const auto head = noteHead_.load( std::memory_order_relaxed );
        if( head - noteTail_.load( std::memory_order_acquire ) == notes_.size() )
        {
            droppedNotes_.fetch_add( 1 );
            panic_.store( true, std::memory_order_release );
            return false;
        }
        notes_[head % notes_.size()] = kick::clap::Event::note( on && velocity > 0, key, velocity, 0, channel );
        noteHead_.store( head + 1, std::memory_order_release );
        return true;
    }

    Control::GraphState Session::graphState() const
    {
        return {graph_.describe(),nodes_,revision_,staged_};
    }

    Control::SessionStatus Session::sessionStatus() const
    {
        auto result = hostStatus_;
        if(result.sampleRate == 0) result.sampleRate = sampleRate_;
        if(result.bufferSize == 0) result.bufferSize = maxFrames_;
        for(const auto& [node,name] : nodes_) result.deadlineMisses += graph_.host(node).statistics().deadlineMisses;
        return result;
    }

    void Session::remove( NodeId node )
    {
        if( !graph_.contains(node) ) throw std::invalid_argument("unknown node");
        graph_.remove(node);
        nodes_.erase(node);
        outputDrops_.erase(node);
        for( auto it = pending_.begin(); it != pending_.end(); )
            if( it->first.first == node ) { commandCount_ -= it->second.size(); it = pending_.erase(it); }
            else ++it;
        std::erase_if(observed_,[&](const auto& entry){ return entry.first.first == node; });
        staged_ = true;
    }

    void Session::commit()
    {
        graph_.commit();
        staged_ = false;
        ++revision_;
        publish(ChangeKind::Graph);
    }

    void Session::publish( ChangeKind kind, NodeId node, uint32_t param, double value, bool plugin )
    {
        if( history_.size() == historyCapacity ) history_.pop_front();
        history_.push_back({++sequence_,kind,node,param,value,plugin});
    }

    Control::Changes Session::changesAfter( uint64_t sequence ) const
    {
        Changes result{sequence_,false,{}};
        result.lost = sequence > sequence_ || (!history_.empty() && sequence < history_.front().sequence - 1);
        for( const auto& change : history_ ) if( change.sequence > sequence ) result.events.push_back(change);
        return result;
    }

    bool Session::queue( NodeId node, uint32_t param, ChangeKind kind, double value )
    {
        const Address address{node,param};
        auto it = pending_.find(address);
        if( kind == ChangeKind::Value && it != pending_.end() && !it->second.empty() && it->second.back().kind == kind )
            it->second.back().value = value;
        else
        {
            if( commandCount_ == commandCapacity ) return false;
            pending_[address].push_back({kind,value});
            ++commandCount_;
        }
        publish(kind,node,param,value);
        return true;
    }

    bool Session::parameterGesture( NodeId node, uint32_t param, bool begin )
    {
        if( findParameter(node,param).flags & CLAP_PARAM_IS_READONLY )
            throw std::invalid_argument("parameter is read-only");
        return queue(node,param,begin ? ChangeKind::GestureBegin : ChangeKind::GestureEnd,0);
    }

    void Session::service()
    {
        for( const auto& [node,name] : nodes_ )
        {
            auto& host = graph_.host(node);
            host.service();
            if( outputDrops_[node] != host.outputEventDrops() )
            {
                outputDrops_[node] = host.outputEventDrops();
                publish(ChangeKind::Resync,node,0,0,true);
            }
            kick::clap::Event event;
            // Bounded even if a plugin continuously emits on the audio thread.
            for( size_t i = 0; i < kick::clap::Host::maxEvents && host.popOutput(event); ++i )
            {
                const auto type = event.header()->type;
                if( type == CLAP_EVENT_PARAM_VALUE )
                {
                    const auto* p = reinterpret_cast<const clap_event_param_value_t*>(event.header());
                    if( p->note_id == -1 && p->port_index == -1 && p->channel == -1 && p->key == -1 && hasParameter(node,p->param_id) )
                    {
                        publish(ChangeKind::Value,node,p->param_id,p->value,true);
                        observed_[{node,p->param_id}] = p->value;
                    }
                }
                else if( type == CLAP_EVENT_PARAM_GESTURE_BEGIN || type == CLAP_EVENT_PARAM_GESTURE_END )
                {
                    const auto* p = reinterpret_cast<const clap_event_param_gesture_t*>(event.header());
                    publish(type == CLAP_EVENT_PARAM_GESTURE_BEGIN ? ChangeKind::GestureBegin : ChangeKind::GestureEnd,node,p->param_id,0,true);
                }
            }
            // Rescans may update a value without emitting an event.
            for( const auto& p : host.parameters() )
            {
                const Address address{node,p.id};
                const auto value = host.parameterValue(p.id);
                if( !observed_.contains(address) || observed_[address] != value )
                    publish(ChangeKind::Value,node,p.id,value,true);
                observed_[address] = value;
            }
        }
        for( auto it = pending_.begin(); it != pending_.end(); )
        {
            const auto [node,param] = it->first;
            auto& commands = it->second;
            if( !hasParameter(node,param) || (parameterInfo(node,param).flags & CLAP_PARAM_IS_READONLY) )
            {
                commandCount_ -= commands.size();
                publish(ChangeKind::Resync,node,param);
                it = pending_.erase(it);
                continue;
            }
            auto& host = graph_.host(node);
            bool sentValue = false;
            while( !commands.empty() )
            {
                const auto command = commands.front();
                if( command.kind == ChangeKind::Value )
                {
                    if( sentValue || !host.setParameter(param,command.value) ) break;
                    sentValue = true;
                    observed_[{node,param}] = command.value;
                }
                else if( !host.parameterGesture(param,command.kind == ChangeKind::GestureBegin) ) break;
                commands.pop_front();
                --commandCount_;
            }
            it = commands.empty() ? pending_.erase(it) : std::next(it);
        }
        graph_.collect();
    }

    bool Session::process( float* const* outputs, uint32_t frames,
                           std::span<const kick::clap::Event> events, std::span<const MidiClockEvent> clocks ) noexcept
    {
        if( !outputs || !outputs[0] || !outputs[1] )
            return false;
        if( frames == 0 || frames > maxFrames_ || events.size() > maxInputEvents || clocks.size() > maxClockEvents )
        {
            std::fill_n( outputs[0], frames, 0.0f );
            std::fill_n( outputs[1], frames, 0.0f );
            return false;
        }

        const auto source = clockSource_.load();
        if( source != following_ )
        {
            transport_.setClockSource( source );
            following_ = source;
        }
        if( source == Transport::ClockSource::MidiClock )
            for( const auto& e : clocks )
                transport_.midiClock( e.status, e.sampleTime, e.songPosition );
        else
        {
            transport_.setTempo( tempo_.load() );
            transport_.setPlaying( playing_.load() );
        }

        size_t count = 0;
        auto tail = noteTail_.load( std::memory_order_relaxed );
        // Consume the overflow flag before taking the queue snapshot. If it is set,
        // the snapshot must include all notes published before that overflow; taking
        // the snapshot first could leave a new note behind its emergency release.
        const bool panic = panic_.exchange( false, std::memory_order_acquire );
        const auto head = noteHead_.load( std::memory_order_acquire );
        if( panic )
        {
            // Fail safe after a lost UI event: discard the queued burst and broadcast
            // release to graph-input destinations. Voices from other sources on the
            // same destinations can also be released by this emergency operation.
            auto release = kick::clap::Event::note( false, -1, 0.0, 0, -1 );
            merged_[count++] = release;
            tail = head;
        }
        else
            while( tail != head )
            {
                merged_[count++] = notes_[tail % notes_.size()];
                ++tail;
            }
        noteTail_.store( tail, std::memory_order_release );
        for( const auto& e : events )
            merged_[count++] = e;

        const auto t = transport_.current();
        statusPlaying_.store( ( t.flags & CLAP_TRANSPORT_IS_PLAYING ) != 0 );
        statusTempo_.store( t.tempo );
        statusBeat_.store( (double)t.song_pos_beats / (double)CLAP_BEATTIME_FACTOR );
        const bool ok = graph_.process( outputs, frames, &t, merged_.data(), (uint32_t)count );
        transport_.advance( frames );
        return ok;
    }
}
