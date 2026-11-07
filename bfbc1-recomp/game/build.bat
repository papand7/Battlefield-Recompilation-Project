@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0.."
cmake -S game -B build\game -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" -DCMAKE_CXX_COMPILER="C:/Program Files/LLVM/bin/clang-cl.exe" > build_game.log 2>&1
cmake --build build\game >> build_game.log 2>&1
echo build_exit=%errorlevel%
