# Bundled tools

The release ZIP keeps upstream license and copyright files with each tool.
These tools run locally; no game content is included.

| Tool | Version | License location / upstream |
| --- | --- | --- |
| Python | 3.12.7 | `tools/python/LICENSE.txt`; https://www.python.org/downloads/release/python-3127/ |
| Capstone | 5.0.9 distribution | `tools/python/Lib/site-packages/capstone-5.0.9.dist-info/LICENSE.TXT`; https://github.com/capstone-engine/capstone |
| Git for Windows (MinGit, x64) | 2.55.0.5 | `tools/git/LICENSE.txt`, `tools/git/mingw64/share/licenses/`, `tools/git/usr/share/licenses/`; https://github.com/git-for-windows/git/releases/tag/v2.55.0.windows.5 |
| CMake (x64) | 4.2.1 | `tools/cmake/doc/cmake/LICENSE.rst`, `tools/cmake/doc/cmake/`, `tools/cmake/share/cmake-4.2/Licenses/`; https://github.com/Kitware/CMake/releases/tag/v4.2.1 |
| XboxDev extract-xiso | build-202505152050 | `tools/artifacts/LICENSE.TXT`; https://github.com/XboxDev/extract-xiso/releases/tag/build-202505152050 |
| SMAA | 71c806a838bdd7d517df19192a20f0c61b3ca29d | `third_party/smaa/LICENSE.txt`; https://github.com/iryoku/smaa |

The extractor includes software developed by in <in@fishtank.com>.
Capstone's 5.0.9 distribution reports binding version 5.0.7.
MinGit is the unmodified portable upstream distribution. CMake includes the
unmodified upstream command-line tools, modules, documentation and licenses;
the unused Qt-based cmake-gui.exe is omitted.

## Downloaded during setup

These are not included in this ZIP. Setup downloads each official release
and checks its SHA-256 before use.

- **llvm-mingw 20261006** (UCRT, x86-64), the C and C++ compiler:
  https://github.com/mstorsjo/llvm-mingw/releases/tag/20261006. Setup
  extracts it into `tools/llvm-mingw`; its licenses (LLVM's Apache-2.0 with
  LLVM exceptions, and mingw-w64's) are in that folder.
- **SDL 3.4.16** (zlib license): `SDL3-devel-3.4.16-VC.zip` from
  https://github.com/libsdl-org/SDL/releases/tag/release-3.4.16. Setup copies
  `SDL3.dll` beside the game runner; its license is `LICENSE.txt` in the
  downloaded package under your setup's build directory.

## Microsoft prerequisites

The included `Prerequisites/vc_redist.x86.exe` is the Microsoft Visual C++
runtime installer for the 32-bit ISO extractor. Microsoft's license terms
apply. Setup no longer installs Visual Studio Build Tools or a Windows SDK.

## Corresponding source

MinGit's exact component versions are in `tools/git/etc/package-versions.txt`.
This is the same MinGit as 0.5.3; its matching source distribution and build
instructions are provided on the 0.5.3 release page:
https://github.com/NoRain211/doaxbv-re/releases/tag/v0.5.3

CMake source (including bundled library source):
https://github.com/Kitware/CMake/releases/download/v4.2.1/cmake-4.2.1.tar.gz

DOAXBV source: https://github.com/NoRain211/doaxbv-re/tree/v0.6.2
The pinned lifter is fetched during setup from NoRain211/xboxrecomp;
its GPL-3.0 license is included in that checkout.
