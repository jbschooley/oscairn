# Desktop UI: in-process by default, or --connect to a separate engine.
juce_add_gui_app(oscairn_standalone
    PRODUCT_NAME "Oscairn"
    COMPANY_NAME "Oscairn"
    BUNDLE_ID org.oscairn.standalone
    MICROPHONE_PERMISSION_ENABLED FALSE)

juce_add_binary_data(oscairn_standalone_layouts SOURCES
    layouts/desktop.json
    layouts/kick.json
    layouts/kick-screen.json
    layouts/ignite.json
    layouts/keys49.json)

target_sources(oscairn_standalone PRIVATE Main.cpp Engine.cpp Layout.cpp Panel.cpp Screen.cpp)
target_compile_definitions(oscairn_standalone PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:oscairn_standalone,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:oscairn_standalone,JUCE_VERSION>"
    # The demo loads the plugins from the build tree unless --plugins says otherwise.
    OSCAIRN_PLUGIN_DIR="$<TARGET_FILE_DIR:oscairn_step_sequencer>")
target_link_libraries(oscairn_standalone PRIVATE
    oscairn_audio_host
    oscairn_control_socket
    oscairn_standalone_layouts
    juce::juce_audio_utils
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
add_dependencies(oscairn_standalone oscairn_step_sequencer oscairn_basic_synth oscairn_delay)

install(TARGETS oscairn_standalone RUNTIME DESTINATION bin BUNDLE DESTINATION Applications)
