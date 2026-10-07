#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Usage: ./BuildGame.sh /path/to/game.iso
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
[ $# -eq 1 ] || { echo "Usage: $0 /path/to/game.iso" >&2; exit 1; }
iso="$(realpath "$1")"
"$root/tools/setup_linux_deps.sh"
deps="$root/private/linux-deps"
export PATH="$deps/bin:$PATH"
export PKG_CONFIG_PATH="$deps/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export CMAKE_PREFIX_PATH="$deps${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
"$deps/venv/bin/python" "$root/tools/build_game.py" --iso "$iso"
echo "Build complete. Start the game with ./RunGame.sh"
