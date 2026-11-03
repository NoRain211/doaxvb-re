# SPDX-License-Identifier: GPL-3.0-or-later
"""Prerequisite checks without network downloads."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import shlex
import shutil
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
                mock.patch.object(prerequisites, "install_llvm_mingw") as install, \
                mock.patch.object(prerequisites, "install_dxc") as install_dxc:
            self.assertEqual(prerequisites.ensure_prerequisites(self.root, True), Path("clang"))
            install.assert_not_called()
            # Windows setup also fetches DXC for RTGI, once, and only when asked to install.
            self.assertEqual(install_dxc.call_count, int(os.name == "nt"))
            dxc = self.root / "tools/dxc/bin/x64/dxc.exe"
            dxc.parent.mkdir(parents=True)
            dxc.touch()
            prerequisites.ensure_prerequisites(self.root, True)
            prerequisites.ensure_prerequisites(self.root)
            self.assertEqual(install_dxc.call_count, int(os.name == "nt"))

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
        # Its final path is 250 characters, so the staging path passes 260 (#171).
        self.deep_name = "lib/" + "a" * (250 - len(str(self.root / "tools/llvm-mingw/lib")) - 1)
        data = io.BytesIO()
        with zipfile.ZipFile(data, "w") as archive:
            archive.writestr("llvm-mingw-test-ucrt-x86_64/bin/clang.exe", b"clang")
            archive.writestr("llvm-mingw-test-ucrt-x86_64/" + self.deep_name, b"deep")
        return data.getvalue()

    def test_download_installs_verified_toolchain(self):
        payload = self.llvm_mingw_zip()
        stale = self.root / "private/llvm-mingw-old/extract"
        stale.mkdir(parents=True)
        (stale / "locked.dll").write_bytes(b"")
        self.download.side_effect = lambda *args, **kwargs: io.BytesIO(payload)
        with mock.patch.object(prerequisites, "LLVM_MINGW_SHA256", hashlib.sha256(payload).hexdigest()), \
                mock.patch.object(prerequisites.tempfile, "TemporaryDirectory",
                                  wraps=tempfile.TemporaryDirectory) as staging:
            prerequisites.install_llvm_mingw(self.root)
        # Long paths may be enabled on the test host, so check the staging path directly (#171).
        staging.assert_called_once_with(prefix="llvm-mingw-",
                                        dir=prerequisites.extended_path(self.root / "private"),
                                        ignore_cleanup_errors=True)
        self.assertEqual((self.root / "tools/llvm-mingw/bin/clang.exe").read_bytes(), b"clang")
        self.assertEqual((self.root / "tools/llvm-mingw" / self.deep_name).read_bytes(), b"deep")
        self.assertEqual(list((self.root / "private").iterdir()), [])
        self.download.assert_called_once_with(prerequisites.LLVM_MINGW_URL, timeout=60)

    def test_download_installs_verified_dxc(self):
        data = io.BytesIO()
        with zipfile.ZipFile(data, "w") as archive:  # the DXC ZIP has no top-level folder
            archive.writestr("bin/x64/dxc.exe", b"dxc")
            archive.writestr("LICENSE-MS.txt", b"license")
        payload = data.getvalue()
        self.download.side_effect = lambda *args, **kwargs: io.BytesIO(payload)
        with mock.patch.object(prerequisites, "DXC_SHA256", hashlib.sha256(payload).hexdigest()):
            prerequisites.install_dxc(self.root)
        self.assertEqual((self.root / "tools/dxc/bin/x64/dxc.exe").read_bytes(), b"dxc")
        self.assertTrue((self.root / "tools/dxc/LICENSE-MS.txt").is_file())
        self.assertEqual(list((self.root / "private").iterdir()), [])
        self.download.assert_called_once_with(prerequisites.DXC_URL, timeout=60)

    def test_extended_path_prefixes_only_windows_paths(self):
        if os.name != "nt":
            self.assertEqual(prerequisites.extended_path("x"), os.path.abspath("x"))
            return
        for path, expected in (("C:/a/../x", "\\\\?\\C:\\x"),
                               ("\\\\srv\\share\\x", "\\\\?\\UNC\\srv\\share\\x"),
                               ("\\\\?\\C:\\x", "\\\\?\\C:\\x")):
            with self.subTest(path=path):
                self.assertEqual(prerequisites.extended_path(path), expected)

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

    def test_locked_extraction_is_retried_then_reported(self):
        payload = self.llvm_mingw_zip()
        self.download.side_effect = lambda *args, **kwargs: io.BytesIO(payload)
        move = Path.replace
        for failures, installed in ((10, False), (2, True)):
            with self.subTest(failures=failures):
                calls = []

                def locked(path, target):
                    calls.append(target)
                    if len(calls) <= failures:
                        raise PermissionError(13, "Access is denied")
                    return move(path, target)

                with mock.patch.object(prerequisites, "LLVM_MINGW_SHA256", hashlib.sha256(payload).hexdigest()), \
                        mock.patch.object(Path, "replace", locked), \
                        mock.patch.object(prerequisites.time, "sleep") as sleep:
                    if installed:
                        prerequisites.install_llvm_mingw(self.root)
                    else:
                        with self.assertRaisesRegex(ValueError, "rerun setup"):
                            prerequisites.install_llvm_mingw(self.root)
                self.assertEqual(sleep.call_args_list, [mock.call(3)] * min(failures, 9))
                self.assertEqual((self.root / "tools/llvm-mingw/bin/clang.exe").is_file(), installed)
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

    def test_missing_capstone_hint_matches_platform(self):
        recipe = self.root / "tools/game-recipe/recipe.json"
        recipe.parent.mkdir(parents=True)
        recipe.write_text(json.dumps({"files": {}}), encoding="utf-8")
        expected = {"win32": f'& "{sys.executable}" -m pip install capstone==5.0.9',
                    "darwin": f"run {shlex.quote(str(self.root / 'build_game.sh'))}, which installs it into private/venv"}
        args = argparse.Namespace(generate_only=True, imported=None, iso=Path("input.iso"), extractor="extract-xiso")
        for platform, hint in expected.items():
            with self.subTest(platform=platform), \
                    mock.patch.object(build_game, "ROOT", self.root), \
                    mock.patch.object(build_game.shutil, "which", return_value="tool"), \
                    mock.patch.object(build_game.sys, "platform", platform), \
                    mock.patch.dict(sys.modules, capstone=None):
                with self.assertRaises(ValueError) as caught:
                    build_game.build(args, verify_parity=False)
                self.assertEqual(str(caught.exception), "Install Capstone: " + hint)

    def test_extract_xiso_reuses_only_a_finished_build(self):
        source = self.root / "tools/extract-xiso" / prerequisites.EXTRACT_XISO_COMMIT[:12]
        tool = source / "build/extract-xiso"

        def fake_run(args, log, cwd=None):
            if args[:2] == ["cmake", "--build"]:
                tool.parent.mkdir(parents=True, exist_ok=True)
                tool.write_bytes(b"built")

        with mock.patch.object(prerequisites.shutil, "which", return_value="tool"), \
                mock.patch.object(prerequisites, "run_logged", side_effect=fake_run) as run:
            self.assertEqual(prerequisites.ensure_extract_xiso(self.root, self.root), tool)
            self.assertEqual(run.call_count, 4)
            run.reset_mock()
            self.assertEqual(prerequisites.ensure_extract_xiso(self.root, self.root), tool)
            run.assert_not_called()
            (source / "build-complete").unlink()  # as after an interrupted link
            tool.write_bytes(b"partial")
            prerequisites.ensure_extract_xiso(self.root, self.root)
            self.assertEqual(run.call_count, 4)
            self.assertEqual(tool.read_bytes(), b"built")
        shutil.rmtree(source)
        with mock.patch.object(prerequisites.shutil, "which", side_effect=lambda name: None if name == "cmake" else "tool"), \
                mock.patch.object(prerequisites, "run_logged") as run:
            with self.assertRaisesRegex(ValueError, "Install cmake to build extract-xiso"):
                prerequisites.ensure_extract_xiso(self.root, self.root)
            run.assert_not_called()

    def test_missing_extractor_is_built_only_when_not_chosen(self):
        recipe = self.root / "tools/game-recipe/recipe.json"
        recipe.parent.mkdir(parents=True)
        recipe.write_text(json.dumps({"files": {}}), encoding="utf-8")
        built = Path("built/extract-xiso")
        for extractor, on_path, expected in (("extract-xiso", False, built),
                                             ("extract-xiso", True, "extract-xiso"),
                                             ("/opt/extract-xiso", False, "/opt/extract-xiso")):
            args = argparse.Namespace(generate_only=True, imported=None, iso=Path("input.iso"), extractor=extractor)
            with self.subTest(extractor=extractor, on_path=on_path), \
                    mock.patch.object(build_game, "ROOT", self.root), \
                    mock.patch.object(build_game.sys, "platform", "linux"), \
                    mock.patch.object(build_game.shutil, "which",
                                      side_effect=lambda name: None if name == "extract-xiso" and not on_path else "tool"), \
                    mock.patch.dict(sys.modules, capstone=mock.Mock()), \
                    mock.patch.object(build_game, "ensure_extract_xiso", return_value=built) as ensure, \
                    mock.patch.object(build_game, "extract", side_effect=ValueError("extraction reached")) as extract:
                with self.assertRaisesRegex(ValueError, "extraction reached"):
                    build_game.build(args, verify_parity=False)
                self.assertEqual(ensure.called, expected is built)
                self.assertEqual(extract.call_args.args[2], expected)


if __name__ == "__main__":
    unittest.main()
