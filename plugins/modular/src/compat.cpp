// SPDX-License-Identifier: AGPL-3.0-or-later
#include "prefix.h"

// User presets live in Oscairn's data directory: OSCAIRN_DATA_HOME, else the platform's
// application data folder.
juce::File getAppDirs(DirType)
{
    const auto home = juce::SystemStats::getEnvironmentVariable("OSCAIRN_DATA_HOME", {});
    if(home.isNotEmpty()) return juce::File(home).getChildFile("modular");
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Oscairn").getChildFile("modular");
}
