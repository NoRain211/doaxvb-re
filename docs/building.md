# Building

## Build a local runner from a user-owned ISO

The current `tools/build_game.py` workflow extracts a supported user-owned ISO,
checks its XBE, generates the game program with the `tools/xboxrecomp` submodule
at the fork revision named by `lifter_revision` in the recipe, and
builds an x64 Release runner. The authenticated `tools/game-recipe/recipe.json`
provides the lifter revision; setup initializes the submodule when needed (a
release ZIP clones it from NoRain211/xboxrecomp) and stops if it is modified or
checked out at another revision. The recipe also
provides the original function boundaries, 61 ordered recovery inputs, manual
call targets, and expected generated-file hashes. Setup stops before compiling
if any recipe input or generated file differs. It creates a new `private/setup-*` directory for
each attempt. The receipt, logs, generated program, and runner stay there.

For a source checkout or GitHub's automatic source archive, install:

- Python 3.12 or newer
- Capstone 5.0.9 distribution for that Python installation
- Git with network access
- CMake 3.20 or newer
- Visual Studio 2022 or Build Tools with the C++ desktop workload and a Windows SDK
- XboxDev `extract-xiso`

The repository does not include an all-in-one build executable. Place
`extract-xiso.exe` at `tools/artifacts/extract-xiso.exe`, add it to `PATH`, or
pass its path to `tools/build_game.py`.

The runner icon is optional and kept out of git. Place it at
`private/doaxbv.ico` and the build embeds it; without it, the runner uses
the default Windows icon.

The **DOAXBV-0.5.3-Beta** ZIP bundles Python, Capstone, MinGit, CMake and the
tagged extractor. Setup prefers the bundled Git and CMake for its own process;
it does not change the system PATH. Extract the whole ZIP into a new folder
with a short path before setup
(for example, a folder directly under Downloads). Use the scripts
from that folder.
The bundled Capstone distribution is 5.0.9; its Python binding reports 5.0.7.
`BuildGame.cmd` checks for the Visual Studio 2022 C++ tools and a Windows SDK.
If missing, it offers to download Microsoft's official Build Tools installer
and install the needed components. The download is signature-checked before
execution. This step needs internet access, several GB of disk space and
Windows administrator approval. The build itself runs without elevation.
If installation requires a restart, restart Windows and run `BuildGame.cmd`
again. A complete Visual Studio IDE is not required.

Direct `tools/build_game.py` invocations only check prerequisites by default.
Add `--install-prerequisites` to enable the same interactive installation
offer as `BuildGame.cmd`. `--generate-only` does not need the compiler or SDK.

If the extractor reports a missing `VCRUNTIME140.dll`, install the included
`Prerequisites/vc_redist.x86.exe` and retry. The extractor remains 32-bit;
the game runner is x64.

To use the Windows wrapper, drop one supported ISO on `BuildGame.cmd` or run:

```powershell
.\BuildGame.cmd "D:\Games\game.iso"
```

This single command extracts the ISO, generates the program, and compiles the
runner. Output appears live in the setup window and is also saved in each
stage's log under `private/setup-*`. After success, double-click `Launcher.cmd`
(resolution, MSAA and SMAA, saved in `private/launcher.json`) or `RunGame.cmd`
in the same folder. Keep that folder in place after setup: build receipts
identify the exact local paths. Setup does not launch the game automatically.

The direct PowerShell route accepts an explicit extractor path:

```powershell
python tools/build_game.py `
  --iso "D:/Games/game.iso" `
  --extractor "C:/Tools/extract-xiso.exe"
```

To reuse a completed extraction, run:

```powershell
python tools/build_game.py --imported private/imported-disc
```

### macOS build

On macOS (Apple Silicon or Intel), install CMake, Ninja, SDL3 and Capstone:

```bash
brew install cmake ninja sdl3
python3 -m pip install capstone==5.0.9
```

Homebrew has no `extract-xiso`; build it from
[XboxDev/extract-xiso](https://github.com/XboxDev/extract-xiso) and put it on
`PATH`, or pass its location with `--extractor`:

```bash
git clone https://github.com/XboxDev/extract-xiso.git
cmake -S extract-xiso -B extract-xiso/build && cmake --build extract-xiso/build
```

Then build the runner:

```bash
./build_game.sh /path/to/game.iso
```

`./launcher.sh` opens the launcher to pick resolution, anti-aliasing, volume
and soundtrack options, then starts the game. `./run_game.sh` starts the game
with the saved choices.

### Linux and SteamOS build

Linux builds use Vulkan through SDL3 and compile shaders at runtime with
shaderc, so the host needs `libshaderc_shared.so.1` (SteamOS includes it).
Build in an x86-64 distribution with CMake, Ninja, a C++17 compiler,
Python 3 with `capstone==5.0.9`, `extract-xiso` built as above, and SDL3's
build dependencies; `build_game.sh` builds SDL3 when the distribution has none.
Ubuntu's system Python refuses `pip install`, so put Capstone in the virtual
environment the scripts look for (Ubuntu needs the `python3-venv` package):

```bash
python3 -m venv private/venv
private/venv/bin/pip install capstone==5.0.9
```

SteamOS has a read-only system and no compiler, so build inside a container
such as an Ubuntu 24.04 `distrobox` that shares your home folder; the
finished checkout then runs on SteamOS itself. Run `./build_game.sh` as on
macOS. `./launcher.sh` opens a gamepad-friendly settings screen, then starts
the game; `./run_game.sh` starts the game with the saved choices.

For Steam Game Mode, add `launcher.sh` (or `run_game.sh`) with **Add a
Non-Steam Game** in Desktop Mode. Under Steam the game and launcher open full
screen. If one controller drives two player ports, turn Steam Input off for
that shortcut, as for any game launched through Steam.

Use `--generate-only` to stop after generation. A completed build writes
`private/setup-*/build-receipt.json` with status `built-unverified`. This status
records a build. A supervised native run provides the behavior evidence.

The recipe retains the tested local program and adds the bounded activity
callback and Rest predicate recoveries. Its 19-file manifest is
`program_manifest_sha256` in the recipe.
All 20 generated files, including unresolved stubs, are checked individually.
The receipt records the recipe, lifter revision, generated manifest, XBE, and runner
identities. A matching generated program does not imply an identical executable:
hand-written runtime changes and compiler inputs also affect the build.

An earlier experimental recipe used the same upstream pin with
`runtime-bootstrap.patch` and fresh function discovery. Its two successful
Exhibition runs did not establish parity with the original local build. The
current builder preserves the proven boundaries and recoveries and includes
the x87 camera and result-sign corrections alongside the current activity
recoveries. Local testing of the release runtime reports Take a Rest and Hopping
Game working alongside four volleyball stages. A passing Rest variant does not
establish coverage of other predicates.
Natural local water, shading and activity checks apply to matching
inputs; complete offline gameplay and rendering acceptance remain open in #16.

## Public test route

Install Git, CMake 3.20 or newer, and Visual Studio 2022 or Build Tools with
Desktop development with C++ and a Windows SDK. In PowerShell, run:

```powershell
git clone --recurse-submodules https://github.com/NoRain211/doaxbv-re.git
cd doaxbv-re
cmake -S recomp-runtime -B build/recomp-runtime -G "Visual Studio 17 2022"
cmake --build build/recomp-runtime --config Release --parallel 2
ctest --test-dir build/recomp-runtime -C Release --output-on-failure
```

On macOS, install CMake, Ninja, and SDL3 (`brew install cmake ninja sdl3`), then run:

```bash
git clone --recurse-submodules https://github.com/NoRain211/doaxvb-re.git
cd doaxvb-re
cmake -S recomp-runtime -B build/recomp-runtime -G Ninja
cmake --build build/recomp-runtime
ctest --test-dir build/recomp-runtime --output-on-failure
```

These commands build public tests in `build/recomp-runtime/Release`. They do
not build a playable game runner and do not require game files. The fixture
tests guest memory, registers, and dispatch at the generated-function seam. It
does not contain generated game code or prove game parity.

CMake configuration downloads the official SDL3 3.4.16 Visual C++ package from
GitHub and checks its SHA-256. The build copies `SDL3.dll` beside the runner
and the input test; keep it next to `recomp_program_runner.exe` if you move it.

The named 0.5.3 Beta ZIP includes the ISO setup workflow and bundled
Python, Capstone, MinGit, CMake and extractor. GitHub's automatic source archives contain
the same project source and recipe but require separately installed tools.
Neither download includes generated game code or a prebuilt game runner.

## Extract a user-owned ISO

`ExtractIso.cmd` extracts one ISO to `private/imported-disc/disc`. Drop one ISO
on the command file or run:

```powershell
.\ExtractIso.cmd "D:\Games\game.iso"
```

For an explicit output directory and extractor, run:

```powershell
python tools/extract_iso.py "D:/Games/game.iso" `
  --extractor "C:/Tools/extract-xiso.exe" `
  --output private/imported-disc
```

The output directory must be new and beneath this checkout's `private/`
directory. The script lists the image, checks its paths, extracts its files,
and verifies names and sizes against the listing. It does not rewrite the image
or patch executables. The source image SHA-256 stays unchanged.

The script rejects an existing output directory and symlink or junction
destinations. An interrupted run leaves partial output without a completion
receipt. Use a new directory for another attempt. Do not use partial output.

This route accepts Xbox images supported by `extract-xiso`. It does not read a
physical DVD drive. Extraction alone does not authenticate the supported game
revision or build a runner.

## Run a generated runner

After `BuildGame.cmd` or `tools/build_game.py` completes, drop its
`build-receipt.json` on `RunGame.cmd` or run:

```powershell
.\RunGame.cmd "private\setup-<id>\build-receipt.json"
```

With no receipt argument, `RunGame.cmd` uses the last completed build recorded
in `private/active-build.json`. A failed build does not replace that selection.
For older builds without a selection, it searches `private/setup-*` for one
successful receipt and refuses to choose when multiple receipts match.
It checks the receipt status, runner file and runner SHA-256,
exactly one XBE, and the recorded XBE SHA-256 before launch. It does not
revalidate every extracted file. It writes the run log under `private/`, uses
VSync, and uses full audio gain unless `RECOMP_AUDIO_GAIN` is already set; the
launcher's **Volume** choice sets it. FPS and frame time appear in the
window title; one-second performance samples are included in the log, with the
worst frame gap, late frames (over 1.5x that second's average) and, on
`recomp pacing:` lines, the worst game frame and time spent waiting on the
render thread. Set
`RECOMP_PERF_COUNTER=0` to disable this counter. No continuous capture is enabled. Recipe, lifter revision, generated
manifest, and generation-parity identities are included in each run log when
the build receipt provides them.

`RunGame.cmd` keeps the legacy runner locations at the repository root and
`build/recomp-program/Release`. That route uses the completed
`private/imported-disc` extraction and does not generate a program.

## Legacy: use an existing generated program

Use this route only when you already have a complete generated program and its
receipt. The current ISO workflow performs these steps for you.

```powershell
cmake -S recomp-runtime -B build/recomp-program -G "Visual Studio 17 2022" -A x64 `
  -DRECOMP_PROGRAM_DIR="<generated-program-directory>" `
  -DRECOMP_PROGRAM_MANIFEST_SHA256="<generated-manifest-sha256>" `
  -DRECOMP_PROGRAM_EBP_EXPECTED="<receipt-ebp-count>"
cmake --build build/recomp-program --config Release --parallel 2 --target recomp_program_runner
.\RunGame.cmd
```

The manifest and EBP values come from the matching build receipt. A mismatched
manifest, incomplete program, or wrong EBP count fails configuration. Keep the
generated program and user-owned XBE under ignored local directories.

## Runtime storage

`recomp_program_runner.exe` loads the generated program and the user-owned XBE.
It creates storage beside the XBE under `.recomp-storage`. See
[`docs/recomp-save-contract.md`](recomp-save-contract.md) for save behavior.
The runner also creates the folder named by `RECOMP_USER_MUSIC`, or
`UserMusic` beside the XBE when it is unset; `RunGame.cmd` sets it to
`private/UserMusic`. Drop MP3, WAV, and FLAC files there and restart to list
them in the game's custom soundtrack menu.
See [`docs/native-audio.md`](native-audio.md#custom-soundtracks) for supported
formats, limits, and the remaining gameplay acceptance checks.
Pass `--vsync` for paced presentation. Set `RECOMP_AUDIO_GAIN` from 0 to 1 for
sound. Audio is muted by default when you start the runner directly.
Set `RECOMP_MUSIC_SHUFFLE=1` to shuffle Radio Station playlists; see
[`docs/native-audio.md`](native-audio.md#radio-shuffle).
The game renders 16:9 in an 854x480 window. Set `RECOMP_D3D_WIDESCREEN=0` for
its 4:3 view in a 640x480 window.
Set `RECOMP_D3D_SCALE` from 1 to 8 to multiply the render height, for example
3 for 1440p; above 1 the window is borderless and capped at the screen height.
Set `RECOMP_D3D_MSAA` to a sample count such as 4 or 8; it uses the highest
supported power of two up to that count. Set `RECOMP_D3D_SMAA=1` for SMAA when the
`third_party/smaa` submodule was checked out at build time.
Set `RECOMP_SPLIT_RATE=auto`, or to your display's refresh rate such as 120 or
144, to present smoother motion than the game's 60 Hz while gameplay keeps its
original timing. Use it with `--vsync`. The picture runs one 60 Hz tick
behind, and some objects such as water still move at 60 Hz. See
[the split-rate notes](wayfinder/research/high-frame-rate-split.md#current-state).
