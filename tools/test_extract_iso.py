# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic listing checks; optionally exercise a real extract-xiso binary."""
import os
import io
import json
import shutil
import sys
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from extract_iso import extract, listing_files, run_logged, sha256
from run_game import build_inputs, launch


def listing(entries, count=1):
    return "extract-xiso test\nlisting synthetic.iso:\n" + entries + f"\n{count} files in synthetic.iso total 3 bytes\n"


class ExtractionTests(unittest.TestCase):
    def test_live_log_and_failed_command(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            log, gate = root / "build.log", root / "continue"

            class Console(io.StringIO):
                def write(console, text):
                    if text.strip() == "ready":
                        self.assertIn(b"ready", log.read_bytes())
                        gate.touch()
                    return super().write(text)

            script = ("import pathlib,sys,time; print('ready',flush=True); "
                      "p=pathlib.Path(sys.argv[1]); deadline=time.monotonic()+5\n"
                      "while not p.exists() and time.monotonic()<deadline: time.sleep(.01)\n"
                      "print('compiler error' if p.exists() else 'output was buffered',file=sys.stderr); "
                      "sys.exit(7)")
            console = Console()
            with patch("sys.stdout", console), self.assertRaises(subprocess.CalledProcessError) as failure:
                run_logged([sys.executable, "-u", "-c", script, str(gate)], log)
            self.assertEqual(failure.exception.returncode, 7)
            self.assertTrue(gate.exists(), "Output must reach the console before the child exits")
            self.assertIn("compiler error", console.getvalue())
            self.assertIn(b"compiler error", log.read_bytes())

    def test_built_runner_receipt_selection_and_identity(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder).resolve()
            work = root / "private/setup-synthetic"
            disc = work / "disc"
            disc.mkdir(parents=True)
            image = disc / "synthetic.xbe"
            image.write_bytes(b"synthetic game input")
            runner = work / "runner.exe"
            runner.write_bytes(b"synthetic runner")
            receipt = work / "build-receipt.json"
            data = {"status": "built-unverified", "disc": str(disc), "runner": str(runner),
                    "runner_sha256": sha256(runner), "xbe_sha256": sha256(image),
                    "recipe_sha256": "synthetic-recipe", "generation_parity": "exact-local-match"}
            receipt.write_text(json.dumps(data))
            with patch("run_game.subprocess.run") as run, patch.dict(os.environ):
                os.environ.pop("RECOMP_AUDIO_GAIN", None)
                run.return_value.returncode = 0
                self.assertEqual(launch(root), 0)
                self.assertEqual(run.call_args.args[0], [str(runner), "--xbe", str(image), "--vsync"])
                self.assertEqual(run.call_args.kwargs["env"]["RECOMP_AUDIO_GAIN"], "1")
                self.assertEqual(run.call_args.kwargs["env"]["RECOMP_USER_MUSIC"],
                                 str(root / "private" / "UserMusic"))
                log = next((root / "private").glob("run-*.log")).read_text()
                self.assertIn(data["runner_sha256"], log)
                self.assertIn(str(receipt), log)
                self.assertIn("recipe_sha256: synthetic-recipe", log)
                self.assertIn("generation_parity: exact-local-match", log)
            second = root / "private/setup-second"
            second.mkdir()
            (second / "build-receipt.json").write_text(json.dumps(data))
            with self.assertRaisesRegex(ValueError, "Multiple builds"):
                build_inputs(root)
            selected = root / "private/active-build.json"
            selected.write_text(json.dumps({"receipt": str(receipt.relative_to(root))}))
            self.assertEqual(build_inputs(root), (runner, image, receipt))
            self.assertEqual(build_inputs(root, receipt), (runner, image, receipt))
            runner.write_bytes(b"changed binary")
            with self.assertRaisesRegex(ValueError, "differs from the build receipt"):
                build_inputs(root, receipt)
            runner.write_bytes(b"synthetic runner")
            image.write_bytes(b"changed game")
            with self.assertRaisesRegex(ValueError, "differs from the build receipt"):
                build_inputs(root, receipt)
            data["status"] = "failed"
            receipt.write_text(json.dumps(data))
            with self.assertRaisesRegex(ValueError, "Build is incomplete"):
                build_inputs(root, receipt)
            outside = root / "outside.json"
            outside.write_text(json.dumps(data))
            with self.assertRaisesRegex(ValueError, "inside this checkout"):
                build_inputs(root, outside)

    def test_launch_prerequisites_and_command(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            with self.assertRaisesRegex(ValueError, "No playable runner"):
                launch(root)
            (root / "recomp_program_runner.exe").touch()
            imported = root / "private/imported-disc"
            disc = imported / "disc"
            disc.mkdir(parents=True)
            with self.assertRaises(OSError):
                launch(root)
            (imported / "receipt.json").write_text(json.dumps({"status": "extracted"}))
            (disc / "synthetic.xbe").touch()
            with patch("run_game.subprocess.run") as run, \
                    patch.dict(os.environ, {"RECOMP_AUDIO_GAIN": "0.5"}):
                run.return_value.returncode = 7
                self.assertEqual(launch(root), 7)
                self.assertEqual(run.call_args.args[0],
                                 [str(root / "recomp_program_runner.exe"), "--xbe",
                                  str(disc / "synthetic.xbe"), "--vsync"])
                self.assertEqual(run.call_args.kwargs["env"]["RECOMP_AUDIO_GAIN"], "0.5")
                self.assertEqual(run.call_args.kwargs["cwd"], disc)

    def test_paths_and_complete_listing(self):
        self.assertEqual(listing_files(listing("/sub/ (0 bytes)\n/sub/a.txt (3 bytes)")),
                         {"sub/a.txt": 3})
        for name in ("../escape", "a/../escape", "/absolute", "C:/escape", "NUL", "a:stream", "a.", "a\nname"):
            with self.subTest(name=name), self.assertRaises(ValueError):
                listing_files(listing(f"/{name} (3 bytes)"))
        for entries, count in (("/a (3 bytes)\n/A (3 bytes)", 2),
                               ("/a (3 bytes)\n/a/b (3 bytes)", 2),
                               ("/a (3 bytes)", 2), ("/a (3 bytes)\nWARNING: truncated", 1)):
            with self.assertRaises(ValueError):
                listing_files(listing(entries, count))

    @unittest.skipUnless(os.environ.get("EXTRACT_XISO_TEST_TOOL"), "optional real extractor check")
    def test_real_extraction_and_no_overwrite(self):
        private = Path(__file__).resolve().parents[1] / "private"
        with tempfile.TemporaryDirectory(prefix="extract-test-", dir=private) as folder:
            root = Path(folder).resolve()
            self.assertTrue(root.is_relative_to(private.resolve()))
            source = root / "synthetic input"
            source.mkdir()
            (source / "sub").mkdir()
            (source / "sub" / "payload.txt").write_bytes(b"synthetic payload\n")
            iso = root / "synthetic.iso"
            tool = os.environ["EXTRACT_XISO_TEST_TOOL"]
            subprocess.run([tool, "-m", "-c", str(source), str(iso)], check=True,
                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            before = sha256(iso)
            output = root / "result"
            disc = extract(iso, output, tool)
            self.assertEqual((disc / "sub/payload.txt").read_bytes(), b"synthetic payload\n")
            # Exercise the same defaults as a dropped ISO, including spaces in paths.
            package = root / "drop package"
            artifacts = package / "tools/artifacts"
            artifacts.mkdir(parents=True)
            shutil.copy2(tool, artifacts / "extract-xiso.exe")
            script = package / "tools/extract_iso.py"
            shutil.copy2(Path(__file__).with_name("extract_iso.py"), script)
            subprocess.run([sys.executable, str(script), str(iso)], check=True,
                           cwd=root, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.assertEqual((package / "private/imported-disc/disc/sub/payload.txt").read_bytes(),
                             b"synthetic payload\n")
            self.assertEqual(before, sha256(iso))
            self.assertTrue((output / "receipt.json").exists())
            with self.assertRaises(FileExistsError):
                extract(iso, output, tool)
            self.assertEqual((disc / "sub/payload.txt").read_bytes(), b"synthetic payload\n")


if __name__ == "__main__":
    unittest.main()
