cmake_minimum_required(VERSION 3.21)
project(oscairn VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The JUCE desktop app and audio/MIDI helper. Off by default: these download JUCE
# and need platform device libraries. The core never depends on JUCE.
option(OSCAIRN_STANDALONE "Build the desktop app (downloads JUCE)" OFF)
option(OSCAIRN_ENGINE "Build the separate audio/MIDI engine helper (downloads JUCE)" OFF)
option(OSCAIRN_LV2 "Enable LV2 hosting (requires Lilv and LV2 development packages)" OFF)
set(OSCAIRN_LV2_EXAMPLE_DIR "" CACHE PATH "Optional upstream LV2 example bundles for integration tests")

enable_testing()

add_subdirectory(core)
add_subdirectory(plugins)
add_subdirectory(hosts)
