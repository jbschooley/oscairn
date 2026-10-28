add_executable(oscairn-render render/main.cpp)
target_link_libraries(oscairn-render PRIVATE oscairn_engine)
target_compile_options(oscairn-render PRIVATE -Wall -Wextra)

# Sequencer playing the synth. Rendering with very different block sizes must give
# byte-identical files: events land on their exact sample, not on block boundaries.
set(demo_chain $<TARGET_FILE:oscairn_step_sequencer> $<TARGET_FILE:oscairn_basic_synth>)
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
