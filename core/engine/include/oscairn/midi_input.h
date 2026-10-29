// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <kick_clap/Host.h>

#include <cstddef>
#include <cstdint>

namespace oscairn
{
    /// Converts a MIDI 1.0 channel message from a keyboard, pads or a controller into the
    /// CLAP event for the graph's note input, at `time` (samples into the block):
    ///
    /// - note on and note off (a note on with velocity 0 is a note off);
    /// - polyphonic aftertouch: pressure on that key (a CLAP pressure note expression, 0..1);
    /// - channel aftertouch: pressure on every note of that channel (the same expression with
    ///   CLAP's wildcard key);
    /// - sustain (CC64), all sound off (CC120), and all notes off (CC123), as CLAP MIDI.
    ///
    /// Returns false, leaving `out` alone, for messages it does not convert (other controllers,
    /// pitch bend, system messages, and malformed ones).
    bool midiToClap( const uint8_t* bytes, size_t size, uint32_t time, kick::clap::Event& out, int16_t port = 0 ) noexcept;
}
