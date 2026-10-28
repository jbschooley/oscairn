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
endfunction()

oscairn_add_plugin(oscairn_step_sequencer oscairn-step-sequencer step-sequencer/step_sequencer.cpp)
oscairn_add_plugin(oscairn_basic_synth oscairn-basic-synth basic-synth/basic_synth.cpp)
oscairn_add_plugin(oscairn_delay oscairn-delay delay/delay.cpp)
