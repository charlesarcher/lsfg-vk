set(LAYER_SOURCES
    "src/entrypoint.cpp"
    "src/instance.cpp"
    "src/swapchain.cpp"
    "src/capture_context.cpp"
    "src/isolated_swapchain.cpp")

add_library(lsfg-vk-layer SHARED ${LAYER_SOURCES})

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBDRM REQUIRED libdrm)
pkg_check_modules(LIBDRM_AMDGPU REQUIRED libdrm_amdgpu)

target_include_directories(lsfg-vk-layer PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(lsfg-vk-layer PRIVATE
    ${LIBDRM_INCLUDE_DIRS} ${LIBDRM_AMDGPU_INCLUDE_DIRS})

target_link_libraries(lsfg-vk-layer
    PUBLIC lsfg-vk-common
    PUBLIC lsfg-vk-backend
    PRIVATE ${LIBDRM_LIBRARIES} ${LIBDRM_AMDGPU_LIBRARIES})

target_compile_options(lsfg-vk-layer PRIVATE
    -Wno-unknown-warning-option
    -Wno-unsafe-buffer-usage) # Array indexing

set_target_properties(lsfg-vk-layer PROPERTIES
    CXX_VISIBILITY_PRESET hidden)

configure_file(
    "VkLayer_LSFGVK_frame_generation.json.in"
    "VkLayer_LSFGVK_frame_generation.json"
    @ONLY)

install(TARGETS lsfg-vk-layer
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/VkLayer_LSFGVK_frame_generation.json"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/vulkan/implicit_layer.d")

add_library(lsfg-vk-csstrip SHARED src/csstrip.c)
target_include_directories(lsfg-vk-csstrip PRIVATE
    ${LIBDRM_INCLUDE_DIRS})
target_link_libraries(lsfg-vk-csstrip PRIVATE ${LIBDRM_LIBRARIES} ${CMAKE_DL_LIBS})
set_target_properties(lsfg-vk-csstrip PROPERTIES
    PREFIX "lib")
install(TARGETS lsfg-vk-csstrip
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
