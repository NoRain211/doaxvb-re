# SPDX-License-Identifier: GPL-3.0-or-later
"""Prerequisite checks without downloads, UAC, or software installation."""
import argparse
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

    def test_existing_prerequisites_skip_prompt_and_install(self):
        with mock.patch.object(prerequisites, "find_toolchain", return_value=Path("VS")), \
                mock.patch.object(prerequisites, "install_build_tools") as install, \
                mock.patch("builtins.input") as consent:
            self.assertEqual(prerequisites.ensure_prerequisites(self.root, True), Path("VS"))
            consent.assert_not_called()
            install.assert_not_called()

    def test_missing_prerequisites_require_flag_and_default_to_no(self):
        with mock.patch.object(prerequisites, "find_toolchain", return_value=None), \
                mock.patch.object(prerequisites, "install_build_tools") as install, \
                mock.patch("builtins.input") as consent:
            with self.assertRaisesRegex(ValueError, "--install-prerequisites"):
                prerequisites.ensure_prerequisites(self.root)
            consent.assert_not_called()
            for answer in ("", "no", "anything else", EOFError(), KeyboardInterrupt()):
                with self.subTest(answer=answer):
                    consent.side_effect = answer if isinstance(answer, BaseException) else None
                    consent.return_value = answer
                    with self.assertRaisesRegex(ValueError, "declined"):
                        prerequisites.ensure_prerequisites(self.root, True)
            install.assert_not_called()

    def test_install_success_is_rechecked(self):
        for after in (Path("VS"), None):
            with self.subTest(after=after), \
                    mock.patch.object(prerequisites, "find_toolchain", side_effect=[None, after]) as find, \
                    mock.patch.object(prerequisites, "install_build_tools") as install, \
                    mock.patch("builtins.input", return_value="yes") as consent:
                if after:
                    self.assertEqual(prerequisites.ensure_prerequisites(self.root, True), after)
                else:
                    with self.assertRaisesRegex(ValueError, "still fails"):
                        prerequisites.ensure_prerequisites(self.root, True)
                self.assertEqual(find.call_count, 2)
                consent.assert_called_once()
                install.assert_called_once_with(self.root)

    @unittest.skipUnless(os.name == "nt", "Windows toolchain discovery")
    def test_probe_tries_other_installed_vs_when_first_sdk_check_fails(self):
        with mock.patch.object(prerequisites, "visual_studios", return_value=[Path("bad"), Path("good")]), \
                mock.patch.object(prerequisites.subprocess, "run", side_effect=[
                    subprocess.CompletedProcess([], 1), subprocess.CompletedProcess([], 0)]) as run:
            self.assertEqual(prerequisites.find_toolchain(self.root), Path("good"))
            self.assertEqual(run.call_count, 2)
            self.assertIn("-DCMAKE_GENERATOR_INSTANCE=good", run.call_args.args[0])
            self.assertNotIn("-DCMAKE_SYSTEM_VERSION", " ".join(run.call_args.args[0]))

    def test_download_failure_never_launches_installer(self):
        self.download.side_effect = urllib.error.URLError("offline")
        with mock.patch.object(prerequisites, "visual_studios", return_value=[]), \
                mock.patch.object(prerequisites.subprocess, "run") as run:
            with self.assertRaisesRegex(ValueError, "Could not download"):
                prerequisites.install_build_tools(self.root)
            run.assert_not_called()

    def test_installer_results_and_private_download_cleanup(self):
        outcomes = ((0, None), (1223, "cancelled"), (1602, "cancelled"), (5004, "cancelled"),
                    (1641, "restart"), (3010, "restart"), (1603, "exit 1603"),
                    (1, "valid Microsoft Authenticode"))
        self.download.side_effect = lambda *args, **kwargs: io.BytesIO(b"mock installer")
        for code, message in outcomes:
            with self.subTest(code=code), \
                    mock.patch.object(prerequisites, "visual_studios", return_value=[Path("Existing Build Tools")]), \
                    mock.patch.object(prerequisites.subprocess, "run", return_value=subprocess.CompletedProcess(
                        [], code, "", "Installer does not have a valid Microsoft Authenticode signature.")) as run:
                if message:
                    with self.assertRaisesRegex(ValueError, message):
                        prerequisites.install_build_tools(self.root)
                else:
                    prerequisites.install_build_tools(self.root)
                env = run.call_args.kwargs["env"]
                target = Path(env["DOAXBV_INSTALLER"])
                self.assertTrue(target.is_relative_to(self.root / "private"))
                self.assertFalse(target.exists())
                self.assertEqual(env["DOAXBV_BUILD_TOOLS"], "Existing Build Tools")
                self.assertEqual(env["PSModulePath"], str(prerequisites.POWERSHELL.parent / "Modules"))
                self.assertEqual(run.call_args.args[0][0], str(prerequisites.POWERSHELL))
                self.download.assert_called_with(prerequisites.INSTALLER_URL, timeout=60)

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
            args.generate_only = False
            clang = self.root / "tools/llvm-mingw/bin/clang.exe"
            clang.parent.mkdir(parents=True)
            clang.touch()
            with mock.patch.object(build_game.sys, "platform", "win32"), \
                    mock.patch.dict(prerequisites.os.environ), \
                    self.assertRaisesRegex(ValueError, "extraction reached"):
                build_game.build(args, verify_parity=False)
            ensure.assert_not_called()

    @unittest.skipUnless(prerequisites.POWERSHELL.is_file(), "Windows PowerShell required")
    def test_real_signature_check_rejects_unsigned_download(self):
        self.download.side_effect = lambda *args, **kwargs: io.BytesIO(b"unsigned test file")
        original_run = subprocess.run

        def inert_run(args, **kwargs):
            # Even if signature validation regresses, this test cannot elevate or execute a file.
            args = [*args[:-1], "function Start-Process { throw 'Unexpected launch' }\n" + args[-1]]
            return original_run(args, **kwargs, timeout=15)

        with mock.patch.object(prerequisites, "visual_studios", return_value=[]), \
                mock.patch.object(prerequisites.subprocess, "run", side_effect=inert_run), \
                mock.patch.dict(os.environ, PSModulePath="inherited modules must not be used"):
            with self.assertRaisesRegex(ValueError, "valid Microsoft Authenticode signature"):
                prerequisites.install_build_tools(self.root)

    @unittest.skipUnless(prerequisites.POWERSHELL.is_file(), "Windows PowerShell required")
    def test_powershell_signature_gate_and_uac_cancellation(self):
        # Execute the real launch script with inert cmdlets: no file is executed or elevated.
        mocks = r"""
function Get-AuthenticodeSignature {
    $certificate = [pscustomobject]@{}
    $certificate | Add-Member ScriptMethod GetNameInfo { return $env:TEST_SIGNER }
    return [pscustomobject]@{ Status = $env:TEST_SIGNATURE; SignerCertificate = $certificate }
}
function Start-Process {
    param($FilePath, $ArgumentList, $Verb, [switch]$PassThru, [switch]$Wait)
    if ($Verb -ne 'RunAs' -or !$PassThru -or !$Wait -or
        $ArgumentList -notlike '*--passive --norestart --wait*' -or
        $ArgumentList -notlike '*--add Microsoft.VisualStudio.Component.VC.Tools.x86.x64*' -or
        $ArgumentList -notlike '*--add Microsoft.VisualStudio.Component.Windows11SDK.26100*') {
        throw 'Incorrect installer invocation'
    }
    if ($env:DOAXBV_BUILD_TOOLS -and
        !$ArgumentList.StartsWith('modify --channelId VisualStudio.17.Release --installPath "' +
            $env:DOAXBV_BUILD_TOOLS + '" ')) {
        throw 'Incorrect modify path quoting'
    }
    [Console]::Out.WriteLine('MOCK_LAUNCH')
    if ($env:TEST_UAC -eq 'cancel') { throw (New-Object System.ComponentModel.Win32Exception 1223) }
    return [pscustomobject]@{ ExitCode = 3010 }
}
"""
        cases = (("Valid", "Microsoft Corporation", "", 3010),
                 ("Valid", "Microsoft Corporation", "cancel", 1223),
                 ("HashMismatch", "Microsoft Corporation", "", 1),
                 ("NotSigned", "", "", 1), ("Valid", "Other Publisher", "", 1))
        for status, signer, uac, code in cases:
            with self.subTest(status=status, signer=signer, uac=uac):
                result = subprocess.run(
                    [str(prerequisites.POWERSHELL), "-NoProfile", "-NonInteractive", "-Command",
                     mocks + prerequisites.INSTALL_SCRIPT], capture_output=True, text=True,
                    env=dict(os.environ, TEST_SIGNATURE=status, TEST_SIGNER=signer, TEST_UAC=uac,
                             DOAXBV_INSTALLER=str(self.root / "never executed.exe"),
                             DOAXBV_BUILD_TOOLS="C:/Build Tools/with spaces"), timeout=15)
                self.assertEqual(result.returncode, code, result.stdout + result.stderr)
                if code == 1:
                    self.assertNotIn("MOCK_LAUNCH", result.stdout)
                    self.assertIn("valid Microsoft Authenticode", result.stderr)
                else:
                    self.assertIn("MOCK_LAUNCH", result.stdout)


if __name__ == "__main__":
    unittest.main()
