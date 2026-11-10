#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# macOS and Linux counterpart of Launcher.cmd. Builds the launcher on its own
# the first time (tools/build_launcher.py), then opens it: its Build page
# builds the game from an ISO, and Play saves the settings to
# private/launcher.json and starts run_game.sh, which applies them.
set -euo pipefail
DOAXBV_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${DOAXBV_ROOT}/tools/setup_env.sh"
# Progress stays visible: a first run may download SDL headers or build SDL3.
"${PYTHON}" "${DOAXBV_ROOT}/tools/build_launcher.py"
# The launcher exits 0 for Play and 1 when closed.
if "${DOAXBV_ROOT}/private/launcher-build/recomp_launcher" "${DOAXBV_ROOT}" "$@"; then
    exec "${DOAXBV_ROOT}/run_game.sh"
fi
