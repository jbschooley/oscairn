add_executable(oscairn-render render/main.cpp)
target_link_libraries(oscairn-render PRIVATE oscairn_engine)
target_compile_options(oscairn-render PRIVATE -Wall -Wextra)
install(TARGETS oscairn-render RUNTIME DESTINATION bin)

# Sequencer playing the synth, through the delay. Rendering with very different block sizes must give
# byte-identical files: events land on their exact sample, not on block boundaries.
set(demo_chain $<TARGET_FILE:oscairn_step_sequencer> $<TARGET_FILE:oscairn_basic_synth> $<TARGET_FILE:oscairn_delay>)
foreach(block 512 37)
    add_test(NAME render-block-${block}
        COMMAND oscairn-render ${CMAKE_CURRENT_BINARY_DIR}/demo-${block}.wav --seconds 4 --block ${block} ${demo_chain})
    set_tests_properties(render-block-${block} PROPERTIES FIXTURES_SETUP render-${block})
endforeach()
add_test(NAME render-is-block-size-independent
    COMMAND ${CMAKE_COMMAND} -E compare_files ${CMAKE_CURRENT_BINARY_DIR}/demo-512.wav ${CMAKE_CURRENT_BINARY_DIR}/demo-37.wav)
set_tests_properties(render-is-block-size-independent PROPERTIES FIXTURES_REQUIRED "render-512;render-37")

# Rendering twice gives the same file.
add_test(NAME render-repeat
    COMMAND oscairn-render ${CMAKE_CURRENT_BINARY_DIR}/demo-512-again.wav --seconds 4 --block 512 ${demo_chain})
set_tests_properties(render-repeat PROPERTIES FIXTURES_SETUP render-again)
add_test(NAME render-is-repeatable
    COMMAND ${CMAKE_COMMAND} -E compare_files ${CMAKE_CURRENT_BINARY_DIR}/demo-512.wav ${CMAKE_CURRENT_BINARY_DIR}/demo-512-again.wav)
set_tests_properties(render-is-repeatable PROPERTIES FIXTURES_REQUIRED "render-512;render-again")

# Following a (simulated) 140 BPM MIDI clock, rendering twice gives the same file.
foreach(run a b)
    add_test(NAME render-midi-clock-${run}
        COMMAND oscairn-render ${CMAKE_CURRENT_BINARY_DIR}/demo-clock-${run}.wav --seconds 4 --midi-clock 140 ${demo_chain})
    set_tests_properties(render-midi-clock-${run} PROPERTIES FIXTURES_SETUP render-clock-${run})
endforeach()
add_test(NAME render-midi-clock-is-repeatable
    COMMAND ${CMAKE_COMMAND} -E compare_files ${CMAKE_CURRENT_BINARY_DIR}/demo-clock-a.wav ${CMAKE_CURRENT_BINARY_DIR}/demo-clock-b.wav)
set_tests_properties(render-midi-clock-is-repeatable PROPERTIES FIXTURES_REQUIRED "render-clock-a;render-clock-b")

# Independently packaged upstream examples, when explicitly supplied by CI/developers.
if(OSCAIRN_LV2 AND OSCAIRN_LV2_EXAMPLE_DIR)
    foreach(example eg-amp eg-fifths)
        if(NOT EXISTS "${OSCAIRN_LV2_EXAMPLE_DIR}/${example}.lv2/manifest.ttl")
            message(FATAL_ERROR "Missing LV2 example ${example} in OSCAIRN_LV2_EXAMPLE_DIR")
        endif()
    endforeach()
endif()
if(OSCAIRN_LV2 AND EXISTS "${OSCAIRN_LV2_EXAMPLE_DIR}/eg-amp.lv2/manifest.ttl")
    # eg-amp is mono. Compare against a mono synth chain, before stereo delay.
    set(amp_chain $<TARGET_FILE:oscairn_step_sequencer> $<TARGET_FILE:oscairn_basic_synth>)
    add_test(NAME lv2-example-reference COMMAND oscairn-render ${CMAKE_CURRENT_BINARY_DIR}/lv2-reference.wav
        --seconds 4 --block 512 ${amp_chain})
    set_tests_properties(lv2-example-reference PROPERTIES FIXTURES_SETUP lv2-reference)
    add_test(NAME lv2-example-amp COMMAND oscairn-render ${CMAKE_CURRENT_BINARY_DIR}/lv2-amp.wav
        --seconds 4 --block 512 ${amp_chain} "${OSCAIRN_LV2_EXAMPLE_DIR}/eg-amp.lv2")
    set_tests_properties(lv2-example-amp PROPERTIES FIXTURES_SETUP lv2-amp)
    add_test(NAME lv2-example-amp-unity COMMAND ${CMAKE_COMMAND} -E compare_files
        ${CMAKE_CURRENT_BINARY_DIR}/lv2-reference.wav ${CMAKE_CURRENT_BINARY_DIR}/lv2-amp.wav)
    set_tests_properties(lv2-example-amp-unity PROPERTIES FIXTURES_REQUIRED "lv2-reference;lv2-amp")
endif()
if(OSCAIRN_LV2 AND EXISTS "${OSCAIRN_LV2_EXAMPLE_DIR}/eg-fifths.lv2/manifest.ttl")
    foreach(block 512 37)
        add_test(NAME lv2-example-fifths-${block} COMMAND oscairn-render
            ${CMAKE_CURRENT_BINARY_DIR}/lv2-fifths-${block}.wav --seconds 4 --block ${block}
            $<TARGET_FILE:oscairn_step_sequencer> "${OSCAIRN_LV2_EXAMPLE_DIR}/eg-fifths.lv2" $<TARGET_FILE:oscairn_basic_synth>)
        set_tests_properties(lv2-example-fifths-${block} PROPERTIES FIXTURES_SETUP lv2-fifths-${block})
    endforeach()
    add_test(NAME lv2-example-fifths-timing COMMAND ${CMAKE_COMMAND} -E compare_files
        ${CMAKE_CURRENT_BINARY_DIR}/lv2-fifths-512.wav ${CMAKE_CURRENT_BINARY_DIR}/lv2-fifths-37.wav)
    set_tests_properties(lv2-example-fifths-timing PROPERTIES FIXTURES_REQUIRED "lv2-fifths-512;lv2-fifths-37")
endif()

if(UNIX)
    add_subdirectory(process)
    add_executable(oscairn-sound-picker-test standalone/tests/sound_picker.cpp standalone/SoundPickerModel.cpp)
    target_link_libraries(oscairn-sound-picker-test PRIVATE oscairn_control_socket)
    add_test(NAME desktop-sound-picker COMMAND oscairn-sound-picker-test $<TARGET_FILE:oscairn_basic_synth> $<TARGET_FILE:oscairn_step_sequencer> $<TARGET_FILE:oscairn_sampler>)
    set_tests_properties(desktop-sound-picker PROPERTIES TIMEOUT 30)
    add_executable(oscairn-parts-model-test standalone/tests/parts_model.cpp standalone/PartsModel.cpp)
    target_link_libraries(oscairn-parts-model-test PRIVATE oscairn_control_socket)
    add_dependencies(oscairn-parts-model-test oscairn-plugin-scan oscairn_basic_synth oscairn_delay oscairn_transpose)
    add_test(NAME desktop-parts-model COMMAND oscairn-parts-model-test "$<TARGET_FILE_DIR:oscairn_basic_synth>" $<TARGET_FILE:oscairn-plugin-scan>)
    set_tests_properties(desktop-parts-model PROPERTIES TIMEOUT 60)
    if(OSCAIRN_LV2)
        add_test(NAME desktop-sound-picker-lv2 COMMAND oscairn-sound-picker-test "${PROJECT_BINARY_DIR}/core/lv2-host/tests/test fixture.lv2" --lv2)
        set_tests_properties(desktop-sound-picker-lv2 PROPERTIES TIMEOUT 30)
    endif()
endif()

if(OSCAIRN_STANDALONE OR OSCAIRN_ENGINE)
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 9.0.4
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(JUCE)
    add_subdirectory(audio)
    add_subdirectory(engine)
endif()

if(OSCAIRN_STANDALONE)
    add_subdirectory(standalone)
endif()
