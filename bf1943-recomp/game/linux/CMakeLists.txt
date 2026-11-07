# Linux console server (bf1943_server): the recompiled game and the runtime,
# without GPU, audio, window or PC menus (runtime/linux_server.cpp has
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
list(REMOVE_ITEM RUNTIME_SOURCES
  ${ROOT}/runtime/pc_settings.cpp ${ROOT}/runtime/pc_menu.cpp ${ROOT}/runtime/pc_ui.cpp)
set(STUBS ${CMAKE_CURRENT_BINARY_DIR}/import_stubs.cpp)
add_custom_command(OUTPUT ${STUBS}
  COMMAND Python3::Interpreter ${ROOT}/tools/gen_import_stubs.py
          ${ROOT}/generated/ppc/ppc_recomp_shared.h ${ROOT}/runtime ${STUBS}
  DEPENDS ${RUNTIME_SOURCES} ${ROOT}/tools/gen_import_stubs.py ${ROOT}/generated/ppc/ppc_recomp_shared.h)

add_executable(bf1943_server ${RUNTIME_SOURCES} ${STUBS})
target_include_directories(bf1943_server PRIVATE ${ROOT}/runtime ${ROOT}/runtime/xenia_shims)
target_compile_options(bf1943_server PRIVATE -march=x86-64-v3 -fno-strict-aliasing)
target_link_libraries(bf1943_server PRIVATE bf1943_ppc bf1943_xexload Threads::Threads ${CMAKE_DL_LIBS})
target_link_options(bf1943_server PRIVATE -rdynamic)  # symbol names in crash stacks
