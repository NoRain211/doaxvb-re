#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON="python3"
if [[ -f "${SCRIPT_DIR}/private/venv/bin/python3" ]]; then
    PYTHON="${SCRIPT_DIR}/private/venv/bin/python3"
fi
if [ "$#" -gt 0 ] && [[ "$1" != -* ]]; then
    exec "${PYTHON}" "${SCRIPT_DIR}/tools/build_game.py" --iso "$@"
else
    exec "${PYTHON}" "${SCRIPT_DIR}/tools/build_game.py" "$@"
fi
