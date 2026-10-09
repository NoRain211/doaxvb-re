# Changelog

## 0.6.0 Beta RC1 — 2026-10-09

This release candidate improves rendering and adds opt-in high refresh
presentation and texture replacement. It updates the generation recipe, so
download the new ZIP and run `BuildGame.cmd` again; saves are unchanged.

### Added

- **High refresh presentation** (#98). `RECOMP_SPLIT_RATE=auto`, or your
  display's exact refresh rate, shows extra frames between the game's 60 Hz
  ticks by interpolating the camera, characters and props. A rate below the
  display's paces unevenly, so prefer `auto`. Gameplay keeps its original
  timing. Use it with `--vsync`. The picture runs one tick behind, and some
  objects such as water still move at 60 Hz. Unset, nothing changes.
- **Texture replacement** (#95). With `RECOMP_TEXTURES` set, F12 dumps the
  current frame's textures as PNG and Ctrl+F12 reloads edited PNGs while the
  game runs. See `docs/texture-replacement.md`.
- **Experimental VRR** (#82). `RECOMP_D3D_VRR=1` uses a tearing-capable
  presentation path for G-Sync and FreeSync. VRR must already be active for
  the game window, or the picture tears. Its effect on displayed pacing is
  not yet verified.

### Fixed

- Scene transitions, such as between match rounds and entering the Hopping
  Game, showed a black screen for about two seconds instead of the held frame
  (#97).
- Even 60 Hz pacing with V-Sync on 120 and 240 Hz displays (#82).
- Depth-of-field backgrounds shimmered and characters showed a stair-stepped
  halo at upscaled resolutions (#79).
- Distant geometry had no fog (#94).
- Fixed-function point lights were dropped or lit as directional (#92).
- Textures at steep angles blurred and shimmered, and foliage and hair edges
  stair-stepped under MSAA (#93).
- The title screen sea had no shimmer (#96).
- Closing the window mid-frame could exit with an error code (#80).

### Changed

- The recipe routes selected pose calls through hand-written adapters for
  high refresh presentation (#98). The lifter is unchanged.

## 0.5.3 Beta "Mixtape" — 2026-09-30

This release adds Radio Station shuffle. It updates the generation recipe, so
download the new ZIP and run `BuildGame.cmd` again; saves are unchanged.

### Added

- **Shuffle** (#78, #77). The game has no random play: its music player steps
  to the next playlist song. Ticking **Shuffle music** in the launcher, or
  setting `RECOMP_MUSIC_SHUFFLE=1`, plays each playlist in random order,
  custom soundtrack songs included. Songs still follow the game's location
  rules, the playing song is never picked again next, and saved playlists keep
  their order. "Previous" steps back one entry from the shuffled next song.
  An option in the game's own settings screen is planned (#77, #22).

### Changed

- The recipe routes the direct calls to the music player's two song-start
  functions through manual dispatch. The lifter is unchanged.

## 0.5.2 Beta "Mixtape" — 2026-09-30

This hotfix adds volume control. It changes only the launcher and run script;
the recipe, runtime and saves are unchanged. Extract the new ZIP into a new
folder and run `BuildGame.cmd` again.

### Changed

- **Volume** (#76). The game always started at 20% volume. The launcher now
  has a **Volume** choice (100%, 75%, 50%, 25%, Mute), saved with its other
  settings, and the game starts at 100% by default. `RunGame.cmd` also uses
  100% unless `RECOMP_AUDIO_GAIN` (0 to 1) is already set.

## 0.5.1 Beta "Mixtape" — 2026-09-29

This hotfix moves the custom music folder. It changes only the runtime and
launcher; the recipe and saves are unchanged. Extract the new ZIP into a new
folder and run `BuildGame.cmd` again.

### Changed

- **Music folder** (#75). Custom songs now go in `private\UserMusic` in the
  release folder instead of `private\imported-disc\disc\UserMusic`, and the
  launcher has a **Music folder** button that opens it. Move any songs from
  the 0.5.0 location.

## 0.5.0 Beta "Mixtape" — 2026-09-29

This release adds custom soundtracks. It updates the generation recipe, so
download the new ZIP and run `BuildGame.cmd` again; keep your existing saves.
Releases now carry a name; 0.5.0 is "Mixtape".

### Added

- **Custom soundtracks** (#73). MP3, WAV, and FLAC files in the `UserMusic`
  folder beside the game executable (`private\imported-disc\disc\UserMusic`
  in the release folder) appear as one soundtrack in the game's own custom
  soundtrack menu, such as the Radio Station playlist. The game still picks,
  starts, stops, and mixes the songs; Windows Media Foundation decodes them.
  Files are read, never changed, and the folder is scanned at startup.

### Fixed

- **Formatting crash** (#73). Selecting a custom song stopped the runtime
  because its text formatter handled only a few fixed patterns. It now
  handles every `%s %c %d %i %u %x %X` field with width and flags.

### Changed

- The recipe routes eight direct soundtrack calls through manual dispatch.
  The lifter is unchanged.
- On Windows N editions without the Media Feature Pack, the game still starts
  and custom soundtracks stay off.

### Unchanged

- The save journal is unchanged from 0.4.9.
- The known Hopping Game loading issue (#54), pool transition flash (#55)
  and possible underwater texture issue (#56) remain.

## 0.4.9 Beta — 2026-09-29

This release fixes deleting a save. It updates the generation recipe, so
download the new ZIP and run `BuildGame.cmd` again; keep your existing saves.

### Fixed

- **Deleting a save** (#71). Confirming deletion left the Vacation slot
  occupied. The slot now shows as New and its files are removed; other slots
  are unchanged. Save files also follow Windows rules for sharing, creation,
  attributes and timestamps, and a save whose file operation fails is rolled
  back instead of committed.

### Changed

- The save journal is now version 2. The first start of 0.4.9 upgrades the
  journal in its save folder, and earlier builds then refuse to start with
  that folder. Copy saves into each new installation instead of sharing one
  folder between versions.
- The recipe sends two direct string comparisons to the runtime's native
  comparator. The lifter is unchanged.

### Unchanged

- The known Hopping Game loading issue (#54), pool transition flash (#55)
  and possible underwater texture issue (#56) remain.

## 0.4.8 Beta — 2026-09-28

This release adds native PlayStation 4 and 5 controller support, up to four
controllers and controller vibration. The generation recipe is unchanged
from 0.4.7, but the runner changed: download the new ZIP and run
`BuildGame.cmd` again; keep your existing saves.

**Do not run DS4Windows with this release.** Connect the PlayStation
controller directly. DS4Windows adds a second, emulated Xbox controller, so
one pad would appear twice and take two player ports. Close it, or disable
its controller output, before starting the game. If you launch the game
through Steam, turn Steam Input off for it for the same reason.

### Added

- **PlayStation 4 and 5 controllers** (#65, #70). Controllers are now read
  through SDL3, so DualShock 4 and DualSense pads work without DS4Windows.
  Cross/Circle/Square/Triangle act as A/B/X/Y, L1/R1 as Black/White, Options
  as Start and Share/Create as Back. The mapping passes automated checks;
  it has not yet been confirmed on a physical PlayStation controller.
- **Up to four controllers** (#68). Each connected controller takes the
  next free player port and keeps it until disconnected. Two physical
  controllers in one match have not been play-tested.
- **Vibration** (#67). The game's rumble now reaches the controller.
  **Saves from earlier builds load with Vibration off**; turn it on once in
  Controller Settings. Confirmed in play: rumble works after switching it
  on. The runner defaults Vibration to on for new options.

### Changed

- Setup downloads SDL 3.4.16 from its official GitHub release while
  building, and places `SDL3.dll` beside the game runner.

### Unchanged

- The known Hopping Game loading issue (#54), pool transition flash (#55)
  and possible underwater texture issue (#56) remain.

## 0.4.7 Beta — 2026-09-26

This release fixes the crash when viewing your collection. It updates the
generation recipe, so download the new ZIP and run `BuildGame.cmd` again to
get the fix; keep your existing saves.

### Fixed

- **View Collection crash** (#64, #66). Choosing View Collection in the hotel
  room menu stopped the game, and so did leaving the collection screen. Both
  now work. The lifter now gives switch cases outside their function their
  own entry, which also covers 14 similar cases elsewhere in the game.

### Unchanged

- The known Hopping Game loading issue (#54), pool transition flash (#55)
  and possible underwater texture issue (#56) remain.

## 0.4.6 Beta — 2026-09-26

This release fixes swimsuits whose fabric did not render. The generation
recipe, launcher and bundled setup tools are unchanged from 0.4.5. Download
the new ZIP and run `BuildGame.cmd` to get the fix; keep your existing saves.

### Fixed

- **Missing swimsuit fabric** (#62). Some swimsuits, such as Diamond, showed
  only their strings and ties, both on the character and in swimsuit
  previews. Their fabric meshes now render.

### Unchanged

- The known Hopping Game loading issue (#54), pool transition flash (#55)
  and possible underwater texture issue (#56) remain.

## 0.4.5 Beta — 2026-09-26

This release simplifies setup. Its game runtime, generation recipe and launcher
are unchanged from 0.4; existing 0.4 players do not need to rebuild.

### Added

- **Bundled Git and CMake.** The release ZIP now includes portable MinGit and
  CMake alongside Python, Capstone and the ISO extractor. Setup uses these
  copies without changing the system PATH or requiring separate installations.
- **Microsoft prerequisite setup.** `BuildGame.cmd` checks for the Visual
  Studio 2022 C++ tools and a Windows SDK before extracting the ISO. If needed,
  it offers to download Microsoft's installer, verifies its Microsoft
  signature and waits for installation. Only the installer needs administrator
  approval. Internet access and several GB of disk space are still required.
- **Setup documentation and tool licenses.** The packaged quick-start guide
  and build instructions explain what is bundled, the remaining Microsoft
  installation step, and how to build from a source-only archive.

### Unchanged

- No game files, generated game code or prebuilt game runner are included.
  A supported USA ISO is still required, and setup builds the game locally.
- Resolution, MSAA and SMAA remain available in `Launcher.cmd`.
- The known Hopping Game loading issue (#54), pool transition flash (#55)
  and possible underwater texture issue (#56) remain.

## 0.4 Beta — 2026-09-25

0.4 is the first Beta. Long player sessions report the game playable from
start to finish: every stage, the Casino, Hotel and Shops, saving and
reloading, and a vacation's last day carrying into a new vacation. Formal
offline acceptance continues in #16.

This entry lists every change since 0.3.51. Together with the 0.3.51 and 0.35
entries below, it covers everything since 0.3. 0.4 was first published as
"0.4 Alpha (experimental)"; the Beta label changes no files, and the download
keeps its original name, `DOAXBV-0.4-Alpha.zip`.

### Added

- **Launcher** (#57). `Launcher.cmd` opens a small window with a resolution
  list (480p windowed, 720p, 1080p, 1440p, 2160p), MSAA (Off, 2x, 4x, 8x) and
  an SMAA checkbox. Choices are saved in `private/launcher.json`, and **Play**
  starts `RunGame.cmd`, which still runs with the defaults on its own. On first
  run the launcher picks the largest preset that fits your display.
- **Render scale, MSAA and SMAA** (#44). `RECOMP_D3D_SCALE` (1–8) multiplies
  the render height; `3` renders 2560x1440. Above 1, the window is borderless
  and fills the screen height. `RECOMP_D3D_MSAA` sets the sample count and
  falls back to the highest count your GPU supports. `RECOMP_D3D_SMAA=1` runs
  SMAA on the finished frame. The package bundles the SMAA shader and lookup
  tables (MIT, `third_party/smaa`). All three are off by default.
- **16:9 presentation** (#38). The game renders true 16:9 into an 854x480
  window by default. `RECOMP_D3D_WIDESCREEN=0` reports the dashboard's
  Normal video setting, so the game renders 4:3 into a 640x480 window.
- **Casino and hotel gift tapes** (#40). Blackjack, Poker and Slots no longer
  stop at missing or split functions, and playing a gift tape in the hotel no
  longer stops the game. The recipe grows from 45 to 61 recovery inputs.
- **App icon** (#48). When `private/doaxbv.ico` is present, setup embeds it in
  the runner for the title bar and taskbar. The package ships the icon.
- **Frame pacing report** (#53). With `RECOMP_PERF_COUNTER=1` (the launcher
  default), the once-a-second performance line adds the worst frame gap, late
  frames and the slowest present. A `recomp pacing:` line shows whether the
  game thread or the render thread was slow.

### Fixed: gameplay and generated code

- **Lifter carry and loop fixes** (#40, #50). Compare and test instructions now
  set the carry flag when the next flag reader is ADC or SBB, including a
  reader in another block; before, every such pair read a stale carry. This
  affected 40 generated functions across game, D3D, DirectSound, WMA decoder
  and controller code, and two sort comparators that sorted wrongly. LOOP,
  LOOPE and LOOPNE now decrement ECX and branch on it; their back edge used to
  read a flag value that was never set. A jump table whose first slot is
  unusable is now read from its second slot.
- **Shop and menu sound effects** (#51). Buying items quickly in a shop no
  longer silences shop and menu sound effects until you leave the scene. The
  game stops looping sounds with DirectSound `StopEx`, which the audio adapter
  did not handle, so short shop loops kept "playing" and filled the voice pool.

### Fixed: rendering

- **Casino rendering** (#39). The Poker and Blackjack double-up prompt no
  longer flashes solid white, Zack's icon draws once instead of twice, and
  Poker cards show their faces. The fixes cover supersampled back buffers,
  texture clamp and wrap modes, and back-face culling.
- **Retail shop meshes** (#41). Some shop models use vertex streams shorter than
  their declared layout. They were skipped; they now draw, including their
  reflection material.
- **Item previews** (#49). Each ball in View items now shows its own colors.
  The texture cache served the first ball's texture because the viewer refills
  one buffer without freeing it. Cached textures now rebuild when their
  contents change.
- **Two vertex-program defects** (#37). A texture could be freed while still
  bound during alpha-mask draws, which could crash with heap corruption. A
  paired instruction could also corrupt a shader register and shade with the
  wrong color.
- Depth buffers at the main render size no longer count against the render
  target budget (#44).

### Fixed: audio

- **Crackle and dropouts** (#42). A dedicated high-priority thread now feeds
  audio output instead of the game thread, so game stalls, loading and window
  dragging no longer starve it. Each voice starts behind 50 ms of silence, and
  a small pitch trim (at most 1%) holds that cushion against XAudio2's rate
  drift. On the same route, dropped buffers fell from 50 to 0 and underruns
  from 352 to 0.

### Fixed: saves

- **Faster, safer save journal** (#52). A save now writes one undo file of
  the pre-save data; deleting it commits the save. This cuts a save's file
  work on the game thread from about 48 ms to about 2.5 ms. An undo file cut
  short by an interruption is discarded, since it never allowed the game to
  write. Recovery validates the undo image and the live save data before it
  restores anything, keeps a malformed undo image for inspection instead of
  deleting it, rejects absolute paths, and compares file names the way
  Windows does.

### Performance

- **Menus and maps at 60 FPS** (#43). The texture cache grew from 256 to 4096
  entries. Menu and map screens sample about 300 textures a frame, so the old
  cache rebuilt every texture every frame. Two per-draw system calls were also
  removed. Menu and map screens went from a median of 54 FPS to 60, and a Niki
  Beach match from 24 seconds below 55 FPS to 1.
- **Render thread** (#46). D3D11 rendering runs on its own thread. Vertex and
  index uploads use a ring buffer, and common shaders compile at boot. Loading
  the island map no longer drops to 4–5 FPS, and first use of a scene no
  longer hitches while its shaders compile.
- **Game-thread stalls** (#52). The game and render threads run at high
  priority; a busy machine had dropped matches to 12–18 FPS. Vblank waits use a
  high-resolution timer, so 99% of waits overshoot by at most 0.1 ms instead of
  1.9 ms. The renderer no longer zero-fills every capture packet, and error
  output is buffered and flushed every 250 ms instead of written one character
  at a time.
- **Guest memory** (#45). Plain RAM reads and writes, the hottest calls in the
  generated code, take an inline fast path. Process CPU during a match fell
  from about 750 to 620–710 ms per second.

### Known issues

- The second Hopping Game in a session can be played before it finishes
  loading (#54).
- An upside-down map briefly flashes when leaving the pool area (#55).
- Some underwater beach textures may be missing (#56).
- Boot movies still run near their 30 FPS source rate, and loading can still
  pause for up to about a second.

### Release and setup

- Setup builds the lifter from the `codex/doaxbv-recipe` branch of
  `NoRain211/xboxrecomp` (#50) and fails unless the submodule is a clean
  checkout of `LIFTER_REVISION`. The earlier build-time patch against upstream
  is gone. `tools/update_lifter_pin.py` moves the pin and updates the recipe.
- Setup regenerates the game program byte for byte to match the play-tested
  local program, and builds an x64 Release runner with Visual Studio 2022
  Build Tools.
- A save journal left by an interrupted save in 0.35 or earlier stops startup.
  Run that older build once to finish its recovery, then start 0.4.
- Download and build in a new folder. Keep your previous install and saves;
  saves are not imported automatically.
- The README and status pages were rewritten, and the status now labels the
  game fully playable (#47, #59).

## 0.3.51 Alpha — 2026-09-17

### Fixed

- Restore a guaranteed external MSVC definition of `recomp_memory` by removing
  `__forceinline` from the public runtime entry point. This fixes the final-link
  `LNK2001` failure reported as `recomp_memory` on VS2022/x64 and as
  `_recomp_memory` on the earlier VS2019/Win32 build.

### Release and setup

- This is a narrowly scoped hotfix based directly on the published R6 / 0.35
  release. Runtime and generated-game behavior are otherwise unchanged.
- Use Visual Studio 2022 Build Tools, x64 Release, as documented for 0.35.

## 0.35 Alpha — 2026-09-17

### Fixed

- Correct constant-color blend factors that caused the huge white glare at the
  dusk pool. The corrected local view ran at approximately 60 FPS.
- Keep input-history expiry comparisons and branches in one recovered body.
  Two corrected local volleyball matches completed fifteen rallies without the
  delayed crouch seen in the comparison runs, while retaining fresh serving.
- Preserve the recovered Radio Station callback; a local manual run opened it.
- Recover a missing activity callback and its two dependencies associated with
  a reported 0.3 Take a Rest stop. The original variant is uncertain;
  subsequent local testing reports Take a Rest working.
- Restore both returns of two Rest predicates reached by local package tests,
  including the reported Tina midday stop. The latter also fails in the preserved
  local runner; earlier Rest acceptance covered a different variant. A synthetic
  generation regression checks that each predicate retains both returns.
- Reuse freed guest allocations instead of exhausting the arena during extended
  play. The reproduced dusk Jungle crash followed an out-of-memory sound-buffer
  failure. Allocation tests cover reuse, split/coalesce, bounds and live data.

### Release and setup

- Include the tested local runtime fixes alongside the current recovery recipe.
- Build with Visual Studio 2022 in x64 Release, matching the working local
  runner. Retain the per-file optimizer workaround without disabling Release
  optimization globally.
- Show FPS and frame time in the window title and log performance once per
  second. Continuous image capture is disabled by default.
- Install into a new folder and rebuild. Preserve the previous saves; setup
  does not migrate them automatically.

### Validation and remaining limits

- Fresh generation through the packaged builder matches all 20 reviewed files.
  The x64 Release build passes ten native tests and eight setup, authentication,
  extraction and launcher tests. Generation reused a verified disc extraction;
  extraction was separately tested with a synthetic ISO.
- The tested R6 runtime and generation are retained unchanged for this release.
  The player reports Jungle, Beach, Niki Beach, Private Beach, Take a Rest and
  Hopping Game working. That session ran for approximately 27 minutes and closed
  normally, with final performance samples at 60 FPS. This is one session,
  not full acceptance of every activity, court or time variant.
- A constrained-memory sound-buffer test completes 3,072 create/free cycles
  after repair; the prior build failed after 161. This is isolated function
  validation, not natural gameplay acceptance.
- Occasional 1–4 second volleyball slowdowns were reported in the same build;
  their cause and any connection to allocation remain unverified.
- Broader Take a Rest variants, complete offline flows, hair rendering, movie
  performance, audio crackle, the previously reported shop-exit crash and
  area-transition slowdown remain open. The recipe still has
  unsupported paths; this is not complete gameplay acceptance.

## 0.3 Alpha — 2026-09-13

### Fixed

- Restore pool-water refraction and reflections by initializing framebuffer
  viewport scales and preserving the guest sample grid. Daytime and dusk
  captures show the floor through the water.
- Add directional character lighting and shaded platform materials. Preserve
  the reached shader working set to avoid Hopping Game cache thrashing.
- Recover reached Hopping Game, Take a rest, Jungle-stage and Options/hotel
  transition paths, including the incomplete dusk pool function.
- Apply Gamma, Brightness and Contrast to the displayed picture.
- Default to Digital Control with new and older saves, while preserving an
  explicit subsequent Analog or Digital choice as a PC preference.
- Correct the inventory action text used for accessory equip and lotion use.
- Ignore keyboard input when the game window is not in the foreground.

### Release and setup

- Update the source and authenticated generation recipe to the tested local
  runtime, retaining the earlier camera, save, audio and movie corrections.
- Keep the existing ISO setup workflow, bundled prerequisite tools and licenses.
- Install into a new folder and rebuild; saves are not migrated automatically.
- Keep checksums internal, with no checksum text attachment or bundled game data.

### Validation

- The local candidate passes all ten native tests.
- Daytime pool-water rendering was observed in two independent natural runs;
  a later manual dusk run reached gameplay and the failure/retry screen, with
  positive player feedback. Its planned time limit ended the run normally.
- Fresh generation through the public builder matches all 20 tested local files
  byte-for-byte. The public Win32 Release rebuild passes ten native tests; seven
  setup/authentication/extraction/launcher checks and five focused lifter tests
  pass. Generation reuses a verified game extraction; the extractor is separately
  checked with a synthetic ISO. No fresh package gameplay run was performed.
- Reused natural-run evidence applies to matching runtime and generated inputs;
  it does not establish complete gameplay acceptance for the package.

### Known limitations

- Complete Hopping Game outcomes, dusk return/repeat coverage, other time and
  activity variants, sustained performance and original rendering parity remain
  unproven. Lighting, camera and image coverage are still incomplete.
- Casino and collection routes still have known unsupported paths. Inventory
  persistence and all item/action variants are not fully accepted.
- Physical two-player play and comprehensive pressure/action outcomes remain
  unproven. Rollback multiplayer is not integrated into gameplay.

## 0.0.2 Alpha — 2026-09-12

### Fixed

- Include the current local x87 arithmetic correction, which fixes explicit
  floating-point destination registers used by camera calculations. Local
  match and island-return evidence shows reduced abrupt camera swings; full
  original-camera and physical two-player parity remain unproven.

- Restore surfaces in reflective item previews, including visor and lotion
  models, through corrected texture coordinates and texture blending.
- Initialize the graphics transforms to the original identity defaults.
- Handle the observed padded item vertex layout without reading a missing
  texture-coordinate field. Unsupported resource layouts remain rejected.

### Release and setup

- Rebuild with the current local runtime and generation recipe, including the
  camera arithmetic correction. Preserve the proven function boundaries,
  ordered recovery inputs and generated-file checks.
- Include the accepted ISO setup workflow in the tagged source, alongside
  the bundled-prerequisite Alpha ZIP.
- Refresh setup and project documentation and add a Ko-fi support link.
- Omit checksum text files from the download. Setup still verifies its
  recipe and generated program internally.

### Validation

- Win32 Release rebuild passed all nine native runtime tests.
- Seven setup, generation-authentication, extraction and launcher checks passed.
- Fresh generation through the packaged recipe matches all 20 current local
  generated files byte-for-byte. This used a verified existing ISO extraction;
  no new full ISO extraction or gameplay acceptance run was performed.

### Known limitations

- This is an experimental Alpha. Lighting, camera behavior, other missing
  images and complete offline-mode coverage remain under development.
- The existing unresolved-function stop in a later island-menu flow remains.
- Selected item previews have natural-run evidence in development builds;
  that evidence does not establish complete gameplay acceptance for this ZIP.
- Rollback multiplayer is not integrated into gameplay.

### Packaging correction

The first 0.0.2 upload mistakenly retained the older 0.0.1 generated program.
The corrected package matches the current local generation, including the
camera arithmetic fix. Rebuild from the corrected package if you used the
initial upload.

## 0.0.1 Alpha

The initial Alpha introduced local ISO extraction, authenticated generation
and a Win32 Release build, with native rendering, input, audio, movies and
save support. Its named ZIP was updated to use the accepted generation recipe;
the original Git tag did not contain that complete setup workflow.

The 0.0.2 changes above are compared with the final published 0.0.1 Alpha ZIP,
not just the older source tag. Features already in that ZIP are not counted as
new fixes here.
