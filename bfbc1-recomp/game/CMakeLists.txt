# Battlefield: Bad Company. Guest code from generated/ppc (not in git); the
# runtime, Xenia GPU, FFmpeg, XenonUtils and tools are shared with ../bf1943-recomp.
# Built with clang-cl like bf1943 (see game/build.bat).
cmake_minimum_required(VERSION 3.25)
project(bfbc1_game LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(BC ${CMAKE_CURRENT_SOURCE_DIR}/..)
# Shared code (the included .cmake files use ROOT): next to this tree in the
# repo; the launcher's setup passes -DROOT=<release>/src.
if(NOT ROOT)
  set(ROOT ${BC}/../bf1943-recomp)
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

file(GLOB PPC_SOURCES CONFIGURE_DEPENDS ${BC}/generated/ppc/*.cpp)
if(NOT PPC_SOURCES)
  message(FATAL_ERROR "generated/ppc is empty: run XenonRecomp first")
endif()
add_library(bfbc1_ppc STATIC ${PPC_SOURCES})
target_include_directories(bfbc1_ppc PUBLIC
  ${BC}/generated/ppc
  ${ROOT}/thirdparty/XenonRecomp/XenonUtils
  ${ROOT}/thirdparty/XenonRecomp/thirdparty/simde)
target_compile_options(bfbc1_ppc PRIVATE -march=x86-64-v3 -fno-strict-aliasing -Wno-unused-variable -Wno-unused-label /GS-
  "/FI${ROOT}/game/ppc_overrides.h" /Z7 -gline-tables-only)

set(XU ${ROOT}/thirdparty/XenonRecomp/XenonUtils)
set(XT ${ROOT}/thirdparty/XenonRecomp/thirdparty)
add_library(bfbc1_xexload STATIC
  ${XU}/xex.cpp ${XU}/image.cpp ${XU}/xex_patcher.cpp ${XU}/memory_mapped_file.cpp
  ${XT}/libmspack/libmspack/mspack/lzxd.c ${XT}/tiny-AES-c/aes.c)
set_target_properties(bfbc1_xexload PROPERTIES LINKER_LANGUAGE CXX)
target_compile_definitions(bfbc1_xexload PRIVATE NOMINMAX)
target_include_directories(bfbc1_xexload PUBLIC ${XU} ${XT}/simde
  PRIVATE ${XT}/libmspack/libmspack/mspack ${XT}/tiny-AES-c ${XT}/TinySHA1)

# Shared runtime minus the BF1943-only parts (its guest patches and bots);
# BFBC1's own replacements live in runtime/.
file(GLOB RUNTIME_SOURCES CONFIGURE_DEPENDS ${ROOT}/runtime/*.cpp)
list(REMOVE_ITEM RUNTIME_SOURCES ${ROOT}/runtime/game_patches.cpp ${ROOT}/runtime/bots.cpp ${ROOT}/runtime/bot_players.cpp)
file(GLOB BC_RUNTIME_SOURCES CONFIGURE_DEPENDS ${BC}/runtime/*.cpp)
# The bots' navigation meshes (tools/navgen) are read and routed with Detour.
file(GLOB DETOUR_SOURCES CONFIGURE_DEPENDS ${ROOT}/thirdparty/recastnavigation/Detour/Source/*.cpp)
list(APPEND BC_RUNTIME_SOURCES ${DETOUR_SOURCES})
set(STUBS ${CMAKE_CURRENT_BINARY_DIR}/import_stubs.cpp)
add_custom_command(OUTPUT ${STUBS}
  COMMAND Python3::Interpreter ${ROOT}/tools/gen_import_stubs.py
          ${BC}/generated/ppc/ppc_recomp_shared.h ${ROOT}/runtime ${STUBS}
  DEPENDS ${RUNTIME_SOURCES} ${ROOT}/tools/gen_import_stubs.py ${BC}/generated/ppc/ppc_recomp_shared.h)

add_executable(bfbc1 ${RUNTIME_SOURCES} ${BC_RUNTIME_SOURCES} ${STUBS})
target_include_directories(bfbc1 PRIVATE ${ROOT}/runtime ${ROOT}/thirdparty/recastnavigation/Detour/Include)
target_compile_options(bfbc1 PRIVATE -march=x86-64-v3 -fno-strict-aliasing /Z7)
target_link_libraries(bfbc1 PRIVATE bfbc1_ppc bfbc1_xexload xenia_gpu xinput dbghelp shcore dwmapi winmm bcrypt ws2_32 synchronization)
target_link_options(bfbc1 PRIVATE /DEBUG /OPT:REF /OPT:ICF)
include(${ROOT}/game/ffmpeg.cmake)
include(${ROOT}/game/xenia_gpu.cmake)
# --pbr reads its shader table from next to the exe.
add_custom_command(TARGET bfbc1 POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${BC}/data/pbr_shaders.txt $<TARGET_FILE_DIR:bfbc1>/pbr_shaders.txt)
add_custom_command(TARGET bfbc1 POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${ROOT}/data/fonts $<TARGET_FILE_DIR:bfbc1>/fonts)
# What the bots learned of each map (runtime/bot_knowledge.h, tools/bot_knowledge.py --to data/bot_knowledge).
add_custom_command(TARGET bfbc1 POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${BC}/data/bot_knowledge $<TARGET_FILE_DIR:bfbc1>/bot_knowledge)
# FSR 3 and DLSS, as bf1943's (bf_fsr3.cc, bf_dlss.cc load them at run time).
add_custom_command(TARGET bfbc1 POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ROOT}/thirdparty/fidelityfx/bin/amd_fidelityfx_loader_vk.dll
          ${ROOT}/thirdparty/fidelityfx/bin/amd_fidelityfx_upscaler_vk.dll $<TARGET_FILE_DIR:bfbc1>)
add_custom_command(TARGET bfbc1 POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ROOT}/thirdparty/dlss/bin/nvngx_dlss.dll $<TARGET_FILE_DIR:bfbc1>)
