#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the pinned Linux runner dependencies into private/linux-deps.
# Needs a compiler, CMake, Git, Python 3.12+ and distro dev headers; see docs/linux.md.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
prefix="$root/private/linux-deps"
src="$prefix/src"
mkdir -p "$src"
jobs="${DOAXBV_JOBS:-2}"

fetch() {  # url sha256 file
    [ -f "$src/$3" ] || curl --fail --location --retry 3 "$1" --output "$src/$3.part"
    [ -f "$src/$3" ] || mv "$src/$3.part" "$src/$3"
    echo "$2  $src/$3" | sha256sum -c --quiet -
}

if [ ! -f "$prefix/lib/cmake/SDL3/SDL3Config.cmake" ] && [ ! -f "$prefix/lib64/cmake/SDL3/SDL3Config.cmake" ]; then
    echo "== SDL 3.4.16"
    fetch https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz \
        7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68 SDL3-3.4.16.tar.gz
    tar -xzf "$src/SDL3-3.4.16.tar.gz" -C "$src"
    cmake -S "$src/SDL3-3.4.16" -B "$src/sdl-build" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib \
        -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_TEST_LIBRARY=OFF
    cmake --build "$src/sdl-build" --parallel "$jobs"
    cmake --install "$src/sdl-build"
fi

if [ ! -f "$prefix/lib/pkgconfig/dxvk-d3d11.pc" ]; then
    echo "== DXVK Native 3.1.1"
    fetch https://github.com/doitsujin/dxvk/releases/download/v3.1.1/dxvk-native-3.1.1-steamrt-sniper.tar.gz \
        506fa1ea5bb6ce9a89e11436f7ab5191bc0196e8a577e0cf0b16a45bbc9991b7 dxvk-native-3.1.1.tar.gz
    tar -xzf "$src/dxvk-native-3.1.1.tar.gz" -C "$prefix" --strip-components=1
    # The release's pkg-config files name its build prefix; point them here.
    sed -i "s|^prefix=.*|prefix=$prefix|" "$prefix"/lib/pkgconfig/dxvk-*.pc
fi

if [ ! -f "$prefix/lib/pkgconfig/libvkd3d-shader.pc" ]; then
    echo "== vkd3d-shader 1.17"
    fetch https://dl.winehq.org/vkd3d/source/vkd3d-1.17.tar.xz \
        bc61cb9e84d5045cbcaffbdd707940d399d8bf62874663dfe5809a0bfb87e9b6 vkd3d-1.17.tar.xz
    tar -xJf "$src/vkd3d-1.17.tar.xz" -C "$src"
    (cd "$src/vkd3d-1.17" && ./configure --prefix="$prefix" --libdir="$prefix/lib" \
        --disable-tests --disable-demos --disable-doxygen-doc && make -j"$jobs" && make install)
fi

if [ ! -x "$prefix/bin/extract-xiso" ]; then
    echo "== extract-xiso build-202505152050"
    [ -d "$src/extract-xiso" ] || git clone --quiet https://github.com/XboxDev/extract-xiso.git "$src/extract-xiso"
    git -C "$src/extract-xiso" checkout --quiet --detach b72e5b60d598ec6df80534cda19cdcd4361aa18c
    cmake -S "$src/extract-xiso" -B "$src/xiso-build" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$src/xiso-build" --parallel "$jobs"
    install -D "$src/xiso-build/extract-xiso" "$prefix/bin/extract-xiso"
fi

if [ ! -x "$prefix/venv/bin/python" ]; then
    echo "== Python venv with Capstone 5.0.9"
    python3 -c 'import sys; sys.exit(sys.version_info < (3, 12))' ||
        { echo "Python 3.12 or newer is required" >&2; exit 1; }
    python3 -m venv "$prefix/venv"
    "$prefix/venv/bin/python" -m pip install --quiet capstone==5.0.9
fi
echo "Linux dependencies ready in $prefix"
