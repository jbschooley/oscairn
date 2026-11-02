// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <oscairn-clap/harmony.h>

namespace oscairn
{
    /// RFC 0033: reads the chord the keys in `harmony.held` make into its chord fields, by the
    /// table and rule oscairn-clap/harmony.h gives. Audio thread safe: no allocation.
    void recognizeChord( oscairn_harmony_t& harmony ) noexcept;
}
