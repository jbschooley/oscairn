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
        if( name.empty() || nodes_.contains( name ) )
            throw std::invalid_argument( "node name must be nonempty and unique: " + name );
        const auto id = graph_.add( path, pluginId );
        nodes_.emplace( name, id );
        return id;
    }

    NodeId Session::nodeId( const std::string& name ) const
    {
        const auto it = nodes_.find( name );
        if( it == nodes_.end() )
            throw std::invalid_argument( "no node " + name );
        return it->second;
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
        return graph_.host( node ).setParameter( param, value );
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

    void Session::service()
    {
        for( const auto& [name, node] : nodes_ )
            graph_.host( node ).service();
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
