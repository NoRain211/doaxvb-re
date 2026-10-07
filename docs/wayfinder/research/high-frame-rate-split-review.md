# Split-rate release candidate review

Review date: 2026-10-06. Subject: the split-rate track on
`codex/high-frame-rate` at `406b367` (commits `e2d7ce6` through `406b367`,
including the `aed179d` checkpoint). This is a static code review checked
against the design notes in [high-frame-rate-split.md](high-frame-rate-split.md)
and public primary sources. No runtime was run for this review, so every
runtime claim below is a falsifiable hypothesis with a proposed check.

Line numbers refer to `406b367`.

## Status on the split-rate branch

The split commits were ported alone onto `origin/main` as
`codex/split-rate`, which resolves H3. The whole-game scheduler, scene,
camera, world and sound work stays on its own track. The `aed179d`
checkpoint was ported because it touches only split-rate files.

| Finding | Resolution |
| --- | --- |
| H1 | Split guards fall back to the captured 60 Hz draw or actor state and log each reason once. `RECOMP_SPLIT_STRICT=1` restores the stops for gates. Verification and pose experiments still stop. |
| H2 | Every frame-adapter presenter call, including the kernel's memory release, takes the Swap close path. |
| M1 | `RECOMP_SPLIT_RATE=auto` uses the primary display refresh. A rate within 1% snaps to it; with `--vsync` a higher rate is lowered to it, and a lower rate warns. Variable refresh remains untested. |
| Docs | The design notes have a current-state section with settings and known limitations, and the errors listed below are corrected. |

M2 to M6 and L1 to L5 remain open.

## Summary

The core design is sound and matches standard practice. Gameplay keeps its
fixed 60 Hz step. Each present samples between the previous and current tick.
Draws that cannot be paired are snapped, and the worker owns every input it
replays. No data race was found between the game thread and the render
worker. With `RECOMP_SPLIT_RATE` unset, every split path is skipped.

Three issues block shipping split rate as an opt-in feature: a visual-only mode
can still stop the game on unseen content, closing the window can exit with an
error, and the branch carries unrelated whole-game work. Pacing has a
display-rate gap that needs at least documentation and one display-side
measurement. Vertex-program draws are the largest coverage gap and the likely
cause of the 60 Hz water steps.

## Findings, ranked

### High

**H1. Guard failures stop the game in split mode.** These checks call
`recomp_stop` while split rate is on:

- [animation_split_adapter.c](../../../recomp-runtime/animation_split_adapter.c):
  56 (`split:binding-capacity`, 512 bindings per tick), 259
  (`split:camera-mismatch`), 343 and 345 (payload size and allocation), 397
  (`split:seam-links-unimplemented`), 423, 427 and 430 (seam stream checks).
- [animation_probe.c](../../../recomp-runtime/animation_probe.c): 106
  (`animation:seam-record-count`), 153 (`animation:height-input`), 337, 346
  and 357 (palette recipe checks) and 390 (`animation:palette-mismatch` above
  1e-3).

These checks were the right tool for bounded agent gates. In a release
build, though, any untested scene or camera mode that trips one ends the
session, and the camera check uses a 1e-4 absolute tolerance on raw matrix
elements. The design notes already list the open cases: optional seam-link
streams, wider secondary geometry, uncommon cuts. The S3 pool-hopping stop
showed the pattern in practice. Each failure has a safe fallback: submit the
captured draw unchanged, or mark the actor or camera as not ready for the
tick. That is what ordinary 60 Hz presentation would show. The worker
already does this for `recomp_d3d_split_draw` failures
([d3d_presenter_thread.cpp](../../../recomp-runtime/d3d_presenter_thread.cpp)
227-236).
*Fix:* fall back and log once per reason by default. Keep the stops behind a
strict diagnostic setting for gates. *Check:* force a guard (for example
lower the camera tolerance) and confirm the run continues with unblended draws.

**H2. Closing the window can exit with code 2.** The worker records
`RECOMP_D3D_PRESENTER_CLOSED` as a sticky status
([d3d_presenter_thread.cpp](../../../recomp-runtime/d3d_presenter_thread.cpp)
178-180), so every later submit returns it. Only Swap treats it as a normal
exit ([d3d_frame_adapter.c](../../../recomp-runtime/d3d_frame_adapter.c)
360-364). Clear (216-227), Reset (around 185) and gamma (around 1394) stop
with `d3d-*:presenter:9`. This matches the reported
`d3d-clear:presenter:9`. The race also exists at 60 Hz. Split mode makes it
more likely because the worker pumps messages several times per tick, so a
close often lands mid-tick. *Fix:* route `CLOSED` from every frame-adapter
submit through the same `host-window-close` exit, for example with one
shared helper. *Check:* close the window repeatedly during a split run; the
exit reason should always be `host-window-close`.

**H3. The release branch carries unrelated work.** Relative to `origin/main`,
`406b367` contains about 13,000 added lines, including the whole-game
scheduler, scene, camera and world code and two `wip` checkpoints
(`aed179d`, `8e45782`). That code is gated by `RECOMP_GAME_HZ`,
`RECOMP_GAME_SCHEDULER` and `RECOMP_GAME_SCENE`, but it would ship
unreviewed and break the one-concern-per-PR rule. *Fix:* port the split
commits onto `origin/main` as their own PR. The split code does not depend on
the whole-game scheduler.

### Medium

**M1. Pacing ignores the display's actual refresh.** The worker sleeps on a
CPU timer to `RECOMP_SPLIT_RATE`, then calls `Present(1)`
([d3d_presenter_d3d11.cpp](../../../recomp-runtime/d3d_presenter_d3d11.cpp)
3197-3199) with three buffers (752-753). The rate is never compared with
the output's refresh, and there is no tearing or VRR path. Expected
consequences:

- With `120` on a 119.88 Hz display, the producer runs 0.12 frames/s fast.
  The queue fills, `Present` blocks, and `split_next` drifts until the
  reset at
  [d3d_presenter_thread.cpp](../../../recomp-runtime/d3d_presenter_thread.cpp)
  419-420 skips a sample. That predicts a visible step about every 8 s, plus
  up to two queued frames of steady latency.
- With `100` on a fixed 144 Hz display, presents land on alternating 2- and
  1-vblank intervals (judder).
- On a VRR display, sync interval 1 caps presentation at the refresh rate.
  Microsoft's VRR guidance requires `DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING` and
  sync interval 0 for unthrottled presentation
  ([Variable refresh rate displays](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/variable-refresh-rate-displays)).

The S2 and S4 tables time the CPU-side start of each present, so they cannot
show these effects. *Check:* capture display-side intervals with
[PresentMon](https://github.com/GameTechDev/PresentMon) at 120 on a 119.88 Hz
mode and at 100 on a 144 Hz mode. *Fix (minimum):* document that the rate
must equal the display refresh. *Fix (better):* default to the output
refresh, and sample at the predicted display time using
[frame statistics](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-getframestatistics)
or a
[frame-latency waitable swap chain](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-getframelatencywaitableobject).

**M2. Vertex-program draws get no camera interpolation.** Split tagging runs
only when `program_count == 0`
([d3d_draw_adapter.c](../../../recomp-runtime/d3d_draw_adapter.c) 1927).
Program draws therefore render with the current tick's camera at every
fraction, while fixed-function draws use the interpolated camera. During
camera motion the two sets of geometry separate by up to one tick of camera
travel. The S3 section says the water "gets the interpolated camera but
keeps its 60 Hz motion". That holds only if the water is fixed-function. If
it is a program draw, as water surfaces often are, this gap would explain
the 60 Hz steps spread over the pool. *Check:* in a pool-hopping burst, log
per present how many untagged draws have `program_count != 0`, and compare
their screen regions with the stepped blocks. *Fix:* find the program
constants that hold the tick's view-projection product, then substitute the
interpolated product the same way the fixed-function path does.

**M3. Untagged moving content leads tagged content by up to one tick.**
Tagged draws blend from tick N-1 to N, while untagged draws show tick N from
fraction 0. Static scenery is unaffected because only the camera matters for
it. Moving untagged content is shown ahead of the tagged characters:
particles, CPU-deformed secondary geometry, repeated props (snapped
deliberately in `49cec74`), the rotating shop preview, and HUD elements tied
to world positions. At 120 Hz the offset is 8 ms on average. It is most
visible on effects attached to characters. *Check:* frame-step a fast serve
or dive and look for splash or hair offsets against the body. *Fix:* extend
pairing (below), or present the untagged part from the previous packet.

**M4. Camera pairing keeps only the last build per slot per tick.**
[animation_split_adapter.c](../../../recomp-runtime/animation_split_adapter.c)
227-243 overwrites `inputs[1]`, `view` and `projection` each time a slot
is rebuilt within a tick, and draws match by exact view/projection equality.
A slot that is built more than once in a tick, or the alternate camera bound
by `0x001221E0` for offscreen passes, leaves the earlier passes unmatched.
Those passes, such as reflections, stay at tick N while the main pass
interpolates. *Check:* count `recomp_animation_split_camera` calls per slot
per tick on the pool and island routes.

**M5. Resource releases repeat on every present.** `execute` replays the
whole packet for each present, including `RELEASE` records
([d3d_presenter_thread.cpp](../../../recomp-runtime/d3d_presenter_thread.cpp)
270-272). A packet that releases memory therefore drops and re-uploads the
affected textures and targets 2x at 120 Hz and 4x at 240 Hz. Loading and
streaming ticks carry most releases, which may explain part of the 16.7 to
42.6 ms first-frame spike in S2. *Fix:* apply `RELEASE` only on the packet's
first present. Later replays then reuse the cache entries built from the
same packet bytes. *Check:* count releases and texture creations per packet
across the S2 load window before and after.

**M6. Added input-to-photon latency is unmeasured.** The worker begins tick
N's interpolation 4 ms after the packet is published and reaches tick N one
tick later
([d3d_presenter_thread.cpp](../../../recomp-runtime/d3d_presenter_thread.cpp)
355-363). That is the expected one-tick delay of render interpolation
([Fiedler, "Fix Your Timestep!"](https://gafferongames.com/post/fix_your_timestep/)).
On top of it sit three buffers and the default DXGI frame latency, so the
estimate is about 20 to 37 ms more than the 60 Hz path. No measurement
exists. *Fix:* set a maximum frame latency of 1 with a waitable swap chain
([Microsoft: reduce latency with DXGI 1.3 swap chains](https://learn.microsoft.com/en-us/windows/uwp/gaming/reduce-latency-with-dxgi-1-3-swap-chains)),
then measure.

### Low

- **L1.** `RECOMP_SPLIT_RATE` with `RECOMP_GAME_HZ` is documented as
  unsupported, but no code rejects the combination. Disable split rate with a
  log line when both are set.
- **L2.** `RECOMP_SPLIT_RATE=60` is accepted
  ([d3d_split_pose.c](../../../recomp-runtime/d3d_split_pose.c) 15). It runs
  the full split pipeline at 60 Hz, which only adds latency.
- **L3.** `RECOMP_SPLIT_TRACE` without `RECOMP_SPLIT_TRACE_LIMIT` appends one
  record per present until exit
  ([d3d_presenter_thread.cpp](../../../recomp-runtime/d3d_presenter_thread.cpp)
  408). At 240 Hz that is roughly 75 MB per hour. It is diagnostic only.
- **L4.** The presenter tick is fixed at `1000/60` (344), matching the guest
  vblank. Ticks with a skipped swap counter are snapped rather than
  stretched. That is correct, but scenes that present every second vblank
  would get no interpolation.
- **L5.** Split payloads are allocated per draw and freed after submit
  ([d3d_draw_adapter.c](../../../recomp-runtime/d3d_draw_adapter.c) 1965).
  No leak was found. The rigid tables clear about 1.5 MB per tick, which is
  small.

### Checked and found correct

- Packet hand-off: the game writes only the open slot. `published_ms` is set
  before `pending` is incremented under the lock, and `prepare_next` reads
  only a published packet. Sealing outside the lock (`b57b791`) is safe for
  the same reason.
- Interpolation, not extrapolation: endpoints are previous and current tick,
  and fractions are clamped to [0,1).
- Cut and teleport snapping (`8b4cede`) uses the ratio of successive steps
  and resets history on nonconsecutive ticks and on camera flag changes.
- Newest-binding selection (`a927266`) matches the order in which the game
  builds palettes.
- Shutdown: the worker treats shutdown as "next packet ready", so it drains at
  most the queued ticks.
- Split off: all tagging, capture ownership, three-buffer presentation and the
  pacing loop are gated on the rate. The changes that reach the ordinary path
  are the capture span index (`c05ee0a`), the indexed-draw change
  (`23ed370`, from the whole-game track), and env-gated diagnostics. Their
  evidence belongs in their own PRs.

## Research gap check

Shipped interpolating renderers follow the same overall shape. Fiedler's
fixed-step loop renders a blend of the previous and current state, which
costs one step of latency. RT64 replays the game's captured draw calls at a
different rate. It pairs calls between frames using tracked parameters and
explicit tags, and its extended GBI gives each matrix group interpolate or
skip flags per component (`G_EX_COMPONENT_INTERPOLATE`,
`G_EX_COMPONENT_SKIP`) ([RT64](https://github.com/rt64/rt64),
[rt64_extended_gbi.h](https://github.com/rt64/rt64/blob/main/include/rt64_extended_gbi.h)).
Zelda64Recomp builds on that and renders "game objects and terrain, texture
scrolling, screen effects, and most HUD elements" at high rates. It defaults
to the monitor's refresh rate
([Zelda64Recomp](https://github.com/Zelda64Recomp/Zelda64Recomp#high-framerate-support)).
Ship of Harkinian records matrix operations per frame and uses a camera
epoch to stop interpolation across camera cuts
([frame_interpolation.cpp](https://github.com/HarbourMasters/Shipwright/blob/develop/soh/soh/frame_interpolation.cpp)).

Compared with those, this track is missing:

| Technique | Status here | Suggested approach |
| --- | --- | --- |
| Default to the display refresh | Manual numeric rate | Read the output mode and use it when the setting is absent or set to "auto" (M1). |
| Display-synchronized sample time and a VRR path | CPU timer with sync interval 1 | Frame statistics or a waitable swap chain; tearing flag and interval 0 on VRR (M1). |
| Vertex-program draws | Not tagged | Substitute camera constants (M2). |
| Texture scrolling and animated UVs (water, scrolling UI) | Steps at 60 Hz | Pair texture transforms between matched draws, as Zelda64Recomp does for texture scrolling. |
| Non-character animated objects (shop preview, repeated props) | Snapped or untagged | Pair by stable keys (object plus vertex source plus texture) instead of draw order; snap when pairing is ambiguous. |
| Particles and 2D/HUD | Untagged | Low priority for this game; RT64 matches them heuristically and snaps on failure. |
| Explicit cut signal | Ratio heuristic | Hook the game's own shot-change event (the camera script is now decompiled on the whole-game track), as SoH uses a camera epoch. Keep the heuristic as a fallback. |
| Latency budget | Unmeasured | Maximum frame latency 1, waitable object, then measure (M6). |
| Display-side frame timing | CPU-side intervals only | PresentMon display metrics for S2 and S4. |

Extrapolation, the alternative to rendering one step behind (offered, for
example, as Unity's rigid-body extrapolate mode), would remove the delay. It
guesses wrong at hits, cuts and direction changes, though, which this game
has constantly. Interpolation is the right choice.

## Documentation check

[high-frame-rate-split.md](high-frame-rate-split.md) has a
`public-export.json` entry and is stored with LF endings. It names no
private paths, assets or filenames. Raw captures are described only as kept
under `private/`. Problems to fix before release:

- The page is a chronological log. Its earlier sections still say
  `RECOMP_SPLIT_RATE` "is not implemented" (lines 396, 566, 633, 752). Add a
  short current-state section at the top covering the settings
  (`RECOMP_SPLIT_RATE`, `RECOMP_SPLIT_TRACE=1|present`,
  `RECOMP_SPLIT_TRACE_LIMIT`, `RECOMP_SPLIT_FAILURE`), the requirement that
  the rate match the display, incompatibility with `RECOMP_GAME_HZ`, and the
  known limitations (M2 to M4, H1 guards, water and shop preview).
- M3 says the worker "samples the actual elapsed fraction". Since `b57b791`
  it samples the scheduled deadline.
- S1 says a snapped tick "holds the previous visual state". It actually shows
  the current tick for the whole interval, which is what 60 Hz shows.
- S3's water explanation depends on M2.
- S3 cites `s3check.py`, which is not in the public tree. Publish it under
  `tools/` or describe its method well enough to reproduce.
- S2 names a specific desktop program seen during stalls. That is machine
  run evidence; "other desktop programs" is enough.
- `docs/building.md`, `docs/public-status.md` and `CHANGELOG.md` do not
  mention split rate yet.

## Release readiness

Must fix before shipping split rate as opt-in:

1. Replace the H1 stops with per-draw or per-tick fallback, keeping a strict
   mode for gates.
2. Fix the H2 close path.
3. Port the split commits alone onto `origin/main` (H3) and pass `public-ci`.
4. Document that the rate must equal the display refresh and that VRR is
   untested. Record one PresentMon display-side run at 120 and 144 (M1).
5. Get the user's play test of the named build, with split off as well as on.
   Cover pool hopping, a shop, a match, the hotel and the island map.
6. Add the current-state section and fix the documentation errors above.
   Add the user-facing setting to `docs/building.md` or the controls page.

Nice to have, in rough value order: vertex-program camera interpolation (M2),
releases on the first present only (M5), latency reduction and measurement
(M6), automatic refresh detection with a VRR path (M1), texture-transform
interpolation for water and scrolling, stable pairing for the shop preview
and props, an explicit cut signal, and the L1 to L3 guards.
