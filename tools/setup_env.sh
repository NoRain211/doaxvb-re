# SPDX-License-Identifier: GPL-3.0-or-later
# Sourced by build_game.sh and launcher.sh: prepare private/venv with Capstone.
# The caller sets DOAXBV_ROOT; this sets PYTHON.
VENV="${DOAXBV_ROOT}/private/venv"
PYTHON="${VENV}/bin/python3"

# A venv records its own path, so one from before the game folder moved is rebuilt.
if [[ -f "${VENV}/doaxbv-root" && "$(cat "${VENV}/doaxbv-root")" != "${DOAXBV_ROOT}" ]]; then
    echo "The game folder moved; recreating private/venv..."
    rm -rf "${VENV}"
fi
# Homebrew and Ubuntu Pythons refuse pip install, so setup keeps Capstone in its own venv.
# -I keeps the caller's folder and PYTHON* variables out of these checks.
if ! "${PYTHON}" -I -c "import capstone, importlib.metadata as m, sys; sys.exit(m.version('capstone') != '5.0.9')" 2>/dev/null; then
    echo "Installing Capstone 5.0.9 into private/venv..."
    if ! "${PYTHON}" -I -m pip --version >/dev/null 2>&1; then
        python3 -m venv --clear "${VENV}"
    fi
    "${PYTHON}" -I -m pip install --disable-pip-version-check --force-reinstall --require-hashes \
        -r "${DOAXBV_ROOT}/tools/capstone-requirements.txt"
fi
printf '%s\n' "${DOAXBV_ROOT}" > "${VENV}/doaxbv-root"
