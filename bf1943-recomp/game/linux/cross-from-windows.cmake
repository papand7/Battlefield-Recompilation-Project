# Build the Linux server on Windows: clang + lld (LLVM for Windows) and a Debian
# sysroot made by tools/fetch_linux_sysroot.py.
#   python tools/fetch_linux_sysroot.py build/linux-sysroot
#   cmake -S game/linux -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_TOOLCHAIN_FILE=game/linux/cross-from-windows.cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
if(NOT BF_LINUX_SYSROOT)
  set(BF_LINUX_SYSROOT "${CMAKE_CURRENT_LIST_DIR}/../../build/linux-sysroot")
endif()
set(CMAKE_SYSROOT "${BF_LINUX_SYSROOT}")
set(CMAKE_C_COMPILER "C:/Program Files/LLVM/bin/clang.exe")
set(CMAKE_CXX_COMPILER "C:/Program Files/LLVM/bin/clang++.exe")
set(CMAKE_C_COMPILER_TARGET x86_64-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET x86_64-linux-gnu)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
