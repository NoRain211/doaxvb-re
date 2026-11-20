# SPDX-License-Identifier: GPL-3.0-or-later
"""Build the macOS and Linux launcher on its own, before any game build.

launcher.sh runs this first, so the launcher opens before the game exists and
its Build page can run that build. It uses the compiler and SDL3 that the game
build uses (see build_game.unix_cmake_args) and rebuilds only when needed.
"""
import json
from pathlib import Path
import shutil
import subprocess
import sys

import build_game
from build_prerequisites import ensure_prerequisites

BUILD = Path("private/launcher-build")


def configure_needed(build, wanted):
    """True when the build folder is missing, was configured with other arguments, or moved."""
    try:
        return json.loads((build / "doaxbv-configure.json").read_text(encoding="utf-8")) != wanted
    except (OSError, ValueError):
        return True


def run(command, log):
    with log.open("w", encoding="utf-8") as output:
        result = subprocess.run([str(part) for part in command], stdout=output, stderr=subprocess.STDOUT)
    if result.returncode != 0:
        tail = log.read_text(encoding="utf-8", errors="replace").splitlines()[-20:]
        raise ValueError("\n".join(tail + [f"Launcher build failed; see {log}"]))


def build_launcher(root=build_game.ROOT):
    if sys.platform == "win32":
        raise ValueError("Windows uses Launcher.cmd; this launcher is for macOS and Linux.")
    compiler = ensure_prerequisites(root)
    generator, args, _ = build_game.unix_cmake_args(compiler)
    build = root / BUILD
    # The folder path is part of the key: CMake refuses a build folder that moved.
    wanted = {"source": str(root.resolve()), "generator": generator, "args": args}
    if configure_needed(build, wanted):
        shutil.rmtree(build, ignore_errors=True)
        build.mkdir(parents=True)
        print("Preparing the launcher...", flush=True)
        run(["cmake", "-S", root / "tools/launcher", "-B", build, "-G", generator,
             "-DCMAKE_BUILD_TYPE=Release", *args], build / "configure.log")
        (build / "doaxbv-configure.json").write_text(json.dumps(wanted) + "\n", encoding="utf-8")
    run(["cmake", "--build", build, "--target", "recomp_launcher"], build / "build.log")
    return build / "recomp_launcher"


if __name__ == "__main__":
    try:
        build_launcher()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Cannot build the launcher: {error}", file=sys.stderr)
        sys.exit(1)
