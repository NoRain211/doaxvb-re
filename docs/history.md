# Project history

This page records how the port got to its current state: what was built,
why each direction was chosen, and which early conclusions turned out wrong.
It draws on the git history and on the project's development chats,
including archived ones going back to May 2026.
[CHANGELOG.md](../CHANGELOG.md) lists what each release changed, and
[public-status.md](public-status.md) states what the public tree and local
builds prove. Branches marked local exist only on the maintainer's machine.

+## Before the public tree, May to August 2026

The project went through four approaches before it settled on whole-program
static recompilation. Each change of direction came from the same question:
was the work producing visible or audible game output, or only more evidence
about why it did not?

| Dates | Approach | What happened |
|---|---|---|
| 05-18 to 05-19 | Emulator fix | Work started as an investigation of a xemu freeze. Finding no existing decompilation led to a private reverse-engineering workspace with Ghidra, symbol maps and xemu correlation notes. |
| 05-20 to 06-15 | Native host | A Windows program loaded the XBE and ran its x86 code directly, with shims for kernel imports. Windows already used the XBE's preferred low address range, so the host relocated the image and still hit missed relocations. Six "loops" reached the display mode call, a blank window, sustained GPU command traffic and the movie player, but never a recognizable title screen. |
| 06-16 to 06-21 | Decomp first | After the player said nothing was getting done, the repo added specs, work packets and source gates, and small Go "oracles" that modeled file cache, input and archive functions from static evidence. Most gates ended "held". The phase produced careful notes and very little forward motion. |
| 06-21 to 07-02 | Crash-driven recomp | A first `xboxrecomp` spike judged its output unusable for two input functions, yet a private runner built on it the next day advanced crash by crash. It later ran millions of frames without crashing, with no proof that anything rendered or played. |
| 07-03 to 07-15 | Native host again | A review found no recorded proof that the native host had been a dead end. With a five-day kill criterion, the exact-base mapping was solved, and the host presented its first non-uniform frame from parsed GPU commands. Controller enumeration stalled. |
| 07-16 to 08-14 | Whole-program recomp | The player asked to stop building harnesses and "start the actual recomp". The tracked `recomp-runtime` gained an XBE entry runner, then a runner that compiled every generated chunk with fail-loud stubs. Lifter discovery fixes went into `xboxrecomp`. Guest TLS was modeled from xemu observations, and kernel imports, a second main-loop frame, DirectSound creation and deterministic Start input followed. |

The public tree began on 2026-08-15 (`ef3bc2e`), and the native host was
frozen as reference material the next day. Bring-up then advanced by
numbered rounds, each ending at a classified stop. xemu with a GDB stub served
as the oracle for movie timing. A lifter defect in packed SSE instructions
explained black frames across thousands of call sites. Rounds 95 to 179
closed the front-end milestone #14 on 08-31, when two natural runs reached
island activity loading. Later rounds pushed through further screens until
scripted input reached an activity and a natural cancel input was consumed
there (#18). A 2026-09-05 assessment called
the result "a credible executable port scaffold" and set audio (#15) and
offline flows (#16) as the next work, which led to 0.0.1 Alpha.

Several current rules in `AGENTS.md` come from this period. Progress means
reaching a later natural event, not running longer. A bounded run finishes
within a minute. After two failed attempts at the same event, compare the
receipts before trying again. Generated C is never edited by hand.


## Releases up to 0.5.3

| Date | Milestone |
|---|---|
| 2026-08-15 | Public source baseline (`ef3bc2e`). |
| 2026-09-08 to 09-17 | 0.0.1 to 0.3.51 Alpha: boot, saves, audio, movies, water and activity repairs. |
| 2026-09-19 to 09-24 | Casino Blackjack and Poker, and hotel gift tapes (#39, #40), 16:9 by default (#38), shop vertex streams (#41), audio crackle (#42) and renderer stalls (#43). |
| 2026-09-25 | 0.4 Beta, the first fully playable build. Render scale, MSAA and SMAA, the D3D11 render worker thread, 60 Hz pacing and the launcher landed for it (#44 to #60). |
| 2026-09-26 to 09-29 | 0.4.5 to 0.4.9 Beta: bundled build tools, swimsuit fabric (#62), View Collection (#64), SDL3 controllers (#67, #68, #70), save deletion (#71). |
| 2026-09-29 to 09-30 | 0.5.0 to 0.5.3 Beta "Mixtape": custom soundtracks (#73), the private music folder (#75), launcher volume (#76), Radio Station shuffle (#78). |

Other chats in this period added community health files, rewrote
`AGENTS.md` from a review of 41 earlier sessions, and pinned the lifter to
the `NoRain211/xboxrecomp` fork (#50).

## Presentation and graphics, 2026-09-26 to 2026-10-09

After 0.5.3, work moved from playability to presentation. Most of it ran in
one long development chat, with subagents doing bounded pieces in separate
worktrees. Eleven pull requests from that period merged on 2026-10-09.

| Date | Work | Result |
|---|---|---|
| 09-26 | Reviewed anti-aliasing options. The pinned SMAA is still upstream HEAD. | Kept SMAA 1x Ultra; S2x and T2x deferred. |
| 09-26 to 10-04 | Ray-traced global illumination (RTGI) with D3D12 DXR beside the D3D11 renderer. | Local branch `codex/rtgi-dxr`; not merged. |
| 09-27 | Closing the window between frames exited with `Clear presenter failed (9)`. | Every presenter call now treats a closed window as a normal exit (#80). |
| 09-27 | Stair-stepped halo around characters above native resolution. The game's depth-of-field pass samples a small copy of the frame. | That copy now reads a mip chain of the scaled backbuffer (#79). |
| 09-27 | "Shimmer" on the volleyball on a 240 Hz monitor. Frames were held for 3, 4 or 5 refreshes. | Fixed-refresh pacing uses sync interval 2 at 120 Hz and 4 at 240 Hz (#82). |
| 09-27 to 09-30 | Variable refresh rate (VRR). | Opt-in `RECOMP_D3D_VRR=1` with a window-sized swap chain and 60 Hz slot pacing (#82). Research in [vrr-frame-pacing.md](wayfinder/research/vrr-frame-pacing.md). |
| 09-30 | Draw census of three attract scenes. Characters in sunset scenes use point lights, which the D3D11 path ignored. | Fixed-function point lights (#92). |
| 09-30 | Shimmer on cutouts and textures seen at an angle. | 16x anisotropic filtering and alpha-to-coverage with MSAA (#93). |
| 09-27 to 10-04 | Texture replacement: planned (#69, #81), then built with F12 dump and Ctrl+F12 reload. | [texture-replacement.md](texture-replacement.md) (#95). |
| 10-04 | Mapped the render pipeline with an F11 draw capture. The game requests fog on most draws, and the runtime drew none of it. | Xbox fog, on by default (#94). Pipeline map on `codex/rtgi-dxr`. |
| 10-04 | Per-pixel lighting and smoothed normals to reduce mesh-shaped skin banding. | Small visible gain; local branch `codex/per-pixel-lighting`. |
| 10-04 to 10-05 | Archive ripper, texture sorter, bone mapping and glTF export. Ripped texture keys match runtime dump keys. | Local branch `codex/asset-ripper`. |
| 10-04 | Tested whether game logic scales with time. At a 120 Hz vblank the attract demo ran twice as fast, so the game counts frames. | Both high frame rate paths keep logic at 60 ticks per second. |
| 10-05 to 10-07 | Split rate: hand-written animation decoding and skeleton solving, then renderer-side frames between ticks. A review found five release blockers, which were fixed on a clean branch. | `RECOMP_SPLIT_RATE` (#98). [Design](wayfinder/research/high-frame-rate-split.md) and [review](wayfinder/research/high-frame-rate-split-review.md). |
| 10-05 to 10-08 | Whole game at high frame rate: hand-written main loop and scheduler, with the game's own draw code run again between ticks. | Local branch `codex/high-frame-rate`; prototype. |
| 10-08 to 10-09 | Black screens between rounds and at the Hopping Game, handled in a separate chat. | The held frame stays visible at full resolution (#97). |
| 10-09 | Shimmer on the sea in the title screen. | Stage-1 texture combiner support (#96). |
| 10-09 | Review loop over #79 to #82 and #92 to #98. | All eleven merged as GitHub merge commits; `main` at `2f1be48`. |

## Decisions

**D3D11 stays the complete renderer.** RTGI runs as a D3D12 side pass that
receives each frame's depth, color and triangles, then hands its result back
to D3D11 for compositing before the UI. Most games run one API at a time,
either one renderer or a choice at startup. The hybrid was the smallest way
to test ray tracing without rewriting the presenter. A native D3D12 renderer
is worth building only once ray-traced lighting proves worth keeping.

**Vendor-neutral features only.** Everything uses standard D3D11, DXR 1.1 and
the DXGI tearing flag. There is no NVAPI, AMD AGS or vendor check. If an
upscaler is added, prefer FSR or XeSS over DLSS. All testing so far used one
NVIDIA GPU.

**Fix D3D11 fidelity before adding effects.** Point lights, fog and combiner
stages reproduce what the game already asks for, and every player gets them.
RTGI upgrades come after.

**Fixed-refresh pacing uses whole sync intervals; VRR is opt-in.** At 120 and
240 Hz each 60 Hz frame is held for exactly 2 or 4 refreshes. Under VRR the
presenter waits for its slot on a 60 Hz grid before presenting. That can add
up to one frame of latency.

**Fog is on by default** because the game requests it. `RECOMP_D3D_FOG=0`
turns it off for comparison.

**Texture keys hash the full texture content.** The original format, size,
mips and palette are hashed, so a replacement applies wherever that image is
used. Runtime dumps define the names, and the ripper only helps because it
produces the same keys. Replacement folders may live anywhere the player
chooses. Extracted textures are never committed.

**High frame rate keeps 60 Hz game logic.** Running physics, timers and AI at
120 Hz changes how the game plays and diverges from 60 Hz. Both paths spend
extra refreshes only on drawing. Split rate is the release candidate because
it blends between two real ticks and costs little CPU. Whole game is the end
goal, because it redraws everything the game draws and leads toward
hand-written game source. Rates are parameters, never a fixed 2x.

**The intro keeps its 60 Hz shot timing,** which follows audio playback as on
the original.

**Merge shape.** Pull requests land as GitHub merge commits. During the review
loop, `main` was merged into open branches instead of rebasing them.

**Point-light ambient falls off with distance,** as in D3D8. A review comment
that asked for constant ambient was declined.

## Conclusions that turned out wrong

- **The native host was a dead end.** It was dropped in June without a
  recorded technical proof. In July the exact-base mapping that had blocked
  it was solved within days. The project still chose whole-program recomp,
  because that path leads to hand-written source.
- **`xboxrecomp` output was unusable.** A June spike stopped after its output
  for two input functions did not match the static notes. The same lifter,
  fixed upstream and in the fork, now generates the whole game.
- **Running more frames without a crash meant progress.** The June runner
  reached millions of frames with no proof of rendering or audio. Progress
  now means reaching a later natural event.
- **The round 116 hang was a runtime bug.** A lifter upgrade had changed
  almost every generated body. A control tree built with the earlier lifter
  and the same recoveries ran past the hang.
- **The opening movie stalled because of file read ordering.** Two
  read-ordering theories were tested and retired. The xemu oracle showed the
  real path through the movie classifier.
- **VRR was worse than fixed pacing.** The first PresentMon comparison on
  09-26 found VRR frames uneven. Earlier "scale 3" test runs had really run at
  4.5 because of a script bug. VRR became even once presents waited for their
  60 Hz slot (#82).
- **RTGI would visibly change the lighting.** The first pass only added
  bounced light from on-screen surfaces. On sunny scenes it was almost
  invisible, because the game's baked ambient already fills shadows. Darkening
  near geometry, a form of occlusion, made the difference visible.
- **The ball shimmer came from RTGI or the ball's rendering.** A window
  recording showed uneven frame holds at 240 Hz with sync interval 1.
- **A DXGI refresh count of 2 under VRR meant doubled frames.** PresentMon
  showed those frames were on screen for about 15 ms. The counter does not
  measure display time under VRR. The real problem was uneven spacing between
  `Present` calls.
- **Waiting for the GPU after each present would even out VRR.** It cut
  throughput to 43 to 55 fps and was reverted.
- **The title-screen black specks were unrelated to RTGI.** They came from
  RTGI's depth-buffer fallback on water, where rays started just below the
  rebuilt surface.
- **The sand used bump mapping that the runtime drew wrong.** The captured
  attract scenes used only fixed-function stages. The real bug found there was
  ignored point lights.
- **Hit color caused RTGI's match slowdown.** Turning it off reduced the
  slowdown but did not remove it. The render thread was also skinning far more
  character vertices than each draw used. Trimming that fixed the attract
  demo; match performance is still unverified.
- **Per-pixel lighting would fix skin banding.** The bands follow the
  low-poly mesh normals, so lighting each pixel changed little. Dithering
  removed the 8-bit steps.
- **Alpha-to-coverage would not help.** The first test on a flower bush
  showed no visible difference and was shelved. Combined with anisotropic
  filtering, it later reduced cutout shimmer (#93).
- **Whole game could predict the next tick.** Forward prediction overshot
  when a character stopped. Blending between the last two real ticks replaced
  it.
- **Title-screen trees were popping in and out.** Palm sway was read as the
  wrong number type between ticks, which threw palms about 100 units off.

## Unmerged work

| Branch | Contents | State |
|---|---|---|
| `codex/rtgi-dxr` (local) | DXR global illumination, hit color, hotkeys F5 to F10 for strength, split view, on/off, pause and frame step, the F11 draw capture, and research on sun shadows, the render pipeline and a three-stage lighting plan (#83 shadows, #84 bounce, #85 relight). | Holds 60 fps in the attract demo; slows down and hitches in matches. |
| `codex/per-pixel-lighting` (local) | `RECOMP_D3D_PIXEL_LIGHTING` and `RECOMP_D3D_SMOOTH_NORMALS`. | Small gain. Built on an older point-light branch with #79 merged in, so it needs redoing on `main`. |
| `codex/asset-ripper` (local) | Archive unpacker, texture ripper and sorter, bone mapping, glTF export, and a model format note. | Tools only. Publishing it is undecided. |
| `codex/high-frame-rate` (local) | Whole game at high frame rate and the earlier split-rate history. | Prototype. Fails the export check because two files have CRLF endings. |

## State on 2026-10-09

`main` (`2f1be48`) contains #79 to #82 and #92 to #98. No release includes
them yet, and `CHANGELOG.md` has no entry for them. The player play-tested
point lights, texture filtering, fog and texture replacement before review.
The review fixes then changed rendering or pacing in #82, #93, #96, #97 and
#98, and #97 and #98 were never play-tested before they merged. The new
`main` has only agent smoke runs, which show that it starts and presents
frames.

## Open items

- Play-test current `main`, then prepare release notes and update
  public-status.md for what that run proves.
- Split rate: water and the shop item preview still move at 60 Hz, because
  vertex-program draws get no smoothed camera. Objects without smoothing data
  can run up to one tick ahead. Textures are re-uploaded on every present.
  Input latency and VRR with split rate are untested.
- Whole game: a 120 Hz match reached 119.3 fps on average, with a worst second
  of 115. 144 and 240 Hz are not steady. Particles still draw one tick ahead.
  Exact parity with 60 Hz holds only with a test clock that drives audio
  timing from game ticks.
- RTGI: slowdown and hitches in matches, a dark smear on the ball, and stages
  #83 to #85.
- Short freezes of 0.7 to 3.8 s seen during play remain unexplained. Logs
  suggested the game thread, not the renderer.
- Xbox pixel shaders and combiners are only partly supported. The hotel,
  pool, shops, casino and gallery have not been captured to see which draws
  use them.
- The launcher does not expose VRR, fog, split rate or texture replacement
  yet.
- Security scan fixes #86 (Media Foundation loaded from System32 only), #87
  (guest buffer bounds) and #88 (lifter section extractor note) are open.
- Known issues #54, #55 and #56 are unchanged.
