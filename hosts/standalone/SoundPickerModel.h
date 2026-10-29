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
    };
    struct SoundAction
    {
        enum Kind { Refresh, Prepare, Select, Discard } kind = Refresh;
        NodeId node = 0;
        std::string pluginId;
        uint32_t context = 0;
        bool cutTail = false;
        SoundChoice choice;
    };
    SoundPickerState readSoundPicker(Control&);
    void applySoundAction(Control&, const SoundAction&);
}
