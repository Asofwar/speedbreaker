#!/usr/bin/env bash
# SpeedBreaker. GPL-3.0-or-later (see COPYING).
#
#   scripts/windows/deps.sh <prefix>
#
# The Windows build's libraries, cross-compiled on Linux (clang, mingw-w64,
# lld, cmake, ninja, git, curl; Arch: pacman -S mingw-w64-gcc clang lld) and
# installed into <prefix>: SDL3, Vulkan's headers and the import library of
# vulkan-1.dll (the loader itself comes with the graphics driver), glslang
# with SPIRV-Tools' optimizer, all static, and FFmpeg for XMA audio in
# <prefix>/ffmpeg-xma. The versions are those of the Linux build. What is
# already there is kept: delete <prefix> to build again.
set -euo pipefail
[ $# -eq 1 ] || { echo "usage: $0 <prefix>" >&2; exit 2; }
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
mkdir -p "$1"
P="$(cd "$1" && pwd)"
S="$P/src"
B="$P/build"
mkdir -p "$S" "$B"
VK=vulkan-sdk-1.4.363.0
SDL=release-3.4.18
FFMPEG_VERSION=7.1.1
FFMPEG_SHA256=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1
TC=(-G Ninja -DCMAKE_TOOLCHAIN_FILE="$ROOT/scripts/windows/mingw-toolchain.cmake" -DSB_WIN_DEPS="$P"
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$P" -DBUILD_SHARED_LIBS=OFF)

fetch() {  # fetch <name> <repository> <tag>
    [ -d "$S/$1" ] || git -c advice.detachedHead=false clone -q --depth 1 --branch "$3" "$2" "$S/$1"
}
build() {  # build <name> [cmake options...]
    local name="$1"
    shift
    cmake -S "$S/$name" -B "$B/$name" "${TC[@]}" "$@" > "$B/$name.configure.log"
    ninja -C "$B/$name" > "$B/$name.build.log"
    cmake --install "$B/$name" > /dev/null
}

if [ ! -f "$P/include/vulkan/vulkan.h" ]; then
    fetch Vulkan-Headers https://github.com/KhronosGroup/Vulkan-Headers.git "$VK"
    build Vulkan-Headers
fi
if [ ! -f "$P/lib/libvulkan-1.a" ]; then
    fetch Vulkan-Loader https://github.com/KhronosGroup/Vulkan-Loader.git "$VK"
    mkdir -p "$P/lib"
    x86_64-w64-mingw32-dlltool -d "$S/Vulkan-Loader/loader/vulkan-1.def" -l "$P/lib/libvulkan-1.a" -D vulkan-1.dll
fi
if [ ! -f "$P/lib/libSDL3.a" ]; then
    fetch SDL https://github.com/libsdl-org/SDL.git "$SDL"
    build SDL -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_EXAMPLES=OFF -DSDL_VULKAN=ON
fi
if [ ! -f "$P/lib/libSPIRV-Tools-opt.a" ]; then
    fetch SPIRV-Headers https://github.com/KhronosGroup/SPIRV-Headers.git "$VK"
    fetch SPIRV-Tools https://github.com/KhronosGroup/SPIRV-Tools.git "$VK"
    build SPIRV-Headers
    build SPIRV-Tools -DSPIRV-Headers_SOURCE_DIR="$S/SPIRV-Headers" -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON \
        -DSPIRV_WERROR=OFF
fi
if [ ! -f "$P/lib/libglslang.a" ]; then
    fetch glslang https://github.com/KhronosGroup/glslang.git "$VK"
    build glslang -DALLOW_EXTERNAL_SPIRV_TOOLS=ON -DENABLE_OPT=ON -DGLSLANG_TESTS=OFF -DENABLE_GLSLANG_BINARIES=OFF \
        -DENABLE_SPVREMAPPER=OFF -DENABLE_HLSL=OFF
fi
if [ ! -f "$P/ffmpeg-xma/lib/libavcodec.a" ]; then
    tarball="$S/ffmpeg-$FFMPEG_VERSION.tar.xz"
    if [ ! -f "$tarball" ]; then
        curl -sfL -o "$tarball.part" "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz"
        echo "$FFMPEG_SHA256  $tarball.part" | sha256sum -c --quiet
        mv "$tarball.part" "$tarball"
    fi
    "$ROOT/scripts/build_ffmpeg_xma.sh" "$tarball" "$P/ffmpeg-xma" --enable-cross-compile --target-os=mingw32 \
        --arch=x86_64 --cross-prefix=x86_64-w64-mingw32- --cc="clang --target=x86_64-w64-mingw32" --disable-x86asm
fi
echo "deps.sh: the Windows libraries are in $P"
