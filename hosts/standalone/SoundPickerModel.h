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
        std::string sfz{}, library{}, directory{}; // patch text, and the asset library folder its files are in
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
        enum Kind { Refresh, Prepare, Select, Discard, BrightnessLocks, Replace, SavePerformance, LoadPerformance, SaveToLibrary, LoadFromLibrary, SaveSound, LoadSound, PartFromSound, ImportPerformance,
                    ExportPerformance, InstallArchive } kind = Refresh;
        NodeId node = 0;
        std::string pluginId;
        uint32_t context = 0, track = 0;
        int16_t channel = -1;
        bool cutTail = false;
        bool enableLocks = false;
        SoundChoice choice;
        std::string replacementPath,replacementId,directory;
        std::string libraryId; // LoadFromLibrary, LoadSound, PartFromSound, ExportPerformance (that library item, not the open one); SaveToLibrary and SaveSound save under choice.name
        uint32_t part = 0;     // SaveSound, LoadSound
        std::vector<uint32_t> parts, scenes; // ImportPerformance from directory: by its IDs; both none for all of it
        bool asNew = false;    // SaveToLibrary, SaveSound: a new library item, not the next revision of the one it came from;
                               // ExportPerformance: with edited parts' Sounds; InstallArchive: as a locked library
        std::vector<std::string> items; // InstallArchive from directory (the archive file): these items; none for all
        std::string importPath; // Prepare from this .sfz or .wav instead of choice
    };
    SoundPickerState readSoundPicker(Control&);
    // A .sfz file as written (its folder registered as an asset library), or a one-region SFZ
    // patch with a 30 ms release for a .wav file copied into the user library; throws if unusable.
    SoundChoice importSound(Control&, const std::string& path);
    // Returns what to tell the user when it went well, if anything.
    std::string applySoundAction(Control&, const SoundAction&);
}
