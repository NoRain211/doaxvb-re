# SPDX-License-Identifier: GPL-3.0-or-later
"""Prerequisite checks without network downloads."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import urllib.error
import zipfile

import build_game
import build_prerequisites as prerequisites


class PrerequisiteTests(unittest.TestCase):
    def setUp(self):
        private = build_game.ROOT / "private"
        private.mkdir(exist_ok=True)
        temporary = tempfile.TemporaryDirectory(prefix="prerequisites-test-", dir=private)
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.download = self.enterContext(mock.patch.object(
            prerequisites.urllib.request, "urlopen", side_effect=AssertionError("Unexpected download")))
        self.enterContext(mock.patch("builtins.print"))

    def test_bundled_path_precedes_system_and_is_inherited(self):
        with mock.patch.dict(os.environ, PATH="system-tools"):
            prerequisites.prefer_bundled_tools(self.root)
            self.assertEqual(os.environ["PATH"], "system-tools")
            for name in ("tools/git/cmd/git.exe", "tools/cmake/bin/cmake.exe"):
                path = self.root / name
                path.parent.mkdir(parents=True)
                path.touch()
            prerequisites.prefer_bundled_tools(self.root)
            expected = os.pathsep.join([str(self.root / "tools/git/cmd"),
                                        str(self.root / "tools/cmake/bin"), "system-tools"])
            self.assertEqual(os.environ["PATH"], expected)
            prerequisites.prefer_bundled_tools(self.root)
            self.assertEqual(os.environ["PATH"], expected)
            child = subprocess.check_output(
                [sys.executable, "-c", "import os; print(os.environ['PATH'])"], text=True).strip()
            self.assertEqual(child, expected)
            if os.name == "nt":
                self.assertEqual(Path(prerequisites.shutil.which("git")), self.root / "tools/git/cmd/git.exe")
                self.assertEqual(Path(prerequisites.shutil.which("cmake")), self.root / "tools/cmake/bin/cmake.exe")

    def test_existing_compiler_skips_download(self):
        with mock.patch.object(prerequisites, "find_toolchain", return_value=Path("clang")), \
                mock.patch.object(prerequisites, "install_llvm_mingw") as install:
            self.assertEqual(prerequisites.ensure_prerequisites(self.root, True), Path("clang"))
            install.assert_not_called()

    def test_missing_compiler_requires_flag_and_is_rechecked(self):
        with mock.patch.object(prerequisites, "find_toolchain", return_value=None), \
                mock.patch.object(prerequisites, "install_llvm_mingw") as install:
            with self.assertRaisesRegex(ValueError, "--install-prerequisites"):
                prerequisites.ensure_prerequisites(self.root)
            install.assert_not_called()
            with self.assertRaisesRegex(ValueError, "still missing"):
                prerequisites.ensure_prerequisites(self.root, True)
            install.assert_called_once_with(self.root)

    def llvm_mingw_zip(self):
        data = io.BytesIO()
        with zipfile.ZipFile(data, "w") as archive:
            archive.writestr("llvm-mingw-test-ucrt-x86_64/bin/clang.exe", b"clang")
        return data.getvalue()

    def test_download_installs_verified_toolchain(self):
        payload = self.llvm_mingw_zip()
        self.download.side_effect = lambda *args, **kwargs: io.BytesIO(payload)
        with mock.patch.object(prerequisites, "LLVM_MINGW_SHA256", hashlib.sha256(payload).hexdigest()):
            prerequisites.install_llvm_mingw(self.root)
        self.assertEqual((self.root / "tools/llvm-mingw/bin/clang.exe").read_bytes(), b"clang")
        self.assertEqual(list((self.root / "private").iterdir()), [])
        self.download.assert_called_once_with(prerequisites.LLVM_MINGW_URL, timeout=60)

    def test_failed_download_or_hash_installs_nothing(self):
        payload = self.llvm_mingw_zip()
        for failure, message in ((urllib.error.URLError("offline"), "Could not download"),
                                 (None, "SHA-256")):
            with self.subTest(message=message):
                self.download.side_effect = failure or (lambda *args, **kwargs: io.BytesIO(payload))
                with self.assertRaisesRegex(ValueError, message):
                    prerequisites.install_llvm_mingw(self.root)
                self.assertFalse((self.root / "tools/llvm-mingw").exists())
                self.assertEqual(list((self.root / "private").iterdir()), [])

    def test_build_checks_before_extraction_and_skips_for_generation(self):
        recipe = self.root / "tools/game-recipe/recipe.json"
        recipe.parent.mkdir(parents=True)
        recipe.write_text(json.dumps({"files": {}}), encoding="utf-8")
        with mock.patch.object(build_game, "ROOT", self.root), \
                mock.patch.object(build_game.shutil, "which", return_value="tool"), \
                mock.patch.dict(sys.modules, capstone=mock.Mock()), \
                mock.patch.object(build_game, "ensure_prerequisites", side_effect=ValueError("missing tools")) as ensure, \
                mock.patch.object(build_game, "extract", side_effect=ValueError("extraction reached")) as extract:
            args = argparse.Namespace(generate_only=False, imported=None, iso=Path("input.iso"), extractor="extract-xiso")
            with self.assertRaisesRegex(ValueError, "missing tools"):
                build_game.build(args, verify_parity=False)
            ensure.assert_called_once_with(self.root, False)
            extract.assert_not_called()
            ensure.reset_mock()
            args.generate_only = True
            with self.assertRaisesRegex(ValueError, "extraction reached"):
                build_game.build(args, verify_parity=False)
            ensure.assert_not_called()
            extract.assert_called_once()


if __name__ == "__main__":
    unittest.main()
