# Tools

## xboxrecomp

`xboxrecomp` is pinned as a submodule at `tools/xboxrecomp`. Clone this
repository with `--recurse-submodules`, or initialize it afterward:

```powershell
git submodule update --init --recursive
```

Use its supported XBE parsing, disassembly, function-identification, and
recompilation entry points. Keep every user-owned input and all generated
output under ignored local directories. `build_game.py` generates from this
submodule and requires it to be clean at the recipe revision. Lifter changes are
commits in NoRain211/xboxrecomp; general fixes also belong upstream.

The upstream repository also contains experimental runtime code. That code is
not part of this port's architecture: the tracked runtime remains
`recomp-runtime/`, D3D8 is replaced at the API level, and no NV2A emulator may
be placed beneath the game.

## Public export verifier

`public_export.py` enforces the tracked public allowlist, scans regular files,
and verifies each declared submodule's path, URL, and pinned commit.

## Scripted play

`drive_game.py` presses pad buttons in a running game and waits for the
runtime's `[auto:ui]` log lines to confirm each step. It has routes to the
shop, the island map, the pool and the items screen. See
[driving the game by script](../docs/automation.md). `test_drive_game.py` tests
it against a fake game.

## Deliberately absent

The private repository contains historical frontier runners, provenance locks,
candidate-specific leaf gates, and an NV097 capsule raster tool. They embed
game-specific evidence or obsolete workflow assumptions and are not public
tooling. New import or analysis tools must expose a game-independent interface
and keep all derived game data local.

## Build from an ISO

`BuildGame.cmd` runs `build_game.py` to extract a supported ISO, reproduce the
accepted generation recipe and compile a Win32 Release runner. The recipe reproduces
the current proven local generated program, including its x87 destination
correction, with the original function boundaries and recoveries. Generated files
must match their recorded hashes before compilation. See the
[build guide](../docs/building.md#build-a-local-runner-from-a-user-owned-iso).
`RunGame.cmd` uses the successful build receipt and verifies the selected
runner and game executable before launch.

## ISO extraction

Windows users can drop one ISO onto the root `ExtractIso.cmd` launcher.
Python 3.12 or newer is required. The launcher finds an extractor in
`tools/artifacts/extract-xiso.exe` or on PATH and extracts to
`private/imported-disc/disc`, refusing existing output. `RunGame.cmd` launches
a separately built runner against that completed extraction.

`extract_iso.py` wraps a separately installed XboxDev `extract-xiso` executable
in extraction mode. See [usage and boundaries](../docs/building.md#extract-a-user-owned-iso).
`test_extract_iso.py` uses synthetic listings and can additionally exercise a
real extractor when `EXTRACT_XISO_TEST_TOOL` names its executable. Neither tool
contains game data or replaces the remaining local generation pipeline.

## Local archive and texture extraction

These Python tools use only the standard library. Inputs are opened read-only.
Output must be a new directory beneath this checkout's `private/`, with no
symlinks or junctions in its path. Keep extracted files, names, inventories,
keys, and analysis notes there. The tests construct synthetic data in memory.

```powershell
python tools/afs_extract.py <archive.afs>
python tools/texture_rip.py private/rip/afs --relaxed-xpr --raw
```

`afs_extract.py` writes `private/rip/afs/<archive-stem>/<index>` by default.
`--names` appends a sanitized optional AFS name. It checks the entry ranges and
supports the optional name-table pointer after the index or just before the
first payload. It does not decompress or recursively unpack entries. Use
`--output` when archives have the same stem.

`texture_rip.py` accepts files or directories and writes
`private/rip/textures/<key>.png` plus a private `manifest.json`. It parses XPR0
resource records and skips length-delimited user records. `--relaxed-xpr`
explicitly permits modified outer magic/length fields while retaining resource
and payload bounds checks; it does not decrypt or decompress arbitrary data.
Cube and volume textures are rejected. Unsupported textures and ambiguous
palettes are recorded in the manifest instead of silently assigned a key.

The replacement key is `ff-WxH-mN-<sha256>`, where `ff` is the lowercase,
two-digit Xbox format byte, dimensions and mip count are decimal, and the hash
covers original mip storage in order, followed by 1,024 palette bytes for P8.
Mip count zero in the resource header means one. Swizzled textures are hashed
before deswizzling; DXT textures retain their compressed blocks. Linear
textures include row padding except after the final row, matching runtime
capture. Archive alignment padding is excluded. PNGs contain only the base
level. `--raw` also retains the exact hashed bytes in a `.bin` file; the private
manifest records the mip and palette boundaries and surface description.
These raw files are not DDS files.

The decoder handles DXT1/3/5, Morton-ordered A8R8G8B8, X8R8G8B8, R5G6B5,
A1R5G5B5, X1R5G5B5, A4R4G4B4, P8, A8, L8, A8L8, R8G8B8A8, and the listed
formats' supported linear equivalents. A8 previews use white RGB with the
stored alpha. P8 pairing uses an immediately following 256-entry palette;
the manifest marks that convention as unverified until its key matches a
runtime dump. Offline support for a format does not imply runtime support.
Use key matches with runtime dumps to establish identity; a successful decode
alone does not prove that a game's loader leaves every resource unchanged.

Public format references:
[AFS reader/writer](https://github.com/soyjxck/cri-afs),
[XPR header](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded/blob/master/src/common/xbe/Xbe.h),
and [Xbox format fields](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded/blob/master/src/core/hle/D3D8/XbD3D8Types.h).
