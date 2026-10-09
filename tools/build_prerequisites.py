# SPDX-License-Identifier: GPL-3.0-or-later
"""Check local Windows build tools and optionally install Microsoft's Build Tools."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import urllib.error
import urllib.request


INSTALLER_URL = "https://aka.ms/vs/17/release/vs_BuildTools.exe"
POWERSHELL = Path(os.environ.get("SystemRoot", "C:/Windows")) / "System32/WindowsPowerShell/v1.0/powershell.exe"
HELP = ("Visual Studio 2022 C++ tools and a usable Windows SDK are required. "
        "Install Desktop development with C++ and a Windows SDK using Visual Studio Installer, "
        "or rerun with --install-prerequisites (BuildGame.cmd offers this automatically).")

# Keep signature validation and launch together; paths arrive as data, not PowerShell code.
INSTALL_SCRIPT = r"""
$ErrorActionPreference = 'Stop'
try {
    $signature = Get-AuthenticodeSignature -LiteralPath $env:DOAXBV_INSTALLER
    if ($signature.Status -ne 'Valid' -or $null -eq $signature.SignerCertificate -or
        $signature.SignerCertificate.GetNameInfo(
            [System.Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false
        ) -ne 'Microsoft Corporation') {
        throw 'Installer does not have a valid Microsoft Authenticode signature.'
    }
    $arguments = '--passive --norestart --wait ' +
        '--add Microsoft.VisualStudio.Workload.VCTools ' +
        '--add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ' +
        '--add Microsoft.VisualStudio.Component.Windows11SDK.26100'
    if ($env:DOAXBV_BUILD_TOOLS) {
        $arguments = 'modify --channelId VisualStudio.17.Release --installPath "' +
            $env:DOAXBV_BUILD_TOOLS + '" ' + $arguments
    }
    $process = Start-Process -FilePath $env:DOAXBV_INSTALLER -ArgumentList $arguments `
        -Verb RunAs -PassThru -Wait
    exit $process.ExitCode
} catch {
    if ($_.Exception.NativeErrorCode -eq 1223 -or
        $_.Exception.InnerException.NativeErrorCode -eq 1223) { exit 1223 }
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
"""

PROBE_CMAKE = """cmake_minimum_required(VERSION 3.20)
project(build_prerequisites C CXX RC)
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
try_compile(READY "${CMAKE_BINARY_DIR}/probe"
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/probe.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/probe.rc"
    LINK_LIBRARIES d3d11 d3dcompiler dxgi xaudio2
    OUTPUT_VARIABLE DETAILS)
if(NOT READY)
    message(FATAL_ERROR "C++ / Windows SDK check failed: ${DETAILS}")
endif()
"""


def prefer_bundled_tools(root):
    bundled = [str((root / name).parent) for name in
               ("tools/git/cmd/git.exe", "tools/cmake/bin/cmake.exe")
               if (root / name).is_file()]
    if bundled:
        existing = os.environ.get("PATH", "").split(os.pathsep)
        os.environ["PATH"] = os.pathsep.join(bundled + [p for p in existing if p not in bundled])


def visual_studios(*filters):
    vswhere = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio/Installer/vswhere.exe"
    if not vswhere.is_file():
        return []
    output = subprocess.check_output(
        [str(vswhere), "-version", "[17.0,18.0)", "-sort", "-utf8",
         "-property", "installationPath", *filters], text=True, encoding="utf-8")
    return [Path(line.strip()) for line in output.splitlines() if line.strip()]


def find_toolchain(root):
    if os.name != "nt":
        # CMake finds the host compiler; check only that the tools are installed.
        compiler = shutil.which("c++") or shutil.which("clang++") or shutil.which("g++")
        if compiler is None or shutil.which("cmake") is None:
            raise ValueError("CMake and a C++17 compiler are required; see docs/building.md.")
        return compiler
    instances = visual_studios("-products", "*", "-requires",
                               "Microsoft.VisualStudio.Component.VC.Tools.x86.x64")
    if not instances:
        return None
    private = root / "private"
    private.mkdir(exist_ok=True)
    log_path = private / "prerequisites-check.log"
    print("Checking Visual Studio 2022 C++ and Windows SDK...", flush=True)
    with tempfile.TemporaryDirectory(prefix="prerequisites-check-", dir=private) as folder:
        source = Path(folder)
        (source / "CMakeLists.txt").write_text(PROBE_CMAKE, encoding="utf-8")
        (source / "probe.cpp").write_text(
            "#include <windows.h>\n#include <d3d11.h>\n#include <xaudio2.h>\n"
            "#include <string>\nint main() { return std::to_string(GetCurrentProcessId()).empty(); }\n",
            encoding="utf-8")
        (source / "probe.rc").write_text("#include <windows.h>\n1 RCDATA { 0 }\n", encoding="utf-8")
        with log_path.open("w", encoding="utf-8") as log:
            for index, instance in enumerate(instances):
                result = subprocess.run(
                    ["cmake", "-S", str(source), "-B", str(source / str(index)),
                     "-G", "Visual Studio 17 2022", "-A", "x64",
                     f"-DCMAKE_GENERATOR_INSTANCE={instance}"],
                    stdout=log, stderr=subprocess.STDOUT)
                if result.returncode == 0:
                    return instance
    print(f"The C++ / Windows SDK check failed. Details: {log_path}", flush=True)
    return None


def install_build_tools(root):
    private = root / "private"
    private.mkdir(exist_ok=True)
    existing = visual_studios("-products", "Microsoft.VisualStudio.Product.BuildTools")
    with tempfile.TemporaryDirectory(prefix="prerequisites-install-", dir=private) as folder:
        installer = Path(folder) / "vs_BuildTools.exe"
        print("Downloading Microsoft Visual Studio 2022 Build Tools installer...", flush=True)
        try:
            with urllib.request.urlopen(INSTALLER_URL, timeout=60) as response, installer.open("xb") as target:
                shutil.copyfileobj(response, target)
        except (OSError, urllib.error.URLError) as error:
            raise ValueError(f"Could not download the Microsoft installer: {error}. Retry when online.") from error
        print("Verifying Microsoft's signature, then opening the installer.\n"
              "Approve its Windows UAC prompt to continue; installation can take several minutes.\n"
              "Keep this window open. The build will resume after installation.", flush=True)
        result = subprocess.run(
            [str(POWERSHELL), "-NoProfile", "-NonInteractive", "-Command", INSTALL_SCRIPT],
            # A PowerShell 7 parent can otherwise make Windows PowerShell load incompatible modules.
            env=dict(os.environ, DOAXBV_INSTALLER=str(installer),
                     PSModulePath=str(POWERSHELL.parent / "Modules"),
                     DOAXBV_BUILD_TOOLS=str(existing[0]) if existing else ""),
            capture_output=True, text=True)
    code = result.returncode
    if code in (1223, 1602, 5004, 0xC000013A, -1073741510):
        raise ValueError("Build Tools installation or UAC was cancelled. Rerun BuildGame.cmd when ready.")
    if code in (1641, 3010):
        raise ValueError("Build Tools requires a Windows restart. Restart, then rerun BuildGame.cmd.")
    if code:
        detail = (result.stderr or result.stdout).strip()
        raise ValueError(f"Microsoft Build Tools installer failed (exit {code}). {detail}\n"
                         "See the dd_bootstrapper, dd_client and dd_setup logs in %TEMP%.")


def ensure_prerequisites(root, install=False):
    instance = find_toolchain(root)
    if instance:
        return instance
    if not install:
        raise ValueError(HELP)
    print("\nVisual Studio 2022 C++ tools or a working Windows SDK are missing.\n"
          "Microsoft Build Tools will download and install several gigabytes of software.\n"
          "Only Microsoft's installer requests administrator permission (UAC).\n"
          "The build continues in this window without elevation; a restart may be required.", flush=True)
    try:
        consent = input("Download and install these prerequisites? [y/N] ").strip().lower()
    except (EOFError, KeyboardInterrupt):
        consent = ""
    if consent not in ("y", "yes"):
        raise ValueError("Prerequisite installation declined. " + HELP)
    install_build_tools(root)
    instance = find_toolchain(root)
    if not instance:
        raise ValueError("Installation finished, but the C++ / Windows SDK check still fails. " + HELP)
    return instance
