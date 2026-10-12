#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# macOS and Linux counterpart of Launcher.cmd: pick resolution, anti-aliasing,
# volume and music options (saved to private/launcher.json), then start
# run_game.sh, which applies those saved settings.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Linux builds its launcher beside the runner (tools/launcher_sdl.cpp).
if [[ "$(uname -s)" != "Darwin" ]]; then
    exec "${ROOT}/run_game.sh" --launcher "$@"
fi

SOURCE="${ROOT}/tools/launcher_macos.swift"
BINARY="${ROOT}/private/launcher_macos"

# Built on first use (and after edits) with the Xcode command line tools.
if [[ ! -x "${BINARY}" || "${SOURCE}" -nt "${BINARY}" ]]; then
    if ! command -v swiftc >/dev/null 2>&1; then
        echo "The launcher needs the Xcode command line tools: xcode-select --install" >&2
        echo "Or edit private/launcher.json and run ./run_game.sh directly." >&2
        exit 1
    fi
    mkdir -p "${ROOT}/private"
    swiftc -O "${SOURCE}" -o "${BINARY}"
fi

if "${BINARY}" "${ROOT}"; then
    exec "${ROOT}/run_game.sh"
else
    exit 1
fi
