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
target_compile_definitions(plat PRIVATE USE_TERMINAL_UI)

set(TIGT_WITH_KEYBOARD ON)
add_subdirectory("${CMAKE_SOURCE_DIR}/submodules/tigt" "${CMAKE_CURRENT_BINARY_DIR}/tigt")
target_link_libraries(plat PRIVATE tigt::tigt)
target_link_libraries(plat PRIVATE ${CMAKE_DL_LIBS})
