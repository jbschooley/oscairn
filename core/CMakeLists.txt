add_subdirectory(clap-host)

# CLAP API headers for first-party plugins (vendored in clap-host).
add_library(oscairn_clap_headers INTERFACE)
target_include_directories(oscairn_clap_headers INTERFACE clap-host/third_party/clap/include)

add_library(oscairn_engine STATIC
    engine/src/chain.cpp
    engine/src/transport.cpp
    engine/src/wav_file.cpp)
target_include_directories(oscairn_engine PUBLIC engine/include)
target_link_libraries(oscairn_engine PUBLIC kick_clap_host)
target_compile_options(oscairn_engine PRIVATE -Wall -Wextra)

add_executable(oscairn-sequencer-timing-test engine/tests/sequencer_timing.cpp)
target_link_libraries(oscairn-sequencer-timing-test PRIVATE oscairn_engine)
add_test(NAME sequencer-timing COMMAND oscairn-sequencer-timing-test $<TARGET_FILE:oscairn_step_sequencer>)
