// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Pitch bend and the mod wheel reach the basic synth through the graph's note input, as a
// keyboard sends them. Key 57 (220 Hz) is held; at 0.25 s the MIDI message is sent, and the
// frequency is measured from the rendered output from 0.3 s, by its rising zero crossings.
// - full bend up: two semitones up (the default Pitch bend range); full down: two down;
// - with the range at 12: an octave up;
// - bend reaches a released voice too;
// - the mod wheel at full adds vibrato (the period varies); at 0, or with the Mod wheel
//   parameter Off, none.
// The two parameters are saved with the sound; a state saved before them still loads.

#include "oscairn/graph.h"
#include "oscairn/midi_input.h"
#include "oscairn/transport.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    constexpr double   rate  = 48000.0;
    constexpr uint32_t block = 256;
    constexpr clap_id  bendRangeParam = 9, wheelParam = 10;

    // Renders 0.5 s of key 57 held (or released at 0.25 s), with `midi` sent at 0.25 s.
    std::vector<float> render( const char* synth, std::vector<uint8_t> midi, clap_id param = 0, double value = 0, bool release = false )
    {
        oscairn::Graph g( rate, block );
        const auto     node = g.add( synth );
        g.connectNotes( oscairn::graphInput, node );
        g.connectAudio( node, oscairn::graphOutput );
        g.commit();
        if( param )
            g.host( node ).setParameter( param, value );

        oscairn::Transport transport( rate, 120.0 );
        transport.setPlaying( false );
        std::vector<float> left( block ), right( block ), out;
        float*             outputs[] = { left.data(), right.data() };
        const size_t       total = (size_t)rate / 2, at = (size_t)rate / 4;
        for( size_t pos = 0; pos < total; pos += block )
        {
            std::vector<kick::clap::Event> events;
            kick::clap::Event              e;
            const uint8_t                  on[] = { 0x90, 57, 100 }, off[] = { 0x80, 57, 0 };
            if( pos == 0 && oscairn::midiToClap( on, 3, 0, e ) )
                events.push_back( e );
            if( pos <= at && at < pos + block )
            {
                if( release && oscairn::midiToClap( off, 3, (uint32_t)( at - pos ), e ) )
                    events.push_back( e );
                if( !midi.empty() && oscairn::midiToClap( midi.data(), midi.size(), (uint32_t)( at - pos ), e ) )
                    events.push_back( e );
            }
            const auto t = transport.current();
            if( !g.process( outputs, block, &t, events.data(), (uint32_t)events.size() ) )
                return {};
            out.insert( out.end(), left.begin(), left.end() );
            transport.advance( block );
        }
        return out;
    }

    // The periods (in samples) between rising zero crossings from 0.3 s, interpolated.
    std::vector<double> periods( const std::vector<float>& x )
    {
        std::vector<double> crossings, result;
        for( size_t i = (size_t)( rate * .3 ); i < x.size(); ++i )
            if( x[i - 1] < 0 && x[i] >= 0 )
                crossings.push_back( (double)i - 1 + x[i - 1] / ( x[i - 1] - x[i] ) );
        for( size_t i = 1; i < crossings.size(); ++i )
            result.push_back( crossings[i] - crossings[i - 1] );
        return result;
    }

    double frequency( const std::vector<float>& x )
    {
        const auto p = periods( x );
        double     sum = 0;
        for( double d : p )
            sum += d;
        return p.empty() ? 0 : rate * (double)p.size() / sum;
    }

    // How far the period swings, as a ratio of the longest to the shortest.
    double swing( const std::vector<float>& x )
    {
        const auto p = periods( x );
        return p.empty() ? 0 : *std::max_element( p.begin(), p.end() ) / *std::min_element( p.begin(), p.end() );
    }
}

int main( int argc, char** argv )
{
    if( argc != 2 )
    {
        std::fprintf( stderr, "usage: wheels path/to/oscairn-basic-synth.clap\n" );
        return 2;
    }
    const char* synth  = argv[1];
    const auto  none   = render( synth, {} );
    const auto  up     = render( synth, { 0xE0, 0x7F, 0x7F } );
    const auto  down   = render( synth, { 0xE0, 0x00, 0x00 } );
    const auto  octave = render( synth, { 0xE0, 0x7F, 0x7F }, bendRangeParam, 12 );
    const auto  freed  = render( synth, { 0xE0, 0x7F, 0x7F }, 0, 0, true );
    const auto  wheel  = render( synth, { 0xB0, 1, 127 } );
    const auto  zero   = render( synth, { 0xB0, 1, 0 } );
    const auto  off    = render( synth, { 0xB0, 1, 127 }, wheelParam, 1 );
    if( none.empty() || up.empty() || down.empty() || octave.empty() || freed.empty() || wheel.empty() || zero.empty() || off.empty() )
        return std::fprintf( stderr, "FAIL: render\n" ), 1;

    const double f0 = frequency( none ), fu = frequency( up ), fd = frequency( down ), fo = frequency( octave ), fr = frequency( freed );
    const double s0 = swing( none ), sw = swing( wheel ), sz = swing( zero ), so = swing( off );
    std::printf( "frequency: none %.2f, bend up %.2f, down %.2f, up with range 12 %.2f, up after release %.2f Hz\n", f0, fu, fd, fo, fr );
    std::printf( "period swing: none %.4f, wheel full %.4f, wheel 0 %.4f, wheel full but Off %.4f\n", s0, sw, sz, so );
    int  failures = 0;
    auto check    = [&]( bool ok, const char* what ) {
        if( !ok )
        {
            std::fprintf( stderr, "FAIL: %s\n", what );
            ++failures;
        }
    };
    auto near = [&]( double f, double semitones, double within = .002 ) { return std::abs( f / ( 220.0 * std::pow( 2.0, semitones / 12 ) ) - 1 ) < within; };
    check( near( f0, 0 ), "the unbent note is 220 Hz" );
    check( near( fu, 2 ), "full bend up is two semitones up" );
    check( near( fd, -2 ), "full bend down is two semitones down" );
    check( near( fo, 12 ), "with the Pitch bend range at 12, an octave up" );
    // As a released voice fades its filter closes, which delays its crossings a little more each period.
    check( near( fr, 2, .01 ), "bend moves a released voice too" );
    // 50 cents either side swings the period by about 6 %.
    check( sw > 1.04, "the mod wheel at full adds vibrato" );
    check( s0 < 1.002 && sz < 1.002, "no vibrato without the wheel" );
    check( so < 1.002, "no vibrato with the Mod wheel parameter Off" );

    {
        oscairn::Graph g( rate, block );
        auto&          host = g.host( g.add( synth ) );
        const auto     dir  = std::filesystem::temp_directory_path() / ( "oscairn-wheels-" + std::to_string( std::chrono::steady_clock::now().time_since_epoch().count() ) );
        host.setParameter( bendRangeParam, 7 );
        host.setParameter( wheelParam, 1 );
        host.saveState( dir.string() );
        oscairn::Graph other( rate, block );
        auto&          restored = other.host( other.add( synth ) );
        restored.loadState( dir.string() );
        check( restored.parameterValue( bendRangeParam ) == 7 && restored.parameterValue( wheelParam ) == 1, "the bend range and wheel target are saved" );
        // The format is replaced, not read in its old form: a version 1 state, or a sound missing values, is refused.
        auto state = [&]( const char* version, const char* values ) {
            std::ofstream( dir / "state.clap", std::ios::binary ) << version << "\n1 \"Default\"" << values << "\n0\n0\n0\n0\n0\n0\n0\n0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n";
            try { restored.loadState( dir.string() ); return true; } catch( const std::runtime_error& ) { return false; }
        };
        check( state( "oscairn-synth-2", " 0.5 1 0.5 0 7 0 0 0 4 1" ) && restored.parameterValue( bendRangeParam ) == 4, "a version 2 state loads" );
        check( !state( "oscairn-synth-1", " 0.5 1 0.5 0 7 0 0 0" ), "a version 1 state is refused" );
        check( !state( "oscairn-synth-2", " 0.5 1 0.5 0 7 0 0 0" ), "a sound with eight values is refused" );
        std::filesystem::remove_all( dir );
    }
    if( failures == 0 )
        std::printf( "ok: pitch bend and the mod wheel reach the synth through the graph\n" );
    return failures == 0 ? 0 : 1;
}
