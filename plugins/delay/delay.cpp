// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Oscairn Delay: a tempo-synced stereo ping-pong delay (CLAP audio effect).
// The delay time is a dotted eighth note at the transport tempo (375 ms when the host
// gives no tempo). Each repeat crosses to the other side. The output does not depend
// on the host's block size.
//
// When the tempo changes, the delay time glides to the new value (a 50 ms time constant,
// at most half a sample per sample), like a tape delay, rather than jumping, which would
// click.

#include <clap/clap.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
    constexpr double feedback = 0.45;
    constexpr double wet      = 0.3;
    constexpr double maxDelaySeconds = 2.0;
    constexpr double glideSeconds    = 0.05; // time constant of the delay time's glide

    const char* const features[] = { CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_DELAY, CLAP_PLUGIN_FEATURE_STEREO, nullptr };

    const clap_plugin_descriptor_t descriptor = {
        CLAP_VERSION_INIT,
        "org.oscairn.delay",
        "Oscairn Delay",
        "Oscairn",
        "https://github.com/jbschooley/oscairn",
        "",
        "",
        "0.1.0",
        "A tempo-synced stereo ping-pong delay",
        features,
    };

    struct Delay
    {
        clap_plugin_t      plugin{};
        double             sampleRate = 48000.0;
        std::vector<float> left, right; // circular buffers
        size_t             write = 0;
        double             delay = -1.0; // current delay in samples, gliding; -1 until the first block
        double             glide = 0.0;  // per-sample glide coefficient
    };

    Delay& self( const clap_plugin_t* p )
    {
        return *static_cast<Delay*>( p->plugin_data );
    }

    clap_process_status process( const clap_plugin_t* p, const clap_process_t* process )
    {
        auto& s = self( p );
        if( process->audio_outputs_count < 1 || process->audio_outputs[0].channel_count < 2 || s.left.empty() )
            return CLAP_PROCESS_ERROR;

        const bool    hasInput = process->audio_inputs_count > 0 && process->audio_inputs[0].channel_count >= 2;
        const float*  inL      = hasInput ? process->audio_inputs[0].data32[0] : nullptr;
        const float*  inR      = hasInput ? process->audio_inputs[0].data32[1] : nullptr;
        float*        outL     = process->audio_outputs[0].data32[0];
        float*        outR     = process->audio_outputs[0].data32[1];

        const auto* t        = process->transport;
        const double seconds = ( t && ( t->flags & CLAP_TRANSPORT_HAS_TEMPO ) && t->tempo > 0.0 ) ? 0.75 * 60.0 / t->tempo : 0.375;
        const size_t size    = s.left.size();
        const double target  = (double)std::clamp<long long>( std::llround( seconds * s.sampleRate ), 1, (long long)size - 2 );
        if( s.delay < 0.0 )
            s.delay = target;

        for( uint32_t i = 0; i < process->frames_count; ++i )
        {
            if( s.delay != target )
            {
                // At most half a sample per sample: the echoes bend in pitch by up to
                // half (an octave down to a fifth up) while gliding, never backwards.
                s.delay += std::clamp( ( target - s.delay ) * s.glide, -0.5, 0.5 );
                if( std::abs( target - s.delay ) < 1e-3 )
                    s.delay = target;
            }
            // Read between samples while gliding; at a whole number of samples this is
            // exactly the stored sample.
            const double whole = std::floor( s.delay );
            const auto   frac  = (float)( s.delay - whole );
            const size_t read  = ( s.write + size - (size_t)whole ) % size; // `whole` samples ago
            const size_t older = ( read + size - 1 ) % size;
            const float  dryL  = inL ? inL[i] : 0.0f;
            const float  dryR  = inR ? inR[i] : 0.0f;
            const float  echoL = frac == 0.0f ? s.left[read] : s.left[read] + frac * ( s.left[older] - s.left[read] );
            const float  echoR = frac == 0.0f ? s.right[read] : s.right[read] + frac * ( s.right[older] - s.right[read] );

            // Ping-pong: the left line is fed from the input and the right echoes,
            // and the other way round, so each repeat crosses sides.
            s.left[s.write]  = (float)( 0.5 * ( dryL + dryR ) + feedback * echoR );
            s.right[s.write] = (float)( feedback * echoL );
            s.write          = ( s.write + 1 ) % size;

            outL[i] = (float)( dryL + wet * echoL );
            outR[i] = (float)( dryR + wet * echoR );
        }
        return CLAP_PROCESS_CONTINUE;
    }

    uint32_t audioPortsCount( const clap_plugin_t*, bool )
    {
        return 1;
    }

    bool audioPortsGet( const clap_plugin_t*, uint32_t index, bool isInput, clap_audio_port_info_t* info )
    {
        if( index != 0 )
            return false;
        info->id            = 0;
        info->flags         = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type     = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        std::snprintf( info->name, sizeof( info->name ), "%s", isInput ? "Input" : "Output" );
        return true;
    }

    const clap_plugin_audio_ports_t audioPorts = { audioPortsCount, audioPortsGet };

    void clear( Delay& s )
    {
        std::fill( s.left.begin(), s.left.end(), 0.0f );
        std::fill( s.right.begin(), s.right.end(), 0.0f );
        s.write = 0;
        s.delay = -1.0;
    }

    const clap_plugin_t* create()
    {
        auto* s                    = new Delay;
        s->plugin.desc             = &descriptor;
        s->plugin.plugin_data      = s;
        s->plugin.init             = []( const clap_plugin_t* ) { return true; };
        s->plugin.destroy          = []( const clap_plugin_t* p ) { delete &self( p ); };
        s->plugin.activate         = []( const clap_plugin_t* p, double rate, uint32_t, uint32_t ) {
            auto& s      = self( p );
            s.sampleRate = rate;
            s.left.assign( (size_t)( maxDelaySeconds * rate ) + 1, 0.0f ); // allocate here, not on the audio thread
            s.right.assign( s.left.size(), 0.0f );
            s.write = 0;
            s.delay = -1.0;
            s.glide = 1.0 - std::exp( -1.0 / ( glideSeconds * rate ) );
            return true;
        };
        s->plugin.deactivate       = []( const clap_plugin_t* ) {};
        s->plugin.start_processing = []( const clap_plugin_t* ) { return true; };
        s->plugin.stop_processing  = []( const clap_plugin_t* ) {};
        s->plugin.reset            = []( const clap_plugin_t* p ) { clear( self( p ) ); };
        s->plugin.process          = process;
        s->plugin.get_extension    = []( const clap_plugin_t*, const char* id ) -> const void* {
            return std::strcmp( id, CLAP_EXT_AUDIO_PORTS ) == 0 ? &audioPorts : nullptr;
        };
        s->plugin.on_main_thread   = []( const clap_plugin_t* ) {};
        return &s->plugin;
    }

    const clap_plugin_factory_t factory = {
        []( const clap_plugin_factory_t* ) -> uint32_t { return 1; },
        []( const clap_plugin_factory_t*, uint32_t index ) -> const clap_plugin_descriptor_t* { return index == 0 ? &descriptor : nullptr; },
        []( const clap_plugin_factory_t*, const clap_host_t*, const char* id ) -> const clap_plugin_t* {
            return std::strcmp( id, descriptor.id ) == 0 ? create() : nullptr;
        },
    };
}

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    []( const char* ) { return true; },
    []() {},
    []( const char* id ) -> const void* { return std::strcmp( id, CLAP_PLUGIN_FACTORY_ID ) == 0 ? &factory : nullptr; },
};
