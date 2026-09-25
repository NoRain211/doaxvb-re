<div align="center">

# DOAXBV Native PC Port

**A native Windows port of _Dead or Alive Xtreme Beach Volleyball_, built by static recompilation and rewritten into readable source.**

[![Latest release](https://img.shields.io/github/v/release/NoRain211/doaxbv-re?include_prereleases&label=alpha)](https://github.com/NoRain211/doaxbv-re/releases/latest)
[![CI](https://github.com/NoRain211/doaxbv-re/actions/workflows/public-ci.yml/badge.svg)](https://github.com/NoRain211/doaxbv-re/actions/workflows/public-ci.yml)
[![License: GPL-3.0-or-later](https://img.shields.io/badge/license-GPL--3.0--or--later-blue)](LICENSE)
![Platform: Windows x64](https://img.shields.io/badge/platform-Windows%2010%2F11%20x64-0078D6)

[Download](https://github.com/NoRain211/doaxbv-re/releases/latest) ·
[Changelog](CHANGELOG.md) ·
[Status](docs/public-status.md) ·
[Controls](docs/recomp-controls.md) ·
[Report a bug](https://github.com/NoRain211/doaxbv-re/issues)

</div>

> [!IMPORTANT]
> This project ships **no game files**. You need your own legally obtained copy
> of the USA Xbox release. It is not affiliated with or endorsed by the game's
> rights holders.

## About

The original Xbox executable is lifted to C with a static recompiler, then
linked against a hand-written runtime that replaces the Xbox kernel, audio and
Direct3D 8 with native Windows equivalents. Generated code is the scaffold:
game logic is replaced with readable, hand-written source cluster by cluster
while the game keeps running.

## Status

> [!WARNING]
> This is an experimental alpha. Occasional volleyball stalls are under
> investigation, and lighting, camera behavior and some offline activities are
> incomplete.

| Area | State |
| --- | --- |
| Exhibition volleyball, character select | Working, including Xbox Series controllers |
| Jungle, Beach, Niki Beach, Private Beach | Working in local testing |
| Take a Rest, Hopping Game, Radio Station | Working in local testing |
| Movies and native audio | Working, including natural endings and skips |
| Store purchases, saves and reloads | Working, with interrupted-save recovery |
| Portraits, pool water, item previews | Restored |
| Lighting, camera, full rendering fidelity | Incomplete |
| Casino, collection, activity variants | Not yet verified |
| Rollback multiplayer | Planned ([#20](https://github.com/NoRain211/doaxbv-re/issues/20)) |

Remaining work is tracked in the
[offline acceptance tracker](https://github.com/NoRain211/doaxbv-re/issues/16).
Development results do not guarantee every route works in the packaged alpha.

## Play the alpha

**Requirements:** Windows 10/11 x64, a supported USA game ISO, Git with
internet access, CMake 3.20+, and Visual Studio 2022 Build Tools with the C++
desktop workload and a Windows SDK.

1. Download the named Alpha ZIP from the
   [latest release](https://github.com/NoRain211/doaxbv-re/releases/latest)
   and extract it into a new folder with a short path.
2. Drag your ISO onto `BuildGame.cmd` and wait for **Setup complete**.
3. Run `RunGame.cmd` from the same folder.

Setup extracts your ISO and builds an x64 Release runner locally; the package
contains tools and source, never a prebuilt runner or game data. When updating,
build in a new folder and keep your previous install and saves. GitHub's
automatic source archives are source only; see the
[source build guide](docs/building.md) for their prerequisites.

New and older saves start in Digital control mode until you pick a PC
control mode; later Analog or Digital choices are remembered. See the
[controller guide](docs/recomp-controls.md).

## Build the tests from source

The runtime tests build without any game files or submodules:

```powershell
git clone https://github.com/NoRain211/doaxbv-re.git
cd doaxbv-re
cmake -S recomp-runtime -B build/recomp-runtime -G "Visual Studio 17 2022"
cmake --build build/recomp-runtime --config Release --parallel 2
ctest --test-dir build/recomp-runtime -C Release --output-on-failure
```

This produces test executables, not the game runner. For ISO setup from source,
follow the [source build guide](docs/building.md). Passing tests does not
establish full game accuracy.

## Repository layout

| Path | Contents |
| --- | --- |
| [`recomp-runtime/`](recomp-runtime) | Runtime, kernel and input adapters, audio, D3D8 replacements, presentation, tests |
| [`xbe/`](xbe) | XBE parsing and hashing |
| [`tools/`](tools) | Extraction and launcher scripts, lifter submodule, source patches, export checks |
| [`docs/`](docs) | Build guide, controls, saves, graphics, status and research |
| [`third_party/`](third_party) | Pinned rbengine and recomp-net dependencies ([notes](third_party/README.md)) |
| `private/` | Ignored local game inputs, generated output and run evidence |

## Contributing

Read the [contribution guide](CONTRIBUTING.md) first. Translation defects are
fixed in the lifter and regenerated; generated game C is never patched by hand.

When [reporting a bug](https://github.com/NoRain211/doaxbv-re/issues), include
your build version, steps to reproduce, what happened, your hardware and
controller, and the stop or crash message
([how to read logs](recomp-runtime/README.md#reading-stop-and-crash-logs)).
Remove private paths from logs, and never upload game binaries, generated game
C, assets, extracted filenames, saves, BIOS data or private run evidence.

## AI assistance

This project uses large language models and AI coding agents extensively for
code, reverse-engineering analysis, debugging, tests and documentation. Their
output can contain mistakes; changes are reviewed and tested, and unverified
behavior is tracked as such.

## License

Original code and documentation are licensed under
[GPL-3.0-or-later](LICENSE); see also [NOTICE](NOTICE). Third-party components
keep their own licenses. The license grants no rights to the game or its assets.

## Support

If you like this work, consider [buying me a coffee on Ko-fi](https://ko-fi.com/norainsrecomps).
