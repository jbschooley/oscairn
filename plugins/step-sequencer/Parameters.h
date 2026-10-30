// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstdint>

namespace oscairn::sequencer
{
    constexpr uint32_t tracks = 2, patterns = 2, steps = 16;
    constexpr uint32_t requestedPattern = 1, activePattern = 2;
    enum StepKind : uint32_t
    {
        On = 1,
        Note = 2,
        Lock = 3
    };
    constexpr uint32_t channel(uint32_t track)
    {
        return 10 + track * 10;
    }
    constexpr uint32_t mute(uint32_t track)
    {
        return 11 + track * 10;
    }
    constexpr uint32_t playhead(uint32_t track)
    {
        return 12 + track * 10;
    }
    constexpr uint32_t lockScope(uint32_t track)
    {
        return 13 + track * 10;
    }
    constexpr uint32_t length(uint32_t pattern, uint32_t track)
    {
        return pattern * 10000 + track * 1000 + 50;
    }
    constexpr uint32_t step(uint32_t pattern, uint32_t track, StepKind kind, uint32_t index)
    {
        return pattern * 10000 + track * 1000 + uint32_t(kind) * 100 + index;
    }
    constexpr uint32_t parametersPerTrack = 1 + 3 * steps;
    constexpr uint32_t parameterCount = 2 + 4 * tracks + patterns * tracks * parametersPerTrack;
    constexpr uint32_t parameterId(uint32_t index)
    {
        if (index < 2)
            return index + 1;
        index -= 2;
        if (index < 4 * tracks)
            return channel(index / 4) + index % 4;
        index -= 4 * tracks;
        const auto bank = index / parametersPerTrack, member = index % parametersPerTrack;
        return member == 0 ? length(bank / tracks, bank % tracks)
                           : step(bank / tracks, bank % tracks, StepKind((member - 1) / steps + 1), (member - 1) % steps);
    }
    constexpr uint32_t parameterIndex(uint32_t id)
    {
        for (uint32_t i = 0; i < parameterCount; ++i)
            if (parameterId(i) == id)
                return i;
        return parameterCount;
    }
} // namespace oscairn::sequencer
