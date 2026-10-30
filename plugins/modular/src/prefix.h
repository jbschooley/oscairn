// SPDX-License-Identifier: AGPL-3.0-or-later
//
// What KICK's JUCE module build supplied to the upstream engine files, which have no
// includes of their own. Included first in every file of this plugin.
#pragma once
#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_data_structures/juce_data_structures.h>
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

// KICK's directory lookup (Kick/modules/common/source/kick_paths.h); only MTPrefs is used,
// for user presets. Oscairn's version is in compat.cpp.
enum DirType { MTAssets, MTSamples, MTTemp, MTProjects, MTLog, MTPrefs, MTDemos };
juce::File getAppDirs(DirType);

#include "../upstream/mt_instrument_engine.h"
#include "../upstream/modular/mt_modular_modules.h"
#include "../upstream/modular/mt_modular_patch.h"
#include "../upstream/modular/mt_modular_engine.h"
