// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace oscairn
{
    /// Where an event entered the graph: a MIDI input endpoint (RFC 0012 selectors). The host
    /// numbers its inputs 1..63; 0 is the built-in panel, keys and pads. Events from plugins,
    /// or from a host that does not say, are untagged and pass every zone's source mask.
    inline constexpr uint8_t builtInSource = 0, untaggedSource = 0xFF;
    inline constexpr size_t maxSources = 64;

    /// A zone on a note connection (RFC 0011): which notes pass, and how they change. A note
    /// passes when its key, velocity, MIDI channel and source are in range; it is then
    /// transposed and its velocity shaped by the curve, or set to a fixed velocity.
    struct NoteZone
    {
        uint8_t lowKey = 0, highKey = 127;
        double lowVelocity = 0, highVelocity = 1; // as CLAP velocities, 0..1 (MIDI velocity / 127)
        uint16_t channels = 0xFFFF;               // one bit per MIDI channel
        int8_t transpose = 0;
        double curve = 1;                         // velocity out = in^curve: above 1 softer, below 1 harder
        double fixedVelocity = 0;                 // above 0: every note plays at this velocity
        uint64_t sources = ~0ull;                 // one bit per source; none: no device plays it
        // Notes at once on the connection (RFC 0018 step 2), the first zone's for all of them; 0:
        // no limit. A note past it chokes the oldest released note, else the oldest held one.
        uint16_t maxNotes = 0;
        bool operator==(const NoteZone&) const = default;

        bool valid() const
        {
            return lowKey <= highKey && highKey <= 127 && lowVelocity >= 0 && lowVelocity <= highVelocity && highVelocity <= 1 &&
                   channels && transpose >= -127 && std::isfinite(curve) && curve > 0 && curve <= 16 && fixedVelocity >= 0 && fixedVelocity <= 1;
        }
        bool hears(uint8_t source) const noexcept { return source >= maxSources || (sources >> source & 1); }
        bool accepts(int key, double velocity, int channel, uint8_t source = untaggedSource) const noexcept
        {
            return key >= lowKey && key <= highKey && velocity >= lowVelocity && velocity <= highVelocity && (channel < 0 || channel > 15 || channels & (1u << channel)) && hears(source);
        }
        double shape(double velocity) const noexcept { return fixedVelocity > 0 ? fixedVelocity : std::pow(velocity, curve); }
    };

    /// The zones of one note connection, fixed in size so the audio thread never allocates.
    /// None: every note passes unchanged.
    struct NoteZones
    {
        static constexpr size_t capacity = 8;
        std::array<NoteZone, capacity> zones{};
        size_t count = 0;
        bool passes(int channel, uint8_t source) const noexcept // a MIDI message other than a note
        {
            for(size_t i = 0; i < count; ++i) if(zones[i].channels & (1u << channel) && zones[i].hears(source)) return true;
            return false;
        }
        bool hears(uint8_t source) const noexcept
        {
            for(size_t i = 0; i < count; ++i) if(zones[i].hears(source)) return true;
            return false;
        }
    };
}
