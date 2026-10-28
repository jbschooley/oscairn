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

    private:
        Control& control_;
    };
}
