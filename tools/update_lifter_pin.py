# SPDX-License-Identifier: GPL-3.0-or-later
"""Change the lifter revision or manual-call selection and authenticate regeneration.

The script generates the current pin (which must still match the recipe) and the
target revision, rewrites the recipe identities, stages them with the submodule and lists
changed game functions. It never commits: review and test the new program first.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

import build_game
from extract_iso import sha256

ROOT = build_game.ROOT
LIFTER = build_game.LIFTER
BODY = re.compile(r"^void (sub_[0-9A-Fa-f]{8})\(void\)", re.MULTILINE)
PINNED = ("tools/game-recipe/recipe.json", "tools/build_game.py", "public-export.json",
          "tools/game-recipe/manual-call-targets.json")


def git(*args):
    return subprocess.check_output(["git", *args], cwd=LIFTER, text=True).strip()


def function_hashes(generated):
    """Hash each generated function body so two programs can be compared by name."""
    bodies = {}
    for path in sorted(generated.glob("recomp_[0-9][0-9][0-9][0-9].c")):
        text = path.read_text(encoding="utf-8")
        starts = [match.start() for match in BODY.finditer(text)] + [len(text)]
        for begin, end in zip(starts, starts[1:]):
            name = BODY.match(text, begin).group(1)
            bodies[name] = hashlib.sha256(text[begin:end].encode("utf-8")).hexdigest()
    return bodies


def compare(old, new):
    return (sorted(name for name in old.keys() & new.keys() if old[name] != new[name]),
            sorted(new.keys() - old.keys()), sorted(old.keys() - new.keys()))


def add_manual_targets(root, additions):
    """Extend the authenticated selection; the lifter validates entry ownership."""
    if not additions:
        return
    if any(not 0 < address <= 0xffffffff for address in additions):
        raise ValueError("Manual-call targets must be nonzero 32-bit addresses")
    recipe_path = root / "tools/game-recipe/recipe.json"
    recipe = json.loads(recipe_path.read_text(encoding="utf-8"))
    name = recipe["manual_call_targets"]
    if name != "tools/game-recipe/manual-call-targets.json":
        raise ValueError("Unexpected manual-call target file")
    targets_path = root / name
    addresses = {int(value, 16) for value in json.loads(targets_path.read_text(encoding="utf-8"))}
    addresses.update(additions)
    targets_path.write_text(json.dumps([f"0x{value:08X}" for value in sorted(addresses)],
                                      indent=2) + "\n", encoding="utf-8", newline="\n")
    recipe["files"][name] = sha256(targets_path)
    recipe_path.write_text(json.dumps(recipe, indent=2) + "\n", encoding="utf-8", newline="\n")


def pin_metadata(root, generated, revision):
    """Rewrite every identity that names the lifter or the generated program."""
    recipe_path = root / "tools/game-recipe/recipe.json"
    recipe = json.loads(recipe_path.read_text(encoding="utf-8"))
    files = {path.name: sha256(path) for path in sorted(generated.iterdir()) if path.is_file()}
    changed = sorted(name for name in files.keys() | recipe["generated_files"].keys()
                     if files.get(name) != recipe["generated_files"].get(name))
    manifest, ebp = build_game.program_manifest(generated)
    recipe.update(lifter_revision=revision, generated_files=files,
                  program_manifest_sha256=manifest, ebp_overrides=ebp)
    # LF on every platform: RECIPE_SHA256 must hash the bytes git stores.
    recipe_path.write_text(json.dumps(recipe, indent=2) + "\n", encoding="utf-8", newline="\n")

    builder = root / "tools/build_game.py"
    source = builder.read_text(encoding="utf-8")
    for name, value in (("LIFTER_REVISION", revision), ("RECIPE_SHA256", sha256(recipe_path))):
        source, count = re.subn(rf'(?m)^{name} = "[0-9a-f]+"$', f'{name} = "{value}"', source)
        if count != 1:
            raise ValueError(f"Could not update {name} in build_game.py")
    builder.write_text(source, encoding="utf-8", newline="\n")

    export_path = root / "public-export.json"
    export = json.loads(export_path.read_text(encoding="utf-8"))
    links = [link for link in export["gitlinks"] if link["path"] == "tools/xboxrecomp"]
    if len(links) != 1:
        raise ValueError("public-export.json must list tools/xboxrecomp once")
    links[0]["commit"] = revision
    export_path.write_text(json.dumps(export, indent=2) + "\n", encoding="utf-8", newline="\n")
    return changed


def generate(imported, verify_parity, revision=None):
    args = argparse.Namespace(imported=imported, iso=None, extractor=None, generate_only=True)
    return build_game.build(args, verify_parity=verify_parity, lifter_revision=revision) / "generated"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--imported", type=Path, required=True, help="Completed import beneath private/")
    parser.add_argument("--revision", help="Lifter commit (default: the fork's codex/doaxbv-recipe tip)")
    parser.add_argument("--manual-call-target", action="append", default=[],
                        type=lambda value: int(value, 0),
                        help="Add a manual-dispatch entry; repeat for a cluster. "
                             "Defaults to the current pin when --revision is omitted.")
    args = parser.parse_args()
    if git("status", "--porcelain"):
        raise SystemExit("tools/xboxrecomp has local changes; commit or discard them first")
    if subprocess.check_output(["git", "status", "--porcelain", "--", *PINNED], cwd=ROOT, text=True).strip():
        raise SystemExit(f"Commit or discard local changes to {', '.join(PINNED)} first")
    # An older checkout may still point the submodule at upstream, which lacks the recipe branch.
    subprocess.run(["git", "submodule", "sync", "--", "tools/xboxrecomp"], cwd=ROOT, check=True)
    previous = git("rev-parse", "HEAD")
    if args.revision:
        revision = git("rev-parse", "--verify", f"{args.revision}^{{commit}}")
    elif args.manual_call_target:
        revision = previous
    else:
        # The fork's main follows upstream, whose lifter line regresses gameplay.
        git("fetch", "origin", "codex/doaxbv-recipe")
        revision = git("rev-parse", "FETCH_HEAD")
    old = generate(args.imported, verify_parity=True)
    saved = {name: (ROOT / name).read_bytes() for name in PINNED}
    try:
        git("checkout", "--detach", revision)
        add_manual_targets(ROOT, args.manual_call_target)
        new = generate(args.imported, verify_parity=False, revision=revision)
        changed_files = pin_metadata(ROOT, new, revision)
    except Exception:
        for name, data in saved.items():
            (ROOT / name).write_bytes(data)
        git("checkout", "--detach", previous)
        raise
    subprocess.run(["git", "add", "tools/xboxrecomp", *PINNED], cwd=ROOT, check=True)
    changed, added, removed = compare(function_hashes(old), function_hashes(new))
    report = new.parent / "changed-functions.txt"
    report.write_text("".join(f"{kind} {name}\n" for kind, names in
                              (("changed", changed), ("added", added), ("removed", removed))
                              for name in names), encoding="utf-8")
    print(f"Recipe regenerated at lifter {revision}. Nothing was committed.")
    print(f"Generated files changed: {', '.join(changed_files) or 'none'}")
    print(f"Functions: {len(changed)} changed, {len(added)} added, {len(removed)} removed; see {report}")


if __name__ == "__main__":
    main()
