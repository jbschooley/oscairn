// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "oscairn/graph.h"
#include "oscairn/transport.h"

#include <string>
#include <vector>

namespace oscairn
{
    /// In-process control boundary. All calls belong to one control/main thread;
    /// audio and hardware input do not call this interface. No device or plugin handles
    /// escape it. The socket/JSON adapter will implement the same control operations.
    class Control
    {
    public:
        virtual ~Control() = default;

        struct ParamInfo
        {
            uint32_t    id = 0;
            std::string name;
            double      minimum = 0, maximum = 1, defaultValue = 0;
            bool        stepped = false;
            uint32_t    flags = 0;
        };
        struct TransportState
        {
            bool   playing = false;
            double tempo = 120.0;
            double beat = 0.0;
        };

        virtual NodeId nodeId( const std::string& name ) const = 0;
        virtual bool hasParameter( NodeId node, uint32_t param ) const = 0;
        virtual std::vector<ParamInfo> parameters( NodeId node ) const = 0;
        virtual ParamInfo parameterInfo( NodeId node, uint32_t param ) const = 0;
        virtual double parameter( NodeId node, uint32_t param ) const = 0;
        virtual std::string parameterText( NodeId node, uint32_t param, double value ) const = 0;
        /// Invalid addresses/values throw std::invalid_argument. False means the bounded
        /// plugin queue refused the command; callers must not report that it was applied.
        virtual bool setParameter( NodeId node, uint32_t param, double value ) = 0;

        virtual void setPlaying( bool playing ) = 0;
        virtual void setTempo( double bpm ) = 0;
        virtual void setClockSource( Transport::ClockSource source ) = 0;
        virtual Transport::ClockSource clockSource() const = 0;
        /// Last rendered block's transport. Fields are atomic telemetry, not a transaction.
        virtual TransportState transportState() const = 0;

        /// UI notes at the next block boundary; channel is zero-based. Physical MIDI
        /// bypasses this queue and enters Session::process with sample offsets.
        /// False means overflow; the session discards queued UI notes and broadcasts
        /// release on the next block. This may release other voices on shared destinations.
        virtual bool note( bool on, int key, double velocity, int channel = 0 ) = 0;
    };
}
