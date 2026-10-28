// SPDX-License-Identifier: AGPL-3.0-or-later
//
// oscairn-render: plays a chain of CLAP plugins against the Oscairn transport
// faster than real time and writes the result to a WAV file.
//
//   oscairn-render out.wav [--seconds 8] [--tempo 120] [--block 512] [--rate 48000]
//                  plugin.clap[#plugin-id] ...
//
// Plugins are chained in order: each one plays the notes of the one before it,
// so "sequencer.clap synth.clap" has the sequencer playing the synth.

#include "oscairn/chain.h"
#include "oscairn/transport.h"
#include "oscairn/wav_file.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

namespace
{
    int usage()
    {
        std::fprintf( stderr,
                      "usage: oscairn-render out.wav [--seconds S] [--tempo BPM] [--block N] [--rate HZ] plugin.clap[#id] ...\n" );
        return 2;
    }
}

int main( int argc, char** argv )
{
    if( argc < 3 )
        return usage();

    std::string              output = argv[1];
    double                   seconds = 8.0, tempo = 120.0, rate = 48000.0;
    uint32_t                 block   = 512;
    std::vector<std::string> plugins;
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
        else if( arg.rfind( "--", 0 ) == 0 )
            return usage();
        else
            plugins.push_back( arg );
    }
    if( plugins.empty() || seconds <= 0 || tempo <= 0 || rate <= 0 || block == 0 )
        return usage();

    try
    {
        oscairn::Chain chain( rate, block );
        for( const auto& spec : plugins )
        {
            const auto hash = spec.find( '#' );
            chain.add( spec.substr( 0, hash ), hash == std::string::npos ? std::string{} : spec.substr( hash + 1 ) );
            std::printf( "loaded %s (%s)\n", chain.descriptor( chain.size() - 1 ).name.c_str(), chain.descriptor( chain.size() - 1 ).id.c_str() );
        }

        oscairn::Transport transport( rate, tempo );
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
            transport.advance( frames );
            position += frames;
        }

        float peak = 0.0f;
        for( float s : interleaved )
            peak = std::max( peak, std::abs( s ) );
        oscairn::writeWavFile( output, interleaved, oscairn::Chain::channels, (int)rate );
        std::printf( "wrote %s: %.2f s at %.0f Hz, %.0f BPM, block %u, peak %.3f\n", output.c_str(), seconds, rate, tempo, block, peak );
        return peak > 0.0f ? 0 : 1; // silence means something went wrong
    }
    catch( const std::exception& e )
    {
        std::fprintf( stderr, "error: %s\n", e.what() );
        return 1;
    }
}
