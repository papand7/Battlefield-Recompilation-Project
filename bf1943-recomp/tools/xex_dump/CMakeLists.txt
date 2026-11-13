# The recompiler's tools, for a build from source (build_linux.sh): XenonAnalyse
# and XenonRecomp (thirdparty/XenonRecomp) and xex_dump, which
# tools/make_recomp_config.py uses to make the config from default.xex.
#   cmake -S tools/xex_dump -B build/recomp-tools -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/recomp-tools --target XenonAnalyse XenonRecomp xex_dump
cmake_minimum_required(VERSION 3.20)
project(bf1943_recomp_tools LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty/XenonRecomp XenonRecomp)
add_executable(xex_dump xex_dump.cpp)
target_link_libraries(xex_dump PRIVATE XenonUtils)
