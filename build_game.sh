#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV="${SCRIPT_DIR}/private/venv"
PYTHON="${VENV}/bin/python3"
# Homebrew and Ubuntu Pythons refuse pip install, so setup keeps Capstone in its own venv.
if ! "${PYTHON}" -c "import importlib.metadata as m, sys; sys.exit(m.version('capstone') != '5.0.9')" 2>/dev/null; then
    echo "Installing Capstone 5.0.9 into private/venv..."
    if ! "${PYTHON}" -c "" 2>/dev/null; then
        python3 -m venv --clear "${VENV}"
    fi
    "${PYTHON}" -m pip install --disable-pip-version-check --require-hashes \
        -r "${SCRIPT_DIR}/tools/capstone-requirements.txt"
fi
if [ "$#" -gt 0 ] && [[ "$1" != -* ]]; then
    exec "${PYTHON}" "${SCRIPT_DIR}/tools/build_game.py" --iso "$@"
else
    exec "${PYTHON}" "${SCRIPT_DIR}/tools/build_game.py" "$@"
fi
