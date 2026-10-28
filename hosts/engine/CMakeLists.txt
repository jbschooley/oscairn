juce_add_console_app(oscairn_engine_host PRODUCT_NAME "oscairn-engine")
set_target_properties(oscairn_engine_host PROPERTIES OUTPUT_NAME "oscairn-engine")
target_sources(oscairn_engine_host PRIVATE main.cpp)
target_compile_definitions(oscairn_engine_host PRIVATE
    JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1
    OSCAIRN_PLUGIN_DIR="$<TARGET_FILE_DIR:oscairn_step_sequencer>")
target_link_libraries(oscairn_engine_host PRIVATE oscairn_audio_host oscairn_control_socket
    juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
add_dependencies(oscairn_engine_host oscairn_step_sequencer oscairn_basic_synth oscairn_delay)
install(TARGETS oscairn_engine_host RUNTIME DESTINATION bin)
