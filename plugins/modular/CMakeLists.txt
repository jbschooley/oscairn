# Modular: chillbeast's patchable instrument engine (upstream/, unchanged) as an Oscairn
# CLAP plugin. Its engine and patch code use JUCE, so this component links JUCE's core,
# audio-basics and data-structures modules.
if(NOT TARGET juce::juce_core)
    include(FetchContent)
    FetchContent_Declare(JUCE GIT_REPOSITORY https://github.com/juce-framework/JUCE.git GIT_TAG 9.0.4 GIT_SHALLOW ON)
    FetchContent_MakeAvailable(JUCE)
endif()

set(upstream_sources upstream/modular/mt_modular_engine.cpp upstream/modular/mt_modular_modules.cpp upstream/modular/mt_modular_patch.cpp)
oscairn_add_plugin(oscairn_modular oscairn-modular src/modular.cpp)
target_sources(oscairn_modular PRIVATE src/compat.cpp ${upstream_sources})
# The upstream files expect KICK's module headers to be included already.
set_source_files_properties(${upstream_sources} PROPERTIES COMPILE_OPTIONS "-include;${CMAKE_CURRENT_SOURCE_DIR}/src/prefix.h;-w")
target_link_libraries(oscairn_modular PRIVATE juce::juce_core juce::juce_audio_basics juce::juce_data_structures)
target_compile_definitions(oscairn_modular PRIVATE JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 JUCE_STANDALONE_APPLICATION=0
    JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_MODAL_LOOPS_PERMITTED=0 JUCE_DISPLAY_SPLASH_SCREEN=0)

add_executable(oscairn-modular-test tests/modular.cpp)
target_link_libraries(oscairn-modular-test PRIVATE oscairn_plugin_host)
target_include_directories(oscairn-modular-test PRIVATE ${PROJECT_SOURCE_DIR}/core/clap-host/tests)
add_test(NAME modular COMMAND oscairn-modular-test $<TARGET_FILE:oscairn_modular>)
set_tests_properties(modular PROPERTIES TIMEOUT 60)
