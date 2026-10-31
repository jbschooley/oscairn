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

target_sources(oscairn_standalone PRIVATE Main.cpp Engine.cpp Layout.cpp Panel.cpp Screen.cpp SoundPickerModel.cpp Theme.cpp PluginBrowser.cpp PadEditor.cpp PartsView.cpp PartsModel.cpp)
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
set(bundled_plugins oscairn_step_sequencer oscairn_basic_synth oscairn_delay oscairn_transpose oscairn_midi_delay oscairn_sampler oscairn_splitters)
if(TARGET oscairn_modular)
    list(APPEND bundled_plugins oscairn_modular)
endif()
set(bundled_plugin_files "")
foreach(plugin IN LISTS bundled_plugins)
    list(APPEND bundled_plugin_files "$<TARGET_FILE:${plugin}>")
endforeach()
add_dependencies(oscairn_standalone oscairn-plugin-scan oscairn_engine_host ${bundled_plugins})
set_property(TARGET oscairn_standalone APPEND PROPERTY LINK_DEPENDS
    "$<TARGET_FILE:oscairn-plugin-scan>;$<TARGET_FILE:oscairn_engine_host>;${bundled_plugin_files}")
add_custom_command(TARGET oscairn_standalone POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:oscairn-plugin-scan> $<TARGET_FILE_DIR:oscairn_standalone>/oscairn-plugin-scan
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:oscairn_engine_host> $<TARGET_FILE_DIR:oscairn_standalone>/oscairn-engine
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:oscairn_standalone>/plugins
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/themes $<TARGET_FILE_DIR:oscairn_standalone>/themes
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${bundled_plugin_files}
        $<TARGET_FILE_DIR:oscairn_standalone>/plugins
    VERBATIM)

install(TARGETS oscairn_standalone RUNTIME DESTINATION bin BUNDLE DESTINATION Applications)
# Built-in themes (with their fonts) are ordinary theme folders; Linux finds them under share/.
if(NOT APPLE)
    install(DIRECTORY themes/ DESTINATION share/oscairn/themes)
endif()

juce_add_console_app(oscairn_theme_test PRODUCT_NAME "Oscairn Theme Test")
target_sources(oscairn_theme_test PRIVATE tests/theme.cpp Theme.cpp)
target_link_libraries(oscairn_theme_test PRIVATE juce::juce_gui_extra juce::juce_recommended_config_flags)
target_compile_definitions(oscairn_theme_test PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
add_test(NAME desktop-theme COMMAND oscairn_theme_test "${CMAKE_CURRENT_SOURCE_DIR}/themes" "${CMAKE_CURRENT_SOURCE_DIR}/theme-examples/midnight")

# The pad editor against a session served on a socket, without an audio device.
juce_add_console_app(oscairn_pad_editor_test PRODUCT_NAME "Oscairn Pad Editor Test")
target_sources(oscairn_pad_editor_test PRIVATE tests/pad_editor.cpp PadEditor.cpp PartsView.cpp PartsModel.cpp Engine.cpp Theme.cpp SoundPickerModel.cpp)
target_link_libraries(oscairn_pad_editor_test PRIVATE oscairn_audio_host oscairn_host_process oscairn_control_socket juce::juce_audio_utils juce::juce_gui_extra juce::juce_recommended_config_flags)
target_compile_definitions(oscairn_pad_editor_test PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
add_dependencies(oscairn_pad_editor_test oscairn_step_sequencer oscairn_basic_synth oscairn_delay oscairn_splitters oscairn-plugin-scan)
add_test(NAME desktop-pad-editor COMMAND oscairn_pad_editor_test "$<TARGET_FILE_DIR:oscairn_step_sequencer>" $<TARGET_FILE:oscairn-plugin-scan>)
set_tests_properties(desktop-pad-editor PROPERTIES TIMEOUT 60)
