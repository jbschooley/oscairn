juce_add_console_app(oscairn_engine_host PRODUCT_NAME "oscairn-engine" NEEDS_CURL TRUE)
set_target_properties(oscairn_engine_host PROPERTIES OUTPUT_NAME "oscairn-engine")
target_sources(oscairn_engine_host PRIVATE main.cpp)
target_compile_definitions(oscairn_engine_host PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_MODAL_LOOPS_PERMITTED=1
    OSCAIRN_PLUGIN_DIR="$<TARGET_FILE_DIR:oscairn_step_sequencer>"
    OSCAIRN_PLUGIN_WORKER_PATH="$<TARGET_FILE:oscairn-plugin-worker>"
    OSCAIRN_PLUGIN_SCAN_PATH="$<TARGET_FILE:oscairn-plugin-scan>")
target_link_libraries(oscairn_engine_host PRIVATE oscairn_audio_host oscairn_host_process
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
add_dependencies(oscairn_engine_host oscairn_step_sequencer oscairn_basic_synth oscairn_delay oscairn-plugin-worker)
install(TARGETS oscairn_engine_host RUNTIME DESTINATION bin)

# The control socket served as the engine serves it, without an audio device.
juce_add_console_app(oscairn_engine_pump_test PRODUCT_NAME "Oscairn Engine Pump Test")
target_sources(oscairn_engine_pump_test PRIVATE tests/pump.cpp)
target_compile_definitions(oscairn_engine_pump_test PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
target_link_libraries(oscairn_engine_pump_test PRIVATE oscairn_audio_host oscairn_control_socket juce::juce_recommended_config_flags)
add_test(NAME engine-pump COMMAND oscairn_engine_pump_test)
set_tests_properties(engine-pump PROPERTIES TIMEOUT 30)

if(APPLE)
    # Console helpers need an embedded usage description for explicitly enabled capture.
    target_link_options(oscairn_engine_host PRIVATE "LINKER:-sectcreate,__TEXT,__info_plist,${CMAKE_CURRENT_SOURCE_DIR}/Info.plist")
    set_property(TARGET oscairn_engine_host APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist")
endif()
