#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# macOS and Linux counterpart of Launcher.cmd. Builds the launcher on its own
# the first time (tools/build_launcher.py), then opens it: its Build page
# builds the game from an ISO, and Play saves the settings to
# private/launcher.json and starts run_game.sh, which applies them.
set -euo pipefail
DOAXBV_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# The Linux ZIP ships a prebuilt launcher (tools/build_linux_launcher.sh), so a
# machine with no compiler, such as a stock Steam Deck, opens it without setup.
PREBUILT="${DOAXBV_ROOT}/tools/artifacts/launcher-linux-x86_64"
LAUNCHER="${DOAXBV_ROOT}/private/launcher-build/recomp_launcher"
# The tools tools/build_prerequisites.py find_toolchain requires.
have_toolchain() {
    command -v cmake >/dev/null && { command -v c++ || command -v clang++ || command -v g++; } >/dev/null
}
if [[ "$(uname -s)" == Linux && "$(uname -m)" == x86_64 && -x "${PREBUILT}" ]]; then
    LAUNCHER="${PREBUILT}"
elif [[ -x "${LAUNCHER}" ]] && ! have_toolchain; then
    # A checkout on SteamOS has no compiler: reuse the launcher built earlier in
    # a container, without the venv setup or a rebuild.
    echo "No compiler found; using the launcher built earlier." >&2
else
    source "${DOAXBV_ROOT}/tools/setup_env.sh"
    # Progress stays visible: a first run may download SDL headers or build SDL3.
    "${PYTHON}" "${DOAXBV_ROOT}/tools/build_launcher.py"
fi
# The launcher exits 0 for Play and 1 when closed. Arguments, such as a build
# receipt, go to run_game.sh.
if "${LAUNCHER}" "${DOAXBV_ROOT}"; then
    exec "${DOAXBV_ROOT}/run_game.sh" "$@"
fi
exit 1
