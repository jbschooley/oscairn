// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Oscairn Step Sequencer: a CLAP note-effect plugin. It plays a 16-step pattern of
// sixteenth notes in time with the host transport and sends the notes on to the
// next plugin. It has no audio ports.
//
// Step times are worked out in whole samples from the transport position, so the
// output does not depend on the host's block size. A tempo change takes effect from
// the position where it happens, measured from the start of the timeline (good
// enough for a fixed-tempo demo; a tempo map comes later).

#include <clap/clap.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
    // A minor arpeggio bassline, alternating octaves.
    constexpr std::array<int16_t, 16> pattern{ 45, 57, 52, 57, 48, 60, 55, 60, 43, 55, 50, 55, 47, 59, 52, 59 };
    constexpr double                  gateLength = 0.5; // fraction of a step

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
        double             sampleRate = 48000.0;
        int16_t            heldKey    = -1; // the note currently sounding, if any
    };

    Sequencer& self( const clap_plugin_t* p )
    {
        return *static_cast<Sequencer*>( p->plugin_data );
    }

    struct Pending
    {
        int64_t time;
        bool    on;
        int16_t key;
        double  velocity;
    };

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
        const uint32_t frames = process->frames_count;

        const bool playing = t && ( t->flags & CLAP_TRANSPORT_IS_PLAYING ) && ( t->flags & CLAP_TRANSPORT_HAS_TEMPO )
                          && ( t->flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE ) && t->tempo > 0.0;
        if( !playing )
        {
            if( s.heldKey >= 0 )
                pushNote( process->out_events, 0, false, s.heldKey, 0.0 );
            s.heldKey = -1;
            return CLAP_PROCESS_CONTINUE;
        }

        const auto   start       = (int64_t)std::llround( (double)t->song_pos_seconds / (double)CLAP_SECTIME_FACTOR * s.sampleRate );
        const auto   end         = start + frames;
        const double stepSamples = s.sampleRate * 60.0 / t->tempo / 4.0;
        const auto   gateSamples = (int64_t)std::llround( gateLength * stepSamples );

        // Every note-on and note-off that falls in this block, including the off of
        // a step that started in an earlier block.
        std::array<Pending, 64> pending{}; // hosts split blocks to maxFrames, so this is ample
        size_t                 count = 0;
        for( auto k = std::max<int64_t>( 0, (int64_t)std::floor( (double)start / stepSamples ) - 1 ); count + 2 <= pending.size(); ++k )
        {
            const auto on = (int64_t)std::llround( (double)k * stepSamples );
            if( on >= end )
                break;
            const int16_t key      = pattern[(size_t)( k % (int64_t)pattern.size() )];
            const double  velocity = ( k % 4 == 0 ) ? 1.0 : 0.7;
            if( on >= start )
                pending[count++] = { on, true, key, velocity };
            const auto off = on + gateSamples;
            if( off >= start && off < end )
                pending[count++] = { off, false, key, 0.0 };
        }
        std::stable_sort( pending.begin(), pending.begin() + count, []( const Pending& a, const Pending& b ) {
            return a.time != b.time ? a.time < b.time : ( !a.on && b.on ); // offs first
        } );

        for( size_t i = 0; i < count; ++i )
        {
            const auto& e = pending[i];
            pushNote( process->out_events, (uint32_t)( e.time - start ), e.on, e.key, e.velocity );
            s.heldKey = e.on ? e.key : int16_t( -1 );
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

    const clap_plugin_t* create( const clap_host_t* host )
    {
        auto* s                         = new Sequencer;
        s->host                         = host;
        s->plugin.desc                  = &descriptor;
        s->plugin.plugin_data           = s;
        s->plugin.init                  = []( const clap_plugin_t* ) { return true; };
        s->plugin.destroy               = []( const clap_plugin_t* p ) { delete &self( p ); };
        s->plugin.activate              = []( const clap_plugin_t* p, double rate, uint32_t, uint32_t ) {
            self( p ).sampleRate = rate;
            return true;
        };
        s->plugin.deactivate            = []( const clap_plugin_t* ) {};
        s->plugin.start_processing      = []( const clap_plugin_t* ) { return true; };
        s->plugin.stop_processing       = []( const clap_plugin_t* ) {};
        s->plugin.reset                 = []( const clap_plugin_t* p ) { self( p ).heldKey = -1; };
        s->plugin.process               = process;
        s->plugin.get_extension         = []( const clap_plugin_t*, const char* id ) -> const void* {
            return std::strcmp( id, CLAP_EXT_NOTE_PORTS ) == 0 ? &notePorts : nullptr;
        };
        s->plugin.on_main_thread        = []( const clap_plugin_t* ) {};
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
