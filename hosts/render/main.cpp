// SPDX-License-Identifier: AGPL-3.0-or-later
//
// oscairn-render: plays a chain of CLAP and LV2 plugins against the Oscairn transport
// faster than real time and writes the result to a WAV file.
//
//   oscairn-render out.wav [--seconds 8] [--tempo 120] [--block 512] [--rate 48000]
//                  [--midi-clock BPM] [--map SRC.LANE=DST.PARAM ...] plugin.clap[#plugin-id] | bundle.lv2[#URI] ...
//
// Plugins are chained in order: each one plays the notes of the one before it,
// so "sequencer.clap synth.clap" has the sequencer playing the synth.
//
// --midi-clock BPM simulates an external MIDI clock source at that tempo (start, then
// 24 pulses per beat) and makes the transport follow it instead of --tempo.
//
// --map SRC.LANE=DST.PARAM routes oscairn.param-target lane LANE of the plugin at chain
// position SRC (0-based) to parameter PARAM (its name or ID) of the plugin at position DST,
// e.g. --map 0.0=1.Brightness for the sequencer's Lock A on the synth's brightness.

#include "oscairn/chain.h"
#include "oscairn/transport.h"
#include "oscairn/wav_file.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    int usage()
    {
        std::fprintf( stderr,
                      "usage: oscairn-render out.wav [--seconds S] [--tempo BPM] [--block N] [--rate HZ] [--midi-clock BPM] [--map SRC.LANE=DST.PARAM] plugin.clap[#id] or bundle.lv2[#URI] ...\n" );
        return 2;
    }
}

int main( int argc, char** argv )
{
    if( argc < 3 )
        return usage();

    std::string              output = argv[1];
    double                   seconds = 8.0, tempo = 120.0, rate = 48000.0, clockBpm = 0.0;
    uint32_t                 block   = 512;
    std::vector<std::string> plugins, maps;
    for( int i = 2; i < argc; ++i )
    {
        const std::string arg = argv[i];
        auto              value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        const char*       v     = nullptr;
        if( arg == "--seconds" && ( v = value() ) )
            seconds = std::atof( v );
        else if( arg == "--tempo" && ( v = value() ) )
            tempo = std::atof( v );
        else if( arg == "--block" && ( v = value() ) )
            block = (uint32_t)std::atoi( v );
        else if( arg == "--rate" && ( v = value() ) )
            rate = std::atof( v );
        else if( arg == "--midi-clock" && ( v = value() ) )
            clockBpm = std::atof( v );
        else if( arg == "--map" && ( v = value() ) )
            maps.push_back( v );
        else if( arg.rfind( "--", 0 ) == 0 )
            return usage();
        else
            plugins.push_back( arg );
    }
    if( plugins.empty() || seconds <= 0 || tempo <= 0 || rate <= 0 || block == 0 || clockBpm < 0 )
        return usage();

    try
    {
        oscairn::Chain chain( rate, block, oscairn::ProcessingMode::Offline );
        for( const auto& spec : plugins )
        {
            const auto hash = spec.find( '#' );
            chain.add( spec.substr( 0, hash ), hash == std::string::npos ? std::string{} : spec.substr( hash + 1 ) );
            std::printf( "loaded %s (%s)\n", chain.descriptor( chain.size() - 1 ).name.c_str(), chain.descriptor( chain.size() - 1 ).id.c_str() );
        }

        for( const auto& map : maps )
        {
            unsigned src = 0, lane = 0, dst = 0;
            char     param[256] = {};
            if( std::sscanf( map.c_str(), "%u.%u=%u.%255s", &src, &lane, &dst, param ) != 4 || src >= chain.size() || dst >= chain.size() )
                throw std::runtime_error( "bad --map " + map );
            const auto& params = chain.graph().host( chain.node( dst ) ).parameters();
            const auto  found  = std::find_if( params.begin(), params.end(), [&]( const kick::clap::Parameter& q ) {
                return q.name == param || std::to_string( q.id ) == param;
            } );
            if( found == params.end() )
                throw std::runtime_error( "no parameter " + std::string( param ) + " on " + chain.descriptor( dst ).name );
            chain.graph().mapParamTarget( chain.node( src ), lane, chain.node( dst ), found->id );
            std::printf( "mapped %s lane %u -> %s %s\n", chain.descriptor( src ).name.c_str(), lane, chain.descriptor( dst ).name.c_str(), found->name.c_str() );
        }
        if( !maps.empty() )
            chain.graph().commit();

        oscairn::Transport transport( rate, tempo );
        if( clockBpm > 0 )
            transport.setClockSource( oscairn::Transport::ClockSource::MidiClock );
        const double clockInterval = clockBpm > 0 ? rate * 60.0 / ( clockBpm * 24.0 ) : 0.0;
        bool         clockStarted  = false;
        int64_t      nextPulse     = 0;
        const auto         total = (int64_t)std::llround( seconds * rate );
        std::vector<float> interleaved( (size_t)total * oscairn::Chain::channels );
        std::vector<float> left( block ), right( block );
        float*             outputs[] = { left.data(), right.data() };

        for( int64_t position = 0; position < total; )
        {
            const auto frames = (uint32_t)std::min<int64_t>( block, total - position );
            const auto t      = transport.current();
            if( !chain.process( outputs, frames, &t ) )
                std::fprintf( stderr, "warning: a plugin reported an error at sample %lld\n", (long long)position );
            for( uint32_t i = 0; i < frames; ++i )
            {
                interleaved[(size_t)( position + i ) * 2]     = left[i];
                interleaved[(size_t)( position + i ) * 2 + 1] = right[i];
            }
            if( clockBpm > 0 )
            {
                // The simulated clock's messages in this block: start at sample 0, then pulses.
                if( !clockStarted )
                {
                    transport.midiClock( 0xFA, 0 );
                    clockStarted = true;
                }
                for( int64_t at; ( at = (int64_t)std::llround( nextPulse * clockInterval ) ) < position + frames; ++nextPulse )
                    transport.midiClock( 0xF8, at );
            }
            transport.advance( frames );
            position += frames;
        }

        float peak = 0.0f;
        for( float s : interleaved )
            peak = std::max( peak, std::abs( s ) );
        oscairn::writeWavFile( output, interleaved, oscairn::Chain::channels, (int)rate );
        std::printf( "wrote %s: %.2f s at %.0f Hz, %.0f BPM%s, block %u, peak %.3f\n", output.c_str(), seconds, rate,
                     clockBpm > 0 ? clockBpm : tempo, clockBpm > 0 ? " (MIDI clock)" : "", block, peak );
        return peak > 0.0f ? 0 : 1; // silence means something went wrong
    }
    catch( const std::exception& e )
    {
        std::fprintf( stderr, "error: %s\n", e.what() );
        return 1;
    }
}
