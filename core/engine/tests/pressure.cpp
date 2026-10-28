// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Aftertouch reaches the basic synth through the graph's note input and opens the filter
// of the voices it addresses. Two notes are held; after 0.25 s pressure is applied, then
// the second 0.25 s is measured. Brightness is measured as the energy of the signal's
// sample-to-sample differences relative to its energy (higher means brighter).
// - polyphonic aftertouch on the held key: brighter than with no pressure;
// - polyphonic aftertouch on a key that is not held: no change;
// - channel aftertouch: brighter;
// - with the synth's Pressure amount at 0: no change;
// - the same input renders the same output twice.

#include "oscairn/graph.h"
#include "oscairn/midi_input.h"
#include "oscairn/transport.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace
{
    constexpr double   rate  = 48000.0;
    constexpr uint32_t block = 256;
    constexpr clap_id  pressureParam = 3;

    // Renders 0.5 s of keys 45 and 57 held, with `midi` sent at 0.25 s; returns the left channel.
    std::vector<float> render( const char* synth, std::vector<uint8_t> midi, double pressureAmount = -1.0 )
    {
        oscairn::Graph g( rate, block );
        const auto     node = g.add( synth );
        g.connectNotes( oscairn::graphInput, node );
        g.connectAudio( node, oscairn::graphOutput );
        g.commit();
        if( pressureAmount >= 0.0 )
            g.host( node ).setParameter( pressureParam, pressureAmount );

        oscairn::Transport transport( rate, 120.0 );
        transport.setPlaying( false );
        std::vector<float> left( block ), right( block ), out;
        float*             outputs[] = { left.data(), right.data() };
        const size_t       total = (size_t)rate / 2, at = (size_t)rate / 4;
        for( size_t pos = 0; pos < total; pos += block )
        {
            std::vector<kick::clap::Event> events;
            kick::clap::Event              e;
            if( pos == 0 )
                for( uint8_t key : { 45, 57 } )
                {
                    const uint8_t on[] = { 0x90, key, 100 };
                    oscairn::midiToClap( on, 3, 0, e );
                    events.push_back( e );
                }
            if( !midi.empty() && pos <= at && at < pos + block && oscairn::midiToClap( midi.data(), midi.size(), (uint32_t)( at - pos ), e ) )
                events.push_back( e );
            const auto t = transport.current();
            if( !g.process( outputs, block, &t, events.data(), (uint32_t)events.size() ) )
                return {};
            out.insert( out.end(), left.begin(), left.end() );
            transport.advance( block );
        }
        return out;
    }

    // Brightness of the second half, where pressure (if any) applies.
    double brightness( const std::vector<float>& x )
    {
        double diff = 0.0, energy = 0.0;
        for( size_t i = x.size() / 2 + 1; i < x.size(); ++i )
        {
            diff += ( x[i] - x[i - 1] ) * ( x[i] - x[i - 1] );
            energy += x[i] * x[i];
        }
        return energy > 0.0 ? diff / energy : 0.0;
    }
}

int main( int argc, char** argv )
{
    if( argc != 2 )
    {
        std::fprintf( stderr, "usage: pressure path/to/oscairn-basic-synth.clap\n" );
        return 2;
    }
    const auto none    = render( argv[1], {} );
    const auto poly    = render( argv[1], { 0xA0, 57, 127 } );
    const auto other   = render( argv[1], { 0xA0, 60, 127 } );
    const auto channel = render( argv[1], { 0xD0, 127 } );
    const auto off     = render( argv[1], { 0xD0, 127 }, 0.0 );
    const auto again   = render( argv[1], { 0xA0, 57, 127 } );
    if( none.empty() || poly.empty() || other.empty() || channel.empty() || off.empty() || again.empty() )
        return std::fprintf( stderr, "FAIL: render\n" ), 1;

    const double b0 = brightness( none ), bp = brightness( poly ), bo = brightness( other ), bc = brightness( channel ),
                 bz = brightness( off );
    std::printf( "brightness: none %.5f, poly on a held key %.5f, poly on another key %.5f, channel %.5f, amount 0 %.5f\n", b0, bp, bo,
                 bc, bz );
    int failures = 0;
    auto check   = [&]( bool ok, const char* what ) {
        if( !ok )
        {
            std::fprintf( stderr, "FAIL: %s\n", what );
            ++failures;
        }
    };
    check( bp > b0 * 1.5, "polyphonic aftertouch on a held key opens the filter" );
    check( other == none, "polyphonic aftertouch on a key that is not held changes nothing" );
    check( bc > bp, "channel aftertouch opens both voices, more than one" );
    check( off == none, "with the Pressure amount at 0, pressure changes nothing" );
    check( again == poly, "the same input renders the same output" );
    const auto at = (long)rate / 4; // where render() sends the pressure
    check( std::equal( poly.begin(), poly.begin() + at, none.begin() ), "nothing changes before the pressure arrives" );
    check( poly[(size_t)at + 64] != none[(size_t)at + 64], "the change starts at the pressure's sample" );
    if( failures == 0 )
        std::printf( "ok: aftertouch reaches the synth through the graph and opens the voices it addresses\n" );
    return failures == 0 ? 0 : 1;
}
