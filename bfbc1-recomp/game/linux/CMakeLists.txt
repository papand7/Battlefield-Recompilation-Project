# Battlefield: Bad Company on Linux, as bf1943's game/linux: the console
# server (bfbc1_server: no GPU, sound output, window or PC menus;
# ../bf1943-recomp/runtime/linux_server.cpp has their stand-ins) and, with
# BFBC1_CLIENT=ON, the game (bfbc1: Xenia's Vulkan GPU and an SDL3 window,
# runtime/xenia_shims/window_sdl.cc, runtime/linux_client.cpp). Same
# generated/ppc as the Windows build (XenonRecomp on your own default.xex).
#
#   cmake -S game/linux -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ [-DBFBC1_CLIENT=ON]
#   cmake --build build/linux
# From Windows: -DCMAKE_TOOLCHAIN_FILE=../bf1943-recomp/game/linux/cross-from-windows.cmake
# (a sysroot made by ../bf1943-recomp/tools/fetch_linux_sysroot.py).
cmake_minimum_required(VERSION 3.25)
project(bfbc1_linux LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(BC ${CMAKE_CURRENT_SOURCE_DIR}/../..)
if(NOT ROOT)
  set(ROOT ${BC}/../bf1943-recomp)
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(Threads REQUIRED)

# Guest code.
file(GLOB PPC_SOURCES CONFIGURE_DEPENDS ${BC}/generated/ppc/*.cpp)
if(NOT PPC_SOURCES)
  message(FATAL_ERROR "generated/ppc is empty: run XenonRecomp first")
endif()
add_library(bfbc1_ppc STATIC ${PPC_SOURCES})
target_include_directories(bfbc1_ppc PUBLIC
  ${BC}/generated/ppc
  ${ROOT}/thirdparty/XenonRecomp/XenonUtils
  ${ROOT}/thirdparty/XenonRecomp/thirdparty/simde)
target_compile_options(bfbc1_ppc PRIVATE -march=x86-64-v3 -fno-strict-aliasing -Wno-unused-variable
  -Wno-unused-label -include ${ROOT}/game/ppc_overrides.h)

# XEX loading from XenonUtils.
set(XU ${ROOT}/thirdparty/XenonRecomp/XenonUtils)
set(XT ${ROOT}/thirdparty/XenonRecomp/thirdparty)
add_library(bfbc1_xexload STATIC
  ${XU}/xex.cpp ${XU}/image.cpp ${XU}/xex_patcher.cpp ${XU}/memory_mapped_file.cpp
  ${XT}/libmspack/libmspack/mspack/lzxd.c ${XT}/tiny-AES-c/aes.c)
set_target_properties(bfbc1_xexload PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(bfbc1_xexload PUBLIC ${XU} ${XT}/simde
  PRIVATE ${XT}/libmspack/libmspack/mspack ${XT}/tiny-AES-c ${XT}/TinySHA1)

# The shared runtime minus bf1943's own parts (its guest patches and bots) and
# what needs Windows GDI on the server (linux_server.cpp stands in; the game
# draws it with posix_gdiplus.cpp);
# Bad Company's runtime minus its GDI+ Multiplayer menu (bc_mp_menu_posix.cpp).
file(GLOB RUNTIME_SOURCES CONFIGURE_DEPENDS ${ROOT}/runtime/*.cpp)
set(PC_UI_SOURCES  # (GDI / GDI+: the server has stand-ins, the game posix_gdiplus.cpp)
  ${ROOT}/runtime/pc_settings.cpp ${ROOT}/runtime/pc_menu.cpp ${ROOT}/runtime/pc_ui.cpp ${ROOT}/runtime/fps_overlay.cpp)
list(REMOVE_ITEM RUNTIME_SOURCES
  ${ROOT}/runtime/game_patches.cpp ${ROOT}/runtime/bots.cpp ${ROOT}/runtime/bot_players.cpp ${PC_UI_SOURCES})
file(GLOB BC_RUNTIME_SOURCES CONFIGURE_DEPENDS ${BC}/runtime/*.cpp)
list(REMOVE_ITEM BC_RUNTIME_SOURCES ${BC}/runtime/bc_mp_menu.cpp)
file(GLOB DETOUR_SOURCES CONFIGURE_DEPENDS ${ROOT}/thirdparty/recastnavigation/Detour/Source/*.cpp)
set(STUBS ${CMAKE_CURRENT_BINARY_DIR}/import_stubs.cpp)
add_custom_command(OUTPUT ${STUBS}
  COMMAND Python3::Interpreter ${ROOT}/tools/gen_import_stubs.py
          ${BC}/generated/ppc/ppc_recomp_shared.h ${ROOT}/runtime ${STUBS}
  DEPENDS ${RUNTIME_SOURCES} ${ROOT}/tools/gen_import_stubs.py ${BC}/generated/ppc/ppc_recomp_shared.h)

# Audio: Xenia's XMA decoder (FFmpeg's xmaframes) with no sound device.
set(FFMPEG_DIR ${ROOT}/thirdparty/ffmpeg)
include(${FFMPEG_DIR}/sources.cmake)
add_library(bfbc1_ffmpeg STATIC ${FFMPEG_SOURCES})
target_compile_definitions(bfbc1_ffmpeg PRIVATE HAVE_AV_CONFIG_H _USE_MATH_DEFINES)
target_include_directories(bfbc1_ffmpeg PRIVATE ${FFMPEG_DIR}/xenia-config ${FFMPEG_DIR} ${FFMPEG_DIR}/libavcodec)
target_include_directories(bfbc1_ffmpeg INTERFACE ${FFMPEG_DIR}/xenia-config ${FFMPEG_DIR})
target_compile_options(bfbc1_ffmpeg PRIVATE -w -Wno-incompatible-pointer-types)
set(XE ${ROOT}/thirdparty/xenia)
file(GLOB XENIA_APU_SOURCES CONFIGURE_DEPENDS ${XE}/src/xenia/base/*.cc ${XE}/src/xenia/apu/*.cc)
list(FILTER XENIA_APU_SOURCES EXCLUDE REGEX "_win\\.cc$|system_gnulinux\\.cc$|png_utils\\.cc$|base/assert\\.cc$")
list(APPEND XENIA_APU_SOURCES ${ROOT}/runtime/xenia_shims/apu_bridge.cc ${ROOT}/runtime/xenia_shims/memory_shim.cc
     ${ROOT}/runtime/xenia_shims/profiling_stub.cc ${ROOT}/runtime/xenia_shims/linux_shim.cc)
add_library(bfbc1_xenia_apu STATIC ${XENIA_APU_SOURCES})
target_include_directories(bfbc1_xenia_apu PUBLIC ${ROOT}/runtime/xenia_shims ${XE}/src ${XE} ${XE}/third_party/fmt/include)
target_compile_definitions(bfbc1_xenia_apu PUBLIC FMT_HEADER_ONLY XXH_INLINE_ALL)
target_compile_options(bfbc1_xenia_apu PRIVATE -march=x86-64-v3 -w)
target_link_libraries(bfbc1_xenia_apu PUBLIC bfbc1_ffmpeg)

add_executable(bfbc1_server ${RUNTIME_SOURCES} ${BC_RUNTIME_SOURCES} ${DETOUR_SOURCES} ${STUBS})
target_include_directories(bfbc1_server PRIVATE ${ROOT}/runtime ${ROOT}/runtime/xenia_shims
                           ${ROOT}/thirdparty/recastnavigation/Detour/Include)
target_compile_options(bfbc1_server PRIVATE -march=x86-64-v3 -fno-strict-aliasing)
target_link_libraries(bfbc1_server PRIVATE bfbc1_ppc bfbc1_xexload bfbc1_xenia_apu Threads::Threads ${CMAKE_DL_LIBS})
target_link_options(bfbc1_server PRIVATE -rdynamic)  # symbol names in crash stacks
# What the bots learned of each map (runtime/bot_knowledge.h), next to the exe.
add_custom_command(TARGET bfbc1_server POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${BC}/data/bot_knowledge $<TARGET_FILE_DIR:bfbc1_server>/bot_knowledge)

# ---- The game (BFBC1_CLIENT=ON): as bf1943's (its CMake's notes: X11 and
# Vulkan headers, SDL3 fetched, the shaders from tools/build_xenia_shaders.py).
option(BFBC1_CLIENT "Build the game (bfbc1) too" OFF)
if(BFBC1_CLIENT)
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
  list(FILTER XENIA_GPU_SOURCES EXCLUDE REGEX
       "_win\\.cc$|system_gnulinux\\.cc$|png_utils\\.cc$|base/assert\\.cc$|bf_fsr3\\.cc$|bf_dlss\\.cc$")
  add_library(bfbc1_xenia_gpu STATIC ${XENIA_GPU_SOURCES})
  target_include_directories(bfbc1_xenia_gpu PUBLIC ${ROOT}/runtime/xenia_shims ${XE}/src ${XE}
    ${XE}/third_party/fmt/include ${ROOT}/build/xenia_shaders
    ${ROOT}/thirdparty/VulkanMemoryAllocator/include ${ROOT}/thirdparty/fidelityfx/include
    ${ROOT}/thirdparty/dlss/include)
  target_compile_definitions(bfbc1_xenia_gpu PUBLIC FMT_HEADER_ONLY XXH_INLINE_ALL VK_NO_PROTOTYPES BF1943_CLIENT)
  target_compile_options(bfbc1_xenia_gpu PRIVATE -march=x86-64-v3 -w)
  target_link_libraries(bfbc1_xenia_gpu PUBLIC bfbc1_ffmpeg SDL3::SDL3-static X11::X11 X11::X11_xcb X11::xcb)

  # The PC menus, key icons and FPS counter too (posix_gdiplus.cpp draws their
  # GDI+; stb_truetype; the fonts copied next to the exe).
  add_executable(bfbc1 ${RUNTIME_SOURCES} ${PC_UI_SOURCES} ${BC_RUNTIME_SOURCES} ${BC}/runtime/bc_mp_menu.cpp
                 ${DETOUR_SOURCES} ${STUBS})
  target_include_directories(bfbc1 PRIVATE ${ROOT}/runtime ${ROOT}/runtime/xenia_shims
                             ${ROOT}/thirdparty/recastnavigation/Detour/Include ${ROOT}/thirdparty/stb)
  add_custom_command(TARGET bfbc1 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${ROOT}/data/fonts $<TARGET_FILE_DIR:bfbc1>/fonts)
  target_compile_definitions(bfbc1 PRIVATE BF1943_CLIENT)
  target_compile_options(bfbc1 PRIVATE -march=x86-64-v3 -fno-strict-aliasing)
  target_link_libraries(bfbc1 PRIVATE bfbc1_ppc bfbc1_xexload bfbc1_xenia_gpu Threads::Threads ${CMAKE_DL_LIBS})
  target_link_options(bfbc1 PRIVATE -rdynamic)
  add_custom_command(TARGET bfbc1 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${BC}/data/pbr_shaders.txt $<TARGET_FILE_DIR:bfbc1>/pbr_shaders.txt)
  add_custom_command(TARGET bfbc1 POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${BC}/data/bot_knowledge $<TARGET_FILE_DIR:bfbc1>/bot_knowledge)
endif()
