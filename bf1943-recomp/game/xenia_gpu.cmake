# Xenia's GPU emulation (BSD-3, thirdparty/xenia/LICENSE): PM4 command processor,
# Xenos shader -> SPIR-V translator, texture/render-target caches, Vulkan backend.
# Only the Vulkan path is built. Kernel/CPU/memory dependencies are shimmed in
# runtime/xenia_shims/.
set(XE ${ROOT}/thirdparty/xenia)
# DLSS: NVIDIA's SDK isn't kept in the repo (its licence), fetched from NVIDIA's
# GitHub at the first configure (tools/fetch_dlss.py: a pinned release, checked).
if(NOT EXISTS ${ROOT}/thirdparty/dlss/lib/nvsdk_ngx_d.lib OR NOT EXISTS ${ROOT}/thirdparty/dlss/bin/nvngx_dlss.dll)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  execute_process(COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/fetch_dlss.py COMMAND_ERROR_IS_FATAL ANY)
endif()
file(GLOB XENIA_GPU_SOURCES CONFIGURE_DEPENDS
  ${XE}/src/xenia/base/*.cc
  ${XE}/src/xenia/gpu/*.cc
  ${XE}/src/xenia/gpu/vulkan/*.cc
  ${XE}/src/xenia/ui/*.cc
  ${XE}/src/xenia/ui/vulkan/*.cc
  ${XE}/src/xenia/apu/*.cc            # XMA decoder (audio)
  ${XE}/src/xenia/apu/xaudio2/*.cc
  ${XE}/third_party/glslang/SPIRV/*.cpp
  ${ROOT}/runtime/xenia_shims/*.cc)
# The POSIX/Linux halves of Xenia's base (for the Linux server) are not for Windows.
list(FILTER XENIA_GPU_SOURCES EXCLUDE REGEX "_posix\.cc$|system_gnulinux\.cc$")
add_library(xenia_gpu STATIC ${XENIA_GPU_SOURCES})
target_include_directories(xenia_gpu PUBLIC
  ${ROOT}/runtime/xenia_shims    # must win over the real kernel/cpu/memory headers
  ${XE}/src ${XE}
  ${ROOT}/build/xenia_shaders    # tools/build_xenia_shaders.py output
  $ENV{VULKAN_SDK}/Include
  ${ROOT}/thirdparty/VulkanMemoryAllocator/include
  ${ROOT}/thirdparty/fidelityfx/include  # FSR 3 (bf_fsr3.cc)
  ${ROOT}/thirdparty/dlss/include)       # DLSS (bf_dlss.cc)
target_compile_definitions(xenia_gpu PUBLIC
  NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS
  FMT_HEADER_ONLY XXH_INLINE_ALL VK_NO_PROTOTYPES XE_PLATFORM_WIN32=1
  XE_PRESENTER_NO_DXGI  # no DirectX: the presenter's UI ticks from a timer (presenter.h)
  _mm_cvtsi64x_si128=_mm_cvtsi64_si128)  # MSVC-only spelling
target_compile_options(xenia_gpu PRIVATE -march=x86-64-v3 -Wno-everything /EHsc)
target_link_libraries(xenia_gpu PUBLIC bf1943_ffmpeg
  ${ROOT}/thirdparty/dlss/lib/nvsdk_ngx_d.lib)  # NVIDIA NGX (DLSS), its licence: thirdparty/dlss/LICENSE.txt
