# SPDX-License-Identifier: GPL-3.0-or-later
"""Stage the Windows, macOS and Linux release ZIPs from a tagged source ref.

usage: python tools/package_release.py REF OUT_DIR --bundled RC1_ZIP
       python tools/package_release.py --check --bundled RC1_ZIP

The Windows ZIP adds the bundled tools (Python with Capstone, MinGit, CMake,
extract-xiso, SMAA) and icon that the shipped 0.6.0 RC1 Windows ZIP carries
beyond its tagged source; --check proves this script still rebuilds that ZIP.
Version, change note and release text come from tools/release/, and the icon
must match private/release-assets/doaxbv.ico.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
RELEASE = ROOT / "tools/release"
BUNDLED_REF = "v0.6.0-rc1"
# Packaging inputs stay in the repository, like CI and export tooling.
PACKAGING = ("tools/package_release.py", "tools/test_package_release.py", "tools/release/")
# The netplay matchmaker runs on a server, as in doa2u-re's releases; players only need its address.
SERVER_ONLY = ("tools/matchmaker.py", "tools/test_matchmaker.py")
GAME_DATA = {".xbe", ".iso", ".xiso", ".afs", ".xpr", ".xbx", ".sav", ".jsonl"}
UNIX_PLATFORMS = {
    "macOS": ("macOS (arm64, x64)", "README-macos.txt",
              "Xcode command line tools; Homebrew cmake, ninja, sdl3; setup installs capstone 5.0.9 "
              "into private/venv and builds extract-xiso build-202505152050"),
    "Linux": ("Linux x86-64 (SteamOS via a container build)", "README-linux.txt",
              "Ubuntu packages in README.txt (cmake, ninja, clang, python3-venv, SDL3 build "
              "dependencies, libshaderc1); setup installs capstone 5.0.9 into private/venv, builds "
              "extract-xiso build-202505152050 and fetches SDL3 3.4.12 by hash when absent"),
}


def require(ok, message):
    """Custody checks must hold under python -O too, so they are not asserts."""
    if not ok:
        raise SystemExit(f"package_release: {message}")


def sha(data):
    return hashlib.sha256(data).hexdigest()


def git(*args):
    return subprocess.check_output(["git", *args], cwd=ROOT)


def tree(ref):
    archive = zipfile.ZipFile(io.BytesIO(git("archive", "--format=zip", ref)))
    return {i.filename: archive.read(i) for i in archive.infolist() if not i.is_dir()}


def is_packaging(name):
    return name.startswith(PACKAGING)


def windows_members(common):
    """Windows runs the .cmd launchers, so the root shell scripts stay out."""
    return {n: d for n, d in common.items() if not (n.endswith(".sh") and "/" not in n)}


def unix_members(common):
    """macOS and Linux run the shell scripts, so the root .cmd launchers stay out."""
    return {n: d for n, d in common.items() if not (n.endswith(".cmd") and "/" not in n)}


def check(kept, recipe, gitlinks, allowed_private, banned_suffixes):
    # Custody: nothing private, generated, retail or agent-facing ships.
    require(not any(n in gitlinks for n in kept), "a submodule would ship")
    require(not any(Path(n).name.lower() in ("agents.md", "claude.md") or n.startswith("docs/agents/") for n in kept),
            "agent instructions would ship")
    require(not any(n.startswith("private/") and n not in allowed_private for n in kept), "a private file would ship")
    require(not any(Path(n).suffix.lower() in banned_suffixes for n in kept), "game data or a binary would ship")
    require(not any(Path(n).name.startswith("recomp_") and Path(n).suffix in {".c", ".exe"} for n in kept),
            "generated code or a runner would ship")
    require(not any("checksum" in Path(n).name.lower() or Path(n).name.upper().startswith("SHA256SUM") for n in kept),
            "a checksum file would ship")
    require(not any(is_packaging(n) for n in kept), "packaging inputs would ship")
    require(not any(n in SERVER_ONLY for n in kept), "the matchmaker server would ship")
    for name, digest in recipe["files"].items():
        require(name in kept and sha(kept[name]) == digest, f"{name} differs from the recipe")


def write(out, prefix, kept, modes=None, date=None):
    out.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(out, "x", zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for member, data in sorted(kept.items()):
            if modes is None:
                archive.writestr(prefix + member, data)
                continue
            info = zipfile.ZipInfo(prefix + member, date_time=date)
            info.create_system = 3  # Unix, so unzip and Archive Utility apply the mode
            info.external_attr = (0o100755 if modes.get(member) == "100755" else 0o100644) << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, data)
    with zipfile.ZipFile(out) as archive:
        require(archive.testzip() is None and len(archive.namelist()) == len(kept), f"{out} is incomplete")
        require(all(archive.read(prefix + n) == d for n, d in kept.items()), f"{out} differs from its inputs")
    return dict(archive=str(out), sha256=sha(out.read_bytes()), files=len(kept))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("ref", nargs="?")
    parser.add_argument("out_dir", nargs="?", type=Path)
    parser.add_argument("--bundled", type=Path, required=True, help="the shipped 0.6.0 RC1 Windows ZIP")
    parser.add_argument("--check", action="store_true", help="rebuild the RC1 Windows ZIP and list differences")
    args = parser.parse_args()
    if not args.check and not (args.ref and args.out_dir):
        parser.error("REF and OUT_DIR are required unless --check is given")

    base = zipfile.ZipFile(args.bundled)
    shipped = {i.filename.split("/", 1)[1]: base.read(i) for i in base.infolist() if not i.is_dir()}
    old = tree(BUNDLED_REF)
    excluded = set(old) - set(shipped)  # developer and agent files RC1 left out
    extras = {n: d for n, d in shipped.items() if n not in old and n not in ("README.txt", "package.json")}

    def source(ref):
        return {n: d for n, d in tree(ref).items() if n not in excluded and not is_packaging(n) and n not in SERVER_ONLY}

    if args.check:
        kept = source(BUNDLED_REF)
        kept.update(extras)
        kept["README.txt"], kept["package.json"] = shipped["README.txt"], shipped["package.json"]
        differences = sorted(n for n in set(kept) | set(shipped) if kept.get(n) != shipped.get(n))
        print("RC1 rebuild differs in:", differences)
        require(not differences, "the RC1 Windows ZIP no longer rebuilds")
        return

    release = json.loads((RELEASE / "release.json").read_text(encoding="utf-8"))
    version, channel = release["version"], release["channel"]
    readmes = {name: (RELEASE / name).read_text(encoding="utf-8")
               for name in ("README-windows.txt", "README-macos.txt", "README-linux.txt")}
    for name, text in readmes.items():
        require(text.splitlines()[0].startswith(f"DOAXBV {version} {channel.upper()}"), f"{name} names another version")
    bundled_tools = (RELEASE / "BUNDLED-TOOLS.md").read_text(encoding="utf-8")
    require(f"/tree/v{version}" in bundled_tools, "BUNDLED-TOOLS.md links another source tag")
    icon = (ROOT / "private/release-assets/doaxbv.ico").read_bytes()
    require(extras["private/doaxbv.ico"] == icon, "RC1 icon differs from the release asset")

    commit = git("rev-parse", args.ref + "^{commit}").decode().strip()
    modes = {line.split("\t")[1]: line.split()[0] for line in git("ls-tree", "-r", args.ref).decode().splitlines()}
    gitlinks = {n for n, m in modes.items() if m == "160000"}
    date = tuple(int(part) for part in git("show", "-s", "--date=format:%Y %m %d", "--format=%cd", commit).split()) + (0, 0, 0)
    changelog = tree(args.ref)["CHANGELOG.md"].decode("utf-8")
    latest = next(line for line in changelog.splitlines() if line.startswith("## "))
    require(latest.startswith(f"## {version} "), f"{args.ref} is not the {version} release commit; its CHANGELOG opens with {latest!r}")
    common = source(args.ref)
    recipe = json.loads(common["tools/game-recipe/recipe.json"])
    identity = dict(source_base=commit, lifter_revision=recipe["lifter_revision"],
                    recipe_sha256=sha(common["tools/game-recipe/recipe.json"]),
                    program_manifest_sha256=recipe["program_manifest_sha256"])

    results = {}
    win = windows_members(common)
    win.update(extras)
    win["README.txt"] = readmes["README-windows.txt"].replace("\n", "\r\n").encode()
    win["BUNDLED-TOOLS.md"] = bundled_tools.encode()
    meta = json.loads(shipped["package.json"])
    meta.pop("microsoft_build_tools", None)
    meta.update(name=f"DOAXBV-{version}", version=version, channel=channel, cmake_generator="MinGW Makefiles",
                compiler="llvm-mingw 20261006 (downloaded during setup)", release_name=None,
                runtime_change=release["runtime_change"], **identity)
    win["package.json"] = (json.dumps(meta, indent=2) + "\n").encode()
    check(win, recipe, gitlinks, ("private/README.md", "private/doaxbv.ico"), GAME_DATA)
    results["windows"] = write(args.out_dir / f"DOAXBV-{version}.zip", f"DOAXBV-{version}/", win)

    for target, (platform_name, readme, prerequisites) in UNIX_PLATFORMS.items():
        kept = unix_members(common)
        kept["README.txt"] = readmes[readme].encode()
        kept["package.json"] = (json.dumps(dict(
            name=f"DOAXBV-{version}", version=version, channel=channel, platform=platform_name,
            package_kind="source-and-iso-setup", prebuilt_game_runner=False, cmake_generator="Ninja",
            configuration="Release", full_gameplay_acceptance=False, prerequisites=prerequisites,
            **identity), indent=2) + "\n").encode()
        check(kept, recipe, gitlinks, ("private/README.md",), GAME_DATA | {".exe", ".dll", ".ico"})
        for script in ("build_game.sh", "run_game.sh", "launcher.sh"):
            require(modes.get(script) == "100755", f"{script} is not executable")
        results[target] = write(args.out_dir / f"DOAXBV-{version}-{target}.zip", f"DOAXBV-{version}-{target}/",
                                kept, modes, date)
    print(json.dumps(dict(commit=commit, **results), indent=2))


if __name__ == "__main__":
    main()
