add_subdirectory(clap-host)

add_library(oscairn_plugin_host STATIC plugin-host/src/plugin_host.cpp)
target_include_directories(oscairn_plugin_host PUBLIC plugin-host/include)
target_link_libraries(oscairn_plugin_host PUBLIC kick_clap_host)
target_compile_options(oscairn_plugin_host PRIVATE -Wall -Wextra)
if(OSCAIRN_LV2)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LILV REQUIRED IMPORTED_TARGET lilv-0>=0.24)
    find_package(Threads REQUIRED)
    target_sources(oscairn_plugin_host PRIVATE lv2-host/Host.cpp lv2-host/Worker.cpp)
    target_link_libraries(oscairn_plugin_host PRIVATE PkgConfig::LILV Threads::Threads)
    target_compile_definitions(oscairn_plugin_host PRIVATE OSCAIRN_LV2=1)
    install(FILES lv2-host/third_party/jalv/ISC.txt DESTINATION share/licenses/oscairn RENAME jalv-worker-ISC.txt)
endif()

# CLAP API headers for first-party plugins (vendored in clap-host).
add_library(oscairn_clap_headers INTERFACE)
target_include_directories(oscairn_clap_headers INTERFACE clap-host/third_party/clap/include clap-ext/include)

add_library(oscairn_engine STATIC
    engine/src/chain.cpp
    engine/src/graph.cpp
    engine/src/midi_input.cpp
    engine/src/transport.cpp
    engine/src/wav_file.cpp)
target_include_directories(oscairn_engine PUBLIC engine/include)
target_link_libraries(oscairn_engine PUBLIC oscairn_plugin_host)
target_compile_options(oscairn_engine PRIVATE -Wall -Wextra)

add_subdirectory(control)

add_executable(oscairn-sequencer-timing-test engine/tests/sequencer_timing.cpp)
target_link_libraries(oscairn-sequencer-timing-test PRIVATE oscairn_engine)
add_test(NAME sequencer-timing COMMAND oscairn-sequencer-timing-test $<TARGET_FILE:oscairn_step_sequencer>)

add_executable(oscairn-host-policy-test engine/tests/host_policy.cpp)
target_link_libraries(oscairn-host-policy-test PRIVATE kick_clap_host)
add_test(NAME host-policy COMMAND oscairn-host-policy-test $<TARGET_FILE:oscairn_basic_synth>)

add_executable(oscairn-chain-mono-test engine/tests/chain_mono.cpp)
target_link_libraries(oscairn-chain-mono-test PRIVATE oscairn_engine)
add_test(NAME chain-mono COMMAND oscairn-chain-mono-test $<TARGET_FILE:kick-clap-fixture-normal>)

add_executable(oscairn-midi-clock-test engine/tests/midi_clock.cpp)
target_link_libraries(oscairn-midi-clock-test PRIVATE oscairn_engine)
add_test(NAME midi-clock COMMAND oscairn-midi-clock-test $<TARGET_FILE:oscairn_step_sequencer>)

add_executable(oscairn-graph-test engine/tests/graph_test.cpp)
target_link_libraries(oscairn-graph-test PRIVATE oscairn_engine)
add_test(NAME graph COMMAND oscairn-graph-test $<TARGET_FILE:oscairn_step_sequencer> $<TARGET_FILE:oscairn_basic_synth>
    $<TARGET_FILE:oscairn_delay> $<TARGET_FILE:kick-clap-fixture-normal>)

add_executable(oscairn-param-target-test engine/tests/param_target.cpp)
target_link_libraries(oscairn-param-target-test PRIVATE oscairn_engine)
add_test(NAME param-target COMMAND oscairn-param-target-test $<TARGET_FILE:oscairn_step_sequencer> $<TARGET_FILE:kick-clap-fixture-normal>)

add_executable(oscairn-sequencer-params-test engine/tests/sequencer_params.cpp)
target_link_libraries(oscairn-sequencer-params-test PRIVATE oscairn_engine)
add_test(NAME sequencer-params COMMAND oscairn-sequencer-params-test $<TARGET_FILE:oscairn_step_sequencer>)

add_executable(oscairn-transport-tempo-test engine/tests/transport_tempo.cpp)
target_link_libraries(oscairn-transport-tempo-test PRIVATE oscairn_engine)
add_test(NAME transport-tempo COMMAND oscairn-transport-tempo-test)

add_executable(oscairn-delay-glide-test engine/tests/delay_glide.cpp)
target_link_libraries(oscairn-delay-glide-test PRIVATE oscairn_engine)
add_test(NAME delay-glide COMMAND oscairn-delay-glide-test $<TARGET_FILE:oscairn_delay>)

add_executable(oscairn-midi-input-test engine/tests/midi_input.cpp)
target_link_libraries(oscairn-midi-input-test PRIVATE oscairn_engine)
add_test(NAME midi-input COMMAND oscairn-midi-input-test)

add_executable(oscairn-pressure-test engine/tests/pressure.cpp)
target_link_libraries(oscairn-pressure-test PRIVATE oscairn_engine)
add_test(NAME pressure COMMAND oscairn-pressure-test $<TARGET_FILE:oscairn_basic_synth>)

if(OSCAIRN_LV2)
    add_subdirectory(lv2-host/tests)
endif()
