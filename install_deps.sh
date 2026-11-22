#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Installs the packages build_game.sh needs on Linux: the compiler, CMake,
# Ninja, Git, Python's venv, SDL3's build dependencies and shaderc.
#   ./install_deps.sh [--print]     (--print shows the command without running it)
# On SteamOS, run ./build_steamdeck.sh instead; it runs this in a container.
set -euo pipefail
if [[ -r /etc/os-release ]] && grep -qx 'ID=steamos' /etc/os-release; then
    echo "SteamOS is read-only with no compiler. Run ./build_steamdeck.sh /path/to/game.iso," >&2
    echo "which builds in an Ubuntu 24.04 distrobox." >&2
    exit 1
fi
refresh=()
if command -v apt-get >/dev/null; then
    # Fresh or stale package lists cannot find the packages.
    refresh=(apt-get update)
    cmd=(apt-get install -y build-essential clang cmake ninja-build git pkg-config python3-venv
         libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev
         libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev libdecor-0-dev
         libasound2-dev libpulse-dev libpipewire-0.3-dev libvulkan-dev libshaderc1)
elif command -v dnf >/dev/null; then
    cmd=(dnf install -y gcc-c++ clang cmake ninja-build git pkgconf python3
         libX11-devel libXext-devel libXrandr-devel libXcursor-devel libXfixes-devel libXi-devel
         libXScrnSaver-devel libXtst-devel libxkbcommon-devel wayland-devel libdecor-devel
         alsa-lib-devel pulseaudio-libs-devel pipewire-devel vulkan-loader-devel libshaderc)
elif command -v pacman >/dev/null; then
    cmd=(pacman -S --needed --noconfirm base-devel clang cmake ninja git python
         libx11 libxext libxrandr libxcursor libxfixes libxi libxss libxtst libxkbcommon
         wayland libdecor alsa-lib libpulse pipewire vulkan-icd-loader vulkan-headers shaderc)
else
    echo "No apt, dnf or pacman found. Install the packages listed in docs/building.md by hand." >&2
    exit 1
fi
if [[ ${EUID} -ne 0 ]]; then
    cmd=(sudo "${cmd[@]}")
    if (( ${#refresh[@]} )); then refresh=(sudo "${refresh[@]}"); fi
fi
if [[ "${1:-}" == "--print" ]]; then
    if (( ${#refresh[@]} )); then echo "${refresh[*]}"; fi
    echo "${cmd[*]}"
else
    if (( ${#refresh[@]} )); then "${refresh[@]}"; fi
    "${cmd[@]}"
fi
