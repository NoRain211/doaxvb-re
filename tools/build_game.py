# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a diagnostic runner from a user-owned ISO; gameplay requires validation."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import uuid

from extract_iso import extract, run_logged, sha256
from build_prerequisites import ensure_prerequisites, prefer_bundled_tools

ROOT = Path(__file__).resolve().parents[1]
LIFTER = ROOT / "tools/xboxrecomp"
LIFTER_REPOSITORY = "https://github.com/NoRain211/xboxrecomp.git"
LIFTER_REVISION = "7adaf210a89814c5ca4444e4876fccb51b938878"
RECIPE_SHA256 = "46c5de9cea1dcf319c6dfbe79bbd22acfcc81e42c89192ab0654d4f6a8aa70ba"
SUPPORTED_XBE_SHA256 = "053d44e885fa33c1d15d909a533f39dfbd976e97eeaf67e4fdef8438ea7e5c54"


def command(args, cwd, log):
    run_logged(args, log, cwd)


def program_manifest(generated):
    chunks = sorted(generated.glob("recomp_[0-9][0-9][0-9][0-9].c"))
    if not chunks or [p.name for p in chunks] != [
            f"recomp_{i:04d}.c" for i in range(len(chunks))]:
        raise ValueError("Generated program chunks are missing or non-contiguous")
    members = chunks + [generated / "recomp_dispatch.c", generated / "recomp_funcs.h"]
    manifest = "".join(f"{p.name}\t{p.stat().st_size}\t{sha256(p)}\n" for p in members)
    ebp = sum(p.read_text(encoding="utf-8").count("\n    uint32_t ebp;\n")
              for p in members if p.suffix == ".c")
    return hashlib.sha256(manifest.encode("utf-8")).hexdigest(), ebp


def verify_files(root, expected):
    for name, digest in expected.items():
        path = root / name
        if not path.is_file() or sha256(path) != digest:
            raise ValueError(f"Build parity check failed: {name}")


def prepare_lifter(work, revision=None):
    """Generate from tools/xboxrecomp, fetching it at the recipe revision when absent."""
    revision = revision or LIFTER_REVISION
    if (ROOT / ".git").exists():
        # Older checkouts may still point the submodule at the upstream URL.
        command(["git", "submodule", "sync", "--", "tools/xboxrecomp"], ROOT, work / "sync.log")
    if not (LIFTER / ".git").exists():
        if (ROOT / ".git").exists():
            command(["git", "submodule", "update", "--init", "tools/xboxrecomp"], ROOT, work / "lifter.log")
        else:  # A release ZIP carries no repository metadata.
            command(["git", "clone", "--no-checkout", "--filter=blob:none", LIFTER_REPOSITORY, LIFTER],
                    ROOT, work / "lifter.log")
            command(["git", "checkout", "--detach", revision], LIFTER, work / "checkout.log")
    head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=LIFTER, text=True).strip()
    dirty = subprocess.check_output(["git", "status", "--porcelain"], cwd=LIFTER, text=True).strip()
    if head != revision or dirty:
        raise ValueError(f"tools/xboxrecomp must be a clean checkout of {revision}; "
                         "run: git submodule sync tools/xboxrecomp, then git submodule update tools/xboxrecomp")


def build(args, verify_parity=True, lifter_revision=None):
    prefer_bundled_tools(ROOT)
    if verify_parity:
        verify_files(ROOT, {"tools/game-recipe/recipe.json": RECIPE_SHA256})
    recipe = json.loads((ROOT / "tools/game-recipe/recipe.json").read_text(encoding="utf-8"))
    verify_files(ROOT, recipe["files"])
    if verify_parity and recipe["lifter_revision"] != LIFTER_REVISION:
        raise ValueError("Build recipe and lifter revision differ")
    for tool in (("git",) if args.generate_only else ("git", "cmake")):
        if shutil.which(tool) is None:
            raise ValueError(f"Install {tool} and add it to PATH before running setup.")
    try:
        import capstone
    except ImportError as error:
        raise ValueError("Install Capstone for this Python: python -m pip install capstone==5.0.9") from error
    if not args.generate_only:
        instance = ensure_prerequisites(ROOT, getattr(args, "install_prerequisites", False))
    work = ROOT / "private" / ("setup-" + uuid.uuid4().hex[:12])
    work.mkdir(parents=True)
    receipt = {"status": "in-progress", "work": str(work),
               "lifter_revision": lifter_revision or LIFTER_REVISION, "recipe_sha256": RECIPE_SHA256}
    receipt_path = work / "build-receipt.json"
    try:
        if args.imported:
            imported = args.imported.resolve(strict=True)
            if not imported.is_relative_to((ROOT / "private").resolve()):
                raise ValueError("The existing import must be beneath this checkout's private/")
            imported_receipt = json.loads((imported / "receipt.json").read_text(encoding="utf-8"))
            if imported_receipt.get("status") != "extracted":
                raise ValueError("Existing import is incomplete")
            disc = imported / "disc"
            for name, size in imported_receipt["files"].items():
                path = (disc / name).resolve(strict=True)
                if not path.is_relative_to(disc.resolve()) or path.stat().st_size != size:
                    raise ValueError("Existing import differs from its receipt")
        else:
            disc = extract(args.iso, work / "imported-disc", args.extractor)
            imported_receipt = json.loads((disc.parent / "receipt.json").read_text(encoding="utf-8"))
        images = list(disc.glob("*.xbe"))
        if len(images) != 1 or sha256(images[0]) != SUPPORTED_XBE_SHA256:
            raise ValueError("This build currently supports only the verified USA game executable")
        xbe = images[0]
        receipt.update(iso_sha256=imported_receipt["iso_sha256"],
                       xbe_sha256=SUPPORTED_XBE_SHA256, disc=str(disc))
        prepare_lifter(work, lifter_revision)
        # Reuse the proven function boundaries and ordered recoveries, not fresh discovery.
        functions = work / "functions.json"
        functions.write_text(json.dumps([
            entry for shard in recipe["functions"]
            for entry in json.loads((ROOT / shard).read_text(encoding="utf-8"))
        ]) + "\n", encoding="utf-8")
        targets = ROOT / recipe["manual_call_targets"]
        generated, metadata = work / "generated", work / "metadata"
        generate = [sys.executable, "-u", "-m", "tools.recomp", xbe, "--all", "--split", "1000",
                 "--functions", functions, "--disasm-dir", work / "no-disassembly",
                 "--func-id-dir", work / "no-identification",
                 "--abi-dir", work / "no-abi", "--gen-dir", generated, "--output-dir", metadata,
                 "--manual-call-targets", targets, "--manual-call-targets-sha256", sha256(targets)]
        for recovery in recipe["recoveries"]:
            generate.extend(["--recover-functions", ROOT / recovery])
        command(generate, LIFTER, work / "generate.log")
        summary = json.loads((metadata / "summary.json").read_text(encoding="utf-8"))
        if summary["failed"] or summary["total"] != summary["translated"]:
            raise ValueError("Generation failed; see generate.log")
        receipt["translated"] = summary["translated"]
        unresolved_file = generated / "recomp_stubs_unresolved.c"
        unresolved = len(re.findall(r"void sub_[0-9A-Fa-f]+\(void\)",
                                   unresolved_file.read_text(encoding="utf-8"))) if unresolved_file.exists() else 0
        receipt["unresolved_targets"] = unresolved
        print(f"Generated {summary['translated']} bodies; {unresolved} unresolved targets. "
              "The runner will stop if it reaches an unresolved target.", flush=True)
        manifest, ebp = program_manifest(generated)
        receipt.update(program_manifest_sha256=manifest, ebp_overrides=ebp)
        if verify_parity:
            verify_files(generated, recipe["generated_files"])
            if ({p.name for p in generated.iterdir() if p.is_file()} != set(recipe["generated_files"])
                    or manifest != recipe["program_manifest_sha256"] or ebp != recipe["ebp_overrides"]):
                raise ValueError("Generated program differs from the proven local recipe")
            receipt["generation_parity"] = "exact-local-match"
            print("Generated game code matches the original local build byte-for-byte.", flush=True)
        if args.generate_only:
            receipt["status"] = "generated-unverified"
            return work
        output = work / "build"
        receipt.update(cmake_generator="Visual Studio 17 2022", platform="x64",
                       configuration="Release", build_parallelism=2)
        configure = ["cmake", "-S", ROOT / "recomp-runtime", "-B", output,
                     "-G", "Visual Studio 17 2022", "-A", "x64",
                     f"-DCMAKE_GENERATOR_INSTANCE={instance}",
                     f"-DRECOMP_PROGRAM_DIR={generated}", f"-DRECOMP_PROGRAM_MANIFEST_SHA256={manifest}",
                     f"-DRECOMP_PROGRAM_EBP_EXPECTED={ebp}"]
        icon = ROOT / "private/doaxbv.ico"
        if icon.is_file():
            configure.append(f"-DRECOMP_APP_ICON={icon.as_posix()}")
        command(configure, ROOT, work / "configure.log")
        command(["cmake", "--build", output, "--config", "Release", "--parallel", "2",
                 "--target", "recomp_program_runner", "--", "/nodeReuse:false"], ROOT, work / "build.log")
        runner = output / "Release/recomp_program_runner.exe"
        receipt.update(status="built-unverified", runner=str(runner), runner_sha256=sha256(runner))
        print(f"Built diagnostic runner: {runner}. Gameplay has not been validated.", flush=True)
        return work
    except Exception as error:
        receipt.update(status="failed", error=str(error))
        raise
    finally:
        receipt_path.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
        if receipt["status"] == "built-unverified":
            selected = ROOT / "private" / ("active-build-" + uuid.uuid4().hex + ".tmp")
            selected.write_text(json.dumps({"receipt": str(receipt_path.relative_to(ROOT))}) + "\n", encoding="utf-8")
            selected.replace(ROOT / "private/active-build.json")
            print("Setup complete. Open Launcher.cmd to play this build.", flush=True)
        print(f"Build receipt: {receipt_path}", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--iso", type=Path)
    inputs.add_argument("--imported", type=Path, help="Reuse a completed private import without re-extracting")
    bundled = ROOT / "tools/artifacts/extract-xiso.exe"
    parser.add_argument("--extractor", default=str(bundled) if bundled.is_file() else "extract-xiso")
    parser.add_argument("--generate-only", action="store_true", help="Stop before compilation for diagnosis")
    parser.add_argument("--install-prerequisites", action="store_true",
                        help="Offer to install missing Microsoft C++ tools and Windows SDK (asks first)")
    try:
        build(parser.parse_args())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Build failed: {error}", file=sys.stderr)
        sys.exit(1)
