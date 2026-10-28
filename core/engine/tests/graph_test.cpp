// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Graph behaviour:
// - fan-out and summing: one sequencer playing two synths that both feed one delay
//   gives exactly twice the output of sequencer -> synth -> delay;
// - note merging: notes from graphInput and from a sequencer reach the same instrument;
// - cycles are rejected and the previous plan keeps playing;
// - invalid connections are rejected;
// - live editing: the audio thread keeps rendering while the main thread adds, connects,
//   removes and commits nodes, and the synth keeps its state.

#include "oscairn/chain.h"
#include "oscairn/graph.h"
#include "oscairn/transport.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
    constexpr double   rate  = 48000.0;
    constexpr uint32_t block = 256;

    struct Paths
    {
        const char *sequencer, *synth, *delay, *fixture;
    };

    int fail( const char* why )
    {
        std::fprintf( stderr, "FAIL: %s\n", why );
        return 1;
    }

    template<class Process>
    std::vector<float> render( Process&& process, double seconds )
    {
        oscairn::Transport transport( rate, 120.0 );
        const auto         total = (size_t)( seconds * rate );
        std::vector<float> left( block ), right( block ), out;
        float*             outputs[] = { left.data(), right.data() };
        for( size_t pos = 0; pos < total; pos += block )
        {
            const auto t = transport.current();
            if( !process( outputs, t, pos ) )
                throw std::runtime_error( "process failed" );
            out.insert( out.end(), left.begin(), left.end() );
            out.insert( out.end(), right.begin(), right.end() );
            transport.advance( block );
        }
        return out;
    }

    int fanOutAndSumming( const Paths& p )
    {
        oscairn::Chain chain( rate, block );
        chain.add( p.sequencer );
        chain.add( p.synth );
        chain.add( p.delay );
        const auto single = render( [&]( float* const* o, const clap_event_transport_t& t, size_t ) { return chain.process( o, block, &t ); }, 2.0 );

        oscairn::Graph g( rate, block );
        const auto     seq = g.add( p.sequencer ), a = g.add( p.synth ), b = g.add( p.synth ), fx = g.add( p.delay );
        g.connectNotes( seq, a );
        g.connectNotes( seq, b );
        g.connectAudio( a, fx );
        g.connectAudio( b, fx );
        g.connectAudio( fx, oscairn::graphOutput );
        g.commit();
        const auto layered = render( [&]( float* const* o, const clap_event_transport_t& t, size_t ) { return g.process( o, block, &t ); }, 2.0 );

        bool nonSilent = false;
        for( size_t i = 0; i < single.size(); ++i )
        {
            if( layered[i] != 2.0f * single[i] )
            {
                std::fprintf( stderr, "FAIL: sample %zu: %f, expected 2 x %f\n", i, layered[i], single[i] );
                return 1;
            }
            nonSilent |= single[i] != 0.0f;
        }
        if( !nonSilent )
            return fail( "fan-out test rendered silence" );
        std::printf( "ok: two synths into one delay = exactly 2 x the single chain\n" );
        return 0;
    }

    int noteMerging( const Paths& p )
    {
        // The fixture instrument outputs a constant while a note is held. The sequencer
        // holds a note from each step for half a step (3000 samples at 120 BPM), so
        // 4000..5999 is silent unless the external note-on at 4000 is merged in.
        oscairn::Graph g( rate, block );
        const auto     seq = g.add( p.sequencer ), inst = g.add( p.fixture );
        g.connectNotes( seq, inst );
        g.connectNotes( oscairn::graphInput, inst );
        g.connectAudio( inst, oscairn::graphOutput );
        g.commit();
        const auto external = kick::clap::Event::note( true, 64, 1.0, 4000 % block );
        const auto out      = render(
            [&]( float* const* o, const clap_event_transport_t& t, size_t pos ) {
                const bool now = pos == ( 4000 / block ) * block;
                return g.process( o, block, &t, now ? &external : nullptr, now ? 1 : 0 );
            },
            0.15 );
        auto leftAt = [&]( size_t frame ) { return out[( frame / block ) * 2 * block + frame % block]; };
        if( leftAt( 100 ) == 0.0f || leftAt( 3500 ) != 0.0f )
            return fail( "the sequencer's own notes did not play as expected" );
        if( leftAt( 3999 ) != 0.0f || leftAt( 4000 ) == 0.0f || leftAt( 5900 ) == 0.0f )
            return fail( "the external note was not merged in at its sample" );
        std::printf( "ok: external and sequencer notes merged at their samples\n" );
        return 0;
    }

    int cyclesAndInvalidConnections( const Paths& p )
    {
        oscairn::Graph g( rate, block );
        const auto     seq = g.add( p.sequencer ), syn = g.add( p.synth ), d1 = g.add( p.delay ), d2 = g.add( p.delay );
        g.connectNotes( seq, syn );
        g.connectAudio( syn, d1 );
        g.connectAudio( d1, oscairn::graphOutput );
        g.commit();

        g.connectAudio( d1, d2 );
        g.connectAudio( d2, d1 );
        bool threw = false;
        try
        {
            g.commit();
        }
        catch( const std::runtime_error& )
        {
            threw = true;
        }
        if( !threw )
            return fail( "a cycle was accepted" );
        const auto out = render( [&]( float* const* o, const clap_event_transport_t& t, size_t ) { return g.process( o, block, &t ); }, 0.2 );
        bool sound = false;
        for( float s : out )
            sound |= s != 0.0f;
        if( !sound )
            return fail( "the previous plan stopped playing after a rejected commit" );

        int rejected = 0;
        auto expectThrow = [&]( auto&& f ) {
            try
            {
                f();
            }
            catch( const std::invalid_argument& )
            {
                ++rejected;
            }
        };
        expectThrow( [&] { g.connectAudio( seq, oscairn::graphOutput ); } ); // a sequencer has no audio
        expectThrow( [&] { g.connectNotes( syn, d1 ); } );                     // a synth sends no notes
        expectThrow( [&] { g.connectNotes( oscairn::graphInput, seq ); } );    // the sequencer takes no notes
        expectThrow( [&] { g.connectAudio( syn, 9999 ); } );                   // no such node
        if( rejected != 4 )
            return fail( "an invalid connection was accepted" );
        std::printf( "ok: cycle rejected (previous plan kept playing), 4 invalid connections rejected\n" );
        return 0;
    }

    int liveEditing( const Paths& p )
    {
        oscairn::Graph g( rate, block );
        const auto     seq = g.add( p.sequencer ), syn = g.add( p.synth );
        g.connectNotes( seq, syn );
        g.connectAudio( syn, oscairn::graphOutput );
        g.commit();

        std::atomic<bool> running{ true }, failed{ false };
        std::atomic<int>  blocks{ 0 };
        std::atomic<float> lastPeak{ 0 };
        std::thread audio( [&] {
            oscairn::Transport transport( rate, 120.0 );
            std::vector<float> left( block ), right( block );
            float*             outputs[] = { left.data(), right.data() };
            while( running )
            {
                const auto t = transport.current();
                if( !g.process( outputs, block, &t ) )
                    failed = true;
                float peak = 0;
                for( float s : left )
                    peak = std::max( peak, std::abs( s ) );
                lastPeak = peak;
                transport.advance( block );
                ++blocks;
            }
        } );

        // Insert a delay after the synth, then remove it again, many times over.
        for( int i = 0; i < 40; ++i )
        {
            const auto fx = g.add( p.delay );
            g.disconnectAudio( syn, oscairn::graphOutput );
            g.connectAudio( syn, fx );
            g.connectAudio( fx, oscairn::graphOutput );
            g.commit();
            std::this_thread::sleep_for( std::chrono::milliseconds( 2 ) );
            g.remove( fx );
            g.connectAudio( syn, oscairn::graphOutput );
            g.commit();
            std::this_thread::sleep_for( std::chrono::milliseconds( 2 ) );
        }
        while( blocks < 400 ) // let the sequence play on for a while
            std::this_thread::yield();
        running = false;
        audio.join();
        g.collect();

        if( failed )
            return fail( "a block failed while the graph was being edited" );
        if( lastPeak.load() == 0.0f )
        {
            // The pattern plays a note every step, so a silent block at the end means the
            // synth lost its notes.
            return fail( "the synth stopped playing after the edits" );
        }
        std::printf( "ok: 80 commits while %d blocks rendered, no failures, still playing\n", blocks.load() );
        return 0;
    }
}

int main( int argc, char** argv )
{
    if( argc != 5 )
    {
        std::fprintf( stderr, "usage: graph-test sequencer.clap synth.clap delay.clap fixture.clap\n" );
        return 2;
    }
    const Paths p{ argv[1], argv[2], argv[3], argv[4] };
    try
    {
        if( fanOutAndSumming( p ) || noteMerging( p ) || cyclesAndInvalidConnections( p ) || liveEditing( p ) )
            return 1;
    }
    catch( const std::exception& e )
    {
        std::fprintf( stderr, "FAIL: %s\n", e.what() );
        return 1;
    }
    return 0;
}
