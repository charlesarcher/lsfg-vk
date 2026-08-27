set(APP_SOURCES
    "src/main.cpp"
    "src/stream.cpp"
    "src/wsi/backend_x11.cpp")

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

target_include_directories(lsfg-vk-app
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

install(TARGETS lsfg-vk-app
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
