// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/transport.h"

#include <clap/fixedpoint.h>

#include <cmath>

namespace oscairn
{
    Transport::Transport( double sampleRate, double tempo )
        : sampleRate_( sampleRate )
        , tempo_( tempo )
    {
    }

    clap_event_transport_t Transport::current() const noexcept
    {
        const double seconds = (double)position_ / sampleRate_;
        const double beats   = seconds * tempo_ / 60.0;
        const auto   bar     = (int32_t)std::floor( beats / tsigNum_ );

        clap_event_transport_t t{};
        t.header.size     = sizeof( t );
        t.header.time     = 0;
        t.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        t.header.type     = CLAP_EVENT_TRANSPORT;
        t.flags           = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_SECONDS_TIMELINE
                  | CLAP_TRANSPORT_HAS_TIME_SIGNATURE | ( playing_ ? CLAP_TRANSPORT_IS_PLAYING : 0 );
        t.song_pos_beats   = (clap_beattime)std::llround( beats * (double)CLAP_BEATTIME_FACTOR );
        t.song_pos_seconds = (clap_sectime)std::llround( seconds * (double)CLAP_SECTIME_FACTOR );
        t.tempo            = tempo_;
        t.tempo_inc        = 0.0;
        t.bar_number       = bar;
        t.bar_start        = (clap_beattime)bar * tsigNum_ * CLAP_BEATTIME_FACTOR;
        t.tsig_num         = tsigNum_;
        t.tsig_denom       = tsigDen_;
        return t;
    }

    void Transport::advance( uint32_t frames ) noexcept
    {
        if( playing_ )
            position_ += frames;
    }
}
