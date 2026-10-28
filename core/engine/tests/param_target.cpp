// SPDX-License-Identifier: AGPL-3.0-or-later
//
// oscairn.param-target: the step sequencer's lane 0 ("Lock A") mapped to the gain of the
// clap-host test fixture's instrument, whose output while a note is held is exactly its
// gain. Every step's output must equal that step's lock value from the step's first
// sample: the lock reaches the instrument in the same block and before the step's note.
// Checked with two block sizes. Without the mapping the gain stays at its default.

#include "oscairn/graph.h"
#include "oscairn/transport.h"

#include <array>
#include <cstdio>
#include <vector>

namespace
{
    constexpr double  rate = 48000.0;
    constexpr clap_id gainParam = 7;
    // The sequencer's Lock A values (plugins/step-sequencer/step_sequencer.cpp).
    constexpr std::array<double, 16> locks{ 0.8, 0.2, 0.5, 0.35, 0.8, 0.2, 0.5, 0.35, 0.65, 0.15, 0.4, 0.25, 0.65, 0.15, 0.4, 0.25 };

    std::vector<float> render( const char* sequencer, const char* fixture, uint32_t block, bool map )
    {
        oscairn::Graph g( rate, block );
        const auto     seq = g.add( sequencer ), inst = g.add( fixture );
        g.connectNotes( seq, inst );
        g.connectAudio( inst, oscairn::graphOutput );
        if( map )
            g.mapParamTarget( seq, 0, inst, gainParam );
        g.commit();

        oscairn::Transport transport( rate, 120.0 );
        std::vector<float> left( block ), right( block ), out;
        float*             outputs[] = { left.data(), right.data() };
        for( size_t pos = 0; pos < (size_t)rate; pos += block )
        {
            const auto t = transport.current();
            if( !g.process( outputs, block, &t ) )
                return {};
            out.insert( out.end(), left.begin(), left.end() );
            transport.advance( block );
        }
        return out;
    }
}

int main( int argc, char** argv )
{
    if( argc != 3 )
    {
        std::fprintf( stderr, "usage: param-target sequencer.clap fixture.clap\n" );
        return 2;
    }
    constexpr size_t step = 6000, gate = 3000; // a sixteenth at 120 BPM, and its gate
    for( uint32_t block : { 256u, 37u } )
    {
        const auto out = render( argv[1], argv[2], block, true );
        if( out.empty() )
            return std::fprintf( stderr, "FAIL: process\n" ), 1;
        for( size_t k = 0; ( k + 1 ) * step <= out.size(); ++k )
        {
            const float expected = (float)locks[k % locks.size()];
            const size_t start   = k * step;
            if( out[start] != expected || out[start + gate - 1] != expected || out[start + gate] != 0.0f )
            {
                std::fprintf( stderr, "FAIL: block %u, step %zu: first sample %f, last gated %f, after gate %f; expected %f\n", block, k,
                              out[start], out[start + gate - 1], out[start + gate], expected );
                return 1;
            }
            if( start > 0 && out[start - 1] != 0.0f )
                return std::fprintf( stderr, "FAIL: step %zu began early\n", k ), 1;
        }
    }

    const auto unmapped = render( argv[1], argv[2], 256, false );
    if( unmapped.empty() || unmapped[0] != 0.5f || unmapped[step] != 0.5f )
        return std::fprintf( stderr, "FAIL: without a mapping the gain should stay at its default 0.5\n" ), 1;

    std::printf( "ok: 8 steps x 2 block sizes, each lock applied from its step's first sample; unmapped lane ignored\n" );
    return 0;
}
