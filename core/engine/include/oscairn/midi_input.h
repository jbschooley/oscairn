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
    /// - pitch bend, and the performance controllers as CLAP MIDI: mod wheel (CC1), breath (CC2),
    ///   foot (CC4), expression (CC11), the pedals (CC64-67: sustain, portamento, sostenuto,
    ///   soft), all sound off (CC120) and all notes off (CC123).
    ///
    /// Returns false, leaving `out` alone, for messages it does not convert (other controllers,
    /// program changes, system messages, and malformed ones).
    bool midiToClap( const uint8_t* bytes, size_t size, uint32_t time, kick::clap::Event& out, int16_t port = 0 ) noexcept;
}
