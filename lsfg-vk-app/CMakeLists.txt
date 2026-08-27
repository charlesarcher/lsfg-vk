set(APP_SOURCES
    "src/main.cpp"
    "src/stream.cpp")

add_executable(lsfg-vk-app ${APP_SOURCES})

# the app binds the listener, completes the handshake and holds per-stream
# state. from task 6 onward it also links the backend so it can negotiate the
# exchange layout, import the game's staging images and open real cross-device
# frame-generation contexts (wsi/output still belong to later tasks).
target_link_libraries(lsfg-vk-app
    PUBLIC lsfg-vk-common lsfg-vk-backend)

target_include_directories(lsfg-vk-app
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

install(TARGETS lsfg-vk-app
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
