add_library(oscairn_control STATIC src/session.cpp)
target_include_directories(oscairn_control PUBLIC include)
target_link_libraries(oscairn_control PUBLIC oscairn_engine)
target_compile_options(oscairn_control PRIVATE -Wall -Wextra)

# JSON stays off the render path and out of the typed Control interface.
add_library(oscairn_control_json STATIC src/json_rpc.cpp)
target_include_directories(oscairn_control_json SYSTEM PRIVATE third_party)
target_link_libraries(oscairn_control_json PUBLIC oscairn_control)
target_compile_options(oscairn_control_json PRIVATE -Wall -Wextra)

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
        $<TARGET_FILE:oscairn-json-rpc-test> $<TARGET_FILE:oscairn_basic_synth>)
    set_tests_properties(control-json-protocol PROPERTIES TIMEOUT 30)
endif()
