// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace oscairn
{
    // Which note each pad plays: eight banks (A..H) of 16 pads, numbered from the bottom-left,
    // row by row, as on the KICK. Each pad has its own note, MIDI channel, velocity and pressure,
    // as in the KICK firmware's MIDI Control mode. Saved with the Performance.
    struct PadAssignment
    {
        enum class Pressure : uint8_t { None, Poly, Channel };
        uint8_t note = 36;
        int8_t channel = -1;   // MIDI channel 0..15, or -1: the channel the player is on (the selected track's)
        double velocity = 0;   // 0: as played; above 0 (up to 1): every press at this velocity
        Pressure pressure = Pressure::Poly; // how holding pressure is sent: poly aftertouch, channel pressure or not
        bool operator==(const PadAssignment&) const = default;
    };

    struct PadMap
    {
        static constexpr size_t pads = 16, banks = 8, columns = 4;
        std::array<std::array<PadAssignment, pads>, banks> assignments; // default: chromatic, bank A from 36
        uint8_t bank = 0;
        PadMap();
        bool operator==(const PadMap&) const = default;
        bool valid() const;
    };

    // Filling pads with notes (only notes change; channels, velocities and pressure stay).
    struct PadFill
    {
        // Chromatic: from `first`, 16 per bank (filling every bank wraps past 127 to 0, so all 128
        // notes have a pad; the KICK's own mapping is first = 0). Drums: General MIDI drums. The rest, like Push
        // layouts: rows continue (sequential), or go up a fourth or a third, in key or chromatic,
        // each bank an octave higher, bank A from MIDI 24 (C1) plus the root.
        enum class Kind { Chromatic, Drums, Sequential, Fourths, Thirds } kind = Kind::Chromatic;
        uint8_t first = 36;    // chromatic: the first pad's note
        uint8_t root = 0;      // layouts: 0 = C .. 11 = B
        size_t scale = 0;      // layouts: index into padScales()
        bool inKey = true;     // layouts: only notes of the scale
    };
    // Fills one bank, or every bank when bank is -1. A note outside 0..127 leaves its pad unchanged.
    void fillPads(PadMap& map, int bank, const PadFill& fill);
    // Named scales as semitone offsets from the root, major first.
    const std::vector<std::pair<std::string, std::vector<int>>>& padScales();
    // A note's name, such as C4 for 60 (middle C), as the desktop names notes.
    std::string noteName(int note);
}
