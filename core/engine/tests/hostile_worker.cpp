// SPDX-License-Identifier: AGPL-3.0-or-later
// oscairn-hostile-worker: the plugin worker, but writing an out-of-range voice-budget report after
// each block, as a compromised worker might (voice-extension-hostile). Every other block: counts
// at their largest and NaN levels, then one past the bounds with negative and infinite ones.
#include "Worker.h"
#include <cmath>
#include <limits>

namespace oscairn::worker
{
    void hostileReport(Shared& s) noexcept
    {
        static bool odd = false;
        odd = !odd;
        constexpr auto nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
        s.activeVoices = odd ? UINT32_MAX : (1u << 16) + 1;
        s.voiceNoteCount = odd ? UINT32_MAX : maxVoiceNotes + 1;
        s.voiceCost = odd ? nan : -inf;
        for(auto& n : s.voiceNotes) { n.voices = UINT32_MAX; n.loudness = float(odd ? nan : -1.0); }
        s.voiceNotes[0].loudness = float(inf);
    }
}
