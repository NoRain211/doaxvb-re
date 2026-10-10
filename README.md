<div align="center">

<img src="https://github.com/NoRain211/doaxbv-re/releases/download/readme-media/doaxbv-logo.png" alt="Dead or Alive Xtreme Beach Volleyball logo" width="420">

# DOAXVB Native PC Port

**A native PC port of _Dead or Alive Xtreme Beach Volleyball_ (Windows & macOS), built by static recompilation, with the goal of rewriting it into readable source.**

[![Latest release](https://img.shields.io/github/v/release/NoRain211/doaxbv-re?label=beta)](https://github.com/NoRain211/doaxbv-re/releases/latest)
[![CI](https://github.com/NoRain211/doaxbv-re/actions/workflows/public-ci.yml/badge.svg)](https://github.com/NoRain211/doaxbv-re/actions/workflows/public-ci.yml)
[![License: GPL-3.0-or-later](https://img.shields.io/badge/license-GPL--3.0--or--later-blue)](LICENSE)
![Platform: Windows & macOS](https://img.shields.io/badge/platform-Windows%20%7C%20macOS-0078D6)

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
linked against a hand-written runtime that replaces the Xbox kernel imports,
audio output and key Direct3D 8 and DirectSound APIs with native Windows
equivalents. The rest of the game, including its statically linked libraries,
still runs as generated code. The plan is to replace game logic with readable,
hand-written source one data-structure cluster at a time while the game keeps
running; so far only the startup thread entry has been rewritten.

## Screenshots

<p align="center">
  <img src="https://github.com/NoRain211/doaxbv-re/releases/download/readme-media/doaxbv-match.jpg" alt="Beach volleyball match at dusk" width="100%">
</p>
<table>
  <tr>
    <td width="50%"><img src="https://github.com/NoRain211/doaxbv-re/releases/download/readme-media/doaxbv-pool.jpg" alt="Hopping Game at the pool"></td>
    <td width="50%"><img src="https://github.com/NoRain211/doaxbv-re/releases/download/readme-media/doaxbv-beach.jpg" alt="Kasumi on the beach at dusk"></td>
  </tr>
</table>

<p align="center"><sub>Captured from the native port at 2560x1440.</sub></p>

## Status

> [!NOTE]
> **Fully playable as of 0.4 Beta.** Long player sessions cover every stage,
> the Casino, Hotel and Shops, saving and reloading, and a vacation's end
> carrying into a new one (that run started from an edited day-14 save; see
> [status](docs/public-status.md)). Known issues: a second Hopping Game playable before
> it loads ([#54](https://github.com/NoRain211/doaxbv-re/issues/54)), an
> upside-down map flash leaving the pool
> ([#55](https://github.com/NoRain211/doaxbv-re/issues/55)) and possibly missing
> underwater textures ([#56](https://github.com/NoRain211/doaxbv-re/issues/56)).

| Area | State |
| --- | --- |
| Exhibition volleyball, character select | Working, including Xbox Series controllers |
| PlayStation 4/5 controllers, up to four controllers, vibration | Added in 0.4.8; PlayStation pads not yet confirmed on hardware |
| Jungle, Beach, Niki Beach, Private Beach | Working |
| Take a Rest, Hopping Game, Radio Station | Working |
| Casino, Hotel, full vacation to its end | Working in player sessions |
| Movies and native audio | Working, including natural endings and skips |
| Store purchases, saves and reloads | Working, with interrupted-save recovery |
| Deleting saves | Fixed in 0.4.9 |
| Custom soundtracks (MP3, WAV, FLAC from `UserMusic`) | Added in 0.5.0 |
| Radio Station shuffle (launcher checkbox) | Added in 0.5.3 |
| Portraits, pool water, item previews | Restored |
| Lighting, camera, full rendering fidelity | Incomplete |
| Every collection item and activity/time variant | Not individually verified |
| Hand-written game logic | Startup thread entry only; the rest runs as generated code |
| Rollback multiplayer | Planned ([#20](https://github.com/NoRain211/doaxbv-re/issues/20)) |

Remaining work is tracked in the
[offline acceptance tracker](https://github.com/NoRain211/doaxbv-re/issues/16).
Development results do not guarantee every route works in the packaged beta.

## Play the beta

**Requirements:** Windows 10/11 x64, a supported USA game ISO and internet
access. The 0.5.3 Beta ZIP bundles Python, Capstone, Git, CMake and the ISO
extractor. If the Microsoft C++ compiler or Windows SDK is missing, setup
offers to download and run Microsoft's installer. That installation needs
administrator approval and several GB of disk space; the full Visual Studio
IDE is not required.

1. Download the ZIP from the
   [latest release](https://github.com/NoRain211/doaxbv-re/releases/latest)
   and extract it into a new folder with a short path.
2. Drag your ISO onto `BuildGame.cmd`, follow any prerequisite installation
   prompts and wait for **Setup complete**.
3. Run `Launcher.cmd` from the same folder to pick resolution, MSAA and SMAA,
   then press **Play**. Running `RunGame.cmd` directly uses the defaults.

On macOS:
1. Install the prerequisites in [docs/building.md](docs/building.md#macos-build).
2. Run `./build_game.sh /path/to/game.iso` and wait for setup to complete.
3. Run `./launcher.sh` to select resolution, volume, and soundtrack options, then press **Play** (or run `./run_game.sh` directly).

Setup extracts your ISO and builds a native Release runner locally; the package
contains tools and source, never a prebuilt runner or game data. When updating,
build in a new folder and keep your previous install and saves. GitHub's
automatic source archives are source only; see the
[source build guide](docs/building.md) for their prerequisites.

New and older saves start in Digital control mode until you pick a PC
control mode; later Analog or Digital choices are remembered. See the
[controller guide](docs/recomp-controls.md).

Xbox and PlayStation 4/5 controllers work directly. Do not run DS4Windows
with the game: it adds an emulated Xbox controller, so one pad appears twice
and takes two player ports. If you launch through Steam, turn Steam Input off
for the game.

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

### Contributors

- [Tommy McLeroy (@tmcleroy)](https://github.com/tmcleroy): creator of the
  macOS port (#99).

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
