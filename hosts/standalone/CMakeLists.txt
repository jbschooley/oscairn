# Desktop UI: managed helper by default; --in-process and --connect remain available.
juce_add_gui_app(oscairn_standalone
    PRODUCT_NAME "Oscairn"
    COMPANY_NAME "Oscairn"
    BUNDLE_ID org.oscairn.standalone
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Oscairn uses selected audio inputs for live instruments, effects and routing.")

juce_add_binary_data(oscairn_standalone_layouts SOURCES
    layouts/desktop.json
    layouts/kick.json
    layouts/kick-screen.json
    layouts/ignite.json
    layouts/keys49.json)

target_sources(oscairn_standalone PRIVATE Main.cpp Engine.cpp Layout.cpp Panel.cpp Screen.cpp SoundPickerModel.cpp)
target_compile_definitions(oscairn_standalone PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:oscairn_standalone,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:oscairn_standalone,JUCE_VERSION>"
    # The demo loads the plugins from the build tree unless --plugins says otherwise.
    OSCAIRN_PLUGIN_DIR="$<TARGET_FILE_DIR:oscairn_step_sequencer>")
target_link_libraries(oscairn_standalone PRIVATE
    oscairn_audio_host
    oscairn_host_process
    oscairn_control_socket
    oscairn_standalone_layouts
    juce::juce_audio_utils
    juce::juce_gui_extra
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)
add_dependencies(oscairn_standalone oscairn_engine_host oscairn_step_sequencer oscairn_basic_synth oscairn_delay oscairn_transpose oscairn_midi_delay)
set_property(TARGET oscairn_standalone APPEND PROPERTY LINK_DEPENDS
    "$<TARGET_FILE:oscairn_engine_host>;$<TARGET_FILE:oscairn_step_sequencer>;$<TARGET_FILE:oscairn_basic_synth>;$<TARGET_FILE:oscairn_delay>;$<TARGET_FILE:oscairn_transpose>;$<TARGET_FILE:oscairn_midi_delay>")
add_custom_command(TARGET oscairn_standalone POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:oscairn_engine_host> $<TARGET_FILE_DIR:oscairn_standalone>/oscairn-engine
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:oscairn_standalone>/plugins
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:oscairn_step_sequencer> $<TARGET_FILE:oscairn_basic_synth> $<TARGET_FILE:oscairn_delay> $<TARGET_FILE:oscairn_transpose> $<TARGET_FILE:oscairn_midi_delay>
        $<TARGET_FILE_DIR:oscairn_standalone>/plugins
    VERBATIM)

install(TARGETS oscairn_standalone RUNTIME DESTINATION bin BUNDLE DESTINATION Applications)
