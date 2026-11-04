# SPDX-License-Identifier: GPL-3.0-or-later
"""Check local build tools and optionally download the pinned llvm-mingw compiler and DXC."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import urllib.error
import urllib.request
import zipfile

from extract_iso import run_logged, sha256


LLVM_MINGW_URL = ("https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/"
                  "llvm-mingw-20261006-ucrt-x86_64.zip")
LLVM_MINGW_SHA256 = "317492c456aa27ee607a5919f1d2d38dcdc1112516a24d0bf4b00d078f52d17a"
LLVM_MINGW = Path("tools/llvm-mingw")
# RTGI's shaders need DXC; recomp-runtime/CMakeLists.txt looks for it in tools/dxc.
DXC_URL = ("https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/"
           "dxc_2026_09_29.zip")
DXC_SHA256 = "ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1"
DXC = Path("tools/dxc")
EXTRACT_XISO_URL = "https://github.com/XboxDev/extract-xiso.git"
EXTRACT_XISO_COMMIT = "b72e5b60d598ec6df80534cda19cdcd4361aa18c"  # build-202505152050, as the Windows ZIP bundles
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


def extended_path(path):
    """Return an absolute path that Windows file APIs accept beyond 260 characters."""
    path = os.path.abspath(path)
    if os.name != "nt" or path.startswith("\\\\?\\"):
        return path
    return "\\\\?\\UNC\\" + path[2:] if path.startswith("\\\\") else "\\\\?\\" + path


def install_archive(root, name, url, digest, folder_name, size, single_folder):
    """Download a pinned ZIP, check its SHA-256 and move it into root/folder_name.

    single_folder: the ZIP holds one versioned folder whose contents are the tool.
    """
    target = root / folder_name
    if target.exists():
        raise ValueError(f"{target} exists but is incomplete. Delete it and rerun setup.")
    private = root / "private"
    private.mkdir(exist_ok=True)
    for stale in private.glob(f"{name}-*"):  # staging an earlier run could not clean up
        shutil.rmtree(extended_path(stale), ignore_errors=True)
    # Staging adds ~70 characters to the ZIP's deepest paths, which can pass 260 under a long
    # install folder when Windows long paths are disabled (#171).
    # A folder antivirus still locks must not hide the rename error below.
    with tempfile.TemporaryDirectory(prefix=f"{name}-", dir=extended_path(private),
                                     ignore_cleanup_errors=True) as folder:
        archive = Path(folder) / f"{name}.zip"
        print(f"Downloading {name} ({size})...", flush=True)
        try:
            with urllib.request.urlopen(url, timeout=60) as response, archive.open("xb") as file:
                shutil.copyfileobj(response, file)
        except (OSError, urllib.error.URLError) as error:
            raise ValueError(f"Could not download {name}: {error}. Retry when online.") from error
        if sha256(archive) != digest:
            raise ValueError(f"The {name} download failed its SHA-256 check. Rerun setup.")
        print(f"Extracting {name}...", flush=True)
        staging = Path(folder) / "extract"
        with zipfile.ZipFile(archive) as contents:
            contents.extractall(staging)
        if single_folder:
            (staging,) = staging.iterdir()
        target.parent.mkdir(parents=True, exist_ok=True)
        for attempt in range(10):
            try:
                staging.replace(target)
                break
            except PermissionError as error:
                # Antivirus can hold the freshly extracted files open for a few seconds.
                if attempt == 9:
                    raise ValueError(f"Could not move {name} into {target}: {error}. "
                                     "Antivirus may still be scanning it; rerun setup.") from error
                time.sleep(3)


def install_llvm_mingw(root):
    install_archive(root, "llvm-mingw", LLVM_MINGW_URL, LLVM_MINGW_SHA256, LLVM_MINGW, "190 MB", True)


def install_dxc(root):
    install_archive(root, "dxc", DXC_URL, DXC_SHA256, DXC, "32 MB", False)


def ensure_extract_xiso(root, logs):
    """Build the pinned XboxDev extract-xiso; macOS and Linux have no package for it."""
    source = root / "tools/extract-xiso" / EXTRACT_XISO_COMMIT[:12]
    tool = source / "build/extract-xiso"
    built = source / "build-complete"  # an interrupted link can leave a partial binary
    if built.is_file() and tool.is_file():
        return tool
    missing = [name for name in ("git", "cmake") if shutil.which(name) is None]
    if missing:
        raise ValueError(f"Install {' and '.join(missing)} to build extract-xiso, or pass --extractor <executable>")
    shutil.rmtree(source, ignore_errors=True)  # a build an earlier run did not finish
    generator = "Ninja" if shutil.which("ninja") else "Unix Makefiles"
    try:
        run_logged(["git", "clone", "--quiet", "--no-checkout", EXTRACT_XISO_URL, source],
                   logs / "extract-xiso-download.log")
        run_logged(["git", "-C", source, "checkout", "--quiet", EXTRACT_XISO_COMMIT], logs / "extract-xiso-checkout.log")
        run_logged(["cmake", "-S", source, "-B", source / "build", "-G", generator, "-DCMAKE_BUILD_TYPE=Release"],
                   logs / "extract-xiso-configure.log")
        run_logged(["cmake", "--build", source / "build"], logs / "extract-xiso-build.log")
    except (OSError, subprocess.CalledProcessError) as error:
        raise ValueError(f"Could not build extract-xiso: {error}. Install XboxDev extract-xiso "
                         "or pass --extractor <executable>") from error
    if not tool.is_file():
        raise ValueError(f"extract-xiso built, but {tool} is missing. Pass --extractor <executable>")
    built.touch()
    return tool


def ensure_prerequisites(root, install=False):
    compiler = find_toolchain(root)
    if not compiler:
        if not install:
            raise ValueError(HELP)
        install_llvm_mingw(root)
        compiler = find_toolchain(root)
        if not compiler:
            raise ValueError("llvm-mingw was extracted, but tools/llvm-mingw/bin/clang.exe is still missing. " + HELP)
    # Without DXC the build still succeeds, but without RTGI.
    if os.name == "nt" and install and not (root / DXC / "bin/x64/dxc.exe").is_file():
        install_dxc(root)
    return compiler
