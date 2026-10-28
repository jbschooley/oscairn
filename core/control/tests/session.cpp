// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/session.h"
#include "oscairn/midi_input.h"
#include "AllocationGuard.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
    constexpr double rate = 48000;
    constexpr uint32_t block = 512;
    int failures = 0;

    void check( bool ok, const char* what )
    {
        if( !ok )
        {
            std::fprintf( stderr, "FAIL: %s\n", what );
            ++failures;
        }
    }

    template<class F> void rejects( F&& f, const char* what )
    {
        try { f(); check( false, what ); }
        catch( const std::invalid_argument& ) {}
    }

    struct Audio
    {
        std::array<float, block> left{}, right{};
        float* out[2]{ left.data(), right.data() };
    };

    bool render( oscairn::Session& session, Audio& audio, uint32_t frames = block,
                 std::span<const kick::clap::Event> events = {},
                 std::span<const oscairn::Session::MidiClockEvent> clocks = {} )
    {
        forbidAllocation = true;
        const bool ok = session.process( audio.out, frames, events, clocks );
        forbidAllocation = false;
        return ok;
    }

    oscairn::NodeId instrument( oscairn::Session& s, const char* path )
    {
        const auto id = s.add( "synth", path );
        s.connectNotes( oscairn::graphInput, id );
        s.connectAudio( id, oscairn::graphOutput );
        s.commit();
        return id;
    }

    void contract( const char* synth )
    {
        rejects( [] { oscairn::Session bad( 0, block ); }, "zero sample rate" );
        rejects( [] { oscairn::Session bad( rate, 0 ); }, "zero block size" );
        oscairn::Session session( rate, block );
        auto& control = session.control();
        const auto id = instrument( session, synth );
        check( control.nodeId( "synth" ) == id, "named and numeric node addressing agree" );
        check( control.parameters( id ).size() == 3, "all synth parameters, including Pressure, are exposed" );
        const auto pressure = control.parameterInfo( id, 3 );
        check( pressure.name == "Pressure" && pressure.defaultValue == 0.5 && pressure.maximum == 1,
               "parameter metadata crosses the interface" );
        check( control.parameterText( id, 3, 0.5 ) == "50%", "plugin parameter formatting" );
        rejects( [&] { session.add( "synth", synth ); }, "duplicate name rejected before loading" );
        rejects( [&] { control.nodeId( "missing" ); }, "unknown node" );
        rejects( [&] { control.setParameter( id, 12345, 1 ); }, "unknown parameter" );
        rejects( [&] { control.setParameter( id, 3, std::numeric_limits<double>::quiet_NaN() ); }, "NaN parameter" );
        rejects( [&] { control.setTempo( 0 ); }, "zero tempo" );
        rejects( [&] { control.setTempo( std::numeric_limits<double>::infinity() ); }, "infinite tempo" );
        rejects( [&] { control.note( true, 128, 1 ); }, "out-of-range key" );
        rejects( [&] { control.note( true, 60, 1, 16 ); }, "out-of-range channel" );
        check( !control.hasParameter( 99999, 1 ), "hasParameter on missing node" );

        Audio audio;
        check( render( session, audio ), "empty instrument renders" );
        check( std::all_of( audio.left.begin(), audio.left.end(), [](float v) { return v == 0; } ), "no unsolicited notes" );
        check( control.note( true, 57, 0.8, 2 ), "UI note queued" );
        check( render( session, audio ), "queued note renders while transport is stopped" );
        check( std::any_of( audio.left.begin(), audio.left.end(), [](float v) { return std::abs(v) > 0.01f; } ), "UI note is audible" );

        // A lost note-off must not leave a UI note sounding forever. Fill the queue
        // with unrelated note-offs, then force the held note's release to overflow.
        for( size_t i = 0; i < oscairn::Session::noteQueueCapacity; ++i )
            check( control.note( false, 40, 0, 2 ), "queue fills to its documented capacity" );
        check( !control.note( false, 57, 0, 2 ) && session.droppedNotes() == 1, "overflow is reported" );
        // The synth has an exponential release; allow two seconds for its tail.
        for( uint32_t samples = 0; samples < 2 * (uint32_t)rate; samples += block )
            check( render( session, audio ), "overflow recovery renders" );
        check( std::all_of( audio.left.begin(), audio.left.end(), [](float v) { return std::abs(v) < 1e-6f; } ),
               "overflow releases the held note" );
        check( control.note( true, 60, 1 ) && render( session, audio ), "queue recovers after overflow" );
        check( std::any_of( audio.left.begin(), audio.left.end(), [](float v) { return std::abs(v) > 0.01f; } ),
               "new notes play after overflow" );

        std::array<kick::clap::Event, oscairn::Session::maxInputEvents + 1> tooMany{};
        check( !render( session, audio, block, tooMany ), "over-capacity input rejected" );
        check( std::all_of( audio.left.begin(), audio.left.end(), [](float v) { return v == 0; } ), "rejected render clears audio" );
    }

    // Compare the new session against the existing Graph + Transport path using real
    // plugins, uneven blocks, parameter locks, tempo changes and sample-offset pressure.
    void referenceRender( const char* seq, const char* synth, const char* delay )
    {
        oscairn::Session session( rate, block );
        oscairn::Graph reference( rate, block );
        oscairn::Transport transport( rate );
        const auto a = session.add( "sequencer", seq );
        const auto b = session.add( "synth", synth );
        const auto c = session.add( "delay", delay );
        session.connectNotes( a, b ); session.connectNotes( oscairn::graphInput, b );
        session.connectAudio( b, c ); session.connectAudio( c, oscairn::graphOutput );
        session.mapParameter( a, 0, b, 1 ); session.commit();
        const auto ra = reference.add( seq ), rb = reference.add( synth ), rc = reference.add( delay );
        reference.connectNotes( ra, rb ); reference.connectNotes( oscairn::graphInput, rb );
        reference.connectAudio( rb, rc ); reference.connectAudio( rc, oscairn::graphOutput );
        reference.mapParamTarget( ra, 0, rb, 1 ); reference.commit();
        auto& control = session.control();
        control.setPlaying( true );
        Audio actual, expected;
        double energy = 0;
        bool identical = true;
        const uint32_t sizes[] = { 512, 37, 128, 251 };
        for( int n = 0; n < 600; ++n )
        {
            const auto frames = sizes[n % 4];
            std::vector<kick::clap::Event> events, referenceEvents;
            if( n == 4 )
            {
                control.note( true, 57, 0.7, 2 );
                referenceEvents.push_back( kick::clap::Event::note( true, 57, 0.7, 0, 2 ) );
            }
            if( n == 5 || n == 8 )
            {
                const uint8_t poly[] = { 0xA2, 57, 100 }, channel[] = { 0xD2, 30 };
                kick::clap::Event e;
                check( oscairn::midiToClap( n == 5 ? poly : channel, n == 5 ? 3 : 2, 17, e ), "pressure conversion" );
                events.push_back( e );
            }
            if( n == 30 )
            {
                control.note( false, 57, 0, 2 );
                referenceEvents.push_back( kick::clap::Event::note( false, 57, 0, 0, 2 ) );
            }
            if( n == 100 )
            {
                control.setTempo( 137 ); transport.setTempo( 137 );
                check( control.setParameter( b, 3, 0.8 ) && reference.host( rb ).setParameter( 3, 0.8 ), "pressure amount change" );
            }
            if( n == 400 ) { control.setPlaying( false ); transport.setPlaying( false ); }
            referenceEvents.insert( referenceEvents.end(), events.begin(), events.end() );
            const auto t = transport.current();
            session.service();
            check( render( session, actual, frames, events ), "session renders demo graph" );
            check( reference.process( expected.out, frames, &t, referenceEvents.data(), (uint32_t)referenceEvents.size() ), "reference renders" );
            identical &= std::equal( actual.left.begin(), actual.left.begin() + frames, expected.left.begin() )
                      && std::equal( actual.right.begin(), actual.right.begin() + frames, expected.right.begin() );
            for( uint32_t i = 0; i < frames; ++i ) energy += actual.left[i] * actual.left[i];
            const auto state = control.transportState();
            check( state.playing == transport.playing() && state.tempo == t.tempo
                && state.beat == (double)t.song_pos_beats / (double)CLAP_BEATTIME_FACTOR, "transport telemetry matches rendered block" );
            transport.advance( frames );
            if( n % 10 == 0 ) session.service();
        }
        check( identical && energy > 1, "session audio matches existing graph exactly, including control changes and pressure" );
    }

    void midiClock()
    {
        oscairn::Session session( rate, block );
        auto& control = session.control();
        Audio audio;
        control.setClockSource( oscairn::Transport::ClockSource::MidiClock );
        check( render( session, audio ), "MIDI sync starts without a pulse" );
        check( !control.transportState().playing, "MIDI sync waits for start and clock" );
        const std::array<oscairn::Session::MidiClockEvent, 2> start{{ {0xFA, 0}, {0xF8, 1} }};
        check( render( session, audio, block, {}, start ), "clock source applied before incoming start" );
        check( control.transportState().playing, "external clock starts transport" );
        // One pulse every 1000 samples is 120 BPM at 48 kHz; timestamps are already past.
        for( int i = 0; i < 3; ++i ) check( render( session, audio ), "advance to next clock" );
        const std::array<oscairn::Session::MidiClockEvent, 2> pulses{{ {0xF8, 1001}, {0xF8, 2001} }};
        check( render( session, audio, block, {}, pulses ) && control.transportState().tempo == 120, "timestamped MIDI tempo" );
        const std::array<oscairn::Session::MidiClockEvent, 1> stop{{ {0xFC, 2500} }};
        check( render( session, audio, block, {}, stop ) && !control.transportState().playing, "external stop" );
        control.setTempo( 95 ); control.setPlaying( true );
        control.setClockSource( oscairn::Transport::ClockSource::Internal );
        check( render( session, audio ) && control.transportState().playing && control.transportState().tempo == 95,
               "switching to internal restores requested transport" );
    }

    void concurrentControl( const char* synth )
    {
        oscairn::Session session( rate, block );
        const auto id = instrument( session, synth );
        auto& control = session.control();
        std::atomic<bool> run{ true }, ok{ true };
        std::atomic<uint32_t> blocks{ 0 };
        std::thread audioThread( [&] {
            Audio audio;
            while( run.load() )
            {
                if( !render( session, audio, 64 ) ) ok = false;
                for( auto v : audio.left ) if( !std::isfinite(v) ) ok = false;
                ++blocks;
            }
        } );
        while( blocks.load() == 0 ) std::this_thread::yield();
        for( int i = 0; i < 1000; ++i )
        {
            control.setPlaying( i % 2 ); control.setTempo( 80 + i % 80 );
            control.setParameter( id, 1, (i % 100) / 100.0 );
            control.note( i % 2 == 0, 60, 0.8 );
            control.transportState();
            session.service();
        }
        run = false;
        audioThread.join();
        check( ok && blocks > 0, "concurrent control and rendering remain valid" );
    }
}

int main( int argc, char** argv )
{
    if( argc != 4 ) return 2;
    contract( argv[2] );
    referenceRender( argv[1], argv[2], argv[3] );
    midiClock();
    concurrentControl( argv[2] );
    if( failures == 0 ) std::puts( "ok: control/session audio equivalence, pressure, clock, validation, overflow and concurrent rendering" );
    return failures ? 1 : 0;
}
