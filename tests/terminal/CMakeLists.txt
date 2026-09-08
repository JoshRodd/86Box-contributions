add_executable(terminal_pcjr_tests terminal_pcjr_test.c)
target_include_directories(terminal_pcjr_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/include
    ${PROJECT_SOURCE_DIR}/src/cpu
    ${PROJECT_BINARY_DIR}/src/include
    ${PROJECT_SOURCE_DIR}/submodules/terminal-to-pc-keyboard/include)
target_compile_definitions(terminal_pcjr_tests PRIVATE USE_TERMINAL_UI)
target_compile_options(terminal_pcjr_tests PRIVATE
    -UNDEBUG -ffunction-sections -fdata-sections)
if(APPLE)
    target_link_options(terminal_pcjr_tests PRIVATE -Wl,-dead_strip)
else()
    target_link_options(terminal_pcjr_tests PRIVATE -Wl,--gc-sections)
endif()
target_link_libraries(terminal_pcjr_tests PRIVATE tigt::tigt)
add_test(NAME terminal_pcjr_resolved_palette COMMAND terminal_pcjr_tests)

add_executable(terminal_mda_tests terminal_mda_test.c)
target_include_directories(terminal_mda_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/include
    ${PROJECT_SOURCE_DIR}/src/cpu
    ${PROJECT_BINARY_DIR}/src/include
    ${PROJECT_SOURCE_DIR}/submodules/terminal-to-pc-keyboard/include)
target_compile_definitions(terminal_mda_tests PRIVATE USE_TERMINAL_UI)
target_compile_options(terminal_mda_tests PRIVATE
    -UNDEBUG -ffunction-sections -fdata-sections)
if(APPLE)
    target_link_options(terminal_mda_tests PRIVATE -Wl,-dead_strip)
else()
    target_link_options(terminal_mda_tests PRIVATE -Wl,--gc-sections)
endif()
target_link_libraries(terminal_mda_tests PRIVATE tigt::tigt)
add_test(NAME terminal_mda_video_enable COMMAND terminal_mda_tests)

add_executable(terminal_keyboard_tests terminal_keyboard_test.c)
target_include_directories(terminal_keyboard_tests PRIVATE
    ${PROJECT_SOURCE_DIR}/src/include
    ${PROJECT_SOURCE_DIR}/src/cpu
    ${PROJECT_BINARY_DIR}/src/include
    ${PROJECT_SOURCE_DIR}/submodules/terminal-to-pc-keyboard/include)
target_compile_definitions(terminal_keyboard_tests PRIVATE USE_TERMINAL_UI)
target_compile_options(terminal_keyboard_tests PRIVATE
    -UNDEBUG -ffunction-sections -fdata-sections)
if(APPLE)
    target_link_options(terminal_keyboard_tests PRIVATE -Wl,-dead_strip)
else()
    target_link_options(terminal_keyboard_tests PRIVATE -Wl,--gc-sections)
endif()
target_link_libraries(terminal_keyboard_tests PRIVATE tigt::tigt terminal_keyboard_mapper_static)
add_test(NAME terminal_special_key_releases COMMAND terminal_keyboard_tests)

