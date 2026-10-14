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
- On Windows, nothing else: setup downloads the llvm-mingw compiler (see below)
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

Windows builds compile with [llvm-mingw](https://github.com/mstorsjo/llvm-mingw)
clang; Visual Studio and the Windows SDK are not used. When
`tools/llvm-mingw/bin/clang.exe` is missing, `BuildGame.cmd` downloads the
pinned 20261006 `ucrt-x86_64` release (190 MB, about 715 MB extracted), checks
its SHA-256 and extracts it to `tools/llvm-mingw`. This needs no administrator
approval and happens once per folder. A release ZIP may already include it. The
build receipt records the clang version. The runner links the C++ runtime
statically and needs only Windows system DLLs and `SDL3.dll`.

Direct `tools/build_game.py` invocations only check for the compiler by
default. Add `--install-prerequisites` to download it as `BuildGame.cmd` does.
`--generate-only` does not need the compiler.

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
(resolution, MSAA, SMAA and sun shadows, saved in `private/launcher.json`) or `RunGame.cmd`
in the same folder. Keep that folder in place after setup: build receipts
identify the exact local paths. Setup does not launch the game automatically.

On D3D11, **Sun shadows** (`RECOMP_D3D_SUN_SHADOW=1`, off by default) include
court posts, palm trunks and leaves, and the volleyball net. Leaves and net
mesh cast through texture alpha; blended foliage uses a half-alpha cutoff.
The map keeps its fixed court-area resolution. Verified original post and
palm shadow overlays retain only depth/stencil writes while a valid sun map
is available. Static overlays match by texture payload; live palm textures
must come from silhouette rendering. Unrecognized overlays keep drawing.
`RECOMP_PERF_COUNTER=1` reports hidden overlays as `prop_shadows` beside the
caster count. See [sun shadows](sun-shadow.md) for coverage and limits.

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
shaderc, so the host needs `libshaderc_shared.so.1` (SteamOS includes it) or,
on Debian and Ubuntu, `libshaderc.so.1` from the `libshaderc1` package.
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

To test with the compiler setup uses, place llvm-mingw in `tools/llvm-mingw`
(or let `BuildGame.cmd` download it) and run:

```powershell
$env:PATH = "$PWD\tools\llvm-mingw\bin;$env:PATH"
cmake -S recomp-runtime -B build/clang -G "MinGW Makefiles" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build/clang --parallel 4
ctest --test-dir build/clang --output-on-failure
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

### Optional hardware ray-tracing smoke test

The standalone DXR/DX11 test requires Windows, a recent Windows SDK, and DXC on
`PATH` (or an explicit `-DRECOMP_DXC_EXECUTABLE=...` path to `dxc.exe`). It
uses only synthetic geometry and does not change the game renderer. DX11
remains the default renderer; DXR is optional.

```powershell
cmake -S recomp-runtime -B private/dxr-smoke -G "Visual Studio 17 2022" -A x64 -DRECOMP_BUILD_DXR_SMOKE_TEST=ON
cmake --build private/dxr-smoke --config Release --target recomp_dxr_smoke_test
ctest --test-dir private/dxr-smoke -C Release -L "dxr|dx11" -V
```

The three tests cover:

- `recomp-runtime-dxr-smoke`: selects a hardware adapter with DXR 1.1 and
  Shader Model 6.5, builds one triangle with two instances, and checks two
  hits and two misses, including instance IDs and hit distances. It then
  writes a shared RGBA texture through DXR and samples it in a DX11 pixel
  shader on the same adapter. Two shared fences coordinate writes and reads.
- `recomp-runtime-dx11-fallback`: runs the DX11 drawing path with DXR disabled.
  This mode does not initialize D3D12 or create any D3D12 resources.
- `recomp-runtime-dx11-warp-fallback`: runs the same DX11-only check using WARP,
  Microsoft's software renderer.

Each image test queues eight frames at each of two sizes (8x4 and 13x5),
recreating textures between sizes. It verifies all 776 output pixels, including
a changing frame marker to detect stale data. The interop handoff stays on the
GPU; CPU readbacks only check snapshots after the frames complete. Rendering
is offscreen, so this does not test a window or swap chain.

Both debug layers are enabled when installed, and reported errors fail the
tests. Each GPU wait is limited to 10 seconds; CTest limits each test to 30
seconds. Without compatible DXR hardware, the DXR test exits with code 77,
which CTest reports as skipped. The DX11 checks run independently. Passing
proves synthetic ray queries, texture sharing, and explicit DX11-only modes;
it does not establish game RTGI or automatic recovery from DXR device failure.

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
Set `RECOMP_TITLE_IDLE` to `no-cutscene` or `cpu-exhibition` to change what
the idle title screen leads to; see [`docs/title-idle.md`](title-idle.md).
Tick **Shorter demo cuts** in the launcher, or set `RECOMP_DEMO_CUT_HOLD` from
1 to 59 to shorten the one-second freeze between the demo's scenes; see
[`docs/title-idle.md`](title-idle.md#demo-cut-hold).
Set `RECOMP_UNLOCK_SWIMSUITS=1` to unlock the swimsuits; see
[`docs/swimsuit-unlock.md`](swimsuit-unlock.md).
The game renders 16:9 in an 854x480 window. Set `RECOMP_D3D_WIDESCREEN=0` for
its 4:3 view in a 640x480 window.
Set `RECOMP_D3D_SCALE` from 1 to 8 to multiply the render height, for example
3 for 1440p; above 1 the window is borderless, centered, and shrinks to fit the
screen.
Set `RECOMP_D3D_MSAA` to a sample count such as 4 or 8; it uses the highest
supported power of two up to that count. Set `RECOMP_D3D_SMAA=1` for SMAA when the
`third_party/smaa` submodule was checked out at build time.
Set `RECOMP_D3D_PIXEL_LIGHTING=1` before launch to evaluate the admitted
fixed-function directional/point diffuse and ambient lighting per pixel.
The default remains per vertex. This smooths interpolated normals without
changing the light or material values; point attenuation and RGB clamping also
move to the pixel. Unlit, pretransformed, vertex-color and vertex-program draws
keep their existing paths. Specular and vertex-color lighting remain unsupported.

Set `RECOMP_SPLIT_RATE=auto`, or to your display's refresh rate such as 120 or
144, to present smoother motion than the game's 60 Hz while gameplay keeps its
original timing. Use it with `--vsync`. The picture runs one 60 Hz tick
behind, and some objects such as water still move at 60 Hz. See
[the split-rate notes](wayfinder/research/high-frame-rate-split.md#current-state).
Set `RECOMP_D3D_SMOOTH_NORMALS=1` to rebuild area-weighted normals on lit
indexed fixed-function triangles before upload. Positions within 0.0001 guest
units share contributions when their original normals are within 60 degrees
and their skin weights match. UV seams can smooth within a draw; separate
draws and hard edges stay separate. Incomplete mesh boundaries retain authored
normals to avoid new seams between draw calls. The option is independent of pixel
lighting and defaults off. A 32 MiB contents cache invalidates on vertex or
index changes, including CPU animation. `RECOMP_PERF_COUNTER=1` reports cache
hits, rebuilds and CPU time at shutdown. Vertex programs retain their inputs.
Set `RECOMP_D3D_RTGI=1` for experimental ray-traced global illumination on a
GPU with DXR 1.1. The build includes it when CMake finds DXC. It traces one
bounce of diffuse light off the visible scene through a D3D12 device beside the
D3D11 renderer; `RECOMP_D3D_RTGI_STRENGTH` (0 to 4, default 1) scales it.
`RECOMP_D3D_RTGI_HITCOLOR=0` disables hit-point texture color (default on),
retaining the screen-space color and contact fallback.
Set `RECOMP_D3D_RTGI_SPLIT=1` to leave the left half of the screen unlit for
comparison. Eligible static textured draws also supply off-screen bounce color;
supported alpha-tested textures leave holes in the traced geometry.
`RECOMP_D3D_RTGI_SCALE` (1 to 4, default 2) traces one ray per that many
pixels across and down; the composite upsamples by depth, so 3 or 4 trades
softer contact shading for less GPU time at 4K. `RECOMP_D3D_RTGI_HZ` (default
60) limits traces per second: a frame that starts within three quarters of a
period of the last traced frame, such as a `RECOMP_GAME_HZ=120` display pass,
reprojects that frame's light onto its own depth instead of tracing. Set it to
0 to trace every frame. `RECOMP_PERF_COUNTER=1` appends RTGI costs to the
per-second `recomp performance:` line: traced, refitted and reused frames,
triangles per trace, CPU milliseconds per frame, and GPU milliseconds per stage.
Without DXR support, or in a build without DXC, the runner logs a
`recomp rtgi:` line and renders with D3D11 alone.
