# SPDX-License-Identifier: GPL-3.0-or-later
"""build_game.sh's Linux check for the build tools before setup."""
import itertools
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

import build_prerequisites

ROOT = Path(__file__).resolve().parents[1]
STUB = "#!/bin/sh\nexit 0\n"
COMPILERS = ("c++", "clang++", "g++")


@unittest.skipUnless(platform.system() == "Linux", "the tool check runs on Linux")
class BuildGameShTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        shutil.copy(ROOT / "build_game.sh", self.root)
        # Sourced, so its exit stops build_game.sh before build_game.py.
        (self.root / "tools").mkdir()
        (self.root / "tools/setup_env.sh").write_text(': > "${DOAXBV_ROOT}/setup-ran"; exit 1\n')
        self.bin = self.root / "bin"
        self.bin.mkdir()
        # Only the commands the check itself needs, so no host build tools are visible.
        for tool in ("dirname", "uname", "grep"):
            (self.bin / tool).symlink_to(shutil.which(tool))

    def run_build(self, *tools):
        for tool in ("git", "cmake", "python3", *COMPILERS):
            (self.bin / tool).unlink(missing_ok=True)
        (self.root / "setup-ran").unlink(missing_ok=True)
        for tool in tools:
            (self.bin / tool).write_text(STUB)
            (self.bin / tool).chmod(0o755)
        result = subprocess.run([shutil.which("bash"), str(self.root / "build_game.sh"), "/x.iso"],
                                env={"PATH": str(self.bin)}, stdin=subprocess.DEVNULL,
                                capture_output=True, text=True)
        return result, (self.root / "setup-ran").exists()

    def test_without_a_terminal_names_the_missing_tools_and_stops(self):
        result, setup = self.run_build()
        self.assertEqual((result.returncode, setup), (1, False))
        self.assertIn("Missing build tools: git cmake a C++ compiler Python's venv.", result.stderr)
        self.assertIn("Run ./install_deps.sh in a terminal", result.stderr)

    def test_accepts_exactly_the_compilers_find_toolchain_does(self):
        for count in range(len(COMPILERS) + 1):
            for compilers in itertools.combinations(COMPILERS, count):
                with self.subTest(compilers=compilers):
                    result, setup = self.run_build("git", "cmake", "python3", *compilers)
                    with mock.patch.dict(os.environ, {"PATH": str(self.bin)}):
                        try:
                            build_prerequisites.find_toolchain(self.root)
                            found = True
                        except ValueError:
                            found = False
                    self.assertEqual(setup, found)
                    if not found:
                        self.assertIn("Missing build tools: a C++ compiler.", result.stderr)


if __name__ == "__main__":
    unittest.main()
