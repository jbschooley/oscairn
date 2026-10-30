// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include "oscairn/control.h"
#include <array>

namespace oscairn::standalone
{
    struct SoundChoice
    {
        std::string name, preset;
        std::vector<oscairn_sound_parameter_t> parameters;
        std::string sfz{}, directory{}; // patch text and its asset directory
    };
    struct SoundPickerState
    {
        NodeId node = 0;
        std::string pluginId, instrument;
        bool importsSamples = false; // the instrument prepares SFZ/WAV files
        Control::Sounds sounds{};
        std::vector<SoundChoice> choices;
        std::array<bool,2> canToggleBrightnessLocks{}, brightnessLocks{};
    };
    struct SoundAction
    {
        enum Kind { Refresh, Prepare, Select, Discard, BrightnessLocks, Replace, SavePerformance, LoadPerformance } kind = Refresh;
        NodeId node = 0;
        std::string pluginId;
        uint32_t context = 0, track = 0;
        int16_t channel = -1;
        bool cutTail = false;
        bool enableLocks = false;
        SoundChoice choice;
        std::string replacementPath,replacementId,directory;
        std::string importPath; // Prepare from this .sfz or .wav instead of choice
    };
    SoundPickerState readSoundPicker(Control&);
    // A .sfz file as written, or a one-region SFZ patch for a .wav file; throws if unreadable.
    SoundChoice importSound(const std::string& path);
    void applySoundAction(Control&, const SoundAction&);
}
