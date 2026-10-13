# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic checks for authenticated generation and setup."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import build_game
from build_game import LIFTER_REVISION, RECIPE_SHA256, ROOT, program_manifest, verify_files
from extract_iso import sha256


class ProgramManifestTests(unittest.TestCase):
    def test_release_lifter_checkout_tolerates_foreign_owner(self):
        """A release ZIP on a drive without ownership, after a failed checkout (issue #110)."""
        source = ROOT / "tools/xboxrecomp"
        self.assertTrue((source / ".git").exists(), "Run git submodule update --init tools/xboxrecomp first")
        private = ROOT / "private"
        private.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="lifter-owner-test-", dir=private) as folder:
            release = Path(folder)
            lifter = release / "tools/xboxrecomp"
            subprocess.run(["git", "clone", "--quiet", "--shared", "--no-checkout", str(source), str(lifter)],
                           check=True, timeout=60)
            with mock.patch.object(build_game, "ROOT", release), \
                    mock.patch.object(build_game, "LIFTER", lifter), \
                    mock.patch.dict("os.environ", {"GIT_TEST_ASSUME_DIFFERENT_OWNER": "1"}):
                build_game.prepare_lifter(release)

    def test_recipe_recovery_generation(self):
        verify_files(ROOT, {"tools/game-recipe/recipe.json": RECIPE_SHA256})
        recipe = json.loads((ROOT / "tools/game-recipe/recipe.json").read_text(encoding="utf-8"))
        verify_files(ROOT, recipe["files"])
        recovery = "tools/game-recipe/recoveries/recover-input-history-radio.json"
        self.assertEqual(recipe["recoveries"].count(recovery), 1)
        self.assertEqual(recipe["lifter_revision"], LIFTER_REVISION)
        lifter_source = ROOT / "tools/xboxrecomp"
        self.assertTrue((lifter_source / "tools/recomp/translator.py").is_file(),
                        "Run git submodule update --init tools/xboxrecomp first")
        private = ROOT / "private"
        private.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="recovery-test-", dir=private) as folder:
            work = Path(folder)
            lifter = work / "lifter"

            def run(args, cwd=ROOT):
                result = subprocess.run(args, cwd=cwd, capture_output=True, text=True,
                                        timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

            # Use the builder's pinned revision without modifying the submodule.
            run(["git", "clone", "--quiet", "--shared", "--no-checkout",
                 str(lifter_source), str(lifter)])
            run(["git", "checkout", "--quiet", "--detach", LIFTER_REVISION], lifter)
            run([sys.executable, str(ROOT / "tools/xboxrecomp-patches/check_input_history.py"),
                 str(lifter), str(ROOT / recovery), str(work)])
            rest = "tools/game-recipe/recoveries/recover-rest-predicate.json"
            self.assertEqual(recipe["recoveries"].count(rest), 1)
            run([sys.executable, str(ROOT / "tools/xboxrecomp-patches/check_rest_predicate.py"),
                 str(lifter), str(ROOT / rest), str(work)])

    def test_parity_rejects_changed_or_missing_file(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            source = root / "input.json"
            source.write_text("[]\n", encoding="utf-8")
            expected = {source.name: sha256(source)}
            verify_files(root, expected)
            source.write_text("[1]\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "parity check failed"):
                verify_files(root, expected)
            source.unlink()
            with self.assertRaisesRegex(ValueError, "parity check failed"):
                verify_files(root, expected)

    def test_variable_chunks_and_cmake_authentication(self):
        private = ROOT / "private"
        private.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="manifest-test-", dir=private) as folder:
            root = Path(folder).resolve()
            self.assertTrue(root.is_relative_to(private.resolve()))
            generated = root / "generated"
            generated.mkdir()
            (generated / "recomp_0000.c").write_text("void sub_00001000(void) {}\n")
            (generated / "recomp_0001.c").write_text("void sub_00002000(void) {}\n")
            (generated / "recomp_dispatch.c").write_text("/* synthetic dispatch */\n")
            (generated / "recomp_funcs.h").write_text("/* synthetic declarations */\n")
            manifest, ebp = program_manifest(generated)
            self.assertEqual(ebp, 0)

            def configure(name, digest):
                return subprocess.run(
                    ["cmake", "-S", str(ROOT / "recomp-runtime"), "-B", str(root / name),
                     f"-DRECOMP_PROGRAM_DIR={generated}",
                     f"-DRECOMP_PROGRAM_MANIFEST_SHA256={digest}",
                     "-DRECOMP_PROGRAM_EBP_EXPECTED=0"], capture_output=True, text=True,
                    timeout=60)

            good = configure("valid", manifest)
            self.assertEqual(good.returncode, 0, good.stdout + good.stderr)
            (generated / "recomp_0001.c").write_text("/* changed after receipt */\n")
            wrong = configure("changed", manifest)
            self.assertNotEqual(wrong.returncode, 0)
            self.assertIn("does not match its authenticated manifest", wrong.stdout + wrong.stderr)
            (generated / "recomp_0001.c").rename(generated / "recomp_0002.c")
            with self.assertRaisesRegex(ValueError, "non-contiguous"):
                program_manifest(generated)
            gap = configure("gap", manifest)
            self.assertNotEqual(gap.returncode, 0)
            self.assertIn("chunk sequence has a gap", gap.stdout + gap.stderr)


if __name__ == "__main__":
    unittest.main()
