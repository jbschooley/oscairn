// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <clap/events.h>

#include <array>
#include <cstdint>

namespace oscairn
{
    /// The platform clock: tempo, time signature and play position. Every plugin in a
    /// graph receives the same transport for a block.
    ///
    /// The clock runs from its own tempo (Internal) or follows an external MIDI clock
    /// (MidiClock), estimating the tempo from the pulse intervals and the position from
    /// the pulse count. Start and stop take effect at block boundaries.
    class Transport
    {
    public:
        enum class ClockSource
        {
            Internal,
            MidiClock
        };

        explicit Transport( double sampleRate, double tempo = 120.0 );

        /// Internal tempo; also used while following MIDI clock until two pulses have arrived.
        /// A change keeps the current beat position and applies from there.
        void setTempo( double bpm ) noexcept;
        void setPlaying( bool playing ) noexcept { playing_ = playing; }
        void setClockSource( ClockSource source ) noexcept;

        double  tempo() const noexcept;
        bool    playing() const noexcept;
        int64_t position() const noexcept { return position_; }

        /// External MIDI clock: clock (0xF8), start (0xFA), continue (0xFB), stop (0xFC)
        /// and song position pointer (0xF2, `songPosition` in sixteenth notes), stamped with
        /// their sample time on the transport's timeline. Feed messages in order once their
        /// time has passed: after processing the block they fall in, before current() for
        /// the next block.
        void midiClock( uint8_t status, int64_t sampleTime, uint16_t songPosition = 0 ) noexcept;

        /// The CLAP transport for a block starting now.
        clap_event_transport_t current() const noexcept;

        /// Moves time on by one block.
        void advance( uint32_t frames ) noexcept;

    private:
        double   sampleRate_;
        double   tempo_;
        bool     playing_  = true;
        int64_t  now_      = 0; // samples since the transport was created
        int64_t  position_ = 0; // internal play position in samples
        // The internal beat position is anchorBeats_ at anchorSample_ (the last tempo
        // change), moving on at tempo_ from there.
        int64_t  anchorSample_ = 0;
        double   anchorBeats_  = 0.0;
        uint16_t tsigNum_  = 4;
        uint16_t tsigDen_  = 4;

        ClockSource source_ = ClockSource::Internal;

        // MIDI clock state.
        bool                     clockRunning_ = false;
        int64_t                  pulses_       = -1; // pulse index of the last clock (24 per beat); -1 before the first
        int64_t                  lastPulseTime_ = 0;
        std::array<int64_t, 24>  intervals_{};       // the last beat's worth of pulse intervals
        size_t                   intervalCount_ = 0, intervalNext_ = 0;
    };
}
