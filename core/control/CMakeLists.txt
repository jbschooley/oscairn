add_library(oscairn_control STATIC src/session.cpp)
target_include_directories(oscairn_control PUBLIC include)
target_link_libraries(oscairn_control PUBLIC oscairn_engine)
target_compile_options(oscairn_control PRIVATE -Wall -Wextra)

add_executable(oscairn-session-test tests/session.cpp)
target_include_directories(oscairn-session-test PRIVATE ../clap-host/tests)
target_link_libraries(oscairn-session-test PRIVATE oscairn_control)
target_compile_options(oscairn-session-test PRIVATE -Wall -Wextra)
add_test(NAME control-session COMMAND oscairn-session-test
    $<TARGET_FILE:oscairn_step_sequencer> $<TARGET_FILE:oscairn_basic_synth> $<TARGET_FILE:oscairn_delay>)
set_tests_properties(control-session PROPERTIES TIMEOUT 30)
