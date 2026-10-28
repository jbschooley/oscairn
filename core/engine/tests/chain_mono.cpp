// SPDX-License-Identifier: AGPL-3.0-or-later
//
// A mono instrument in a chain plays in the centre: the chain copies its single
// output channel to both sides. Uses the mono instrument from clap-host's test fixture.

#include "oscairn/chain.h"

#include <array>
#include <cstdio>

int main( int argc, char** argv )
{
    if( argc != 2 )
    {
        std::fprintf( stderr, "usage: chain-mono path/to/kick-clap-fixture-normal.clap\n" );
        return 2;
    }
    oscairn::Chain chain( 48000, 256 );
    chain.add( argv[1] ); // the module's first plugin is the mono instrument

    std::array<float, 256> left{}, right{};
    float*                 out[]  = { left.data(), right.data() };
    const kick::clap::Event note[] = { kick::clap::Event::note( true, 60, 0.8, 10 ) };
    if( !chain.process( out, 256, nullptr, note, 1 ) )
    {
        std::fprintf( stderr, "FAIL: process\n" );
        return 1;
    }
    for( size_t i = 0; i < left.size(); ++i )
        if( left[i] != right[i] || ( i >= 10 && left[i] == 0.0f ) )
        {
            std::fprintf( stderr, "FAIL: sample %zu: left %f right %f\n", i, left[i], right[i] );
            return 1;
        }
    std::printf( "ok: mono instrument plays on both channels (%f)\n", left[100] );
    return 0;
}
