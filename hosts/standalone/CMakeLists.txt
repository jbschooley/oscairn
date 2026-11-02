# Desktop UI: managed helper by default; --in-process and --connect remain available.
juce_add_gui_app(oscairn_standalone
    PRODUCT_NAME "Oscairn"
    COMPANY_NAME "Oscairn"
    BUNDLE_ID org.oscairn.standalone
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/../../branding/app-icon-1024.png"
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/../../branding/app-icon-1024.png"
    MICROPHONE_PERMISSION_ENABLED TRUE
    MICROPHONE_PERMISSION_TEXT "Oscairn uses selected audio inputs for live instruments, effects and routing.")

juce_add_binary_data(oscairn_standalone_layouts SOURCES
    layouts/desktop.json
    layouts/kick.json
    layouts/kick-screen.json
    layouts/modx-m-screen.json
    layouts/ignite.json
    layouts/keys49.json
    layouts/compact.json)
# Device profiles (RFC 0024), apart from the layouts: some share their names.
juce_add_binary_data(oscairn_standalone_profiles HEADER_NAME ProfileData.h NAMESPACE ProfileData SOURCES
    profiles/qemu-aarch64.json
    profiles/kick.json
    profiles/modx-m.json
    profiles/ignite.json
    profiles/smk37-elite.json
    profiles/modx-m-usb.json)

# Icons (Lucide, ISC: icons/LICENSE).
juce_add_binary_data(oscairn_standalone_icons HEADER_NAME IconData.h NAMESPACE IconData SOURCES
    icons/plus.svg
    icons/undo-2.svg
    icons/redo-2.svg
    icons/square-arrow-out-up-right.svg
    icons/list.svg
    icons/tags.svg
    icons/star.svg
    icons/chevron-down.svg
    icons/chevron-right.svg
    icons/check.svg
    icons/house.svg
    icons/arrow-left.svg
    icons/piano.svg
    icons/library-big.svg
    icons/list-music.svg
    icons/layout-grid.svg
    icons/octagon-x.svg
    icons/ellipsis-vertical.svg
    icons/layers.svg
    # Oscairn's mark (branding/README.md) and its alternate; Icons.h names the one shown.
    ../../branding/mark-on-dark.svg
    ../../branding/mark-on-light.svg
    ../../branding/mark-mono.svg
    ../../branding/mark-hills-on-dark.svg
    ../../branding/mark-hills-on-light.svg
    ../../branding/mark-hills-mono.svg)

target_sources(oscairn_standalone PRIVATE Main.cpp DesktopShell.cpp ParameterGrid.cpp CompactEditor.cpp ZoneEditor.cpp Engine.cpp Layout.cpp Panel.cpp Screen.cpp SoundPickerModel.cpp Theme.cpp PluginBrowser.cpp PadEditor.cpp PartsView.cpp PartsModel.cpp LibraryPane.cpp Setlist.cpp Profile.cpp Launcher.cpp PlayView.cpp Controllers.cpp Splash.cpp)
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
    oscairn_standalone_profiles
    oscairn_standalone_icons
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
add_dependencies(oscairn_standalone oscairn-plugin-scan oscairn_engine_host oscairn-plugin-worker ${bundled_plugins})
set_property(TARGET oscairn_standalone APPEND PROPERTY LINK_DEPENDS
    "$<TARGET_FILE:oscairn-plugin-scan>;$<TARGET_FILE:oscairn_engine_host>;$<TARGET_FILE:oscairn-plugin-worker>;${bundled_plugin_files}")
add_custom_command(TARGET oscairn_standalone POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:oscairn-plugin-scan> $<TARGET_FILE_DIR:oscairn_standalone>/oscairn-plugin-scan
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:oscairn_engine_host> $<TARGET_FILE_DIR:oscairn_standalone>/oscairn-engine
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:oscairn-plugin-worker> $<TARGET_FILE_DIR:oscairn_standalone>/oscairn-plugin-worker
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
target_sources(oscairn_theme_test PRIVATE tests/theme.cpp Theme.cpp Splash.cpp)
target_link_libraries(oscairn_theme_test PRIVATE oscairn_standalone_icons juce::juce_gui_extra juce::juce_recommended_config_flags)
target_compile_definitions(oscairn_theme_test PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
add_test(NAME desktop-theme COMMAND oscairn_theme_test "${CMAKE_CURRENT_SOURCE_DIR}/themes" "${CMAKE_CURRENT_SOURCE_DIR}/theme-examples/midnight")

juce_add_console_app(oscairn_profile_test PRODUCT_NAME "Oscairn Profile Test")
target_sources(oscairn_profile_test PRIVATE tests/profile.cpp Profile.cpp)
target_link_libraries(oscairn_profile_test PRIVATE oscairn_standalone_profiles juce::juce_gui_extra juce::juce_recommended_config_flags)
target_compile_definitions(oscairn_profile_test PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
add_test(NAME desktop-profile COMMAND oscairn_profile_test)

# The pad editor against a session served on a socket, without an audio device.
juce_add_console_app(oscairn_pad_editor_test PRODUCT_NAME "Oscairn Pad Editor Test")
target_sources(oscairn_pad_editor_test PRIVATE tests/pad_editor.cpp PadEditor.cpp PartsView.cpp ParameterGrid.cpp ZoneEditor.cpp PartsModel.cpp Engine.cpp Theme.cpp SoundPickerModel.cpp)
target_link_libraries(oscairn_pad_editor_test PRIVATE oscairn_standalone_icons oscairn_audio_host oscairn_host_process oscairn_control_socket juce::juce_audio_utils juce::juce_gui_extra juce::juce_recommended_config_flags)
target_compile_definitions(oscairn_pad_editor_test PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
add_dependencies(oscairn_pad_editor_test oscairn_step_sequencer oscairn_basic_synth oscairn_delay oscairn_splitters oscairn-plugin-scan oscairn-plugin-worker)
juce_add_console_app(oscairn_desktop_shell_test PRODUCT_NAME "Oscairn Desktop Shell Test")
target_sources(oscairn_desktop_shell_test PRIVATE tests/desktop_shell.cpp DesktopShell.cpp PlayView.cpp Profile.cpp LibraryPane.cpp Launcher.cpp Setlist.cpp ParameterGrid.cpp CompactEditor.cpp ZoneEditor.cpp Screen.cpp Panel.cpp Layout.cpp PadEditor.cpp PartsView.cpp PartsModel.cpp Engine.cpp Theme.cpp SoundPickerModel.cpp)
target_link_libraries(oscairn_desktop_shell_test PRIVATE oscairn_standalone_icons oscairn_standalone_profiles oscairn_audio_host oscairn_host_process oscairn_control_socket juce::juce_audio_utils juce::juce_gui_extra juce::juce_recommended_config_flags)
target_compile_definitions(oscairn_desktop_shell_test PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
add_dependencies(oscairn_desktop_shell_test oscairn_step_sequencer oscairn_basic_synth oscairn_delay oscairn_transpose oscairn_sampler oscairn_splitters oscairn-plugin-scan kick-clap-fixture-normal oscairn-plugin-worker)
add_test(NAME desktop-shell COMMAND oscairn_desktop_shell_test "$<TARGET_FILE_DIR:oscairn_step_sequencer>" $<TARGET_FILE:oscairn-plugin-scan> "${CMAKE_CURRENT_SOURCE_DIR}/themes")
set_tests_properties(desktop-shell PROPERTIES TIMEOUT 60)
# A third-party plugin for RFC 0019's Compact gate, used where it is built (Odin 2 beside this
# checkout by default); the test skips it otherwise.
set(OSCAIRN_THIRD_PARTY_CLAP "${PROJECT_SOURCE_DIR}/../ports/odin2/build/Odin2_artefacts/Release/CLAP/Odin2.clap" CACHE FILEPATH "A third-party CLAP plugin for the desktop-shell test")
# The app runs every plugin in a worker, so its tests do too (the budget allows for these
# threads' ordinary scheduling, as in the other worker tests).
set(worker_environment "OSCAIRN_PLUGIN_WORKER=$<TARGET_FILE:oscairn-plugin-worker>" "OSCAIRN_WORKER_BUDGET_US=50000")
# The test fixture's stepped plugin, whose mode hides a control, for editors that follow it.
set(shell_environment "OSCAIRN_TEST_FIXTURE=$<TARGET_FILE:kick-clap-fixture-normal>" ${worker_environment})
if(EXISTS "${OSCAIRN_THIRD_PARTY_CLAP}")
    list(APPEND shell_environment "OSCAIRN_THIRD_PARTY_CLAP=${OSCAIRN_THIRD_PARTY_CLAP}")
endif()
set_tests_properties(desktop-shell PROPERTIES ENVIRONMENT "${shell_environment}")
add_test(NAME desktop-pad-editor COMMAND oscairn_pad_editor_test "$<TARGET_FILE_DIR:oscairn_step_sequencer>" $<TARGET_FILE:oscairn-plugin-scan>)
# A crashing plugin in a Performance leaves the other parts playing and is reported (RFC 0010).
juce_add_console_app(oscairn_plugin_crash_test PRODUCT_NAME "Oscairn Plugin Crash Test")
target_sources(oscairn_plugin_crash_test PRIVATE tests/plugin_crash.cpp PadEditor.cpp PartsView.cpp ParameterGrid.cpp ZoneEditor.cpp PartsModel.cpp Engine.cpp Theme.cpp SoundPickerModel.cpp)
target_link_libraries(oscairn_plugin_crash_test PRIVATE oscairn_standalone_icons oscairn_audio_host oscairn_host_process oscairn_control_socket juce::juce_audio_utils juce::juce_gui_extra juce::juce_recommended_config_flags)
target_compile_definitions(oscairn_plugin_crash_test PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1)
add_dependencies(oscairn_plugin_crash_test oscairn_basic_synth oscairn-plugin-scan oscairn-plugin-worker kick-clap-fixture-process_crash)
add_test(NAME desktop-plugin-crash COMMAND oscairn_plugin_crash_test "$<TARGET_FILE_DIR:oscairn_step_sequencer>" $<TARGET_FILE:oscairn-plugin-scan>
    $<TARGET_FILE:oscairn-plugin-worker> $<TARGET_FILE:kick-clap-fixture-process_crash>)
set_tests_properties(desktop-pad-editor desktop-plugin-crash PROPERTIES TIMEOUT 60 ENVIRONMENT "${worker_environment}")
