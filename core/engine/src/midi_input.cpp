// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/midi_input.h"

namespace oscairn
{
    namespace
    {
        kick::clap::Event pressure( uint32_t time, int16_t port, int16_t channel, int16_t key, uint8_t value )
        {
            clap_event_note_expression_t e{};
            e.header.size     = sizeof( e );
            e.header.time     = time;
            e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            e.header.type     = CLAP_EVENT_NOTE_EXPRESSION;
            e.expression_id   = CLAP_NOTE_EXPRESSION_PRESSURE;
            e.note_id         = -1;
            e.port_index      = port;
            e.channel         = channel;
            e.key             = key; // -1: every note on the channel
            e.value           = value / 127.0;
            return kick::clap::Event::from( e );
        }
    }

    bool midiToClap( const uint8_t* bytes, size_t size, uint32_t time, kick::clap::Event& out, int16_t port ) noexcept
    {
        if( !bytes || size < 2 || bytes[0] < 0x80 || bytes[0] >= 0xF0 || bytes[1] > 127 )
            return false;
        const auto kind    = bytes[0] & 0xF0;
        const auto channel = (int16_t)( bytes[0] & 0x0F );
        if( kind == 0xD0 ) // channel aftertouch: one data byte
        {
            out = pressure( time, port, channel, -1, bytes[1] );
            return true;
        }
        if( size < 3 || bytes[2] > 127 )
            return false;
        switch( kind )
        {
        case 0x90:
        case 0x80:
        {
            const bool on = kind == 0x90 && bytes[2] > 0;
            out           = kick::clap::Event::note( on, bytes[1], bytes[2] / 127.0, time, channel, port );
            return true;
        }
        case 0xA0: out = pressure( time, port, channel, bytes[1], bytes[2] ); return true;
        default: return false;
        }
    }
}
