set(OSCAIRN_AUDIO_BUFFER_FRAMES "128" CACHE STRING "Requested live audio buffer frames (0 uses device default)")
if(NOT OSCAIRN_AUDIO_BUFFER_FRAMES MATCHES "^(0|[1-9][0-9]*)$" OR OSCAIRN_AUDIO_BUFFER_FRAMES GREATER 1048576)
    message(FATAL_ERROR "OSCAIRN_AUDIO_BUFFER_FRAMES must be an integer from 0 to 1048576")
endif()

add_library(oscairn_audio_host STATIC AudioEngine.cpp WebTransport.cpp)
target_compile_definitions(oscairn_audio_host PUBLIC JUCE_WEB_BROWSER=0)
target_compile_definitions(oscairn_audio_host PRIVATE OSCAIRN_AUDIO_BUFFER_FRAMES=${OSCAIRN_AUDIO_BUFFER_FRAMES})
target_link_libraries(oscairn_audio_host PUBLIC oscairn_control juce::juce_audio_devices)
target_compile_options(oscairn_audio_host PRIVATE -Wall -Wextra)
# HTTPS on Linux through libcurl (JUCE_USE_CURL, on by default); macOS uses the system's. The apps
# that fetch from repositories say NEEDS_CURL, and JUCE builds the rest without it.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(oscairn_audio_host PRIVATE juce::pkgconfig_JUCE_CURL_LINUX_DEPS)
endif()

# A control session on stdin and stdout that fetches from the web as the engine does, for
# installing from a repository served over HTTP (RFC 0031 step 2).
juce_add_console_app(oscairn_web_rpc_test PRODUCT_NAME "oscairn-web-rpc-test" NEEDS_CURL TRUE)
target_sources(oscairn_web_rpc_test PRIVATE web_rpc_test.cpp)
target_compile_definitions(oscairn_web_rpc_test PRIVATE JUCE_WEB_BROWSER=0)
target_link_libraries(oscairn_web_rpc_test PRIVATE oscairn_audio_host oscairn_control_json juce::juce_recommended_config_flags)
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME control-repository-web COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/core/control/tests/repository.py" $<TARGET_FILE:oscairn-json-rpc-test> $<TARGET_FILE:oscairn_basic_synth>
        $<TARGET_FILE:oscairn-sign> $<TARGET_FILE:kick-clap-fixture-normal> "${PROJECT_SOURCE_DIR}/tools/package-repository/template/scripts/build-index.py" $<TARGET_FILE:oscairn_web_rpc_test>)
    set_tests_properties(control-repository-web PROPERTIES TIMEOUT 180)
endif()
