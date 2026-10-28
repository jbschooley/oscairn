// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Oscairn Basic Synth: a small polyphonic CLAP instrument for tests and demos.
// Each voice is a band-limited sawtooth through a two-pole low-pass filter whose
// cutoff follows the amplitude envelope, which gives a plucked bass sound.
// Events are applied on their exact sample, and the output does not depend on the
// host's block size.

#include <clap/clap.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
    constexpr double pi = 3.14159265358979323846;

    const char* const features[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, CLAP_PLUGIN_FEATURE_STEREO, nullptr };

    const clap_plugin_descriptor_t descriptor = {
        CLAP_VERSION_INIT,
        "org.oscairn.basic-synth",
        "Oscairn Basic Synth",
        "Oscairn",
        "https://github.com/jbschooley/oscairn",
        "",
        "",
        "0.1.0",
        "A small polyphonic subtractive synth for tests and demos",
        features,
    };

    enum class Stage
    {
        Attack,
        Decay,
        Release
    };

    struct Voice
    {
        bool     active   = false;
        Stage    stage    = Stage::Attack;
        int16_t  key      = 0;
        int16_t  channel  = 0;
        double   phase    = 0.0;
        double   increment = 0.0;
        double   level    = 0.0; // envelope
        double   velocity = 0.0;
        double   low1 = 0.0, low2 = 0.0;
        uint64_t started = 0;
    };

    struct Synth
    {
        clap_plugin_t          plugin{};
        double                 sampleRate = 48000.0;
        std::array<Voice, 16>  voices{};
        uint64_t               notesStarted = 0;
        double                 attackStep = 0.0, decayCoefficient = 0.0, releaseCoefficient = 0.0;
        static constexpr double sustain = 0.35;
    };

    Synth& self( const clap_plugin_t* p )
    {
        return *static_cast<Synth*>( p->plugin_data );
    }

    double polyBlep( double t, double dt )
    {
        if( t < dt )
        {
            t /= dt;
            return t + t - t * t - 1.0;
        }
        if( t > 1.0 - dt )
        {
            t = ( t - 1.0 ) / dt;
            return t * t + t + t + 1.0;
        }
        return 0.0;
    }

    void noteOn( Synth& s, int16_t key, int16_t channel, double velocity )
    {
        Voice* voice = nullptr;
        for( auto& v : s.voices )
            if( !v.active )
            {
                voice = &v;
                break;
            }
        if( !voice ) // steal the oldest voice
        {
            voice = &s.voices[0];
            for( auto& v : s.voices )
                if( v.started < voice->started )
                    voice = &v;
        }
        *voice           = Voice{};
        voice->active    = true;
        voice->key       = key;
        voice->channel   = channel;
        voice->velocity  = velocity;
        voice->increment = 440.0 * std::pow( 2.0, ( key - 69 ) / 12.0 ) / s.sampleRate;
        voice->started   = ++s.notesStarted;
    }

    void noteOff( Synth& s, int16_t key, int16_t channel )
    {
        for( auto& v : s.voices )
            if( v.active && v.stage != Stage::Release && ( key < 0 || v.key == key ) && ( channel < 0 || v.channel == channel ) )
                v.stage = Stage::Release;
    }

    void render( Synth& s, float* left, float* right, uint32_t from, uint32_t to )
    {
        for( uint32_t i = from; i < to; ++i )
        {
            double mix = 0.0;
            for( auto& v : s.voices )
            {
                if( !v.active )
                    continue;

                switch( v.stage )
                {
                    case Stage::Attack:
                        v.level += s.attackStep;
                        if( v.level >= 1.0 )
                        {
                            v.level = 1.0;
                            v.stage = Stage::Decay;
                        }
                        break;
                    case Stage::Decay:
                        v.level = Synth::sustain + ( v.level - Synth::sustain ) * s.decayCoefficient;
                        break;
                    case Stage::Release:
                        v.level *= s.releaseCoefficient;
                        if( v.level < 1e-4 )
                        {
                            v.active = false;
                            continue;
                        }
                        break;
                }

                const double saw = 2.0 * v.phase - 1.0 - polyBlep( v.phase, v.increment );
                v.phase += v.increment;
                if( v.phase >= 1.0 )
                    v.phase -= 1.0;

                const double cutoff = 120.0 + v.level * v.level * 5000.0 * v.velocity;
                const double g      = 1.0 - std::exp( -2.0 * pi * cutoff / s.sampleRate );
                v.low1 += g * ( saw - v.low1 );
                v.low2 += g * ( v.low1 - v.low2 );

                mix += v.low2 * v.level * v.velocity * 0.35;
            }
            const auto sample = (float)std::tanh( mix );
            left[i]           = sample;
            right[i]          = sample;
        }
    }

    clap_process_status process( const clap_plugin_t* p, const clap_process_t* process )
    {
        auto& s = self( p );
        if( process->audio_outputs_count < 1 || process->audio_outputs[0].channel_count < 2 )
            return CLAP_PROCESS_ERROR;
        float* left  = process->audio_outputs[0].data32[0];
        float* right = process->audio_outputs[0].data32[1];

        const uint32_t frames = process->frames_count;
        const uint32_t count  = process->in_events->size( process->in_events );
        uint32_t       done   = 0;
        for( uint32_t e = 0; e < count; ++e )
        {
            const auto* header = process->in_events->get( process->in_events, e );
            if( header->space_id != CLAP_CORE_EVENT_SPACE_ID )
                continue;
            const uint32_t at = header->time < frames ? header->time : frames;
            if( at > done )
            {
                render( s, left, right, done, at );
                done = at;
            }
            if( header->type == CLAP_EVENT_NOTE_ON || header->type == CLAP_EVENT_NOTE_OFF || header->type == CLAP_EVENT_NOTE_CHOKE )
            {
                const auto* note = reinterpret_cast<const clap_event_note_t*>( header );
                if( header->type == CLAP_EVENT_NOTE_ON && note->velocity > 0.0 )
                    noteOn( s, note->key, note->channel, note->velocity );
                else
                    noteOff( s, note->key, note->channel );
            }
        }
        render( s, left, right, done, frames );
        return CLAP_PROCESS_CONTINUE;
    }

    uint32_t audioPortsCount( const clap_plugin_t*, bool isInput )
    {
        return isInput ? 0 : 1;
    }

    bool audioPortsGet( const clap_plugin_t*, uint32_t index, bool isInput, clap_audio_port_info_t* info )
    {
        if( isInput || index != 0 )
            return false;
        info->id            = 0;
        info->flags         = CLAP_AUDIO_PORT_IS_MAIN;
        info->channel_count = 2;
        info->port_type     = CLAP_PORT_STEREO;
        info->in_place_pair = CLAP_INVALID_ID;
        std::snprintf( info->name, sizeof( info->name ), "%s", "Output" );
        return true;
    }

    uint32_t notePortsCount( const clap_plugin_t*, bool isInput )
    {
        return isInput ? 1 : 0;
    }

    bool notePortsGet( const clap_plugin_t*, uint32_t index, bool isInput, clap_note_port_info_t* info )
    {
        if( !isInput || index != 0 )
            return false;
        info->id                 = 0;
        info->supported_dialects = CLAP_NOTE_DIALECT_CLAP;
        info->preferred_dialect  = CLAP_NOTE_DIALECT_CLAP;
        std::snprintf( info->name, sizeof( info->name ), "%s", "Notes" );
        return true;
    }

    const clap_plugin_audio_ports_t audioPorts = { audioPortsCount, audioPortsGet };
    const clap_plugin_note_ports_t  notePorts  = { notePortsCount, notePortsGet };

    const clap_plugin_t* create()
    {
        auto* s                    = new Synth;
        s->plugin.desc             = &descriptor;
        s->plugin.plugin_data      = s;
        s->plugin.init             = []( const clap_plugin_t* ) { return true; };
        s->plugin.destroy          = []( const clap_plugin_t* p ) { delete &self( p ); };
        s->plugin.activate         = []( const clap_plugin_t* p, double rate, uint32_t, uint32_t ) {
            auto& s              = self( p );
            s.sampleRate         = rate;
            s.attackStep         = 1.0 / ( 0.002 * rate );
            s.decayCoefficient   = std::exp( -1.0 / ( 0.25 * rate ) );
            s.releaseCoefficient = std::exp( -1.0 / ( 0.12 * rate ) );
            s.voices             = {};
            return true;
        };
        s->plugin.deactivate       = []( const clap_plugin_t* ) {};
        s->plugin.start_processing = []( const clap_plugin_t* ) { return true; };
        s->plugin.stop_processing  = []( const clap_plugin_t* ) {};
        s->plugin.reset            = []( const clap_plugin_t* p ) { self( p ).voices = {}; };
        s->plugin.process          = process;
        s->plugin.get_extension    = []( const clap_plugin_t*, const char* id ) -> const void* {
            if( std::strcmp( id, CLAP_EXT_AUDIO_PORTS ) == 0 )
                return &audioPorts;
            if( std::strcmp( id, CLAP_EXT_NOTE_PORTS ) == 0 )
                return &notePorts;
            return nullptr;
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
