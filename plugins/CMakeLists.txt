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

oscairn_add_plugin(oscairn_transpose oscairn-transpose midi-effects/midi_effect.cpp)
oscairn_add_plugin(oscairn_midi_delay oscairn-midi-delay midi-effects/midi_effect.cpp)
target_compile_definitions(oscairn_midi_delay PRIVATE OSCAIRN_MIDI_DELAY=1)

# The sampler decodes WAV assets with libsndfile (LGPL-2.1-or-later), a production dependency.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)
oscairn_add_plugin(oscairn_sampler oscairn-sampler sampler/sampler.cpp)
target_link_libraries(oscairn_sampler PRIVATE PkgConfig::SNDFILE)
