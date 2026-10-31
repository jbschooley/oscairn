add_library(oscairn_control STATIC src/session.cpp src/performance.cpp src/parts.cpp src/pads.cpp src/undo.cpp)
target_include_directories(oscairn_control SYSTEM PRIVATE third_party)
target_include_directories(oscairn_control PUBLIC include)
target_link_libraries(oscairn_control PUBLIC oscairn_engine)
target_compile_options(oscairn_control PRIVATE -Wall -Wextra)

# JSON stays off the render path and out of the typed Control interface.
add_library(oscairn_control_json STATIC src/json_rpc.cpp)
target_include_directories(oscairn_control_json SYSTEM PRIVATE third_party)
target_link_libraries(oscairn_control_json PUBLIC oscairn_control)
target_compile_options(oscairn_control_json PRIVATE -Wall -Wextra)

if(UNIX)
    add_library(oscairn_control_socket STATIC src/local_socket.cpp src/remote_control.cpp)
    target_include_directories(oscairn_control_socket SYSTEM PRIVATE third_party)
    target_link_libraries(oscairn_control_socket PUBLIC oscairn_control_json)
    target_compile_options(oscairn_control_socket PRIVATE -Wall -Wextra)
endif()

add_executable(oscairn-session-test tests/session.cpp)
target_include_directories(oscairn-session-test PRIVATE ../clap-host/tests)
target_link_libraries(oscairn-session-test PRIVATE oscairn_control)
target_compile_options(oscairn-session-test PRIVATE -Wall -Wextra)
add_test(NAME control-session COMMAND oscairn-session-test
    $<TARGET_FILE:oscairn_step_sequencer> $<TARGET_FILE:oscairn_basic_synth> $<TARGET_FILE:oscairn_delay>)
set_tests_properties(control-session PROPERTIES TIMEOUT 30)

add_executable(oscairn-json-rpc-test tests/json_rpc.cpp)
target_include_directories(oscairn-json-rpc-test PRIVATE ../clap-host/tests)
target_link_libraries(oscairn-json-rpc-test PRIVATE oscairn_control_json)
target_compile_options(oscairn-json-rpc-test PRIVATE -Wall -Wextra)
add_test(NAME control-json-audio COMMAND oscairn-json-rpc-test $<TARGET_FILE:oscairn_basic_synth>)
set_tests_properties(control-json-audio PROPERTIES TIMEOUT 30)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME control-json-protocol COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/json_rpc.py"
        $<TARGET_FILE:oscairn-json-rpc-test> $<TARGET_FILE:oscairn_basic_synth> $<TARGET_FILE:oscairn_delay>)
    set_tests_properties(control-json-protocol PROPERTIES TIMEOUT 30)
endif()

add_library(oscairn-control-fixture MODULE tests/control_fixture.cpp)
target_include_directories(oscairn-control-fixture PRIVATE ../clap-host/third_party/clap/include)
set_target_properties(oscairn-control-fixture PROPERTIES PREFIX "" SUFFIX ".clap")
add_executable(oscairn-control-flow-test tests/control_flow.cpp)
target_include_directories(oscairn-control-flow-test PRIVATE ../clap-host/tests)
target_link_libraries(oscairn-control-flow-test PRIVATE oscairn_control_json)
target_compile_options(oscairn-control-flow-test PRIVATE -Wall -Wextra)
add_test(NAME control-flow COMMAND oscairn-control-flow-test $<TARGET_FILE:oscairn-control-fixture>)
set_tests_properties(control-flow PROPERTIES TIMEOUT 30)

if(UNIX)
    add_executable(oscairn-local-socket-test tests/local_socket.cpp)
    target_include_directories(oscairn-local-socket-test PRIVATE ../clap-host/tests)
    target_link_libraries(oscairn-local-socket-test PRIVATE oscairn_control_socket)
    target_compile_options(oscairn-local-socket-test PRIVATE -Wall -Wextra)
    add_test(NAME control-local-stream COMMAND oscairn-local-socket-test $<TARGET_FILE:oscairn-control-fixture>)
    add_test(NAME control-local-path-policy COMMAND oscairn-local-socket-test $<TARGET_FILE:oscairn-control-fixture> --path-policy)
    add_test(NAME control-local-path COMMAND oscairn-local-socket-test $<TARGET_FILE:oscairn-control-fixture> --named)
    set_tests_properties(control-local-stream control-local-path-policy control-local-path PROPERTIES TIMEOUT 30)
    set_tests_properties(control-local-path PROPERTIES SKIP_RETURN_CODE 77)
endif()

if(UNIX)
    add_test(NAME control-prepared-instances COMMAND oscairn-local-socket-test $<TARGET_FILE:kick-clap-fixture-normal> --instances)
    set_tests_properties(control-prepared-instances PROPERTIES TIMEOUT 30)
    add_test(NAME control-native-sounds COMMAND oscairn-local-socket-test $<TARGET_FILE:oscairn_basic_synth> --sounds)
    set_tests_properties(control-native-sounds PROPERTIES TIMEOUT 30)
endif()

add_executable(oscairn-sound-management-test tests/sound_management.cpp)
target_link_libraries(oscairn-sound-management-test PRIVATE oscairn_control_json)
target_include_directories(oscairn-sound-management-test SYSTEM PRIVATE third_party)
add_test(NAME control-sound-management COMMAND oscairn-sound-management-test)
if(OSCAIRN_LV2 AND OSCAIRN_LV2_SOURCE_DIR)
    add_test(NAME control-sound-management-lv2 COMMAND oscairn-sound-management-test
        "${PROJECT_BINARY_DIR}/core/lv2-host/tests/upstream sampler.lv2" "${PROJECT_BINARY_DIR}/core/lv2-host/tests/test fixture.lv2")
    set_tests_properties(control-sound-management-lv2 PROPERTIES TIMEOUT 30)
endif()

if(UNIX AND OSCAIRN_LV2 AND OSCAIRN_LV2_SOURCE_DIR)
    add_test(NAME control-remote-properties COMMAND oscairn-local-socket-test
        "${PROJECT_BINARY_DIR}/core/lv2-host/tests/upstream sampler.lv2" --properties)
    set_tests_properties(control-remote-properties PROPERTIES TIMEOUT 30)
endif()

if(UNIX)
    add_test(NAME control-native-effects COMMAND oscairn-local-socket-test $<TARGET_FILE:oscairn_basic_synth> --effects $<TARGET_FILE:oscairn_delay>)
    set_tests_properties(control-native-effects PROPERTIES TIMEOUT 30)
    add_test(NAME control-remote-parts COMMAND oscairn-local-socket-test $<TARGET_FILE:oscairn_basic_synth> --parts $<TARGET_FILE:oscairn_delay>)
    set_tests_properties(control-remote-parts PROPERTIES TIMEOUT 30)
endif()

add_executable(oscairn-parts-test tests/parts.cpp)
target_include_directories(oscairn-parts-test PRIVATE ../clap-host/tests)
target_link_libraries(oscairn-parts-test PRIVATE oscairn_control)
add_test(NAME control-parts COMMAND oscairn-parts-test
    $<TARGET_FILE:kick-clap-fixture-normal> $<TARGET_FILE:oscairn-audio-fixture> $<TARGET_FILE:oscairn_delay> $<TARGET_FILE:oscairn_splitters>)
set_tests_properties(control-parts PROPERTIES TIMEOUT 60)

add_executable(oscairn-plugin-failures-test tests/plugin_failures.cpp)
target_link_libraries(oscairn-plugin-failures-test PRIVATE oscairn_control_json)
add_test(NAME control-plugin-failures COMMAND oscairn-plugin-failures-test $<TARGET_FILE:oscairn-plugin-worker> $<TARGET_FILE:kick-clap-fixture-process_crash>)
set_tests_properties(control-plugin-failures PROPERTIES TIMEOUT 60)

add_executable(oscairn-undo-test tests/undo.cpp)
target_include_directories(oscairn-undo-test PRIVATE ../clap-host/tests)
target_link_libraries(oscairn-undo-test PRIVATE oscairn_control)
add_test(NAME control-undo COMMAND oscairn-undo-test $<TARGET_FILE:kick-clap-fixture-normal> $<TARGET_FILE:oscairn_splitters>)
set_tests_properties(control-undo PROPERTIES TIMEOUT 60)

add_executable(oscairn-pads-test tests/pads.cpp)
target_include_directories(oscairn-pads-test PRIVATE ../clap-host/tests)
target_link_libraries(oscairn-pads-test PRIVATE oscairn_control)
add_test(NAME control-pads COMMAND oscairn-pads-test $<TARGET_FILE:kick-clap-fixture-normal> $<TARGET_FILE:oscairn_basic_synth>)

add_executable(oscairn-performance-test tests/performance.cpp)
target_include_directories(oscairn-performance-test PRIVATE ../clap-host/tests)
target_link_libraries(oscairn-performance-test PRIVATE oscairn_control)
add_test(NAME control-performance COMMAND oscairn-performance-test
    $<TARGET_FILE:oscairn_basic_synth> $<TARGET_FILE:oscairn_delay> $<TARGET_FILE:oscairn_step_sequencer> $<TARGET_FILE:kick-clap-fixture-normal>)
set_tests_properties(control-performance PROPERTIES TIMEOUT 60)
if(Python3_Interpreter_FOUND)
    add_test(NAME control-performance-format COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/performance.py"
        $<TARGET_FILE:oscairn-json-rpc-test> $<TARGET_FILE:oscairn_basic_synth>)
    set_tests_properties(control-performance-format PROPERTIES TIMEOUT 30)
endif()

# Sessions, native sounds and effects and Performances again with every CLAP plugin in a worker (RFC 0010 step 5).
set(control_worker_tests
    "control-json-audio|oscairn-json-rpc-test|$<TARGET_FILE:oscairn_basic_synth>"
    "control-performance|oscairn-performance-test|$<TARGET_FILE:oscairn_basic_synth>|$<TARGET_FILE:oscairn_delay>|$<TARGET_FILE:oscairn_step_sequencer>|$<TARGET_FILE:kick-clap-fixture-normal>")
if(UNIX)
    list(APPEND control_worker_tests
        "control-native-sounds|oscairn-local-socket-test|$<TARGET_FILE:oscairn_basic_synth>|--sounds"
        "control-native-effects|oscairn-local-socket-test|$<TARGET_FILE:oscairn_basic_synth>|--effects|$<TARGET_FILE:oscairn_delay>")
endif()
foreach(entry IN LISTS control_worker_tests)
    string(REPLACE "|" ";" parts "${entry}")
    list(POP_FRONT parts name)
    add_test(NAME ${name}-workers COMMAND ${parts})
    set_tests_properties(${name}-workers PROPERTIES TIMEOUT 120 ENVIRONMENT "OSCAIRN_PLUGIN_WORKER=$<TARGET_FILE:oscairn-plugin-worker>;OSCAIRN_WORKER_BUDGET_US=50000")
endforeach()
