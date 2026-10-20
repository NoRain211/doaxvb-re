# Switch port notes

Collected 2026-10-20 to help decide whether a Nintendo Switch target is worth
building. The sections up to Presentation were written before any Switch
build; Testing and Game build record what has since run in Eden.

The source is [goon-nx](https://github.com/danfromtico/goon-nx) at commit
`49c1a00` (2026-10-02), a separate DOAXBV recompilation for the Switch. On
hardware it reaches the title screen through its Vulkan build; its audio is
noise and nothing later is tested. It shares no code with `recomp-runtime/`: it
uses a vendored, modified copy of upstream `xboxrecomp`'s runtime and emulates
the NV2A beneath the game. Only its findings about the platform and the game
apply here. The repository has no license, so reimplement rather than copy.

## Switch constraints

- **Guest memory cannot be mapped twice.** Horizon refuses a second view of
  code memory (`svcMapProcessMemory` returns `0xD401`), so goon-nx runs
  without RAM mirrors. This runtime needs none: guest RAM is one ordinary
  64 MB buffer, and `recomp_memory_u32` in `runtime.h` folds the
  `0x80000000` and `0xF0000000` windows onto it by masking the address.
- **The game needs title takeover.** It uses nearly all of its 64 MB, so
  goon-nx launches with title takeover (hold R while starting a game) to get
  the console's full memory.
- **File times cannot be set.** The Switch filesystem service does not set
  them, but the game trusts its cached copy of an archive only when the size
  matches, the copy's creation time is within 60 s of the original's, and the
  copy's creation time equals its own last-write time (check at
  `0x000B4940`–`0x000B4A7E`). goon-nx records the times the game sets in a
  file in the save folder and reports them on every query.
- **The first start is slow.** The game copies about 750 MB into its cache
  partition, which takes several minutes on an SD card.
- **Equal priorities do not time-slice.** libnx documents preemptive
  multithreading only at priority `0x3B` on cores 0–2 and `0x3F` on core 3
  ([thread.h](https://switchbrew.github.io/libnx/thread_8h.html)). A guest
  busy loop, such as the idle counter thread at `0x00189D20` that increments
  a counter until a flag is set, can hold its core from threads of the same
  priority. goon-nx hit this as starvation under its single guest lock and
  replaced the loop with one that sleeps 1 ms per turn.
- **OpenGL is risky.** Mesa's compressed-texture upload corrupted the heap
  on the title screen; goon-nx's Vulkan build avoids that path.

## What this runtime already has

- **ARM64 builds.** The macOS build supports Apple Silicon, with an ARM64
  fiber switch and FMA contraction disabled for IEEE results, so the lifted
  game code and device models need no CPU port.
- **A Vulkan path.** Linux presents through Vulkan via SDL3. Whether SDL3
  runs under libnx is open; the alternative is a libnx or deko3d backend
  beside the D3D11 fallback.
- **Native audio and input.** Our audio replaces DirectSound with owned
  PCM/ADPCM output, so goon-nx's noise problem does not transfer. A Switch
  target would need a libnx audio sink and controller input.
- **Portable code.** About 41k lines in 191 runtime files include no Windows,
  D3D11 or SDL header. The POSIX branches mostly fit libnx's newlib and
  pthreads; `fiber_adapter.c` allocates fiber stacks with `mmap`.

## Presentation

The SDL GPU API lists a Switch Vulkan driver, but SDL's Switch port is under
Nintendo's NDA. devkitPro packages SDL2 only; its `switch-sdl-3.4` branch
builds SDL3 without SDL_GPU. The options are a community SDL fork with Vulkan
through Mesa's NVK, a deko3d presenter, or OpenGL through Mesa, where goon-nx
hit the texture-upload corruption above.

## Testing

The Eden emulator runs `.nro` homebrew without keys or firmware. An Eden run
shows the build works; it does not prove real hardware, whose GPU and memory
behavior can differ.

`recomp_runtime_test` builds as an NRO with devkitPro's Docker image:

```powershell
docker run --rm -v "${PWD}:/src" -w /src devkitpro/devkita64 bash -c "cmake -S recomp-runtime -B build/switch -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake -DCMAKE_BUILD_TYPE=Release && cmake --build build/switch --target recomp_runtime_test_nro -j8"
```

Open `build/switch/recomp_runtime_test.nro` in Eden with File > Load File. It
runs the default suite and prints PASS or FAIL. The screen keeps only the last
lines, so all output also goes to `sdmc:/doaxbv-runtime-test.txt`; Eden writes
that file when the suite finishes. The ctest cases that pass a command-line
flag, such as `--invalid-access`, do not run there.

Porting the suite needed Switch guards: fiber stacks use `aligned_alloc`
instead of `mmap`, the save journal skips `flock`, and the guest clock uses
`clock_gettime`.

In Eden 0.2.1 (2026-10-20) every check passes except three `cvtps2pi`
rounding checks, which are an Eden bug: `FPCR` reads 0 (round to nearest),
yet `lrintf(1.2f)` returns 2 because Eden's `frintx` rounds up. The macOS CI
job passes the same test on Apple Silicon. The closing "save recovery failed"
line is expected: the fixture ends by starting the backend with no root.

Horizon's filesystem differs from POSIX in ways the Switch build of
`win32_compat.c` covers:

- It stores no Win32 attributes and cannot set file times. Each directory
  keeps them for its entries in a `.recomp-attrs` index, hidden from listings.
  Creating or deleting an entry clears its line, a write after setting times
  drops the stored write time, and read-only is enforced on open and delete.
  The save journal's snapshot copies the index with the files, so a rollback
  restores attributes and times.
- It cannot open a directory as a file, so a directory handle has no
  descriptor and works by path.
- It cannot delete an open file, so a delete is deferred to the last close,
  as on Windows.
- It lists no `.` or `..`, so an empty directory has no first entry.

The test entry creates `sdmc:/tmp`, the default temp directory. The save
journal's `sync_to_disk` skips directories on the Switch, where Eden crashed
on the attempt, and its rollback leaves file times to the index.

## Game build

`recomp_program_runner` also builds as an NRO from a locally generated
program, with `switch_runner.c` standing in for graphics, audio and
controllers: the presenter draws nothing and paces PRESENT to 60 Hz, audio is
silent, and no controller is connected. Configure as for the PC build, adding
the toolchain file:

```powershell
docker run --rm -v "${PWD}:/src" -w /src devkitpro/devkita64 bash -c "cmake -S recomp-runtime -B build/switch-game -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake -DCMAKE_BUILD_TYPE=Release -DRECOMP_PROGRAM_DIR=<generated> -DRECOMP_PROGRAM_MANIFEST_SHA256=<sha> -DRECOMP_PROGRAM_EBP_EXPECTED=<count> && cmake --build build/switch-game --target recomp_program_runner_nro -- -j3"
```

A full build takes about six minutes; Docker with 6 GB of memory manages
three jobs. The NRO reads the disc from `sdmc:/doaxbv/game/default.xbe`.
Each line of `sdmc:/doaxbv/args.txt` is one more runner argument, and
`KEY=VALUE` lines set environment variables. The log is shown on screen and
written to `sdmc:/doaxbv/log.txt`; when the runner stops, the screen holds
until + is pressed. An `abort()` prints a backtrace there; subtract
`main`'s run-time address minus its `.elf` address before passing the
frames to `aarch64-none-elf-addr2line`.

In Eden 0.2.1 (2026-10-20) the game boots, creates its save folders, plays
the intro movie and reaches the title screen. With
`--input-start-pulse-at 6` and `--input-start-pulse-at 8` it skips the
movie to the title (scene 4 at swap 43), idles there and returns to the
attract movie at swap 1848, as it does on PC. No cache copy happened before
the title. libnx cannot detach a thread, so `host_diagnostics.cpp` skips
its stderr flusher on the Switch.

## Open work for a Switch target

1. A libnx platform layer for files, threads, time, audio output and
   controllers, including the file-time record above.
2. A presentation backend for the D3D8 replacement on the Switch.
3. Checking our thread scheduling against Horizon's: which guest threads
   spin, and which host priority and core each one runs on.
4. Getting past the title screen headless, then real hardware with title
   takeover.
