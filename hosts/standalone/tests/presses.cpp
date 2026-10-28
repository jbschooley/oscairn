// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Presses: what each finger holds, so pads (and other controls) can be held by several
// fingers at once. A note starts when the first finger presses a pad and ends when the
// last finger leaves it.

#include "../Presses.h"

#include <cstdio>

namespace
{
    int failures = 0;

    void check( bool ok, const char* what )
    {
        if( !ok )
        {
            std::fprintf( stderr, "FAIL: %s\n", what );
            ++failures;
        }
    }
}

int main()
{
    using oscairn::standalone::Presses;
    {
        Presses<int> p;
        check( p.press( 0, 3 ), "the first finger on pad 3 starts its note" );
        check( p.press( 1, 7 ), "a second finger on pad 7 starts its note too" );
        check( p.held( 3 ) && p.held( 7 ), "both pads are held" );
        const auto off = p.release( 0 );
        check( off && *off == 3, "lifting the first finger ends pad 3 only" );
        check( !p.held( 3 ) && p.held( 7 ), "pad 7 is still held" );
        const auto off7 = p.release( 1 );
        check( off7 && *off7 == 7 && p.empty(), "lifting the second finger ends pad 7" );
        check( !p.release( 1 ), "lifting a finger that holds nothing does nothing" );
    }
    {
        Presses<int> p;
        check( p.press( 0, 5 ), "the first finger on pad 5 starts its note" );
        check( !p.press( 1, 5 ), "a second finger on the same pad does not start it again" );
        check( !p.release( 0 ), "lifting one of two fingers leaves the note sounding" );
        const auto off = p.release( 1 );
        check( off && *off == 5, "lifting the last finger ends it" );
    }
    {
        Presses<int> p;
        p.press( 0, 2 );
        check( p.press( 0, 4 ), "the same finger pressing another pad starts that one" );
        check( !p.held( 2 ) && p.held( 4 ), "and lets go of the first" );
    }
    if( failures == 0 )
        std::printf( "ok: presses held and released per finger\n" );
    return failures == 0 ? 0 : 1;
}
