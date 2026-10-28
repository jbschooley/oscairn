// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Oscairn Step Sequencer: a CLAP note-effect plugin. It plays a 16-step pattern of
// sixteenth notes in time with the host transport and sends the notes on to the
// next plugin. It has no audio ports.
//
// Steps are placed by the transport's beat position, so the sequencer follows
// whatever drives the transport (its own tempo, MIDI clock, a DAW). Each step plays
// once even if the position jitters, and a jump in position (song position, loop)
// restarts from the new place. With a steady tempo the output does not depend on the
// host's block size.
//
// Each step also sends a parameter lock on lane 0 ("Lock A") through
// oscairn.param-target, at the step's sample and just before its note, when the host
// supports that event space. The host decides which parameter the lane controls.

#include <clap/clap.h>
#include <oscairn-clap/param-target.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
    // A minor arpeggio bassline, alternating octaves.
    constexpr std::array<int16_t, 16> pattern{ 45, 57, 52, 57, 48, 60, 55, 60, 43, 55, 50, 55, 47, 59, 52, 59 };
    constexpr double                  stepBeats  = 0.25; // sixteenth notes
    constexpr double                  gateLength = 0.5;  // fraction of a step
    // Lock A, per step, normalised 0..1 over whatever parameter the host maps it to.
    constexpr std::array<double, 16>  locks{ 0.8, 0.2, 0.5, 0.35, 0.8, 0.2, 0.5, 0.35, 0.65, 0.15, 0.4, 0.25, 0.65, 0.15, 0.4, 0.25 };

    const char* const features[] = { CLAP_PLUGIN_FEATURE_NOTE_EFFECT, nullptr };

    const clap_plugin_descriptor_t descriptor = {
        CLAP_VERSION_INIT,
        "org.oscairn.step-sequencer",
        "Oscairn Step Sequencer",
        "Oscairn",
        "https://github.com/jbschooley/oscairn",
        "",
        "",
        "0.1.0",
        "A 16-step pattern sequencer that follows the host transport",
        features,
    };

    struct Sequencer
    {
        clap_plugin_t      plugin{};
        const clap_host_t* host       = nullptr;
        bool               canLock    = false; // the host offers oscairn.param-target
        uint16_t           lockSpace  = 0;
        double             sampleRate = 48000.0;
        int64_t            lastStep   = -1; // the last step played; -1 when stopped
        int64_t            heldStep   = -1; // the step whose note is sounding
        int16_t            heldKey    = -1;
    };

    Sequencer& self( const clap_plugin_t* p )
    {
        return *static_cast<Sequencer*>( p->plugin_data );
    }

    struct Pending
    {
        enum Kind
        {
            Off, // ordered first at the same time
            Lock,
            On
        };
        int64_t time;
        Kind    kind;
        int16_t key;
        double  value; // velocity for On, lock value for Lock
    };

    bool pushLock( const Sequencer& s, const clap_output_events_t* out, uint32_t time, double value )
    {
        oscairn_event_param_target_t e{};
        e.header.size     = sizeof( e );
        e.header.time     = time;
        e.header.space_id = s.lockSpace;
        e.header.type     = OSCAIRN_EVENT_PARAM_TARGET;
        e.lane            = 0;
        e.mode            = OSCAIRN_PARAM_TARGET_SET;
        e.note_id         = -1;
        e.port_index = e.channel = e.key = -1;
        e.value           = value;
        return out->try_push( out, &e.header );
    }

    bool pushNote( const clap_output_events_t* out, uint32_t time, bool on, int16_t key, double velocity )
    {
        clap_event_note_t e{};
        e.header.size     = sizeof( e );
        e.header.time     = time;
        e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        e.header.type     = on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF;
        e.note_id         = -1;
        e.port_index      = 0;
        e.channel         = 0;
        e.key             = key;
        e.velocity        = velocity;
        return out->try_push( out, &e.header );
    }

    clap_process_status process( const clap_plugin_t* p, const clap_process_t* process )
    {
        auto&          s      = self( p );
        const auto*    t      = process->transport;
        const auto     frames = (int64_t)process->frames_count;

        const bool playing = t && ( t->flags & CLAP_TRANSPORT_IS_PLAYING ) && ( t->flags & CLAP_TRANSPORT_HAS_TEMPO )
                          && ( t->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE ) && t->tempo > 0.0;
        if( !playing )
        {
            if( s.heldKey >= 0 )
                pushNote( process->out_events, 0, false, s.heldKey, 0.0 );
            s.heldKey = -1;
            s.heldStep = s.lastStep = -1;
            return CLAP_PROCESS_CONTINUE;
        }

        const double beatStart      = (double)t->song_pos_beats / (double)CLAP_BEATTIME_FACTOR;
        const double samplesPerBeat = s.sampleRate * 60.0 / t->tempo;
        auto offsetOf = [&]( double beat ) { return (int64_t)std::llround( ( beat - beatStart ) * samplesPerBeat ); };

        // Where to continue: after the last step played, unless the position jumped
        // (backwards, or forwards by more than a step), or we are just starting. On a
        // start or jump, a step that began within its gate time still plays, late,
        // rather than being skipped (a clocked start always arrives slightly after
        // the beat).
        int64_t next = s.lastStep + 1;
        if( s.lastStep < 0 || next * stepBeats < beatStart - stepBeats || s.lastStep * stepBeats > beatStart + 1e-9 )
        {
            const auto current = (int64_t)std::floor( beatStart / stepBeats + 1e-9 );
            const bool inGate  = beatStart - current * stepBeats < gateLength * stepBeats;
            next               = std::max<int64_t>( inGate ? current : current + 1, 0 );
        }

        std::array<Pending, 64> pending{}; // hosts split blocks to maxFrames, so this is ample
        size_t                  count = 0;
        auto endHeldNoteBefore = [&]( int64_t limit ) {
            if( s.heldStep < 0 )
                return;
            const auto off = std::max<int64_t>( 0, offsetOf( ( s.heldStep + gateLength ) * stepBeats ) );
            if( off < limit )
            {
                pending[count++] = { off, Pending::Off, s.heldKey, 0.0 };
                s.heldStep = -1;
                s.heldKey  = -1;
            }
        };

        for( int64_t k = next; count + 3 <= pending.size(); ++k )
        {
            const auto on = std::max<int64_t>( 0, offsetOf( k * stepBeats ) );
            if( on >= frames )
                break;
            endHeldNoteBefore( on + 1 );
            if( s.heldStep >= 0 ) // still sounding at the next step: end it there
            {
                pending[count++] = { on, Pending::Off, s.heldKey, 0.0 };
                s.heldStep = -1;
            }
            const int16_t key = pattern[(size_t)( k % (int64_t)pattern.size() )];
            if( s.canLock )
                pending[count++] = { on, Pending::Lock, 0, locks[(size_t)( k % (int64_t)locks.size() )] };
            pending[count++]  = { on, Pending::On, key, ( k % 4 == 0 ) ? 1.0 : 0.7 };
            s.heldStep        = k;
            s.heldKey         = key;
            s.lastStep        = k;
        }
        endHeldNoteBefore( frames );

        std::stable_sort( pending.begin(), pending.begin() + count, []( const Pending& a, const Pending& b ) {
            return a.time != b.time ? a.time < b.time : a.kind < b.kind; // offs, then locks, then ons
        } );
        for( size_t i = 0; i < count; ++i )
        {
            const auto& e = pending[i];
            if( e.kind == Pending::Lock )
                pushLock( s, process->out_events, (uint32_t)e.time, e.value );
            else
                pushNote( process->out_events, (uint32_t)e.time, e.kind == Pending::On, e.key, e.value );
        }
        return CLAP_PROCESS_CONTINUE;
    }

    uint32_t notePortsCount( const clap_plugin_t*, bool isInput )
    {
        return isInput ? 0 : 1;
    }

    bool notePortsGet( const clap_plugin_t*, uint32_t index, bool isInput, clap_note_port_info_t* info )
    {
        if( isInput || index != 0 )
            return false;
        info->id                 = 0;
        info->supported_dialects = CLAP_NOTE_DIALECT_CLAP;
        info->preferred_dialect  = CLAP_NOTE_DIALECT_CLAP;
        std::snprintf( info->name, sizeof( info->name ), "%s", "Notes" );
        return true;
    }

    const clap_plugin_note_ports_t notePorts = { notePortsCount, notePortsGet };

    const oscairn_plugin_param_target_lanes_t lanes = {
        []( const clap_plugin_t* ) -> uint32_t { return 1; },
        []( const clap_plugin_t*, uint32_t index, oscairn_param_target_lane_info_t* info ) {
            if( index != 0 )
                return false;
            info->lane = 0;
            std::snprintf( info->name, sizeof( info->name ), "%s", "Lock A" );
            return true;
        },
    };

    void stop( Sequencer& s )
    {
        s.lastStep = s.heldStep = -1;
        s.heldKey               = -1;
    }

    const clap_plugin_t* create( const clap_host_t* host )
    {
        auto* s                    = new Sequencer;
        s->host                    = host;
        s->plugin.desc             = &descriptor;
        s->plugin.plugin_data      = s;
        s->plugin.init             = []( const clap_plugin_t* p ) {
            auto&       s        = self( p );
            const auto* registry = static_cast<const clap_host_event_registry_t*>( s.host->get_extension( s.host, CLAP_EXT_EVENT_REGISTRY ) );
            s.canLock = registry && registry->query && registry->query( s.host, OSCAIRN_EVENT_SPACE_PARAM_TARGET, &s.lockSpace );
            return true;
        };
        s->plugin.destroy          = []( const clap_plugin_t* p ) { delete &self( p ); };
        s->plugin.activate         = []( const clap_plugin_t* p, double rate, uint32_t, uint32_t ) {
            self( p ).sampleRate = rate;
            stop( self( p ) );
            return true;
        };
        s->plugin.deactivate       = []( const clap_plugin_t* ) {};
        s->plugin.start_processing = []( const clap_plugin_t* ) { return true; };
        s->plugin.stop_processing  = []( const clap_plugin_t* ) {};
        s->plugin.reset            = []( const clap_plugin_t* p ) { stop( self( p ) ); };
        s->plugin.process          = process;
        s->plugin.get_extension    = []( const clap_plugin_t*, const char* id ) -> const void* {
            if( std::strcmp( id, CLAP_EXT_NOTE_PORTS ) == 0 )
                return &notePorts;
            if( std::strcmp( id, OSCAIRN_EXT_PARAM_TARGET_LANES ) == 0 )
                return &lanes;
            return nullptr;
        };
        s->plugin.on_main_thread   = []( const clap_plugin_t* ) {};
        return &s->plugin;
    }

    const clap_plugin_factory_t factory = {
        []( const clap_plugin_factory_t* ) -> uint32_t { return 1; },
        []( const clap_plugin_factory_t*, uint32_t index ) -> const clap_plugin_descriptor_t* { return index == 0 ? &descriptor : nullptr; },
        []( const clap_plugin_factory_t*, const clap_host_t* host, const char* id ) -> const clap_plugin_t* {
            return std::strcmp( id, descriptor.id ) == 0 ? create( host ) : nullptr;
        },
    };
}

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    []( const char* ) { return true; },
    []() {},
    []( const char* id ) -> const void* { return std::strcmp( id, CLAP_PLUGIN_FACTORY_ID ) == 0 ? &factory : nullptr; },
};
