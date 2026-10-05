# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic checks for moving the lifter pin."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import update_lifter_pin

from build_game import program_manifest
from extract_iso import sha256
from update_lifter_pin import add_manual_targets, compare, function_hashes, pin_metadata


class LifterPinTests(unittest.TestCase):
    def test_generation_failure_restores_selection_and_metadata(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "tools/game-recipe").mkdir(parents=True)
            name = "tools/game-recipe/manual-call-targets.json"
            (root / name).write_text('["0x00001000"]\n', encoding="utf-8")
            (root / "tools/game-recipe/recipe.json").write_text(json.dumps({
                "manual_call_targets": name, "files": {name: sha256(root / name)}}), encoding="utf-8")
            (root / "tools/build_game.py").write_text("builder\n", encoding="utf-8")
            (root / "public-export.json").write_text("{}\n", encoding="utf-8")
            before = {name: (root / name).read_bytes() for name in update_lifter_pin.PINNED}
            def fake_git(*args):
                return "a" * 40 if args == ("rev-parse", "HEAD") else ""
            with patch.object(update_lifter_pin, "ROOT", root), \
                    patch.object(update_lifter_pin, "git", side_effect=fake_git), \
                    patch.object(update_lifter_pin.subprocess, "check_output", return_value=""), \
                    patch.object(update_lifter_pin.subprocess, "run") as run, \
                    patch.object(update_lifter_pin, "generate", side_effect=[root / "old", ValueError("failed")]), \
                    patch("sys.argv", ["update_lifter_pin.py", "--imported", "private/import",
                                       "--manual-call-target", "0x2000"]):
                with self.assertRaisesRegex(ValueError, "failed"):
                    update_lifter_pin.main()
                self.assertEqual(run.call_count, 1)  # submodule sync, never staging
            self.assertEqual(before, {name: (root / name).read_bytes() for name in before})

    def test_manual_selection_preserves_existing_entries_and_authentication(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "tools/game-recipe").mkdir(parents=True)
            name = "tools/game-recipe/manual-call-targets.json"
            targets = root / name
            targets.write_text('["0x00001000"]\n', encoding="utf-8")
            recipe_path = root / "tools/game-recipe/recipe.json"
            recipe_path.write_text(json.dumps({"manual_call_targets": name,
                "files": {name: sha256(targets)}, "lifter_revision": "a" * 40}), encoding="utf-8")
            add_manual_targets(root, [0x2000, 0x1000, 0x2000])
            self.assertEqual(json.loads(targets.read_text()), ["0x00001000", "0x00002000"])
            recipe = json.loads(recipe_path.read_text())
            self.assertEqual(recipe["files"][name], sha256(targets))
            self.assertEqual(recipe["lifter_revision"], "a" * 40)
            before = (targets.read_bytes(), recipe_path.read_bytes())
            for value in (0, -1, 0x100000000):
                with self.assertRaisesRegex(ValueError, "32-bit"):
                    add_manual_targets(root, [value])
                self.assertEqual(before, (targets.read_bytes(), recipe_path.read_bytes()))
            self.assertNotIn(b"\r", targets.read_bytes())

    def test_metadata_and_function_comparison(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "tools/game-recipe").mkdir(parents=True)
            old, new = root / "old", root / "new"
            for generated, second in ((old, "return;"), (new, "eax = 1;")):
                generated.mkdir()
                (generated / "recomp_0000.c").write_text(
                    "void sub_00001000(void) {}\nvoid sub_00002000(void) { " + second + " }\n")
                (generated / "recomp_dispatch.c").write_text("/* dispatch */\n")
                (generated / "recomp_funcs.h").write_text("/* declarations */\n")
            (new / "recomp_0001.c").write_text("void sub_00003000(void) {}\n")
            recipe = {"lifter_revision": "0" * 40, "generated_files": {"gone.c": "0" * 64}}
            (root / "tools/game-recipe/recipe.json").write_text(json.dumps(recipe), encoding="utf-8")
            (root / "tools/build_game.py").write_text(
                f'LIFTER_REVISION = "{"0" * 40}"\nRECIPE_SHA256 = "{"0" * 64}"\n', encoding="utf-8")
            (root / "public-export.json").write_text(json.dumps({"gitlinks": [
                {"path": "tools/xboxrecomp", "commit": "0" * 40}]}), encoding="utf-8")

            changed = pin_metadata(root, new, "a" * 40)

            self.assertIn("gone.c", changed)
            self.assertIn("recomp_0001.c", changed)
            written = json.loads((root / "tools/game-recipe/recipe.json").read_text(encoding="utf-8"))
            self.assertEqual(written["lifter_revision"], "a" * 40)
            self.assertNotIn("gone.c", written["generated_files"])
            self.assertEqual((written["program_manifest_sha256"], written["ebp_overrides"]),
                             program_manifest(new))
            builder = (root / "tools/build_game.py").read_text()
            self.assertIn(f'LIFTER_REVISION = "{"a" * 40}"', builder)
            self.assertIn(f'RECIPE_SHA256 = "{sha256(root / "tools/game-recipe/recipe.json")}"', builder)
            for name in ("tools/game-recipe/recipe.json", "tools/build_game.py", "public-export.json"):
                self.assertNotIn(b"\r", (root / name).read_bytes())
            export = json.loads((root / "public-export.json").read_text(encoding="utf-8"))
            self.assertEqual(export["gitlinks"][0]["commit"], "a" * 40)
            self.assertEqual(compare(function_hashes(old), function_hashes(new)),
                             (["sub_00002000"], ["sub_00003000"], []))


if __name__ == "__main__":
    unittest.main()
