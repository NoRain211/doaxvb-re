#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the prebuilt launcher the Linux ZIP ships, so a machine with no
# compiler (a stock Steam Deck) can still open it. It compiles
# tools/launcher_sdl.cpp in an Ubuntu 22.04 container against a static SDL3 and
# a static libstdc++, which keeps its glibc requirement at 2.35. Needs Docker.
#   tools/build_linux_launcher.sh [output]    (default private/release-assets/launcher-linux-x86_64)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-${ROOT}/private/release-assets/launcher-linux-x86_64}"
mkdir -p "$(dirname "${OUT}")"
# Git Bash on Windows hands Docker Windows paths, and must not rewrite the container's.
if command -v cygpath >/dev/null 2>&1; then
    export MSYS_NO_PATHCONV=1
    SRC="$(cygpath -m "${ROOT}/tools")"
    DEST="$(cygpath -m "$(dirname "${OUT}")")"
else
    SRC="${ROOT}/tools"
    DEST="$(dirname "${OUT}")"
fi
# SDL3 and stb_image match recomp-runtime/cmake/RecompSDL3.cmake and tools/launcher/CMakeLists.txt.
docker run --rm --platform linux/amd64 -v "${SRC}:/src:ro" -v "${DEST}:/out" -e NAME="$(basename "${OUT}")" \
    ubuntu:22.04 bash -euxc '
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends build-essential cmake ninja-build pkg-config curl ca-certificates \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev libxss-dev libxtst-dev \
    libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev libdrm-dev libgbm-dev libegl-dev libgl-dev \
    libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev libdbus-1-dev libusb-1.0-0-dev
cd /tmp
curl -fL -o sdl.tar.gz https://github.com/libsdl-org/SDL/releases/download/release-3.4.12/SDL3-3.4.12.tar.gz
echo "f07b958a9ac5020fb7a44cadb957f658b2149c3c8abb4f63145fac9303249db7  sdl.tar.gz" | sha256sum -c -
curl -fL -o stb_image.h \
    https://raw.githubusercontent.com/nothings/stb/013ac3beddff3dbffafd5177e7972067cd2b5083/stb_image.h
echo "594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3  stb_image.h" | sha256sum -c -
tar xzf sdl.tar.gz
cmake -S SDL3-3.4.12 -B sdl-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/opt/sdl \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_TESTS=OFF
cmake --build sdl-build
cmake --install sdl-build
g++ -O2 -std=c++17 -o "/out/$NAME" /src/launcher_sdl.cpp -I/opt/sdl/include -isystem /tmp \
    -static-libstdc++ -static-libgcc $(PKG_CONFIG_PATH=/opt/sdl/lib/pkgconfig pkg-config --static --libs sdl3)
strip "/out/$NAME"
objdump -p "/out/$NAME" | grep NEEDED
objdump -T "/out/$NAME" | grep -o "GLIBC_[0-9.]*" | sort -uV | tail -1
'
echo "Built ${OUT}"
