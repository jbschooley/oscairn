add_subdirectory(clap-host)

# CLAP API headers for first-party plugins (vendored in clap-host).
add_library(oscairn_clap_headers INTERFACE)
target_include_directories(oscairn_clap_headers INTERFACE clap-host/third_party/clap/include clap-ext/include)

add_library(oscairn_engine STATIC
    engine/src/chain.cpp
    engine/src/graph.cpp
    engine/src/transport.cpp
    engine/src/wav_file.cpp)
target_include_directories(oscairn_engine PUBLIC engine/include)
target_link_libraries(oscairn_engine PUBLIC kick_clap_host)
target_compile_options(oscairn_engine PRIVATE -Wall -Wextra)

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
