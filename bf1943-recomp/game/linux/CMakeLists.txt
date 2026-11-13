# Linux console server (bf1943_server): the recompiled game and the runtime,
# without GPU, sound output, window or PC menus (runtime/linux_server.cpp has
# their stand-ins). Same generated/ppc as the Windows build (run XenonRecomp
# on your own default.xex first, see the README).
#
#   cmake -S game/linux -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
#   cmake --build build/linux
#   build/linux/bf1943_server --game <dump> --dedicated --name "My Server" --maps Wake_island_s
cmake_minimum_required(VERSION 3.25)
project(bf1943_server LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)

# Guest code: as on Windows (tools/strong_funcs.py makes unpatched functions inlinable).
execute_process(COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/strong_funcs.py
                        ${ROOT}/generated/ppc ${ROOT}/runtime COMMAND_ERROR_IS_FATAL ANY)
file(GLOB PPC_SOURCES CONFIGURE_DEPENDS ${ROOT}/generated/ppc/*.cpp)
if(NOT PPC_SOURCES)
  message(FATAL_ERROR "generated/ppc is empty: run XenonRecomp first (see the README)")
endif()
add_library(bf1943_ppc STATIC ${PPC_SOURCES})
target_include_directories(bf1943_ppc PUBLIC
  ${ROOT}/generated/ppc
  ${ROOT}/thirdparty/XenonRecomp/XenonUtils
  ${ROOT}/thirdparty/XenonRecomp/thirdparty/simde)
target_compile_options(bf1943_ppc PRIVATE -march=x86-64-v3 -fno-strict-aliasing -Wno-unused-variable
  -Wno-unused-label -include ${ROOT}/game/ppc_overrides.h)

# XEX loading from XenonUtils.
set(XU ${ROOT}/thirdparty/XenonRecomp/XenonUtils)
set(XT ${ROOT}/thirdparty/XenonRecomp/thirdparty)
add_library(bf1943_xexload STATIC
  ${XU}/xex.cpp ${XU}/image.cpp ${XU}/xex_patcher.cpp ${XU}/memory_mapped_file.cpp
  ${XT}/libmspack/libmspack/mspack/lzxd.c ${XT}/tiny-AES-c/aes.c)
set_target_properties(bf1943_xexload PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(bf1943_xexload PUBLIC ${XU} ${XT}/simde
  PRIVATE ${XT}/libmspack/libmspack/mspack ${XT}/tiny-AES-c ${XT}/TinySHA1)

# The runtime minus what needs Windows (window, Vulkan presenter, XAudio2,
# GDI menus).
file(GLOB RUNTIME_SOURCES CONFIGURE_DEPENDS ${ROOT}/runtime/*.cpp)
set(PC_UI_SOURCES  # (GDI / GDI+: the server has stand-ins, the game posix_gdiplus.cpp)
  ${ROOT}/runtime/pc_settings.cpp ${ROOT}/runtime/pc_menu.cpp ${ROOT}/runtime/pc_ui.cpp)
list(REMOVE_ITEM RUNTIME_SOURCES ${PC_UI_SOURCES} ${ROOT}/runtime/fps_overlay.cpp)  # (its counter: linux_client.cpp)
set(STUBS ${CMAKE_CURRENT_BINARY_DIR}/import_stubs.cpp)
add_custom_command(OUTPUT ${STUBS}
  COMMAND Python3::Interpreter ${ROOT}/tools/gen_import_stubs.py
          ${ROOT}/generated/ppc/ppc_recomp_shared.h ${ROOT}/runtime ${STUBS}
  DEPENDS ${RUNTIME_SOURCES} ${ROOT}/tools/gen_import_stubs.py ${ROOT}/generated/ppc/ppc_recomp_shared.h)

# Audio: Xenia's XMA decoder (FFmpeg's xmaframes) with no sound device. The
# game's audio code needs the decoding to run: without it the front end hangs.
set(FFMPEG_DIR ${ROOT}/thirdparty/ffmpeg)
include(${FFMPEG_DIR}/sources.cmake)
add_library(bf1943_ffmpeg STATIC ${FFMPEG_SOURCES})
target_compile_definitions(bf1943_ffmpeg PRIVATE HAVE_AV_CONFIG_H _USE_MATH_DEFINES)
target_include_directories(bf1943_ffmpeg PRIVATE ${FFMPEG_DIR}/xenia-config ${FFMPEG_DIR} ${FFMPEG_DIR}/libavcodec)
target_include_directories(bf1943_ffmpeg INTERFACE ${FFMPEG_DIR}/xenia-config ${FFMPEG_DIR})
target_compile_options(bf1943_ffmpeg PRIVATE -w -Wno-incompatible-pointer-types)
set(XE ${ROOT}/thirdparty/xenia)
file(GLOB XENIA_APU_SOURCES CONFIGURE_DEPENDS ${XE}/src/xenia/base/*.cc ${XE}/src/xenia/apu/*.cc)
list(FILTER XENIA_APU_SOURCES EXCLUDE REGEX "_win\\.cc$|system_gnulinux\\.cc$|png_utils\\.cc$|base/assert\\.cc$")
list(APPEND XENIA_APU_SOURCES ${ROOT}/runtime/xenia_shims/apu_bridge.cc ${ROOT}/runtime/xenia_shims/memory_shim.cc
     ${ROOT}/runtime/xenia_shims/profiling_stub.cc ${ROOT}/runtime/xenia_shims/linux_shim.cc)
add_library(xenia_apu STATIC ${XENIA_APU_SOURCES})
target_include_directories(xenia_apu PUBLIC ${ROOT}/runtime/xenia_shims ${XE}/src ${XE} ${XE}/third_party/fmt/include)
target_compile_definitions(xenia_apu PUBLIC FMT_HEADER_ONLY XXH_INLINE_ALL)
target_compile_options(xenia_apu PRIVATE -march=x86-64-v3 -w)
target_link_libraries(xenia_apu PUBLIC bf1943_ffmpeg)

file(GLOB DETOUR_SOURCES CONFIGURE_DEPENDS ${ROOT}/thirdparty/recastnavigation/Detour/Source/*.cpp)
add_executable(bf1943_server ${RUNTIME_SOURCES} ${ROOT}/runtime/bots/brain.cpp ${ROOT}/runtime/bots/nav.cpp
               ${DETOUR_SOURCES} ${STUBS})
target_include_directories(bf1943_server PRIVATE ${ROOT}/thirdparty/recastnavigation/Detour/Include)
target_include_directories(bf1943_server PRIVATE ${ROOT}/runtime ${ROOT}/runtime/xenia_shims)
target_compile_options(bf1943_server PRIVATE -march=x86-64-v3 -fno-strict-aliasing)
target_link_libraries(bf1943_server PRIVATE bf1943_ppc bf1943_xexload xenia_apu Threads::Threads ${CMAKE_DL_LIBS})
target_link_options(bf1943_server PRIVATE -rdynamic)  # symbol names in crash stacks

# ---- The game (BF1943_CLIENT=ON): bf1943 with Xenia's Vulkan GPU and an SDL3
# window, sound and input (runtime/xenia_shims/window_sdl.cc,
# runtime/linux_client.cpp). Needs the X11 and Vulkan headers (Debian/Ubuntu:
# libx11-dev libx11-xcb-dev libvulkan-dev) and the shaders made by
# tools/build_xenia_shaders.py (build/xenia_shaders).
#   cmake -S game/linux -B build/linux-client -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DBF1943_CLIENT=ON
#   cmake --build build/linux-client --target bf1943
option(BF1943_CLIENT "Build the game (bf1943) too" OFF)
if(BF1943_CLIENT)
  include(FetchContent)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(sdl3 URL https://github.com/libsdl-org/SDL/releases/download/release-3.2.20/SDL3-3.2.20.tar.gz
                       URL_HASH SHA256=467600ae090dd28616fa37369faf4e3143198ff1da37729b552137e47f751a67)
  FetchContent_MakeAvailable(sdl3)
  find_package(X11 REQUIRED)

  file(GLOB XENIA_GPU_SOURCES CONFIGURE_DEPENDS
    ${XE}/src/xenia/base/*.cc ${XE}/src/xenia/gpu/*.cc ${XE}/src/xenia/gpu/vulkan/*.cc
    ${XE}/src/xenia/ui/*.cc ${XE}/src/xenia/ui/vulkan/*.cc ${XE}/src/xenia/apu/*.cc
    ${XE}/third_party/glslang/SPIRV/*.cpp ${ROOT}/runtime/xenia_shims/*.cc)
  # Windows' halves, and FSR 3 / DLSS (Windows libraries: linux_upscalers.cc).
  list(FILTER XENIA_GPU_SOURCES EXCLUDE REGEX
       "_win\.cc$|system_gnulinux\.cc$|png_utils\.cc$|base/assert\.cc$|bf_fsr3\.cc$|bf_dlss\.cc$")
  add_library(xenia_gpu STATIC ${XENIA_GPU_SOURCES})
  target_include_directories(xenia_gpu PUBLIC ${ROOT}/runtime/xenia_shims ${XE}/src ${XE}
    ${XE}/third_party/fmt/include ${ROOT}/build/xenia_shaders
    ${ROOT}/thirdparty/VulkanMemoryAllocator/include ${ROOT}/thirdparty/fidelityfx/include
    ${ROOT}/thirdparty/dlss/include)
  target_compile_definitions(xenia_gpu PUBLIC FMT_HEADER_ONLY XXH_INLINE_ALL VK_NO_PROTOTYPES BF1943_CLIENT)
  target_compile_options(xenia_gpu PRIVATE -march=x86-64-v3 -w)
  target_link_libraries(xenia_gpu PUBLIC bf1943_ffmpeg SDL3::SDL3-static X11::X11 X11::X11_xcb X11::xcb)

  # The PC menus and key icons too, their GDI / GDI+ drawn by
  # runtime/posix_gdiplus.cpp (stb_truetype; the menus' fonts copied next to the exe).
  set(CLIENT_SOURCES ${RUNTIME_SOURCES} ${PC_UI_SOURCES})
  add_executable(bf1943 ${CLIENT_SOURCES} ${ROOT}/runtime/bots/brain.cpp ${ROOT}/runtime/bots/nav.cpp
                 ${DETOUR_SOURCES} ${STUBS})
  target_include_directories(bf1943 PRIVATE ${ROOT}/thirdparty/recastnavigation/Detour/Include
                             ${ROOT}/runtime ${ROOT}/runtime/xenia_shims ${ROOT}/thirdparty/stb)
  add_custom_command(TARGET bf1943 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${ROOT}/data/fonts $<TARGET_FILE_DIR:bf1943>/fonts)
  target_compile_definitions(bf1943 PRIVATE BF1943_CLIENT)
  target_compile_options(bf1943 PRIVATE -march=x86-64-v3 -fno-strict-aliasing)
  target_link_libraries(bf1943 PRIVATE bf1943_ppc bf1943_xexload xenia_gpu Threads::Threads ${CMAKE_DL_LIBS})
  target_link_options(bf1943 PRIVATE -rdynamic)
endif()
