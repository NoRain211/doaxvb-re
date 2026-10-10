# SPDX-License-Identifier: GPL-3.0-or-later
"""Launch an ISO build by receipt, or a separately installed runner."""
import argparse
from datetime import datetime
import json
import os
from pathlib import Path
import subprocess
import sys

from extract_iso import package_root, sha256


def build_inputs(root, receipt_path=None):
    private = (root / "private").resolve()
    selected = private / "active-build.json"
    if receipt_path is None and selected.is_file():
        receipt_path = root / json.loads(selected.read_text(encoding="utf-8"))["receipt"]
    if receipt_path is None:
        receipts = [path for path in private.glob("setup-*/build-receipt.json")
                    if json.loads(path.read_text(encoding="utf-8")).get("status") == "built-unverified"]
        if len(receipts) > 1:
            raise ValueError("Multiple builds found. Drop the intended build-receipt.json onto RunGame.cmd.")
        if not receipts:
            return None
        receipt_path = receipts[0]
    receipt_path = Path(receipt_path).resolve(strict=True)
    if not receipt_path.is_relative_to(private):
        raise ValueError("Build receipt must be inside this checkout's private folder.")
    receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
    if receipt.get("status") != "built-unverified":
        raise ValueError("Build is incomplete; see its build-receipt.json and logs.")
    runner = Path(receipt["runner"]).resolve(strict=True)
    disc = Path(receipt["disc"]).resolve(strict=True)
    if not runner.is_relative_to(private) or not disc.is_relative_to(private):
        raise ValueError("Build paths must remain inside this checkout's private folder.")
    images = list(disc.glob("*.xbe"))
    if len(images) != 1:
        raise ValueError("Expected exactly one root XBE in the built disc.")
    if sha256(runner) != receipt["runner_sha256"] or sha256(images[0]) != receipt["xbe_sha256"]:
        raise ValueError("Runner or game executable differs from the build receipt; rebuild before launching.")
    return runner, images[0], receipt_path


def launch(root, receipt_path=None, settings=False):
    built = build_inputs(root, receipt_path)
    if built is not None:
        runner, image, receipt_path = built
        print(f"Using build receipt: {receipt_path}", flush=True)
        return run(runner, image, root, receipt_path, settings)
    candidates = [root / "recomp_program_runner.exe",
                  root / "recomp_program_runner",
                  root / "build/recomp-program/Release/recomp_program_runner.exe",
                  root / "build/recomp-program/recomp_program_runner"]
    runner = next((path for path in candidates if path.is_file()), None)
    if runner is None:
        raise ValueError("No playable runner found. Drop your ISO onto BuildGame.cmd first; "
                         "see docs/building.md for required build tools.")
    imported = root / "private/imported-disc"
    receipt = json.loads((imported / "receipt.json").read_text(encoding="utf-8"))
    if receipt.get("status") != "extracted":
        raise ValueError("Extraction is incomplete. Drop your ISO onto ExtractIso.cmd first.")
    images = list((imported / "disc").glob("*.xbe"))
    if len(images) != 1:
        raise ValueError("Expected exactly one root XBE in the extracted disc; cannot choose safely.")
    release_path = root / "release.json"
    if release_path.is_file():
        release = json.loads(release_path.read_text(encoding="utf-8"))
        if sha256(runner) != release["runner_sha256"] or sha256(images[0]) != release["xbe_sha256"]:
            raise ValueError("Runner or game executable does not match this release.")
    return run(runner, images[0], root, release_path if release_path.is_file() else None, settings)


def saved_launcher_settings(root):
    path = root / "private" / "launcher.json"
    try:
        settings = json.loads(path.read_text(encoding="utf-8"))
        return {"RECOMP_D3D_SCALE": f"{int(settings.get('height', 480)) / 480:.4f}",
                "RECOMP_D3D_MSAA": str(int(settings.get("msaa", 1))),
                "RECOMP_D3D_SMAA": "1" if settings.get("smaa") else "0",
                "RECOMP_AUDIO_GAIN": f"{int(settings.get('volume', 100)) / 100:.4f}",
                "RECOMP_MUSIC_SHUFFLE": "1" if settings.get("shuffle") else "0"}
    except (OSError, ValueError, TypeError, AttributeError, OverflowError):
        return {}


def run(runner, image, root, receipt_path=None, settings=False):
    if settings:
        # Linux builds the settings launcher beside the runner; see launcher.sh.
        launcher = runner.parent / "recomp_launcher"
        if not launcher.is_file():
            raise ValueError(f"{launcher} is missing. Rebuild with ./build_game.sh, "
                             "or start the game with ./run_game.sh.")
        if subprocess.run([str(launcher), str(root)]).returncode != 0:
            return 1
    log_path = root / "private" / ("run-" + datetime.now().strftime("%Y%m%d-%H%M%S-%f") + ".log")
    env = dict(os.environ, RECOMP_USER_MUSIC=str(root / "private" / "UserMusic"))
    if os.name != "nt":
        # Launcher.cmd passes its choices in the environment; run_game.sh
        # applies the choices launcher.sh saved.
        for name, value in saved_launcher_settings(root).items():
            env.setdefault(name, value)
    env.setdefault("RECOMP_AUDIO_GAIN", "1")  # the launcher's Volume choice overrides this
    env.setdefault("RECOMP_PERF_COUNTER", "1")
    print(f"Starting runner. Log: {log_path}", flush=True)
    with log_path.open("x", encoding="utf-8") as log:
        log.write(f"Build receipt: {receipt_path or 'legacy runner; no build receipt selected'}\n"
                  f"Runner SHA-256: {sha256(runner)}\nGame executable SHA-256: {sha256(image)}\n")
        if receipt_path:
            receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
            for key in ("recipe_sha256", "lifter_revision", "lifter_patch_sha256",
                        "program_manifest_sha256", "generation_parity"):
                if key in receipt:
                    log.write(f"{key}: {receipt[key]}\n")
        log.flush()
        result = subprocess.run([str(runner), "--xbe", str(image), "--vsync"],
                                cwd=image.parent, env=env, stdout=log,
                                stderr=subprocess.STDOUT)
    print(f"Runner exited with code {result.returncode}. See {log_path}")
    return result.returncode


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("receipt", nargs="?", type=Path, help="Select a private build-receipt.json")
    parser.add_argument("--launcher", action="store_true", help="Pick settings first (Linux; see launcher.sh)")
    args = parser.parse_args()
    try:
        sys.exit(launch(package_root(), args.receipt, args.launcher))
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"Cannot launch: {error}", file=sys.stderr)
        sys.exit(1)
