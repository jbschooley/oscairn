// SPDX-License-Identifier: AGPL-3.0-or-later
//
// A tempo change keeps the beat position and applies from there: at 120 BPM, one second
// is beat 2; changing to 240 BPM must leave it at beat 2, and the next second adds 4.

#include "oscairn/transport.h"

#include <cmath>
#include <cstdio>

namespace
{
    double beats( const oscairn::Transport& t )
    {
        return (double)t.current().song_pos_beats / (double)CLAP_BEATTIME_FACTOR;
    }
}

int main()
{
    constexpr double   rate  = 48000.0;
    constexpr uint32_t block = 480;
    oscairn::Transport t( rate, 120.0 );
    for( int i = 0; i < 100; ++i )
        t.advance( block );
    const double before = beats( t );
    t.setTempo( 240.0 );
    const double after = beats( t );
    for( int i = 0; i < 100; ++i )
        t.advance( block );
    const double later = beats( t );

    if( std::abs( before - 2.0 ) > 1e-6 || std::abs( after - before ) > 1e-6 || std::abs( later - 6.0 ) > 1e-6 )
    {
        std::fprintf( stderr, "FAIL: beats %f before the change, %f just after, %f a second later (expected 2, 2, 6)\n", before, after, later );
        return 1;
    }
    std::printf( "ok: tempo change from 120 to 240 BPM keeps beat 2, then runs at the new tempo\n" );
    return 0;
}
