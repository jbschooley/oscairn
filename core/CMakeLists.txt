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
    engine/src/PreparedInstrument.cpp
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

add_library(oscairn-audio-fixture MODULE engine/tests/audio_fixture.cpp)
target_link_libraries(oscairn-audio-fixture PRIVATE oscairn_clap_headers)
set_target_properties(oscairn-audio-fixture PROPERTIES PREFIX "" SUFFIX ".clap")
add_executable(oscairn-audio-routing-test engine/tests/audio_routing.cpp)
target_link_libraries(oscairn-audio-routing-test PRIVATE oscairn_control_json)
target_include_directories(oscairn-audio-routing-test PRIVATE clap-host/tests)
add_dependencies(oscairn-audio-routing-test oscairn-audio-fixture)
add_test(NAME audio-routing COMMAND oscairn-audio-routing-test $<TARGET_FILE:oscairn-audio-fixture>)
set_tests_properties(audio-routing PROPERTIES TIMEOUT 30)
if(OSCAIRN_LV2)
    add_dependencies(oscairn-audio-routing-test oscairn-lv2-fixture)
    add_test(NAME audio-routing-lv2 COMMAND oscairn-audio-routing-test $<TARGET_FILE:oscairn-audio-fixture> "${CMAKE_CURRENT_BINARY_DIR}/lv2-host/tests/test fixture.lv2")
    set_tests_properties(audio-routing-lv2 PROPERTIES TIMEOUT 30)
endif()

add_executable(oscairn-channel-map-test ../hosts/audio/channel_map_test.cpp)
target_link_libraries(oscairn-channel-map-test PRIVATE oscairn_engine)
target_include_directories(oscairn-channel-map-test PRIVATE clap-host/tests)
add_test(NAME audio-device-channel-map COMMAND oscairn-channel-map-test)

add_library(oscairn-lifecycle-fixture MODULE engine/tests/lifecycle_fixture.cpp)
target_link_libraries(oscairn-lifecycle-fixture PRIVATE oscairn_clap_headers)
set_target_properties(oscairn-lifecycle-fixture PROPERTIES PREFIX "" SUFFIX ".clap")
add_executable(oscairn-graph-lifecycle-test engine/tests/graph_lifecycle.cpp)
target_link_libraries(oscairn-graph-lifecycle-test PRIVATE oscairn_control)
target_include_directories(oscairn-graph-lifecycle-test PRIVATE clap-host/tests)
add_dependencies(oscairn-graph-lifecycle-test oscairn-lifecycle-fixture)
add_test(NAME graph-lifecycle COMMAND oscairn-graph-lifecycle-test $<TARGET_FILE:oscairn-lifecycle-fixture>)
set_tests_properties(graph-lifecycle PROPERTIES TIMEOUT 30)

add_executable(oscairn-sound-contexts-test engine/tests/sound_contexts.cpp)
target_link_libraries(oscairn-sound-contexts-test PRIVATE oscairn_engine)
target_include_directories(oscairn-sound-contexts-test PRIVATE clap-host/tests)
add_dependencies(oscairn-sound-contexts-test oscairn_basic_synth)
add_test(NAME sound-contexts COMMAND oscairn-sound-contexts-test $<TARGET_FILE:oscairn_basic_synth>)
set_tests_properties(sound-contexts PROPERTIES TIMEOUT 30)

add_executable(oscairn-note-routing-test engine/tests/note_routing.cpp)
target_link_libraries(oscairn-note-routing-test PRIVATE oscairn_engine)
target_include_directories(oscairn-note-routing-test PRIVATE clap-host/tests)
add_test(NAME note-routing COMMAND oscairn-note-routing-test $<TARGET_FILE:oscairn_basic_synth> $<TARGET_FILE:oscairn-lifecycle-fixture> $<TARGET_FILE:kick-clap-fixture-normal>)
set_tests_properties(note-routing PROPERTIES TIMEOUT 30)

add_executable(oscairn-write-failure-test engine/tests/write_failure.cpp)
target_link_libraries(oscairn-write-failure-test PRIVATE oscairn_engine)
add_test(NAME write-failure COMMAND oscairn-write-failure-test)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME session-service-stop COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/os/scripts/test_session_stop.py")
endif()

add_executable(oscairn-prepared-instances-test engine/tests/prepared_instances.cpp)
target_link_libraries(oscairn-prepared-instances-test PRIVATE oscairn_control_json)
target_include_directories(oscairn-prepared-instances-test PRIVATE clap-host/tests)
add_test(NAME prepared-instances COMMAND oscairn-prepared-instances-test $<TARGET_FILE:kick-clap-fixture-normal>)
set_tests_properties(prepared-instances PROPERTIES TIMEOUT 45)
if(OSCAIRN_LV2)
    add_test(NAME prepared-instances-lv2 COMMAND oscairn-prepared-instances-test $<TARGET_FILE:kick-clap-fixture-normal> "${CMAKE_CURRENT_BINARY_DIR}/lv2-host/tests/test fixture.lv2")
    set_tests_properties(prepared-instances-lv2 PROPERTIES TIMEOUT 45)
endif()

add_library(oscairn-audio-midi-fixture MODULE engine/tests/audio_to_midi_fixture.cpp)
target_link_libraries(oscairn-audio-midi-fixture PRIVATE oscairn_clap_headers)
set_target_properties(oscairn-audio-midi-fixture PROPERTIES PREFIX "" SUFFIX ".clap")
add_executable(oscairn-midi-effects-test engine/tests/midi_effects.cpp)
target_link_libraries(oscairn-midi-effects-test PRIVATE oscairn_engine)
target_include_directories(oscairn-midi-effects-test PRIVATE clap-host/tests)
add_test(NAME midi-effects COMMAND oscairn-midi-effects-test $<TARGET_FILE:oscairn_transpose> $<TARGET_FILE:oscairn_midi_delay> $<TARGET_FILE:oscairn_basic_synth> $<TARGET_FILE:oscairn-audio-midi-fixture>)
set_tests_properties(midi-effects PROPERTIES TIMEOUT 30)
