// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The transport follows an external MIDI clock, and the step sequencer follows the
// transport. A simulated clock runs at 140 BPM while the transport's own tempo is 120:
// - the tempo estimate settles on 140;
// - every step after the first lands within 2 samples of its clock pulse (a sixteenth
//   is 6 pulses); the first can only start at the block after the first pulse;
// - stop silences the sequencer.

#include "oscairn/transport.h"

#include <kick_clap/Discovery.h>
#include <kick_clap/Host.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace
{
    int fail( const char* why )
    {
        std::fprintf( stderr, "FAIL: %s\n", why );
        return 1;
    }
}

int main( int argc, char** argv )
{
    if( argc != 2 )
    {
        std::fprintf( stderr, "usage: midi-clock path/to/oscairn-step-sequencer.clap\n" );
        return 2;
    }

    constexpr double   rate = 48000.0, clockBpm = 140.0;
    constexpr uint32_t block      = 256;
    constexpr int64_t  startTime  = 4000, firstPulse = 4800;
    constexpr int64_t  lastPulse  = 24 * 8; // eight beats, then stop
    const double       interval   = rate * 60.0 / ( clockBpm * 24.0 );
    auto pulseTime = [&]( int64_t n ) { return firstPulse + (int64_t)std::llround( n * interval ); };
    const int64_t      stopTime   = pulseTime( lastPulse ) + 1;

    const auto scan = kick::clap::inspect( argv[1] );
    if( scan.plugins.empty() )
        return fail( "sequencer not found" );
    kick::clap::Host host;
    if( !host.load( scan.plugins.front(), rate, block ) )
        return fail( "load" );

    oscairn::Transport transport( rate, 120.0 );
    transport.setClockSource( oscairn::Transport::ClockSource::MidiClock );

    oscairn::Transport duplicateClock(rate,120);
    duplicateClock.setClockSource(oscairn::Transport::ClockSource::MidiClock);
    duplicateClock.midiClock(0xfa,0);duplicateClock.midiClock(0xf8,1000);
    for(int i=0;i<40;++i){duplicateClock.midiClock(0xf8,1000);duplicateClock.midiClock(0xf8,999);}
    if(duplicateClock.tempo()!=120)return fail("duplicate initial clocks preserve finite fallback tempo");
    duplicateClock.midiClock(0xf8,2000);
    const auto before=duplicateClock.current();
    duplicateClock.midiClock(0xf8,2000);duplicateClock.midiClock(0xf8,1500);
    if(duplicateClock.tempo()!=120 || duplicateClock.current().song_pos_beats!=before.song_pos_beats)
        return fail("duplicate and backwards clocks preserve tempo and position");
    duplicateClock.midiClock(0xf8,3000);
    if(duplicateClock.tempo()!=120)return fail("clock recovers after rejected timestamps");

    std::vector<int64_t> noteOns;
    double               settledTempo = 0;
    int64_t              nextPulse    = 0;
    bool                 started = false, stopped = false;
    bool                 playingAfterStop = false;
    for( int64_t now = 0; now < pulseTime( lastPulse ) + 4 * (int64_t)block; now += block )
    {
        const auto t = transport.current();
        if( stopped && ( t.flags & CLAP_TRANSPORT_IS_PLAYING ) )
            playingAfterStop = true;
        if( nextPulse > 48 ) // after two beats
            settledTempo = t.tempo;

        if( !host.process( nullptr, 0, nullptr, 0, block, nullptr, 0, &t ) )
            return fail( "process" );
        uint32_t    count  = 0;
        const auto* events = host.outputEvents( count );
        for( uint32_t i = 0; i < count; ++i )
            if( events[i].header()->space_id == CLAP_CORE_EVENT_SPACE_ID && events[i].header()->type == CLAP_EVENT_NOTE_ON )
                noteOns.push_back( now + events[i].header()->time );

        // The clock messages that fell in this block, in order.
        const int64_t end = now + block;
        for( ;; )
        {
            const int64_t pulse = nextPulse <= lastPulse ? pulseTime( nextPulse ) : INT64_MAX;
            const int64_t start = started ? INT64_MAX : startTime;
            const int64_t stop  = stopped ? INT64_MAX : stopTime;
            const int64_t at    = std::min( { pulse, start, stop } );
            if( at >= end )
                break;
            if( at == start )
            {
                transport.midiClock( 0xFA, at );
                started = true;
            }
            else if( at == pulse )
            {
                transport.midiClock( 0xF8, at );
                ++nextPulse;
            }
            else
            {
                transport.midiClock( 0xFC, at );
                stopped = true;
            }
        }
        transport.advance( block );
    }

    if( std::abs( settledTempo - clockBpm ) > 0.1 )
    {
        std::fprintf( stderr, "FAIL: tempo estimate %.3f, expected %.1f\n", settledTempo, clockBpm );
        return 1;
    }
    const auto steps = (size_t)( lastPulse / 6 ); // steps that start before the stop
    if( noteOns.size() < steps )
    {
        std::fprintf( stderr, "FAIL: %zu note-ons, expected at least %zu\n", noteOns.size(), steps );
        return 1;
    }
    if( noteOns[0] < pulseTime( 0 ) || noteOns[0] > pulseTime( 0 ) + (int64_t)block )
        return fail( "the first step should start in the block after the first pulse" );
    int64_t worst = 0;
    for( size_t k = 1; k < steps; ++k )
    {
        const int64_t error = noteOns[k] - pulseTime( (int64_t)k * 6 );
        worst               = std::max( worst, std::abs( error ) );
        if( std::abs( error ) > 2 )
        {
            std::fprintf( stderr, "FAIL: step %zu at %lld, its pulse is at %lld\n", k, (long long)noteOns[k], (long long)pulseTime( (int64_t)k * 6 ) );
            return 1;
        }
    }
    for( auto on : noteOns )
        if( on > stopTime + (int64_t)block )
            return fail( "a note started after stop" );
    if( playingAfterStop )
        return fail( "the transport kept playing after stop" );

    std::printf( "ok: tempo %.3f BPM, %zu steps within %lld samples of their pulses, stopped\n", settledTempo, steps, (long long)worst );
    return 0;
}
