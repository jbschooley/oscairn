// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "oscairn/control.h"

#include <optional>
#include <string_view>

namespace oscairn
{
    /// Transport-independent JSON-RPC adapter. Call only on Control's thread, never
    /// from audio. No I/O or threads; one complete JSON document per dispatch call.
    /// nullopt means notification(s) only: a transport must send no response.
    /// See docs/control-protocol.md for methods, limits and partial queue acceptance.
    class JsonRpc
    {
    public:
        static constexpr size_t maxMessageBytes = 65536;
        static constexpr size_t maxDepth = 32;
        static constexpr size_t maxBatch = 64;
        static constexpr size_t maxParameterChanges = 128;

        explicit JsonRpc( Control& control ) : control_( control ) {}
        std::optional<std::string> dispatch( std::string_view message );
        /// One adapter per client. Host supplies monotonic time; no clock reads in core.
        /// Default notifications are throttled to 60 Hz; allChanges bypasses coalescing.
        std::vector<std::string> notifications( uint64_t nowNanoseconds );
        void subscription( std::string_view topic, bool enabled, bool allChanges = false );

    private:
        Control& control_;
        bool params_ = false, transport_ = false, graph_ = false, allChanges_ = false;
        bool status_ = false;
        uint64_t paramsCursor_ = 0, graphCursor_ = 0;
        std::optional<uint64_t> lastParams_, lastTransport_;
        std::string lastTransportValue_;
        std::optional<uint64_t> lastStatus_;
        std::string lastStatusValue_;
    };
}
