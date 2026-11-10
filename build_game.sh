#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the game from an ISO: ./build_game.sh /path/to/game.iso. launcher.sh's
# Build page runs this too.
set -euo pipefail
DOAXBV_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${DOAXBV_ROOT}/tools/setup_env.sh"
if [ "$#" -gt 0 ] && [[ "$1" != -* ]]; then
    exec "${PYTHON}" "${DOAXBV_ROOT}/tools/build_game.py" --iso "$@"
else
    exec "${PYTHON}" "${DOAXBV_ROOT}/tools/build_game.py" "$@"
fi
