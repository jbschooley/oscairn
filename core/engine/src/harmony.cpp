// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/harmony.h"

#include <array>
#include <initializer_list>

namespace oscairn
{
    namespace
    {
        constexpr uint16_t classes( std::initializer_list<int> intervals )
        {
            uint16_t mask = 0;
            for( int i : intervals )
                mask |= uint16_t( 1u << i );
            return mask;
        }
        struct Shape
        {
            uint16_t intervals;
            uint8_t  type;
        };
        // In the order of enum oscairn_chord_type, which breaks ties between readings.
        constexpr std::array<Shape, 22> shapes{ {
            { classes( { 0, 4, 7 } ), OSCAIRN_CHORD_MAJOR },
            { classes( { 0, 3, 7 } ), OSCAIRN_CHORD_MINOR },
            { classes( { 0, 3, 6 } ), OSCAIRN_CHORD_DIMINISHED },
            { classes( { 0, 4, 8 } ), OSCAIRN_CHORD_AUGMENTED },
            { classes( { 0, 2, 7 } ), OSCAIRN_CHORD_SUS2 },
            { classes( { 0, 5, 7 } ), OSCAIRN_CHORD_SUS4 },
            { classes( { 0, 7 } ), OSCAIRN_CHORD_POWER },
            { classes( { 0, 4, 7, 9 } ), OSCAIRN_CHORD_MAJOR6 },
            { classes( { 0, 3, 7, 9 } ), OSCAIRN_CHORD_MINOR6 },
            { classes( { 0, 4, 7, 10 } ), OSCAIRN_CHORD_DOMINANT7 },
            { classes( { 0, 4, 7, 11 } ), OSCAIRN_CHORD_MAJOR7 },
            { classes( { 0, 3, 7, 10 } ), OSCAIRN_CHORD_MINOR7 },
            { classes( { 0, 3, 7, 11 } ), OSCAIRN_CHORD_MINOR_MAJOR7 },
            { classes( { 0, 3, 6, 10 } ), OSCAIRN_CHORD_HALF_DIMINISHED7 },
            { classes( { 0, 3, 6, 9 } ), OSCAIRN_CHORD_DIMINISHED7 },
            { classes( { 0, 4, 8, 10 } ), OSCAIRN_CHORD_AUGMENTED7 },
            { classes( { 0, 5, 7, 10 } ), OSCAIRN_CHORD_DOMINANT7_SUS4 },
            { classes( { 0, 2, 4, 7 } ), OSCAIRN_CHORD_ADD9 },
            { classes( { 0, 2, 3, 7 } ), OSCAIRN_CHORD_MINOR_ADD9 },
            { classes( { 0, 2, 4, 7, 10 } ), OSCAIRN_CHORD_DOMINANT9 },
            { classes( { 0, 2, 4, 7, 11 } ), OSCAIRN_CHORD_MAJOR9 },
            { classes( { 0, 2, 3, 7, 10 } ), OSCAIRN_CHORD_MINOR9 },
        } };
        // The pitch classes as intervals above `root`.
        uint16_t above( uint16_t pitches, int root ) { return uint16_t( ( ( pitches >> root ) | ( pitches << ( 12 - root ) ) ) & 0xFFF ); }
    }

    void recognizeChord( oscairn_harmony_t& h ) noexcept
    {
        h.chord_root = h.chord_type = h.chord_bass = 0;
        uint16_t pitches = 0;
        int      lowest  = -1;
        for( int k = 0; k < 128; ++k )
            if( h.held[k / 64] >> ( k % 64 ) & 1 )
            {
                pitches |= uint16_t( 1u << ( k % 12 ) );
                if( lowest < 0 )
                    lowest = k;
            }
        if( lowest < 0 )
            return;
        h.chord_bass = uint8_t( lowest % 12 );
        auto reading = [&]( int root ) -> uint8_t {
            const auto intervals = above( pitches, root );
            for( const auto& s : shapes )
                if( s.intervals == intervals )
                    return s.type;
            return OSCAIRN_CHORD_NONE;
        };
        if( const auto type = reading( h.chord_bass ) )
        {
            h.chord_root = h.chord_bass;
            h.chord_type = type;
            return;
        }
        for( const auto& s : shapes )
            for( int root = 0; root < 12; ++root )
                if( ( pitches >> root & 1 ) && above( pitches, root ) == s.intervals )
                {
                    h.chord_root = uint8_t( root );
                    h.chord_type = s.type;
                    return;
                }
    }
}
