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
//
// Every step is editable through CLAP parameters: on/off (ids 100-115), note (200-215)
// and lock value (300-315). The default pattern is an A minor bassline. A parameter
// change takes effect from its sample: it applies to steps that start at or after it.

#include <clap/clap.h>
#include <oscairn-clap/param-target.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{
    // The default pattern: an A minor arpeggio bassline, alternating octaves.
    constexpr std::array<int16_t, 16> pattern{ 45, 57, 52, 57, 48, 60, 55, 60, 43, 55, 50, 55, 47, 59, 52, 59 };
    constexpr double                  stepBeats  = 0.25; // sixteenth notes
    constexpr double                  gateLength = 0.5;  // fraction of a step
    // Default Lock A, per step, normalised 0..1 over whatever parameter the host maps it to.
    constexpr std::array<double, 16>  locks{ 0.8, 0.2, 0.5, 0.35, 0.8, 0.2, 0.5, 0.35, 0.65, 0.15, 0.4, 0.25, 0.65, 0.15, 0.4, 0.25 };

    constexpr size_t steps = pattern.size();

    // Parameter ids: kind * 100 + step.
    enum ParamKind : clap_id
    {
        stepOnKind   = 1,
        stepNoteKind = 2,
        stepLockKind = 3,
    };

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
        // Values are atomic: the main thread reads them (get_value) while audio runs.
        std::array<std::atomic<double>, steps> on, note, lock;

        Sequencer()
        {
            for( size_t i = 0; i < steps; ++i )
            {
                on[i]   = 1.0;
                note[i] = pattern[i];
                lock[i] = locks[i];
            }
        }
    };

    Sequencer& self( const clap_plugin_t* p )
    {
        return *static_cast<Sequencer*>( p->plugin_data );
    }

    // The parameter a step parameter id names, or nullptr.
    std::atomic<double>* stepParam( Sequencer& s, clap_id id )
    {
        const auto step = id % 100;
        if( step >= steps )
            return nullptr;
        switch( id / 100 )
        {
        case stepOnKind: return &s.on[step];
        case stepNoteKind: return &s.note[step];
        case stepLockKind: return &s.lock[step];
        default: return nullptr;
        }
    }

    double clampParam( clap_id id, double value )
    {
        switch( id / 100 )
        {
        case stepOnKind: return value >= 0.5 ? 1.0 : 0.0;
        case stepNoteKind: return std::clamp( std::round( value ), 0.0, 127.0 );
        default: return std::clamp( value, 0.0, 1.0 );
        }
    }

    void applyParam( Sequencer& s, const clap_event_header_t* header )
    {
        if( header->space_id != CLAP_CORE_EVENT_SPACE_ID || header->type != CLAP_EVENT_PARAM_VALUE )
            return;
        const auto* e = reinterpret_cast<const clap_event_param_value_t*>( header );
        if( auto* param = stepParam( s, e->param_id ) )
            *param = clampParam( e->param_id, e->value );
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

        // Input events are in time order; apply parameter changes up to a given sample.
        const auto* in       = process->in_events;
        uint32_t    inNext   = 0;
        const auto  inCount  = in->size( in );
        auto applyParamsUntil = [&]( int64_t time ) {
            for( ; inNext < inCount && (int64_t)in->get( in, inNext )->time <= time; ++inNext )
                applyParam( s, in->get( in, inNext ) );
        };

        const bool playing = t && ( t->flags & CLAP_TRANSPORT_IS_PLAYING ) && ( t->flags & CLAP_TRANSPORT_HAS_TEMPO )
                          && ( t->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE ) && t->tempo > 0.0;
        if( !playing )
        {
            applyParamsUntil( frames );
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
                s.heldKey  = -1;
            }
            s.lastStep = k;
            applyParamsUntil( on );
            const auto step = (size_t)( k % (int64_t)steps );
            if( s.on[step] < 0.5 )
                continue; // a rest: the previous note has already ended
            const auto key = (int16_t)s.note[step].load();
            if( s.canLock )
                pending[count++] = { on, Pending::Lock, 0, s.lock[step] };
            pending[count++]  = { on, Pending::On, key, ( k % 4 == 0 ) ? 1.0 : 0.7 };
            s.heldStep        = k;
            s.heldKey         = key;
        }
        endHeldNoteBefore( frames );
        applyParamsUntil( frames );

        const auto before = []( const Pending& a, const Pending& b ) {
            return a.time != b.time ? a.time < b.time : a.kind < b.kind; // offs, then locks, then ons
        };
        // Stable insertion sort for at most 64 events. std::stable_sort may allocate
        // a temporary buffer, which is not allowed in the audio callback.
        for( size_t i = 1; i < count; ++i )
        {
            const auto event = pending[i];
            auto j = i;
            while( j > 0 && before( event, pending[j - 1] ) )
            {
                pending[j] = pending[j - 1];
                --j;
            }
            pending[j] = event;
        }
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

    uint32_t paramsCount( const clap_plugin_t* )
    {
        return 3 * steps;
    }

    bool paramsInfo( const clap_plugin_t*, uint32_t index, clap_param_info_t* info )
    {
        if( index >= 3 * steps )
            return false;
        const auto kind = (clap_id)( index / steps + 1 ), step = (clap_id)( index % steps );
        *info    = {};
        info->id = kind * 100 + step;
        info->flags = CLAP_PARAM_IS_AUTOMATABLE;
        switch( kind )
        {
        case stepOnKind:
            info->flags |= CLAP_PARAM_IS_STEPPED;
            info->max_value     = 1.0;
            info->default_value = 1.0;
            std::snprintf( info->name, sizeof( info->name ), "Step %u On", step + 1 );
            break;
        case stepNoteKind:
            info->flags |= CLAP_PARAM_IS_STEPPED;
            info->max_value     = 127.0;
            info->default_value = pattern[step];
            std::snprintf( info->name, sizeof( info->name ), "Step %u Note", step + 1 );
            break;
        default:
            info->max_value     = 1.0;
            info->default_value = locks[step];
            std::snprintf( info->name, sizeof( info->name ), "Step %u Lock A", step + 1 );
            break;
        }
        std::snprintf( info->module, sizeof( info->module ), "Step %u", step + 1 );
        return true;
    }

    bool paramsValue( const clap_plugin_t* p, clap_id id, double* value )
    {
        const auto* param = stepParam( self( p ), id );
        if( !param )
            return false;
        *value = *param;
        return true;
    }

    bool paramsToText( const clap_plugin_t* p, clap_id id, double value, char* text, uint32_t size )
    {
        if( !stepParam( self( p ), id ) )
            return false;
        switch( id / 100 )
        {
        case stepOnKind: std::snprintf( text, size, "%s", value >= 0.5 ? "On" : "Off" ); break;
        case stepNoteKind: std::snprintf( text, size, "%.0f", value ); break;
        default: std::snprintf( text, size, "%.0f%%", value * 100.0 ); break;
        }
        return true;
    }

    bool paramsFromText( const clap_plugin_t* p, clap_id id, const char* text, double* value )
    {
        if( !stepParam( self( p ), id ) )
            return false;
        switch( id / 100 )
        {
        case stepOnKind: *value = std::strcmp( text, "On" ) == 0 ? 1.0 : std::strcmp( text, "Off" ) == 0 ? 0.0 : clampParam( id, std::atof( text ) ); break;
        case stepNoteKind: *value = clampParam( id, std::atof( text ) ); break;
        default: *value = clampParam( id, std::atof( text ) / 100.0 ); break;
        }
        return true;
    }

    void paramsFlush( const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t* )
    {
        for( uint32_t i = 0, n = in->size( in ); i < n; ++i )
            applyParam( self( p ), in->get( in, i ) );
    }

    const clap_plugin_params_t params = { paramsCount, paramsInfo, paramsValue, paramsToText, paramsFromText, paramsFlush };

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
            if( std::strcmp( id, CLAP_EXT_PARAMS ) == 0 )
                return &params;
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
