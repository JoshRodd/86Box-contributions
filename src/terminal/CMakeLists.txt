add_library(plat STATIC
    terminal_main.c
    terminal_platform.c
    terminal_renderer.c
    ../unix/sdl_cdrom.c
    ../unix/unix_netsocket.c
    ../unix/unix_dynld.c
    ../unix/dummy_cdrom_ioctl.c
    ../unix/unix_floppy_ioctl.c
)

add_library(ui STATIC terminal_ui.c)
find_program(CARGO_EXECUTABLE cargo REQUIRED)
set(TERMINAL_KEYBOARD_DIR "${CMAKE_SOURCE_DIR}/submodules/terminal-to-pc-keyboard")
set(TERMINAL_KEYBOARD_LIBRARY "${TERMINAL_KEYBOARD_DIR}/target/release/libpc_xt_keyboard.a")

add_custom_command(
    OUTPUT "${TERMINAL_KEYBOARD_LIBRARY}"
    COMMAND "${CARGO_EXECUTABLE}" build --manifest-path "${TERMINAL_KEYBOARD_DIR}/Cargo.toml" --release
    DEPENDS
        "${TERMINAL_KEYBOARD_DIR}/Cargo.toml"
        "${TERMINAL_KEYBOARD_DIR}/src/ffi.rs"
        "${TERMINAL_KEYBOARD_DIR}/src/lib.rs"
    WORKING_DIRECTORY "${TERMINAL_KEYBOARD_DIR}"
)
add_custom_target(terminal_keyboard_mapper DEPENDS "${TERMINAL_KEYBOARD_LIBRARY}")
add_library(terminal_keyboard_mapper_static STATIC IMPORTED GLOBAL)
set_target_properties(terminal_keyboard_mapper_static PROPERTIES
    IMPORTED_LOCATION "${TERMINAL_KEYBOARD_LIBRARY}"
)
add_dependencies(terminal_keyboard_mapper_static terminal_keyboard_mapper)


target_compile_definitions(plat PRIVATE USE_TERMINAL_UI)

add_subdirectory("${CMAKE_SOURCE_DIR}/submodules/tigt" "${CMAKE_CURRENT_BINARY_DIR}/tigt")
target_link_libraries(plat PRIVATE tigt::tigt)
target_link_libraries(plat PRIVATE ${CMAKE_DL_LIBS})
target_include_directories(plat PRIVATE "${TERMINAL_KEYBOARD_DIR}/include")
target_link_libraries(plat PRIVATE terminal_keyboard_mapper_static)
