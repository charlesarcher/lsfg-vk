set(APP_SOURCES
    "src/main.cpp"
    "src/stream.cpp"
    "src/presentation.cpp"
    "src/wsi/backend_x11.cpp"
    "src/wsi/backend_wayland.cpp")

add_executable(lsfg-vk-app ${APP_SOURCES})

# the app binds the listener, completes the handshake and holds per-stream
# state. from task 6 onward it also links the backend so it can negotiate the
# exchange layout, import the game's staging images and open real cross-device
# frame-generation contexts (wsi/output still belong to later tasks).
target_link_libraries(lsfg-vk-app
    PUBLIC lsfg-vk-common lsfg-vk-backend)

# X11/xcb (task 7a): the SurfaceBackend X11 implementation links the xcb and
# xcb-randr client libraries. Declared here so the build fails loudly if the
# toolchain is missing, even though the wayland backend does not exist yet.
find_package(PkgConfig REQUIRED)
pkg_check_modules(XCB REQUIRED xcb xcb-randr)
target_link_libraries(lsfg-vk-app
    PUBLIC ${XCB_LIBRARIES})
target_include_directories(lsfg-vk-app
    PUBLIC ${XCB_INCLUDE_DIRECTORIES})

# Wayland client + scanner codegen are REQUIRED from task 7 onward (task 10/13 add the
# second SurfaceBackend impl + xdg-shell/xdg-output protocols). Declared here so the
# build fails loudly if the toolchain/tooling is missing even though no wayland .cpp yet.
pkg_check_modules(WAYLAND_CLIENT REQUIRED wayland-client)
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
message(STATUS "lsfg-vk-app: wayland deps present (wayland-client ${WAYLAND_CLIENT_VERSION}, wayland-scanner found)")

# Wayland protocol generation (task 10): xdg-shell (stable) + xdg-output (unstable v4)
# Generated headers go to ${CMAKE_CURRENT_BINARY_DIR}/protocols and are included via
# target_include_directories so backend_wayland.cpp can #include them.
set(WAYLAND_PROTOCOLS_DIR "${CMAKE_CURRENT_BINARY_DIR}/protocols")
file(MAKE_DIRECTORY "${WAYLAND_PROTOCOLS_DIR}")

# xdg-shell (stable) - from system wayland-protocols
find_path(WAYLAND_PROTOCOLS_XDG_SHELL_XML xdg-shell/xdg-shell.xml
    PATHS /usr/share/wayland-protocols /usr/share/qt6/wayland/protocols
    NO_DEFAULT_PATH)
if(NOT WAYLAND_PROTOCOLS_XDG_SHELL_XML)
    message(FATAL_ERROR "xdg-shell.xml not found in wayland-protocols")
endif()

# xdg-output (unstable v4) - from system wayland-protocols
find_path(WAYLAND_PROTOCOLS_XDG_OUTPUT_XML xdg-output/xdg-output-unstable-v1.xml
    PATHS /usr/share/wayland-protocols /usr/share/qt6/wayland/protocols
    NO_DEFAULT_PATH)
if(NOT WAYLAND_PROTOCOLS_XDG_OUTPUT_XML)
    message(FATAL_ERROR "xdg-output-unstable-v1.xml not found in wayland-protocols")
endif()

# Generate client headers + C code for xdg-shell
add_custom_command(
    OUTPUT "${WAYLAND_PROTOCOLS_DIR}/xdg-shell-client-protocol.h"
           "${WAYLAND_PROTOCOLS_DIR}/xdg-shell-protocol.c"
    COMMAND ${WAYLAND_SCANNER} client-header
            "${WAYLAND_PROTOCOLS_XDG_SHELL_XML}/xdg-shell/xdg-shell.xml"
            "${WAYLAND_PROTOCOLS_DIR}/xdg-shell-client-protocol.h"
    COMMAND ${WAYLAND_SCANNER} private-code
            "${WAYLAND_PROTOCOLS_XDG_SHELL_XML}/xdg-shell/xdg-shell.xml"
            "${WAYLAND_PROTOCOLS_DIR}/xdg-shell-protocol.c"
    DEPENDS "${WAYLAND_PROTOCOLS_XDG_SHELL_XML}/xdg-shell/xdg-shell.xml"
            ${WAYLAND_SCANNER}
    COMMENT "Generating xdg-shell protocol headers"
    VERBATIM)

# Generate client headers + C code for xdg-output (unstable v1, we use v4 interface version)
add_custom_command(
    OUTPUT "${WAYLAND_PROTOCOLS_DIR}/xdg-output-unstable-v1-client-protocol.h"
           "${WAYLAND_PROTOCOLS_DIR}/xdg-output-unstable-v1-protocol.c"
    COMMAND ${WAYLAND_SCANNER} client-header
            "${WAYLAND_PROTOCOLS_XDG_OUTPUT_XML}/xdg-output/xdg-output-unstable-v1.xml"
            "${WAYLAND_PROTOCOLS_DIR}/xdg-output-unstable-v1-client-protocol.h"
    COMMAND ${WAYLAND_SCANNER} private-code
            "${WAYLAND_PROTOCOLS_XDG_OUTPUT_XML}/xdg-output/xdg-output-unstable-v1.xml"
            "${WAYLAND_PROTOCOLS_DIR}/xdg-output-unstable-v1-protocol.c"
    DEPENDS "${WAYLAND_PROTOCOLS_XDG_OUTPUT_XML}/xdg-output/xdg-output-unstable-v1.xml"
            ${WAYLAND_SCANNER}
    COMMENT "Generating xdg-output protocol headers"
    VERBATIM)

# Custom target to ensure protocol generation runs before compilation
add_custom_target(wayland_protocols ALL
    DEPENDS "${WAYLAND_PROTOCOLS_DIR}/xdg-shell-client-protocol.h"
            "${WAYLAND_PROTOCOLS_DIR}/xdg-shell-protocol.c"
            "${WAYLAND_PROTOCOLS_DIR}/xdg-output-unstable-v1-client-protocol.h"
            "${WAYLAND_PROTOCOLS_DIR}/xdg-output-unstable-v1-protocol.c")

# Link the generated protocol C files into the app (compile as C)
add_library(wayland_protocols_gen STATIC
    "${WAYLAND_PROTOCOLS_DIR}/xdg-shell-protocol.c"
    "${WAYLAND_PROTOCOLS_DIR}/xdg-output-unstable-v1-protocol.c")
set_target_properties(wayland_protocols_gen PROPERTIES
    LINKER_LANGUAGE C
    POSITION_INDEPENDENT_CODE ON)

target_link_libraries(lsfg-vk-app PRIVATE wayland_protocols_gen)

# Include generated protocol headers
target_include_directories(lsfg-vk-app
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${WAYLAND_PROTOCOLS_DIR}")

# Link wayland-client
target_link_libraries(lsfg-vk-app
    PUBLIC ${WAYLAND_CLIENT_LIBRARIES})
target_include_directories(lsfg-vk-app
    PUBLIC ${WAYLAND_CLIENT_INCLUDE_DIRECTORIES})

# Ensure protocol generation runs before app compilation
add_dependencies(lsfg-vk-app wayland_protocols)

install(TARGETS lsfg-vk-app
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
