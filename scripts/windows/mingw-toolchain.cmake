# SpeedBreaker. GPL-3.0-or-later (see COPYING).
#
# Windows x86-64 from Linux: clang for the code, mingw-w64 (its GCC's
# headers, CRT, libstdc++ and winpthreads) for the rest, lld to link.
# scripts/windows/build.sh passes it; SB_WIN_DEPS is the prefix that
# scripts/windows/deps.sh installed SDL3, Vulkan's headers and import
# library, glslang and SPIRV-Tools into.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET x86_64-w64-mingw32)
set(CMAKE_CXX_COMPILER_TARGET x86_64-w64-mingw32)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
set(CMAKE_AR x86_64-w64-mingw32-ar)
set(CMAKE_RANLIB x86_64-w64-mingw32-ranlib)
# lld for every link: GNU ld doesn't resolve the generated code's weak
# aliases in COFF. Static C++ and threads (no libstdc++ or winpthread DLL
# to ship). A DLL (SPIRV-Tools builds one) needs winpthread named for
# libstdc++; not a program: ahead of its objects it would bring in
# winpthreads' nanosleep before runtime/compat/win/win_sleep.c's.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld -static")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld -static -lwinpthread")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld -static -lwinpthread")
set(SB_WIN_DEPS "$ENV{SB_WIN_DEPS}" CACHE PATH "scripts/windows/deps.sh's install prefix")
set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32 ${SB_WIN_DEPS})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
