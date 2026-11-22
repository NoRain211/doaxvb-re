#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the game from an ISO: ./build_game.sh /path/to/game.iso. launcher.sh's
# Build page runs this too.
set -euo pipefail
DOAXBV_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ "$(uname -s)" == Linux ]]; then
    # SteamOS has no compiler, so its script builds in a container.
    if grep -qx 'ID=steamos' /etc/os-release 2>/dev/null; then
        exec "${DOAXBV_ROOT}/build_steamdeck.sh" "$@"
    fi
    # The tools build_game.py and tools/build_prerequisites.py find_toolchain need.
    missing=()
    for tool in git cmake; do
        command -v "${tool}" >/dev/null || missing+=("${tool}")
    done
    { command -v c++ || command -v clang++ || command -v g++; } >/dev/null || missing+=("a C++ compiler")
    python3 -c "import ensurepip, venv" 2>/dev/null || missing+=("Python's venv")
    if (( ${#missing[@]} )); then
        echo "Missing build tools: ${missing[*]}." >&2
        if [[ ! -t 0 ]]; then
            echo "Run ./install_deps.sh in a terminal, then build again." >&2
            exit 1
        fi
        echo "Installing them with ./install_deps.sh..." >&2
        "${DOAXBV_ROOT}/install_deps.sh"
    fi
fi
source "${DOAXBV_ROOT}/tools/setup_env.sh"
if [ "$#" -gt 0 ] && [[ "$1" != -* ]]; then
    exec "${PYTHON}" "${DOAXBV_ROOT}/tools/build_game.py" --iso "$@"
else
    exec "${PYTHON}" "${DOAXBV_ROOT}/tools/build_game.py" "$@"
fi
