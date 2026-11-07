cmake_minimum_required(VERSION 3.25)
project(bf1943_recomp LANGUAGES C CXX)

# Vulkan is the only graphics backend. There is no D3D11/D3D12 code and no RHI.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Vulkan 1.3 REQUIRED COMPONENTS glslc)

# --- Renderer ---------------------------------------------------------------
add_library(bf_vulkan STATIC
  renderer/vulkan/vk_log.cpp
  renderer/vulkan/vk_device.cpp
  renderer/vulkan/vk_frame.cpp
  renderer/vulkan/vk_swapchain.cpp
  renderer/vulkan/vma_impl.cpp)
target_include_directories(bf_vulkan PUBLIC
  renderer/vulkan
  thirdparty/VulkanMemoryAllocator/include)
target_link_libraries(bf_vulkan PUBLIC Vulkan::Vulkan)
if(WIN32)
  target_compile_definitions(bf_vulkan PUBLIC _CRT_SECURE_NO_WARNINGS)
endif()
if(MSVC)
  target_compile_options(bf_vulkan PRIVATE /W4 /wd4100 /wd4189 /wd4324 /wd4127)
  set_source_files_properties(renderer/vulkan/vma_impl.cpp PROPERTIES COMPILE_OPTIONS "/W0")
endif()

# --- Shaders (GLSL -> SPIR-V -> C header) -----------------------------------
function(bf_compile_shader src var)
  set(out ${CMAKE_CURRENT_BINARY_DIR}/shaders/${var}.h)
  add_custom_command(OUTPUT ${out}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/shaders
    COMMAND Vulkan::glslc --target-env=vulkan1.3 -O -mfmt=num ${CMAKE_CURRENT_SOURCE_DIR}/${src} -o ${out}.inc
    COMMAND ${CMAKE_COMMAND} -DIN=${out}.inc -DOUT=${out} -DVAR=${var} -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/spirv_header.cmake
    DEPENDS ${src} tools/spirv_header.cmake
    VERBATIM)
  set(BF_SHADER_HEADERS ${BF_SHADER_HEADERS} ${out} PARENT_SCOPE)
endfunction()

bf_compile_shader(tests/shaders/triangle.vert triangle_vert)
bf_compile_shader(tests/shaders/triangle.frag triangle_frag)
add_custom_target(bf_test_shaders DEPENDS ${BF_SHADER_HEADERS})

# --- Tests ------------------------------------------------------------------
enable_testing()
add_executable(vk_headless_test tests/vk_headless_test.cpp)
target_link_libraries(vk_headless_test PRIVATE bf_vulkan)
target_include_directories(vk_headless_test PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/shaders)
add_dependencies(vk_headless_test bf_test_shaders)
add_test(NAME vk_headless COMMAND vk_headless_test)

if(WIN32)
  add_executable(vk_window_test tests/vk_window_test.cpp)
  target_link_libraries(vk_window_test PRIVATE bf_vulkan)
  add_test(NAME vk_window COMMAND vk_window_test --frames 120)
endif()
