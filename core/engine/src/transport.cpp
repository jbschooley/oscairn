// SPDX-License-Identifier: AGPL-3.0-or-later
#include "oscairn/transport.h"

#include <clap/fixedpoint.h>

#include <algorithm>
#include <cmath>

namespace oscairn
{
    namespace
    {
        constexpr double pulsesPerBeat = 24.0;
    }

    Transport::Transport( double sampleRate, double tempo )
        : sampleRate_( sampleRate )
        , tempo_( tempo )
    {
    }

    void Transport::setTempo( double bpm ) noexcept
    {
        if( bpm == tempo_ )
            return;
        anchorBeats_  = anchorBeats_ + (double)( position_ - anchorSample_ ) / sampleRate_ * tempo_ / 60.0;
        anchorSample_ = position_;
        tempo_        = bpm;
    }

    void Transport::setClockSource( ClockSource source ) noexcept
    {
        source_       = source;
        clockRunning_ = false;
        pulses_       = -1;
        intervalCount_ = intervalNext_ = 0;
    }

    double Transport::tempo() const noexcept
    {
        if( source_ == ClockSource::MidiClock && intervalCount_ > 0 )
        {
            int64_t sum = 0;
            for( size_t i = 0; i < intervalCount_; ++i )
                sum += intervals_[i];
            const double samplesPerPulse = (double)sum / (double)intervalCount_;
            return 60.0 * sampleRate_ / ( samplesPerPulse * pulsesPerBeat );
        }
        return tempo_;
    }

    bool Transport::playing() const noexcept
    {
        return source_ == ClockSource::MidiClock ? ( clockRunning_ && pulses_ >= 0 ) : playing_;
    }

    void Transport::midiClock( uint8_t status, int64_t sampleTime, uint16_t songPosition ) noexcept
    {
        switch( status )
        {
            case 0xFA: // start: the next clock is beat 0
                clockRunning_  = true;
                pulses_        = -1;
                intervalCount_ = intervalNext_ = 0;
                break;
            case 0xFB: // continue from the current position
                clockRunning_ = true;
                break;
            case 0xFC: // stop
                clockRunning_ = false;
                break;
            case 0xF2: // song position, in sixteenths (6 pulses each); the next clock lands on it
                pulses_ = (int64_t)songPosition * 6 - 1;
                break;
            case 0xF8: // clock
                if( !clockRunning_ )
                    break;
                if( pulses_ >= 0 )
                {
                    intervals_[intervalNext_] = sampleTime - lastPulseTime_;
                    intervalNext_             = ( intervalNext_ + 1 ) % intervals_.size();
                    intervalCount_            = std::min( intervalCount_ + 1, intervals_.size() );
                }
                ++pulses_;
                lastPulseTime_ = sampleTime;
                break;
            default:
                break;
        }
    }

    clap_event_transport_t Transport::current() const noexcept
    {
        const double bpm = tempo();
        double       beats;
        if( source_ == ClockSource::MidiClock )
        {
            // Position of the last pulse, moved on at the estimated tempo but never past
            // the next expected pulse, so it cannot run ahead or step backwards.
            const double lastPulseBeat = std::max<int64_t>( pulses_, 0 ) / pulsesPerBeat;
            const double elapsed       = playing() ? (double)( now_ - lastPulseTime_ ) / sampleRate_ * bpm / 60.0 : 0.0;
            beats                      = lastPulseBeat + std::clamp( elapsed, 0.0, 1.0 / pulsesPerBeat );
        }
        else
            beats = anchorBeats_ + (double)( position_ - anchorSample_ ) / sampleRate_ * bpm / 60.0;

        const double seconds = source_ == ClockSource::Internal ? (double)position_ / sampleRate_ : beats * 60.0 / bpm;
        const auto   bar     = (int32_t)std::floor( beats / tsigNum_ );

        clap_event_transport_t t{};
        t.header.size     = sizeof( t );
        t.header.time     = 0;
        t.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        t.header.type     = CLAP_EVENT_TRANSPORT;
        t.flags           = CLAP_TRANSPORT_HAS_TEMPO | CLAP_TRANSPORT_HAS_BEATS_TIMELINE | CLAP_TRANSPORT_HAS_SECONDS_TIMELINE
                  | CLAP_TRANSPORT_HAS_TIME_SIGNATURE | ( playing() ? CLAP_TRANSPORT_IS_PLAYING : 0 );
        t.song_pos_beats   = (clap_beattime)std::llround( beats * (double)CLAP_BEATTIME_FACTOR );
        t.song_pos_seconds = (clap_sectime)std::llround( seconds * (double)CLAP_SECTIME_FACTOR );
        t.tempo            = bpm;
        t.tempo_inc        = 0.0;
        t.bar_number       = bar;
        t.bar_start        = (clap_beattime)bar * tsigNum_ * CLAP_BEATTIME_FACTOR;
        t.tsig_num         = tsigNum_;
        t.tsig_denom       = tsigDen_;
        return t;
    }

    void Transport::advance( uint32_t frames ) noexcept
    {
        now_ += frames;
        if( source_ == ClockSource::Internal && playing_ )
            position_ += frames;
    }
}
