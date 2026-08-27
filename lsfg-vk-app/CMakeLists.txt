set(APP_SOURCES
    "src/main.cpp"
    "src/stream.cpp")

add_executable(lsfg-vk-app ${APP_SOURCES})

# the app binds the listener, completes the handshake and holds per-stream
# state; it links only the common library here (wsi/output belong to later
# tasks and must not be wired in yet).
target_link_libraries(lsfg-vk-app
    PUBLIC lsfg-vk-common)

target_include_directories(lsfg-vk-app
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

install(TARGETS lsfg-vk-app
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
