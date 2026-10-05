# Linux build

The public runtime tests build without a game image or generated game sources.
Use a separate build directory for each platform. Windows continues to use the
pinned SDL3 VC package and its existing D3D11, XAudio2, and Media Foundation paths.

## Public runtime tests

Requirements are a C11/C++17 compiler, CMake 3.20 or later, and SDL3 3.4 or later.
The tested SDL release is 3.4.16. Linux configuration downloads the pinned
[miniaudio 0.11.23 header](https://github.com/mackron/miniaudio/tree/f40cf03f80cdb7e741d43e53b7e706e8c1394bcf)
and verifies its SHA-256. It supplies MP3, WAV, and FLAC decoding; SDL3 supplies
host audio output. An offline build can set
`FETCHCONTENT_SOURCE_DIR_MINIAUDIO` to a directory containing that header.

Install SDL3 into a user-owned prefix if the distribution does not provide it:

```sh
prefix="$HOME/.local/linux-build"
mkdir -p "$prefix/src"
cd "$prefix/src"
curl --fail --location --retry 3 \
  https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz \
  --output SDL3-3.4.16.tar.gz
echo "7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68  SDL3-3.4.16.tar.gz" | sha256sum -c -
tar -xzf SDL3-3.4.16.tar.gz
cmake -S SDL3-3.4.16 -B sdl-build \
  -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_BUILD_TYPE=Release \
  -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_TEST_LIBRARY=OFF \
  -DSDL_UNIX_CONSOLE_BUILD=ON
cmake --build sdl-build --parallel 2
cmake --install sdl-build
```

SDL detects the available system headers and libraries. A build without desktop
or audio development packages can use dummy/offscreen video and dummy/disk
audio for tests. Real presentation and sound require rebuilding SDL with the
appropriate desktop and audio backends.

If CMake is missing, unpack an official
[Linux CMake binary archive](https://cmake.org/download/) into the user prefix,
keeping its `bin` and `share` directories together. Add `"$prefix/bin"` to
`PATH`. No system installation is required.

From the repository root:

```sh
cmake -S recomp-runtime -B build/linux \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH="$HOME/.local/linux-build"
cmake --build build/linux --parallel 2
ctest --test-dir build/linux --output-on-failure
```

The Linux suite includes SDL virtual-gamepad and dummy-audio tests, custom
music, guest runtime models, fibers, save transactions, capture, and controller
settings. Stop-report tests cover fatal signals on Linux and SEH on Windows.
The audio tests select SDL's dummy driver themselves; do not set
`SDL_AUDIO_DRIVER`, because the environment overrides that choice. The D3D11
presenter tests also build on Linux when DXVK Native and vkd3d-shader are found
(see below); they need SDL built with a desktop backend and a Vulkan driver.
Dummy audio tests exercise the output queue without proving audible playback.
Public tests do not establish game compatibility.

## Authenticated local program

The generated program remains private. Use the authenticated snapshot and
manifest receipt produced by the local setup route in [building.md](building.md).
Keep the program build directory under `private/`, because configuration
creates derived game source there. Never copy a snapshot into tracked sources.

The Linux presenter additionally requires [DXVK Native](https://github.com/doitsujin/dxvk)
with SDL3 WSI (tested with 3.1.1), a Vulkan driver, Wine's `libvkd3d-shader`
1.17 or later for runtime HLSL compilation, and SDL3 built with X11 or Wayland
support (the console-only SDL above is enough for tests only). Install these
into a user-owned prefix and expose their pkg-config files through
`PKG_CONFIG_PATH`. Configuring the full program runner fails without
`dxvk-d3d11`, `dxvk-dxgi`, and `libvkd3d-shader`.

With those on `PKG_CONFIG_PATH`, `python3 tools/build_game.py --iso <iso>`
runs the same authenticated setup as on Windows and builds
`private/setup-*/build/recomp_program_runner`. It needs a Linux build of
XboxDev `extract-xiso` on `PATH` or passed with `--extractor`. To configure by
hand instead:

```sh
export PKG_CONFIG_PATH="$HOME/.local/linux-build/lib/pkgconfig:$HOME/.local/linux-build/lib64/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
cmake -S recomp-runtime -B private/linux-program \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH="$HOME/.local/linux-build" \
  -DRECOMP_PROGRAM_DIR="$snapshot/generated" \
  -DRECOMP_PROGRAM_MANIFEST_SHA256="$manifest_sha256" \
  -DRECOMP_PROGRAM_EBP_EXPECTED="$ebp_overrides"
cmake --build private/linux-program --target recomp_program_runner --parallel 2
export DXVK_WSI_DRIVER=SDL3
```

Use `program_manifest_sha256` and `ebp_overrides` from the authenticated
receipt. Missing hashes, altered
sources, and incorrect EBP override counts remain fatal configure errors.
Do not force-include `runtime.h` into complete generated-program chunks.
The existing runner arguments and private game inputs still apply. A successful
configure or build does not establish playable Linux rendering, audio, or saves;
those require a separate play test.
