# First-party CLAP plugins. Each builds as a loadable .clap module.
function(oscairn_add_plugin target output source)
    add_library(${target} MODULE ${source})
    target_link_libraries(${target} PRIVATE oscairn_clap_headers)
    target_compile_options(${target} PRIVATE -Wall -Wextra)
    set_target_properties(${target} PROPERTIES
        PREFIX ""
        SUFFIX ".clap"
        OUTPUT_NAME ${output}
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
    # lib/clap is where CLAP hosts look on Linux (/usr/lib/clap).
    install(TARGETS ${target} LIBRARY DESTINATION lib/clap)
endfunction()

oscairn_add_plugin(oscairn_step_sequencer oscairn-step-sequencer step-sequencer/step_sequencer.cpp)
oscairn_add_plugin(oscairn_basic_synth oscairn-basic-synth basic-synth/basic_synth.cpp)
oscairn_add_plugin(oscairn_delay oscairn-delay delay/delay.cpp)
# Split and Join, the nodes at either end of a part's left/right, mid/side and band lanes.
oscairn_add_plugin(oscairn_splitters oscairn-splitters splitters/splitters.cpp)

oscairn_add_plugin(oscairn_transpose oscairn-transpose midi-effects/midi_effect.cpp)
oscairn_add_plugin(oscairn_midi_delay oscairn-midi-delay midi-effects/midi_effect.cpp)
target_compile_definitions(oscairn_midi_delay PRIVATE OSCAIRN_MIDI_DELAY=1)

# The Sampler lives in its own repository (github.com/oscairn/sampler), fetched at a pinned
# commit; it decodes WAV assets with libsndfile (LGPL-2.1-or-later). To build a local checkout
# instead, set FETCHCONTENT_SOURCE_DIR_OSCAIRN_SAMPLER. The OS image builds it from its
# Buildroot package, which must name the same commit.
set(OSCAIRN_SAMPLER_COMMIT 20b4282c8246415e2bc61024f587996722387d5a)
file(STRINGS "${PROJECT_SOURCE_DIR}/os/buildroot/package/oscairn-sampler/oscairn-sampler.mk" sampler_version REGEX "^OSCAIRN_SAMPLER_VERSION = ")
if(NOT sampler_version STREQUAL "OSCAIRN_SAMPLER_VERSION = ${OSCAIRN_SAMPLER_COMMIT}")
    message(FATAL_ERROR "The Sampler's commit differs between plugins/CMakeLists.txt and os/buildroot/package/oscairn-sampler")
endif()
include(FetchContent)
FetchContent_Declare(oscairn_sampler GIT_REPOSITORY https://github.com/oscairn/sampler.git GIT_TAG ${OSCAIRN_SAMPLER_COMMIT})
FetchContent_MakeAvailable(oscairn_sampler)
set_target_properties(oscairn_sampler PROPERTIES LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}) # beside the other plugins

# Modular instrument (plugins/modular): downloads JUCE.
if(OSCAIRN_MODULAR OR OSCAIRN_STANDALONE)
    add_subdirectory(modular)
endif()
