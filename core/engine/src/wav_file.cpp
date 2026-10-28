// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/wav_file.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace oscairn
{
    namespace
    {
        void put16( std::vector<uint8_t>& out, uint16_t v )
        {
            out.push_back( (uint8_t)( v & 0xFF ) );
            out.push_back( (uint8_t)( v >> 8 ) );
        }

        void put32( std::vector<uint8_t>& out, uint32_t v )
        {
            for( int i = 0; i < 4; ++i )
                out.push_back( (uint8_t)( ( v >> ( 8 * i ) ) & 0xFF ) );
        }

        void putTag( std::vector<uint8_t>& out, const char* tag )
        {
            out.insert( out.end(), tag, tag + 4 );
        }
    }

    void writeWavFile( const std::filesystem::path& path, const std::vector<float>& interleaved, int channels, int sampleRate )
    {
        if( channels <= 0 || sampleRate <= 0 || interleaved.size() % (size_t)channels != 0 )
            throw std::runtime_error( "invalid WAV parameters" );

        const auto frames    = (uint32_t)( interleaved.size() / (size_t)channels );
        const auto dataBytes = (uint32_t)( interleaved.size() * sizeof( float ) );

        std::vector<uint8_t> header;
        putTag( header, "RIFF" );
        put32( header, 4 + ( 8 + 18 ) + ( 8 + 4 ) + ( 8 + dataBytes ) );
        putTag( header, "WAVE" );

        putTag( header, "fmt " ); // WAVE_FORMAT_IEEE_FLOAT, with cbSize as the format requires
        put32( header, 18 );
        put16( header, 3 );
        put16( header, (uint16_t)channels );
        put32( header, (uint32_t)sampleRate );
        put32( header, (uint32_t)sampleRate * (uint32_t)channels * 4 );
        put16( header, (uint16_t)( channels * 4 ) );
        put16( header, 32 );
        put16( header, 0 );

        putTag( header, "fact" ); // required for non-PCM formats
        put32( header, 4 );
        put32( header, frames );

        putTag( header, "data" );
        put32( header, dataBytes );

        std::vector<uint8_t> samples( dataBytes );
        for( size_t i = 0; i < interleaved.size(); ++i )
        {
            uint32_t bits;
            std::memcpy( &bits, &interleaved[i], 4 );
            for( int b = 0; b < 4; ++b )
                samples[i * 4 + b] = (uint8_t)( ( bits >> ( 8 * b ) ) & 0xFF );
        }

        std::ofstream file( path, std::ios::binary );
        if( !file )
            throw std::runtime_error( "cannot write " + path.string() );
        file.write( reinterpret_cast<const char*>( header.data() ), (std::streamsize)header.size() );
        file.write( reinterpret_cast<const char*>( samples.data() ), (std::streamsize)samples.size() );
        if( !file )
            throw std::runtime_error( "error writing " + path.string() );
    }
}
