#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Usage: ./RunGame.sh [private/setup-*/build-receipt.json]
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="$root/private/linux-deps/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$root/private/linux-deps/venv/bin/python" "$root/tools/run_game.py" "$@"
