# SPDX-License-Identifier: GPL-3.0-or-later
"""Which launcher launcher.sh opens, and when it sets up the venv and rebuilds."""
import itertools
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from unittest import mock

import build_prerequisites

ROOT = Path(__file__).resolve().parents[1]
STUB = "#!/bin/sh\nexit 0\n"
TOOLS = ("cmake", "c++", "clang++", "g++")


@unittest.skipIf(os.name == "nt", "launcher.sh runs on macOS and Linux")
class LauncherShTest(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        shutil.copy(ROOT / "launcher.sh", self.root)
        # Sourced, so its exit stops launcher.sh before build_launcher.py.
        self.write("tools/setup_env.sh", ': > "${DOAXBV_ROOT}/setup-ran"; exit 1\n')
        self.bin = self.root / "bin"
        self.bin.mkdir()
        # Only the commands launcher.sh needs, so no host cmake or compiler is visible.
        for tool in ("dirname", "uname"):
            (self.bin / tool).symlink_to(shutil.which(tool))

    def write(self, name, text, executable=False):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        if executable:
            path.chmod(0o755)

    def built_launcher(self):
        self.write("private/launcher-build/recomp_launcher", '#!/bin/sh\n: > "$1/launcher-ran"\nexit 1\n', True)

    def run_launcher(self, *tools):
        for name in (*(f"bin/{tool}" for tool in TOOLS), "setup-ran", "launcher-ran"):
            (self.root / name).unlink(missing_ok=True)
        for tool in tools:
            self.write(f"bin/{tool}", STUB, True)
        result = subprocess.run([shutil.which("bash"), str(self.root / "launcher.sh")],
                                env={"PATH": str(self.bin)}, capture_output=True, text=True)
        return result, (self.root / "setup-ran").exists(), (self.root / "launcher-ran").exists()

    def test_reuses_the_built_launcher_exactly_when_find_toolchain_fails(self):
        self.built_launcher()
        for count in range(len(TOOLS) + 1):
            for tools in itertools.combinations(TOOLS, count):
                with self.subTest(tools=tools):
                    result, setup, launcher = self.run_launcher(*tools)
                    # have_toolchain in launcher.sh must stay in step with find_toolchain.
                    with mock.patch.dict(os.environ, {"PATH": str(self.bin)}):
                        try:
                            build_prerequisites.find_toolchain(self.root)
                            missing = False
                        except ValueError:
                            missing = True
                    self.assertEqual((setup, launcher), (not missing, missing))
                    if missing:
                        self.assertEqual(result.returncode, 1)
                        self.assertIn("No compiler found", result.stderr)

    def test_without_a_built_launcher_sets_up_and_builds(self):
        result, setup, launcher = self.run_launcher()
        self.assertEqual((setup, launcher), (True, False))


if __name__ == "__main__":
    unittest.main()
