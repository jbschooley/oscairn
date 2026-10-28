// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <clap/events.h>

#include <cstdint>

namespace oscairn
{
    /// The platform clock: tempo, time signature and play position, counted in samples.
    /// Every plugin in a graph receives the same transport for a block.
    class Transport
    {
    public:
        explicit Transport( double sampleRate, double tempo = 120.0 );

        void setTempo( double bpm ) noexcept { tempo_ = bpm; }
        void setPlaying( bool playing ) noexcept { playing_ = playing; }

        double  tempo() const noexcept { return tempo_; }
        bool    playing() const noexcept { return playing_; }
        int64_t position() const noexcept { return position_; }

        /// The CLAP transport for a block starting at the current position.
        clap_event_transport_t current() const noexcept;

        /// Moves the position on by one block (only while playing).
        void advance( uint32_t frames ) noexcept;

    private:
        double   sampleRate_;
        double   tempo_;
        bool     playing_  = true;
        int64_t  position_ = 0;
        uint16_t tsigNum_  = 4;
        uint16_t tsigDen_  = 4;
    };
}
