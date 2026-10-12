# SPDX-License-Identifier: GPL-3.0-or-later
"""Check local build tools and optionally download the pinned llvm-mingw compiler."""
import os
from pathlib import Path
import shutil
import tempfile
import urllib.error
import urllib.request
import zipfile

from extract_iso import sha256


LLVM_MINGW_URL = ("https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/"
                  "llvm-mingw-20261006-ucrt-x86_64.zip")
LLVM_MINGW_SHA256 = "317492c456aa27ee607a5919f1d2d38dcdc1112516a24d0bf4b00d078f52d17a"
LLVM_MINGW = Path("tools/llvm-mingw")
HELP = ("The Windows build compiles with llvm-mingw. Rerun with --install-prerequisites "
        "(BuildGame.cmd does this) to download it into tools/llvm-mingw, or extract "
        f"{LLVM_MINGW_URL} there so that tools/llvm-mingw/bin/clang.exe exists.")


def prefer_bundled_tools(root):
    bundled = [str((root / name).parent) for name in
               ("tools/git/cmd/git.exe", "tools/cmake/bin/cmake.exe", LLVM_MINGW / "bin/clang.exe")
               if (root / name).is_file()]
    if bundled:
        existing = os.environ.get("PATH", "").split(os.pathsep)
        os.environ["PATH"] = os.pathsep.join(bundled + [p for p in existing if p not in bundled])


def find_toolchain(root):
    if os.name != "nt":
        # CMake finds the host compiler; check only that the tools are installed.
        compiler = shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")
        if compiler is None or shutil.which("cmake") is None:
            raise ValueError("CMake and a C++17 compiler are required; see docs/building.md.")
        return compiler
    clang = root / LLVM_MINGW / "bin/clang.exe"
    return clang if clang.is_file() else None


def install_llvm_mingw(root):
    target = root / LLVM_MINGW
    if target.exists():
        raise ValueError(f"{target} exists but has no bin/clang.exe. Delete it and rerun setup.")
    private = root / "private"
    private.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="llvm-mingw-", dir=private) as folder:
        archive = Path(folder) / "llvm-mingw.zip"
        print("Downloading the llvm-mingw compiler (190 MB)...", flush=True)
        try:
            with urllib.request.urlopen(LLVM_MINGW_URL, timeout=60) as response, archive.open("xb") as file:
                shutil.copyfileobj(response, file)
        except (OSError, urllib.error.URLError) as error:
            raise ValueError(f"Could not download llvm-mingw: {error}. Retry when online.") from error
        if sha256(archive) != LLVM_MINGW_SHA256:
            raise ValueError("The llvm-mingw download failed its SHA-256 check. Rerun setup.")
        print("Extracting llvm-mingw...", flush=True)
        staging = Path(folder) / "extract"
        with zipfile.ZipFile(archive) as contents:
            contents.extractall(staging)
        (top,) = staging.iterdir()  # the release ZIP holds one versioned folder
        target.parent.mkdir(parents=True, exist_ok=True)
        top.replace(target)


def ensure_prerequisites(root, install=False):
    compiler = find_toolchain(root)
    if compiler:
        return compiler
    if not install:
        raise ValueError(HELP)
    install_llvm_mingw(root)
    compiler = find_toolchain(root)
    if not compiler:
        raise ValueError("llvm-mingw was extracted, but tools/llvm-mingw/bin/clang.exe is still missing. " + HELP)
    return compiler
