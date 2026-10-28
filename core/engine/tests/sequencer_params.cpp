// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The step sequencer's steps are CLAP parameters (on 100-115, note 200-215, lock
// 300-315). At 120 BPM a step is 6000 samples. Checked:
// - the host lists 48 parameters, with the default pattern as default values;
// - setParameter edits a step: step 2 off plays nothing there, step 3's note changes;
// - a parameter event inside a block applies from its sample: a note change sent one
//   sample after a step starts misses that step and takes effect on the next pass,
//   and one sent at the step's sample applies to it.

#include "oscairn/transport.h"

#include <kick_clap/Discovery.h>
#include <kick_clap/Host.h>

#include <cstdio>
#include <map>
#include <vector>

namespace
{
    constexpr double   rate  = 48000.0;
    constexpr uint32_t block = 256;
    constexpr int64_t  step  = 6000;

    int fail( const char* why )
    {
        std::fprintf( stderr, "FAIL: %s\n", why );
        return 1;
    }

    kick::clap::Event paramEvent( clap_id id, double value, uint32_t time )
    {
        clap_event_param_value_t e{};
        e.header.size     = sizeof( e );
        e.header.time     = time;
        e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        e.header.type     = CLAP_EVENT_PARAM_VALUE;
        e.param_id        = id;
        e.note_id         = -1;
        e.port_index = e.channel = e.key = -1;
        e.value           = value;
        return kick::clap::Event::from( e );
    }

    // Note-on keys by sample, for `samples` samples. `events` are sent at absolute times.
    std::map<int64_t, int> play( kick::clap::Host& host, int64_t samples, const std::multimap<int64_t, kick::clap::Event>& events = {} )
    {
        oscairn::Transport     transport( rate, 120.0 );
        std::map<int64_t, int> ons;
        for( int64_t now = 0; now < samples; now += block )
        {
            std::vector<kick::clap::Event> in;
            for( auto it = events.lower_bound( now ); it != events.end() && it->first < now + (int64_t)block; ++it )
            {
                in.push_back( it->second );
                auto* header = reinterpret_cast<clap_event_header_t*>( in.back().bytes.data() );
                header->time = (uint32_t)( it->first - now );
            }
            const auto t = transport.current();
            if( !host.process( nullptr, 0, nullptr, 0, block, in.data(), (uint32_t)in.size(), &t ) )
                return {};
            uint32_t    count = 0;
            const auto* out   = host.outputEvents( count );
            for( uint32_t i = 0; i < count; ++i )
            {
                const auto* h = out[i].header();
                if( h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_NOTE_ON )
                    ons[now + h->time] = reinterpret_cast<const clap_event_note_t*>( h )->key;
            }
            transport.advance( block );
        }
        return ons;
    }
}

int main( int argc, char** argv )
{
    if( argc != 2 )
    {
        std::fprintf( stderr, "usage: sequencer-params path/to/oscairn-step-sequencer.clap\n" );
        return 2;
    }
    const auto scan = kick::clap::inspect( argv[1] );
    if( scan.plugins.empty() )
        return fail( "sequencer not found" );

    {
        kick::clap::Host host;
        if( !host.load( scan.plugins.front(), rate, block ) )
            return fail( "load" );
        if( host.parameters().size() != 48 )
            return fail( "expected 48 parameters" );
        if( host.parameterValue( 100 ) != 1.0 || host.parameterValue( 200 ) != 45.0 || host.parameterValue( 300 ) != 0.8 )
            return fail( "default values" );

        host.setParameter( 101, 0.0 );  // step 2 off
        host.setParameter( 202, 70.0 ); // step 3 note
        const auto ons = play( host, 4 * step );
        if( ons.count( 0 ) != 1 || ons.at( 0 ) != 45 )
            return fail( "step 1 should play its default note" );
        if( ons.count( step ) != 0 )
            return fail( "step 2 is off but played" );
        if( ons.count( 2 * step ) != 1 || ons.at( 2 * step ) != 70 )
            return fail( "step 3 should play the edited note 70" );
        if( host.parameterValue( 101 ) != 0.0 || host.parameterValue( 202 ) != 70.0 )
            return fail( "edited values not reported" );
    }
    {
        kick::clap::Host host;
        if( !host.load( scan.plugins.front(), rate, block ) )
            return fail( "load" );
        // Step 2 (at 6000): note 72 one sample late, so the first pass plays the default;
        // step 3 (at 12000): note 74 exactly at its sample, so it applies at once.
        const std::multimap<int64_t, kick::clap::Event> events{ { step + 1, paramEvent( 201, 72.0, 0 ) },
                                                                { 2 * step, paramEvent( 202, 74.0, 0 ) } };
        const auto ons = play( host, 17 * step + 1, events );
        if( ons.count( step ) != 1 || ons.at( step ) != 57 )
            return fail( "a change one sample after step 2 started should not affect it" );
        if( ons.count( 2 * step ) != 1 || ons.at( 2 * step ) != 74 )
            return fail( "a change at step 3's sample should apply to it" );
        if( ons.count( 17 * step ) != 1 || ons.at( 17 * step ) != 72 )
            return fail( "step 2's change should apply on the next pass" );
    }

    std::printf( "ok: 48 step parameters; on/off and note edits apply, from their exact sample\n" );
    return 0;
}
