#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VENV="${SCRIPT_DIR}/private/venv"
PYTHON="${VENV}/bin/python3"
# Homebrew and Ubuntu Pythons refuse pip install, so setup keeps Capstone in its own venv.
# -I keeps the caller's folder and PYTHON* variables out of these checks.
if ! "${PYTHON}" -I -c "import capstone, importlib.metadata as m, sys; sys.exit(m.version('capstone') != '5.0.9')" 2>/dev/null; then
    echo "Installing Capstone 5.0.9 into private/venv..."
    if ! "${PYTHON}" -I -m pip --version >/dev/null 2>&1; then
        python3 -m venv --clear "${VENV}"
    fi
    "${PYTHON}" -I -m pip install --disable-pip-version-check --force-reinstall --require-hashes \
        -r "${SCRIPT_DIR}/tools/capstone-requirements.txt"
fi
if [ "$#" -gt 0 ] && [[ "$1" != -* ]]; then
    exec "${PYTHON}" "${SCRIPT_DIR}/tools/build_game.py" --iso "$@"
else
    exec "${PYTHON}" "${SCRIPT_DIR}/tools/build_game.py" "$@"
fi
