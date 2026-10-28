// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The delay must not click when the tempo changes. A 5 Hz sine (slow, so a jump shows as a large step) goes through it for two
// seconds at 120 BPM, then the tempo jumps to 150 BPM. A jump in the delay time would put a
// step in the echoes; with the glide, no sample-to-sample step in the output may be more
// than four times the largest step before the change (gliding bends the echoes' pitch,
// which steepens them a little).

#include "oscairn/transport.h"

#include <kick_clap/Discovery.h>
#include <kick_clap/Host.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

int main( int argc, char** argv )
{
    if( argc != 2 )
    {
        std::fprintf( stderr, "usage: delay-glide path/to/oscairn-delay.clap\n" );
        return 2;
    }
    constexpr double   rate  = 48000.0;
    constexpr uint32_t block = 256;
    const auto         scan  = kick::clap::inspect( argv[1] );
    kick::clap::Host   host;
    if( scan.plugins.empty() || !host.load( scan.plugins.front(), rate, block ) )
        return std::fprintf( stderr, "FAIL: load\n" ), 1;

    oscairn::Transport transport( rate, 120.0 );
    std::vector<float> inL( block ), inR( block ), outL( block ), outR( block );
    const float*       inputs[]  = { inL.data(), inR.data() };
    float*             outputs[] = { outL.data(), outR.data() };
    float              last = 0.0f, before = 0.0f, after = 0.0f;
    int64_t            n    = 0;
    const int64_t      change = 2 * (int64_t)rate, end = 4 * (int64_t)rate;
    for( ; n < end; n += block )
    {
        if( n == change - change % block )
            transport.setTempo( 150.0 );
        for( uint32_t i = 0; i < block; ++i )
            inL[i] = inR[i] = 0.5f * (float)std::sin( 2.0 * M_PI * 5.0 * (double)( n + i ) / rate );
        const auto t = transport.current();
        if( !host.process( inputs, 2, outputs, 2, block, nullptr, 0, &t ) )
            return std::fprintf( stderr, "FAIL: process\n" ), 1;
        for( uint32_t i = 0; i < block; ++i )
        {
            const float step = std::abs( outL[i] - last );
            last             = outL[i];
            if( n + i < 1 )
                continue;
            ( n + (int64_t)i < change ? before : after ) = std::max( n + (int64_t)i < change ? before : after, step );
        }
        transport.advance( block );
    }
    if( before <= 0.0f || after > 4.0f * before )
    {
        std::fprintf( stderr, "FAIL: largest step %f after the tempo change, %f before\n", after, before );
        return 1;
    }
    std::printf( "ok: largest step %f after the tempo change, %f before\n", after, before );
    return 0;
}
