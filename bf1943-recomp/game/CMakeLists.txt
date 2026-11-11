# Recompiled guest code (generated/ppc, proprietary, not in git). Built with
# clang-cl: XenonRecomp output relies on clang extensions (__int128, musttail,
# vector builtins). Configure with -DCMAKE_CXX_COMPILER=clang-cl.
cmake_minimum_required(VERSION 3.25)
option(BF_LTO "ThinLTO for the guest code and runtime" ON)
if(BF_LTO OR BF_PGO)
  # LLVM's linker and librarian understand the LTO bitcode objects.
  set(CMAKE_LINKER "C:/Program Files/LLVM/bin/lld-link.exe" CACHE FILEPATH "" FORCE)
  set(CMAKE_AR "C:/Program Files/LLVM/bin/llvm-lib.exe" CACHE FILEPATH "" FORCE)
endif()
project(bf1943_game LANGUAGES C CXX)
file(GLOB BF_CLANG_RT_DIR "C:/Program Files/LLVM/lib/clang/*/lib/windows")
set(CMAKE_CXX_STANDARD 20)
set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
# Guest functions runtime/ does not replace become inlinable (tools/strong_funcs.py).
option(BF_STRONG_FUNCS "Make unpatched guest functions inlinable" ON)
if(BF_STRONG_FUNCS)
  set(BF_STRONG_ARG "")
else()
  set(BF_STRONG_ARG --all-weak)
endif()
execute_process(COMMAND ${Python3_EXECUTABLE} ${ROOT}/tools/strong_funcs.py
                        ${ROOT}/generated/ppc ${ROOT}/runtime ${BF_STRONG_ARG} COMMAND_ERROR_IS_FATAL ANY)
file(GLOB PPC_SOURCES CONFIGURE_DEPENDS ${ROOT}/generated/ppc/*.cpp)
if(NOT PPC_SOURCES)
  message(FATAL_ERROR "generated/ppc is empty: run XenonRecomp first (see README)")
endif()
add_library(bf1943_ppc STATIC ${PPC_SOURCES})
target_include_directories(bf1943_ppc PUBLIC
  ${ROOT}/generated/ppc
  ${ROOT}/thirdparty/XenonRecomp/XenonUtils
  ${ROOT}/thirdparty/XenonRecomp/thirdparty/simde)
target_compile_options(bf1943_ppc PRIVATE -march=x86-64-v3 -fno-strict-aliasing -Wno-unused-variable -Wno-unused-label /GS-
  "/FI${CMAKE_CURRENT_SOURCE_DIR}/ppc_overrides.h"
  /Z7 -gline-tables-only)  # line tables: stack traces point into generated/ppc

# XEX loading (decrypt/decompress) from XenonUtils (MIT), built with our flags.
set(XU ${ROOT}/thirdparty/XenonRecomp/XenonUtils)
set(XT ${ROOT}/thirdparty/XenonRecomp/thirdparty)
add_library(bf1943_xexload STATIC
  ${XU}/xex.cpp ${XU}/image.cpp ${XU}/xex_patcher.cpp ${XU}/memory_mapped_file.cpp
  ${XT}/libmspack/libmspack/mspack/lzxd.c ${XT}/tiny-AES-c/aes.c)
set_target_properties(bf1943_xexload PROPERTIES LINKER_LANGUAGE CXX)
target_compile_definitions(bf1943_xexload PRIVATE NOMINMAX)
target_include_directories(bf1943_xexload PUBLIC ${XU} ${XT}/simde
  PRIVATE ${XT}/libmspack/libmspack/mspack ${XT}/tiny-AES-c ${XT}/TinySHA1)

# Import stubs for everything runtime/ does not implement yet (build folder only).
file(GLOB RUNTIME_SOURCES CONFIGURE_DEPENDS ${ROOT}/runtime/*.cpp)
set(STUBS ${CMAKE_CURRENT_BINARY_DIR}/import_stubs.cpp)
add_custom_command(OUTPUT ${STUBS}
  COMMAND Python3::Interpreter ${ROOT}/tools/gen_import_stubs.py
          ${ROOT}/generated/ppc/ppc_recomp_shared.h ${ROOT}/runtime ${STUBS}
  DEPENDS ${RUNTIME_SOURCES} ${ROOT}/tools/gen_import_stubs.py ${ROOT}/generated/ppc/ppc_recomp_shared.h)

# The bots' navigation meshes (runtime/bots/nav.cpp, tools/navgen) are read and routed with Detour.
file(GLOB DETOUR_SOURCES CONFIGURE_DEPENDS ${ROOT}/thirdparty/recastnavigation/Detour/Source/*.cpp)
add_executable(bf1943 ${RUNTIME_SOURCES} ${ROOT}/runtime/bots/brain.cpp ${ROOT}/runtime/bots/nav.cpp ${DETOUR_SOURCES}
               ${STUBS})
target_include_directories(bf1943 PRIVATE ${ROOT}/thirdparty/recastnavigation/Detour/Include)
target_include_directories(bf1943 PRIVATE ${ROOT}/runtime)
# The bot brain alone, for reloading while a match runs (BF_BOT_BRAIN): built
# only when asked for (cmake --build build/game --target bot_brain).
add_library(bot_brain SHARED EXCLUDE_FROM_ALL ${ROOT}/runtime/bots/brain.cpp)
target_compile_definitions(bot_brain PRIVATE BF_BOT_BRAIN_DLL _CRT_SECURE_NO_WARNINGS)
target_compile_options(bot_brain PRIVATE -march=x86-64-v3)
target_compile_options(bf1943 PRIVATE -march=x86-64-v3 -fno-strict-aliasing)
target_link_libraries(bf1943 PRIVATE bf1943_ppc bf1943_xexload xenia_gpu xinput dbghelp dxgi d3d11 shcore dwmapi winmm bcrypt ws2_32 synchronization)
target_compile_options(bf1943 PRIVATE /Z7)
target_link_options(bf1943 PRIVATE /DEBUG /OPT:REF /OPT:ICF)
# ThinLTO across the guest code and the runtime (needs lld-link and llvm-lib,
# set before project() above).
if(BF_LTO)
  target_compile_options(bf1943_ppc PRIVATE -flto=thin)
  target_compile_options(bf1943 PRIVATE -flto=thin)
  target_link_options(bf1943 PRIVATE /opt:lldltojobs=all)
endif()
# Profile-guided optimisation: BF_PGO=gen builds an instrumented exe that writes
# bf1943.profraw on exit; merge with llvm-profdata into game/bf1943.profdata,
# then BF_PGO=use builds with it.
set(BF_PGO "" CACHE STRING "Profile-guided optimisation: gen, use or empty")
if(BF_PGO STREQUAL "gen")
  target_compile_options(bf1943_ppc PRIVATE -fprofile-instr-generate)
  target_compile_options(bf1943 PRIVATE -fprofile-instr-generate)
  target_link_options(bf1943 PRIVATE "/LIBPATH:${BF_CLANG_RT_DIR}" clang_rt.profile-x86_64.lib)
elseif(BF_PGO STREQUAL "use")
  target_compile_options(bf1943_ppc PRIVATE "-fprofile-instr-use=${CMAKE_CURRENT_SOURCE_DIR}/bf1943.profdata"
                         -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date)
  target_compile_options(bf1943 PRIVATE "-fprofile-instr-use=${CMAKE_CURRENT_SOURCE_DIR}/bf1943.profdata"
                         -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date)
endif()
include(${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg.cmake)
include(${CMAKE_CURRENT_SOURCE_DIR}/xenia_gpu.cmake)
# --pbr reads its shader table from next to the exe.
add_custom_command(TARGET bf1943 POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different ${ROOT}/data/pbr_shaders.txt $<TARGET_FILE_DIR:bf1943>/pbr_shaders.txt)
add_custom_command(TARGET bf1943 POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${ROOT}/data/fonts $<TARGET_FILE_DIR:bf1943>/fonts)
