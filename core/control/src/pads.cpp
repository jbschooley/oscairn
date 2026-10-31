// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Pad maps. The in-key layouts follow chillbeast's PadLayout in his KICK firmware fork
// (github.com/chillbeast/0700-AAM, kick_pad_layout.h, AGPL-3.0-or-later).
#include "oscairn/pads.h"

namespace oscairn
{
    PadMap::PadMap() { fillPads(*this, -1, {}); }

    bool PadMap::valid() const
    {
        if(bank >= banks) return false;
        for(const auto& b : assignments)
            for(const auto& a : b)
                if(a.note > 127 || a.channel < -1 || a.channel > 15 || !(a.velocity >= 0 && a.velocity <= 1) || a.pressure > PadAssignment::Pressure::Channel) return false;
        return true;
    }

    const std::vector<std::pair<std::string, std::vector<int>>>& padScales()
    {
        static const std::vector<std::pair<std::string, std::vector<int>>> scales{
            {"Major", {0, 2, 4, 5, 7, 9, 11}}, {"Minor", {0, 2, 3, 5, 7, 8, 10}}, {"Dorian", {0, 2, 3, 5, 7, 9, 10}},
            {"Phrygian", {0, 1, 3, 5, 7, 8, 10}}, {"Lydian", {0, 2, 4, 6, 7, 9, 11}}, {"Mixolydian", {0, 2, 4, 5, 7, 9, 10}},
            {"Locrian", {0, 1, 3, 5, 6, 8, 10}}, {"Harmonic minor", {0, 2, 3, 5, 7, 8, 11}}, {"Melodic minor", {0, 2, 3, 5, 7, 9, 11}},
            {"Major pentatonic", {0, 2, 4, 7, 9}}, {"Minor pentatonic", {0, 3, 5, 7, 10}}, {"Blues", {0, 3, 5, 6, 7, 10}}};
        return scales;
    }

    std::string noteName(int note)
    {
        static const char* names[]{"C", "C♯", "D", "D♯", "E", "F", "F♯", "G", "G♯", "A", "A♯", "B"};
        return note < 0 || note > 127 ? std::string("–") : names[note % 12] + std::to_string(note / 12 - 1);
    }

    void fillPads(PadMap& map, int bank, const PadFill& fill)
    {
        // General MIDI drums, bottom row first: kick, snare, closed and open hat; toms; cymbals
        // and hand claps; more percussion.
        static constexpr std::array<uint8_t, 16> drums{36, 38, 42, 46, 41, 43, 45, 47, 49, 51, 39, 37, 44, 56, 54, 50};
        const auto& steps = padScales().at(fill.scale < padScales().size() ? fill.scale : 0).second;
        for(int b = 0; b < int(PadMap::banks); ++b)
        {
            if(bank >= 0 && b != bank) continue;
            for(int pad = 0; pad < int(PadMap::pads); ++pad)
            {
                const int row = pad / int(PadMap::columns), column = pad % int(PadMap::columns);
                int note;
                switch(fill.kind)
                {
                case PadFill::Kind::Chromatic: note = (fill.first + 16 * (bank < 0 ? b : 0) + pad) % 128; break; // all banks: every note, wrapping
                case PadFill::Kind::Drums: note = drums[size_t(pad)]; break;
                default:
                {
                    const int base = 12 * (b + 2) + fill.root;
                    if(fill.inKey)
                    {
                        const int rowStep = fill.kind == PadFill::Kind::Fourths ? 3 : fill.kind == PadFill::Kind::Thirds ? 2 : int(PadMap::columns);
                        const int degree = row * rowStep + column, n = int(steps.size());
                        note = base + 12 * (degree / n) + steps[size_t(degree % n)];
                    }
                    else note = base + row * (fill.kind == PadFill::Kind::Fourths ? 5 : int(PadMap::columns)) + column;
                }
                }
                if(note >= 0 && note <= 127) map.assignments[size_t(b)][size_t(pad)].note = uint8_t(note);
            }
        }
    }
}
