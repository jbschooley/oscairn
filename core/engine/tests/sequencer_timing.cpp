// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Plays the step sequencer through the CLAP host against the Oscairn transport, in
// uneven blocks, and checks that every note lands on its exact sample: note-on at
// step * stepSamples and note-off half a step later.

#include "oscairn/transport.h"

#include <kick_clap/Discovery.h>
#include <kick_clap/Host.h>

#include <cmath>
#include <cstdio>
#include <vector>

int main( int argc, char** argv )
{
    if( argc != 2 )
    {
        std::fprintf( stderr, "usage: sequencer-timing path/to/oscairn-step-sequencer.clap\n" );
        return 2;
    }

    constexpr double   rate = 48000.0, tempo = 120.0;
    constexpr uint32_t maxFrames = 512;
    const auto         scan      = kick::clap::inspect( argv[1] );
    if( scan.plugins.empty() )
    {
        std::fprintf( stderr, "FAIL: sequencer not found\n" );
        return 1;
    }
    kick::clap::Host host;
    if( !host.load( scan.plugins.front(), rate, maxFrames ) )
    {
        std::fprintf( stderr, "FAIL: load: %s\n", host.error().c_str() );
        return 1;
    }

    oscairn::Transport transport( rate, tempo );
    struct Note
    {
        int64_t time;
        bool    on;
    };
    std::vector<Note> notes;
    const uint32_t    blocks[] = { 37, 512, 1, 300, 129 }; // uneven on purpose
    int64_t           position = 0;
    for( size_t b = 0; position < (int64_t)( 4 * rate ); ++b )
    {
        const uint32_t frames = blocks[b % 5];
        const auto     t      = transport.current();
        if( !host.process( nullptr, 0, nullptr, 0, frames, nullptr, 0, &t ) )
        {
            std::fprintf( stderr, "FAIL: process at %lld: %s\n", (long long)position, host.error().c_str() );
            return 1;
        }
        uint32_t    count  = 0;
        const auto* events = host.outputEvents( count );
        for( uint32_t i = 0; i < count; ++i )
        {
            const auto* h = events[i].header();
            if( position + h->time < (int64_t)( 4 * rate ) ) // the last block may run past the end
                notes.push_back( { position + h->time, h->type == CLAP_EVENT_NOTE_ON } );
        }
        transport.advance( frames );
        position += frames;
    }

    const double stepSamples = rate * 60.0 / tempo / 4.0; // 6000
    const auto   steps       = (size_t)( 4 * rate / stepSamples );
    if( notes.size() != steps * 2 )
    {
        std::fprintf( stderr, "FAIL: expected %zu note events, got %zu\n", steps * 2, notes.size() );
        return 1;
    }
    for( size_t k = 0; k < steps; ++k )
    {
        const auto on = (int64_t)std::llround( k * stepSamples );
        const auto off = on + (int64_t)std::llround( 0.5 * stepSamples );
        if( !notes[2 * k].on || notes[2 * k].time != on || notes[2 * k + 1].on || notes[2 * k + 1].time != off )
        {
            std::fprintf( stderr, "FAIL: step %zu: expected on %lld / off %lld, got %s %lld / %s %lld\n", k, (long long)on,
                          (long long)off, notes[2 * k].on ? "on" : "off", (long long)notes[2 * k].time,
                          notes[2 * k + 1].on ? "on" : "off", (long long)notes[2 * k + 1].time );
            return 1;
        }
    }
    std::printf( "ok: %zu steps, every note-on and note-off on its exact sample\n", steps );
    return 0;
}
