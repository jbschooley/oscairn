// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "oscairn/control.h"

namespace oscairn::standalone
{
    struct SoundChoice
    {
        std::string name, preset;
        std::vector<oscairn_sound_parameter_t> parameters;
    };
    struct SoundPickerState
    {
        NodeId node = 0;
        std::string pluginId, instrument;
        Control::Sounds sounds{};
        std::vector<SoundChoice> choices;
        bool canToggleBrightnessLocks = false, brightnessLocks = false;
    };
    struct SoundAction
    {
        enum Kind { Refresh, Prepare, Select, Discard, BrightnessLocks, Replace, SavePerformance, LoadPerformance } kind = Refresh;
        NodeId node = 0;
        std::string pluginId;
        uint32_t context = 0;
        bool cutTail = false;
        bool enableLocks = false;
        SoundChoice choice;
        std::string replacementPath,replacementId,directory;
    };
    SoundPickerState readSoundPicker(Control&);
    void applySoundAction(Control&, const SoundAction&);
}
