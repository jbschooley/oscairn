// SPDX-License-Identifier: AGPL-3.0-or-later
//
// midiToClap: MIDI 1.0 channel messages from keyboards and pads into CLAP events.

#include "oscairn/midi_input.h"

#include <cmath>
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

    const clap_event_note_t& note( const kick::clap::Event& e ) { return *reinterpret_cast<const clap_event_note_t*>( e.header() ); }
    const clap_event_note_expression_t& expression( const kick::clap::Event& e )
    {
        return *reinterpret_cast<const clap_event_note_expression_t*>( e.header() );
    }

    bool convert( std::initializer_list<uint8_t> bytes, kick::clap::Event& out, uint32_t time = 0 )
    {
        return oscairn::midiToClap( bytes.begin(), bytes.size(), time, out );
    }
}

int main()
{
    kick::clap::Event e;

    check( convert( { 0x93, 60, 127 }, e, 17 ) && e.header()->type == CLAP_EVENT_NOTE_ON && note( e ).key == 60 && note( e ).channel == 3
               && note( e ).velocity == 1.0 && e.header()->time == 17,
           "note on: key, channel, velocity and time" );
    check( convert( { 0x90, 64, 0 }, e ) && e.header()->type == CLAP_EVENT_NOTE_OFF && note( e ).key == 64,
           "note on with velocity 0 is a note off" );
    check( convert( { 0x81, 64, 40 }, e ) && e.header()->type == CLAP_EVENT_NOTE_OFF && note( e ).channel == 1, "note off" );

    check( convert( { 0xA2, 61, 127 }, e, 5 ) && e.header()->type == CLAP_EVENT_NOTE_EXPRESSION
               && expression( e ).expression_id == CLAP_NOTE_EXPRESSION_PRESSURE && expression( e ).key == 61
               && expression( e ).channel == 2 && expression( e ).value == 1.0 && e.header()->time == 5,
           "polyphonic aftertouch is pressure on that key" );
    check( convert( { 0xD4, 64 }, e ) && e.header()->type == CLAP_EVENT_NOTE_EXPRESSION && expression( e ).key == -1
               && expression( e ).channel == 4 && std::abs( expression( e ).value - 64.0 / 127.0 ) < 1e-12,
           "channel aftertouch is pressure on every note of the channel" );

    kick::clap::Event untouched = kick::clap::Event::note( true, 1, 0.5 );
    e                           = untouched;
    check( !convert( { 0xB0, 1, 64 }, e ) && e.bytes == untouched.bytes, "controllers are not converted, and leave the event alone" );
    check( !convert( { 0xE0, 0, 64 }, e ), "pitch bend is not converted" );
    check( !convert( { 0xF8 }, e ), "system messages are not converted" );
    check( !convert( { 0x90, 200, 64 }, e ) && !convert( { 0x90, 60 }, e ) && !convert( { 0x40, 1, 2 }, e ), "malformed messages are rejected" );

    if( failures == 0 )
        std::printf( "ok: notes, polyphonic and channel aftertouch converted; other messages left alone\n" );
    return failures == 0 ? 0 : 1;
}
