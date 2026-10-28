// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host behaviour Oscairn relies on for live use, checked with the Oscairn Basic Synth:
// a late event in a block is dropped on its own; the notes around it still play.

#include <kick_clap/Discovery.h>
#include <kick_clap/Host.h>

#include <array>
#include <cmath>
#include <cstdio>

namespace
{
    bool fail( const char* why )
    {
        std::fprintf( stderr, "FAIL: %s\n", why );
        return false;
    }
}

int main( int argc, char** argv )
{
    if( argc != 2 )
    {
        std::fprintf( stderr, "usage: host-policy path/to/oscairn-basic-synth.clap\n" );
        return 2;
    }
    const auto scan = kick::clap::inspect( argv[1] );
    if( scan.plugins.empty() )
        return fail( "synth not found" ), 1;

    kick::clap::Host host;
    if( !host.load( scan.plugins.front(), 48000, 256 ) )
        return fail( "load" ), 1;

    std::array<float, 256> left{}, right{};
    float*                 out[] = { left.data(), right.data() };

    // A valid note-on, then an event whose time is past the end of the block.
    const kick::clap::Event events[] = {
        kick::clap::Event::note( true, 57, 1.0, 0 ),
        kick::clap::Event::note( true, 60, 1.0, 300 ), // late: the block is 256 frames
    };
    if( !host.process( nullptr, 0, out, 2, 256, events, 2 ) )
        return fail( "process" ), 1;
    if( host.statistics().droppedEvents != 1 )
        return fail( "the late event should be dropped and counted" ), 1;

    // The first note must still be sounding in the next block: no reset happened.
    if( !host.process( nullptr, 0, out, 2, 256 ) )
        return fail( "second process" ), 1;
    float peak = 0;
    for( float s : left )
        peak = std::max( peak, std::abs( s ) );
    if( peak < 0.01f )
        return fail( "the valid note was cut: the plugin was reset" ), 1;

    std::printf( "ok: late event dropped (%llu), valid note still playing (peak %.3f)\n",
                 (unsigned long long)host.statistics().droppedEvents, peak );
    return 0;
}
