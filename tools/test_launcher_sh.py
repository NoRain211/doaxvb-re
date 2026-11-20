# SPDX-License-Identifier: GPL-3.0-or-later
"""Which launcher launcher.sh opens, and when it sets up the venv and rebuilds."""
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
STUB = "#!/bin/sh\nexit 0\n"


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
        for name in ("bin/cmake", "bin/c++", "setup-ran", "launcher-ran"):
            (self.root / name).unlink(missing_ok=True)
        for tool in tools:
            self.write(f"bin/{tool}", STUB, True)
        result = subprocess.run([shutil.which("bash"), str(self.root / "launcher.sh")],
                                env={"PATH": str(self.bin)}, capture_output=True, text=True)
        return result, (self.root / "setup-ran").exists(), (self.root / "launcher-ran").exists()

    def test_without_a_toolchain_reuses_the_built_launcher(self):
        self.built_launcher()
        for tools in ((), ("cmake",), ("c++",)):
            with self.subTest(tools=tools):
                result, setup, launcher = self.run_launcher(*tools)
                self.assertEqual((result.returncode, setup, launcher), (1, False, True))
                self.assertIn("No compiler found", result.stderr)

    def test_with_a_toolchain_sets_up_and_rebuilds(self):
        self.built_launcher()
        result, setup, launcher = self.run_launcher("cmake", "c++")
        self.assertEqual((setup, launcher), (True, False))

    @unittest.skipUnless(platform.system() == "Linux" and platform.machine() == "x86_64",
                         "the prebuilt launcher is for x86-64 Linux")
    def test_prebuilt_launcher_needs_no_setup_even_with_a_toolchain(self):
        self.built_launcher()
        self.write("tools/artifacts/launcher-linux-x86_64", '#!/bin/sh\n: > "$1/prebuilt-ran"\nexit 1\n', True)
        result, setup, launcher = self.run_launcher("cmake", "c++")
        prebuilt = (self.root / "prebuilt-ran").exists()
        self.assertEqual((result.returncode, setup, launcher, prebuilt), (1, False, False, True))

    def test_without_a_built_launcher_sets_up_and_builds(self):
        result, setup, launcher = self.run_launcher()
        self.assertEqual((setup, launcher), (True, False))


if __name__ == "__main__":
    unittest.main()
