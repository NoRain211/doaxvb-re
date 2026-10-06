# Whole-game 120 Hz timing assessment

Date: 2026-10-03. Research and an opt-in pacing probe; not a supported 120 Hz
mode or gameplay acceptance.

## P1: indexed draws skip the generated D3D8 body (2026-10-05)

The draw adapter for `D3DDevice_DrawIndexedVertices` (`0x001E78B0`) no
longer runs the generated driver body before its host draw. That body did
three things. It flushed lazy D3D state through `0x001EECA0` and
`0x001EEC20`, a chain of about 75 generated functions. It copied the index
list into the push buffer, which the hand KickOff model discards. It set and
cleared bits `0x800`/`0x1000` at device `+8`, and inserted a fence when
`0x1000` was already set. The adapter now only retires the stdcall frame.

Seam: the indexed draw is hand-owned at its manual lookup in
`d3d_draw_adapter.c`. The rest of D3D8 is still generated. The dirty mask at
`0x001F2984` now accumulates until another generated entry point, such as
`DrawVerticesUP`, flushes it. Wholesale D3D8 replacement remains open work.

All checks used one executable that could select either path, with the 60 Hz
attract route and live host input disabled:

| Check | Result |
| --- | --- |
| Frames at Swaps 300, 3000, 5400 | 0 differing pixels at each |
| Game state ranges (actors, ball, camera, scene) at the same Swaps | byte-identical |
| Fence counter (device `+0x30`) and dirty mask at the same Swaps | identical |
| Whole guest RAM at Swap 3000, beyond two-control noise | push buffer, device bookkeeping, stack below ESP, wall-clock audio and feedback buffers |
| 120 Hz match, game-thread ms per pass (phases 0-3) | 10.89 before, 7.72 after (-29%) |
| 120 Hz match, phase 2 (fibers and draws) | 10.60 before, 7.43 after |
| 120 Hz match rate | 91.1 fps (24 of 26 s below 118) before, 120.1 fps (1 of 26 s) after |

The changed bytes are D3D8 bookkeeping, guest stack scratch, or buffers that
follow the wall clock; game state and frames did not change. The control
match on this tree is slower than the M4 numbers below because uncommitted
presenter work shares it; both paths ran on the same executable. The
candidate run ended with the existing shutdown race, a Clear submitted after
the window closed, which earlier runs also show.

## Replay camera at 120 Hz (2026-10-05)

After a point, the game can show a slow-motion replay (match state 5) filmed
by camera mode 19. At 120 Hz two things went wrong; both are fixed in
`game_camera.c`.

The camera stayed in replay mode after the replay. Entering state 5, the
camera director (`0x0002F580`) saves the current camera mode as the one to
return to (camera `+0xACD`) and requests mode 19. It does this on every
camera pass that sees the state-change flag `0x004D5669`. The match state
owner (`0x00099500`) raises that flag once per tick, and the flag stays
raised until that owner runs again a tick later. At 120 Hz the camera runs
twice in that window, so the second pass saved mode 19 over the saved mode.
When the replay ended, the director found nothing to return to, and the
replay camera filmed the next serve and rally until the point ended. Camera
passes after the first one in a tick now see the flag clear; its value is
restored after the camera tick.

The replay camera also moved at double speed. Its plan step `0x00023040`
advances a sweep whose speed itself accelerates, and ages the plan, once per
call. Mode 19 (`0x00022F40`) is now hand source and runs that step on the
authored pass only. The first call after mode entry builds the plan at once.
Held passes keep the staged target, which the per-pass camera smoothing
still follows. Replay playback itself advances once per tick at every rate,
through the match state fiber.

| 120 Hz match, scripted route | Before | After |
| --- | --- | --- |
| Camera mode after a replay ends | 19 until the point ends (4 of 4 runs with a replay) | 5, as at 60 Hz (2 of 2 replays) |
| Replay plan steps per tick | 2 (one per pass) | 1.00 over a 177-tick replay |

At 60 Hz both changes are inactive: every pass is authored and the camera
sees each state change once. These are agent smoke runs. The replayed
players and ball still move once per tick at 120 Hz.

## M4: menus and a served point at 120 Hz (2026-10-05, blocked)

M4 is not passed. In game time, the 120 Hz route matches 60 Hz through the
menus, the serve and the rally. In wall time, the match runs below 120 fps
on the test machine, so it plays 7-15% slower than 60 Hz and drifts against
wall-clock audio. The throughput owners are outside the whole-game hand
source; see the end of this section.

A scripted Exhibition route now reaches a real match at 120 Hz: title,
Start, the menus, character selection, the match introduction, a serve and
a full rally that ends in a point. Before this change the 120 Hz route ran
the menus at double speed and missed menu inputs.

The route uses poll-indexed inputs. Input polls happen once per authored
tick at every rate, so both rates receive each press on the same game tick.
Live host input stays disabled.

Four owners changed:

- **UI frame owner `0x0008A970`.** It still advanced once per present. Above
  60 Hz it now runs only on the pass that polls input. Its draws are recorded
  (`game_frame_replay.c`, through a draw observer in the draw adapter) and
  resubmitted on held passes, so menus keep their authored speed and still
  appear in every present.
- **Tick scope for once-per-tick work.** Counted fiber sleeps in the
  scheduler and the hand scenario tick run inside
  `recomp_game_tick_scope_begin/end`. The scope multiplies the per-pass step
  scalar `0x004D6FC4` back to a whole tick. On a held pass it also swaps in
  the tick's input records (`0x005F3088`, 5 x 44 bytes) and the scene
  fiber's copy (`0x004D9540`, 4 x 44 bytes). Each tick's input edges are
  therefore seen exactly once, whichever pass the fiber resumes on.
- **Clip end.** `game_actor.c` tests the clip end only at the start of a
  tick. Testing it on every substep started the next clip one substep early.
- **Per-actor event bytes.** `0x004D704C + 188 * actor` are cleared only on
  the authored pass, so an event raised on either pass lasts its whole tick.

At 60 Hz every pass is authored and none of these paths changes behavior.
M1 and M2 hold on this tree. Equal-game-time captures against the 60 Hz
reference differ in 0.00%, 0.01% and 0.36% of pixels by more than 8 at
Swaps 300, 3000 and 5400. Two 120 s real-paced 120 Hz runs averaged
120.0 fps (minimum 102 and 119 fps) and reached presents 6000 and 10800 at
49.998/49.995 s and 89.995/89.995 s. The 60 Hz control on the same
executable reached Swaps 3000 and 5400 at 50.184 and 90.206 s, so both runs
are within -0.38% and -0.23%.

The serve and rally were compared in game ticks from match start, with the
ball position at `0x00416630`:

| Measurement | 60 Hz | 120 Hz |
| --- | --- | --- |
| Toss peak | tick 621, height 4.64 | tick 621, height 4.64 |
| Serve contacts | ticks 622-658, every 6 ticks | identical ticks; position within 0.004 |
| Rally hit interval | 75-81 ticks | 70-77 ticks |
| Rally ends (ball lands) | tick 1189 after six exchanges | tick 1156 after six exchanges |
| Gravity per tick | -0.0027 | -0.00265 |

Ball flight therefore takes the same number of game ticks. The rally that
follows differs in its random choices, through the effect owner described
below. Two 60 Hz runs on one executable produce identical audio cues through
the rally. Earlier 60 Hz mismatches came from runs on different builds.

For audio, every DirectSound Play was logged with its present count and
converted to game ticks. The title loop cue repeats every 240 ticks at both
rates, within one tick. Menu and match cues that occur at both rates land on
the same tick after boot, for example 4378, 4407, 4443, 4517, 4525, 5037, 5040 and 5050.
Cue totals are similar across the match introduction: 17/24, 61/69 and
69/56 in three consecutive windows. No cue type plays twice per tick. Sample
playback stays on the wall clock.

The opening movie was captured once per wall second at both rates with no
Start press. The same movie scene appears at each capture index, and both
runs end on the same close-up. The rates differ by at most about half a
second at the logo fade, within the variation of startup timing. The movie
and its audio follow the wall clock at both rates.

The remaining match difference is the actor frame fiber `0x000B42B0`. Its
two per-pass calls, `0x000B4380` and `0x000B4450`, run with the per-pass
step and spawn effects that draw 8, 36 or 44 values from the shared
`rand()` state. Each rate is deterministic over this window. Most bursts
land on the same tick at both rates, but 120 Hz misses the bursts 29 and 32
ticks after match start, adds one at 36, and moves the 8-value bursts later.
The RNG state is identical before the first missed burst. A discrete spawn
test evaluated on each substep is the likely mechanism; it has not been
located. The changed random sequence then alters later AI and rally choices
and which effect sounds play. Two 120 Hz runs agree through the first 320
ticks of the match. They then diverge at a streamed-audio refill that lands
one tick apart, which is expected while game time trails the wall clock.

`private/PlayHfr.cmd` launches the play-test build at 60, 120 or 240 Hz,
or `off` for the default path. It is a local helper and is not tracked.

The match fails the real-time part of the gate. The 60 Hz match holds 60 fps
with about 18% of each frame spent waiting for vblank, so a match frame costs
about 13.7 ms. At 120 Hz each pass costs 9.1-9.8 ms against an 8.3 ms budget,
and the match runs at 102-111 fps (13-20 of 23 seconds below 118 fps). The
attract demo holds 120 fps, as measured for M3.

The scene and scheduler manual lookups read the environment on every
dispatched call to a hand owner. They now read it once. A stack-walking
profile of the match then attributes the game thread as follows:

| Owner | Share of game thread |
| --- | --- |
| Generated D3D8 indexed draw (`0x001E78B0`), run by the draw adapter for byte-identical push-buffer bookkeeping | 29% |
| `animation_probe.c` manual lookup reading the environment on every dispatch | 11% sampled; at most 0.3 ms per pass in an interleaved A/B |
| Presenter capture packet copy and allocation per draw | 8% |
| Guest memory helpers (`recomp_memory`, `recomp_guest_load`) | 6% |
| Whole-game hand source (excluding the generated work it calls) | under 2% |

A cached-settings patch for the probe is prepared privately. It applies to
HEAD, but the file belongs to the split-rate work and was not changed here.
Passing the gate needs another 10-15% from the D3D8 draw path or the
presenter capture copy: the wholesale D3D8 replacement already listed as open
work, or a cheaper capture packet.

These are agent smoke runs. They show that the route reaches the match and
that game-time timings agree; they are not a user play test. Captures,
traces and logs remain private.

## M3: 120 Hz throughput and smoothness (2026-10-05)

A flat instruction sampler (thread suspension plus a linker map, so no
elevation or PDB was needed) found 54% of the 120 Hz title game thread inside
two `getenv` calls. `d3d_draw_adapter.c` evaluated
`RECOMP_POSE_EXPERIMENT_FRAME` and `RECOMP_POSE_STATE_CAPTURE_AT` for every
draw without a vertex program; the second call arrived with the split-rate
pose work. Each call scans the process environment, and the title world pass
issues thousands of such draws. The adapter now reads both variables once.
Nothing sets them during a run. This also explains the earlier comparison in
which concurrent renderer work doubled world-render CPU time.

All results below come from one executable with scale 4.5, MSAA 8, SMAA,
vsync, Start pulses at polls 6 and 8 and live host input disabled.

| Measurement | Before | After |
| --- | --- | --- |
| 120 s real-paced run, mean / minimum per-second rate | 120.8 / 70 fps | 120.0 / 113 fps (run B), 120.0 / 101 fps (run C) |
| Seconds below 118 fps | 67 of 126 | 2 of 124 (B), 5 of 124 (C) |
| Present 6000 / 10800 | 56.92 s / 90.00 s | 49.998 s / 89.995 s (B), 49.997 s / 89.995 s (C) |

The 60 Hz control on the same executable reached Swaps 3000 and 5400 at
50.171 and 90.182 seconds. Both 120 Hz runs are therefore within -0.35% and
-0.21% of equal game time; against the M1 reference they are -0.55% and
-0.32%. M1 timing holds on the current tree.

The remaining dips occur at the same two attract scenes in both runs, near
presents 6700-6900 and 9600-9700. Frame work there briefly exceeds 8.3 ms;
the retained-deadline pacer then catches up, so game time is unaffected.

Three 40-present bursts at presents 1000, 4000 and 9000 decide smoothness. A
moving frame differs from the present two earlier; a new state differs from
the immediately preceding present. All 57 moving odd presents and all 57
moving even presents showed new state (100%).

The 240 Hz timing run (no pass required) averaged 206.5 fps and never fell
below 119 fps. It reached presents 12000 and 21600 at 57.94 and 105.22
seconds: +15.5% and +16.7% of the 60 Hz game time, versus 109.8 and 169.2
seconds before the fix. The game thread remains the limit at 240 Hz.

These are agent smoke measurements; captures and logs remain private.

## M2 passed: equal-game-time visual parity (2026-10-05)

At equal game time, 60 Hz Swap N against 120 Hz present 2N, poses and
lighting match at all three checkpoints. Every capture used the same
executable, the fixed startup seed and no live host input. The image runs
used accelerated guest pacing because each frame is deterministic per
present; timing gates use real pacing.

| Checkpoint | Pixels differing by more than 8 of 255 | Where |
| --- | ---: | --- |
| Swap 300 / present 600 | 0.00% | None. |
| Swap 3000 / present 6000 | 0.014% (1,142 pixels, max 75) | Sub-pixel outline shimmer on deforming cloth; shapes, pose and lighting identical. |
| Swap 5400 / present 10800 | 0.56% (max 57) | 46,525 of 46,844 pixels lie in the waterfall; 319 elsewhere. |

The 60 Hz hand path stayed byte-identical to generated dispatch at Swap 5400.

Two changes closed the earlier pose difference:

- The secondary cluster (chain solver `0x00057300`, its owner `0x00056E90`
  and the morph owner `0x00064220`) now advances once per authored tick at
  every rate. Its generated helpers use per-tick constants, so stepping only
  the particle law fractionally desynchronized the cluster. This cut the
  3000/6000 difference from 1.05% to 0.01% of pixels.
- Held scenery substeps already set the game pause flag. A consumer under the
  jungle scenery draw calls `rand()` without checking that flag, so held
  draws now restore the CRT `rand()` state. A test covers the restore.

Remaining differences and their owners:

- **Waterfall splash timing.** The splash consumer `0x00073AF0`, dispatched
  under jungle scenery effects (`0x00154A90`, `0x00075000`), spawns when a
  particle crosses terrain height. From authored tick 1894 its `rand()` draw
  lands one tick later at 120 Hz than at 60 Hz. The traces of the earlier
  builds show the same first divergence, so it predates this change. Its
  gating particle state is the open owner.
- **Cloth outline shimmer.** Float rounding in the deformation and
  fractional pose path moves some edge pixels; no visible pose change.

A rejected variant put every owner on one shared pass phase, decrementing all
fiber sleeps and deriving input edges only on one pass per tick. It moved the
title Start edge by one present and the jungle transition by one tick; the
3000/6000 difference rose to 11.8-17%, and it did not move the splash
divergence. Each owner keeps its own tick phase.

The attract path is converted. Match systems from the inventory below (ball
flight and contacts, AI cadence, UI fades and feedback durations) are not yet
exercised and belong to the M4 gate. These are agent smoke captures, not play
testing.
## M1 passed: parameterized fixed game clock (2026-10-05)

`RECOMP_GAME_HZ=60`, `120`, or `240` selects the hand scheduler, scene/frame
owners and input derivation. Guest pacing follows that setting unless the
separate pacing-probe override is explicitly set. Converted sleep, script and
input-repeat models receive the number of substeps per authored 60 Hz tick;
continuous advancement uses the reciprocal tick length. There is no fixed
half-step assumption. The default, unset path remains unchanged.

M1's two consecutive 120 Hz runs passed both timing gates on one executable:

| Run | 3000 / 6000 submissions, seconds | Difference | 5400 / 10800 submissions, seconds | Difference |
| --- | ---: | ---: | ---: | ---: |
| 60 Hz control | 50.271355 | reference | 90.281018 | reference |
| 120 Hz A | 49.995329 | -0.549% | 89.994577 | -0.317% |
| 120 Hz B | 49.995219 | -0.549% | 89.994466 | -0.317% |

The control's Swap 5400 image was byte-identical to the preceding generated
control. All three runs closed normally. Timing is the existing monotonic
vblank-arrival probe, before its wait and render-worker submission; these are
not physical scanout timestamps. Live host input was disabled, with only the
scripted Start pulses at polls 6 and 8. Each timing run captured only its final
checkpoint, avoiding a mid-run BMP readback stall. Settings were scale 4.5,
MSAA 8, SMAA enabled and vsync. No tests or builds overlapped the gate runs.

The earlier drift was processing lateness followed by discarded deadlines.
In the failing profile, actor-frame work in slot 11 grew from about 1.1 seconds
per 300 passes to 3.55 seconds, while input, scripts and camera work remained
small. Render queue waits were near zero. Decomposing the complete actor draw
owner `0x000B4450` localized the work to environment drawing, actor traversal
and list submission; fence waits were absent. The existing pacer clamped every
late deadline to the current time, permanently losing that elapsed interval.

The opt-in high-rate game clock now retains scheduled deadlines after a late
frame. It still executes and submits every tick; subsequent faster work can
recover the elapsed-time deficit. The unchanged default presentation clock
continues to discard lateness. A diagnostic run reached present 7500 late at
63.850339 seconds, recovered by 8400 at 69.994239 seconds, and reached 10800 at
89.996351 seconds. This is timing parity, **not uniform pacing or M3 acceptance**;
processing overruns and catch-up bursts still require the later smoothness gate.

The new hand actor draw owner preserves its inherited EBP at generated call
boundaries. Generated D3D fence seams at `0x001E3870` and `0x001E39E0` remain,
along with the documented SetTransform seam; wholesale library replacement
remains open. Optional `RECOMP_GAME_TIMING_TRACE=1` reports main-loop, fiber and
actor-draw costs for further investigation.

All 19 CTest cases passed. The tools suite passed 21 tests with one skip,
including authentication failure checks. Pure pacing tests cover transient
late frames and retained/default deadline policies at all three rates; sleep
and input-repeat duration tests also cover 60, 120 and 240 Hz. Raw profiles,
paired receipts and captures remain private.

M2 is next: poses and lighting still differ. This experimental mode has passed
M1 only; it is not gameplay acceptance. The requested extra 240 Hz timing run
will occur after M2 passes. The older sections below record preceding attempts.

## M2 investigation (2026-10-05, superseded by the result above)

M2 has not passed. The hand camera and lighting owners now reproduce the
60 Hz reference images byte for byte at swaps 300, 3000 and 5400. An earlier
lighting regression came from misidentified transition modes: mode 3 is idle,
4 copies targets, 5 interpolates and 6 disables a record. Comparing each native
record update against the generated owner located the first mismatch before
changing the mode dispatch. That diagnostic oracle was then removed.

Input acquisition now samples the first loop pass and then once per authored
tick. Edge/repeat derivation remains per substep. Delaying the first sample had
exposed uninitialized guest input during the startup movie. The startup movie's
scene age now uses the configured steps per tick. Initialization and display
changes issue additional Swaps outside the main loop; their presentation count
now scales with the configured rate, preserving their authored duration.

The title orbit, camera smoothing limits, lighting transition endpoints,
world UV scroll and decorative angular rates have parameterized hand owners.
The camera's approximate spherical conversion also loses radius each call;
scaling that round-trip loss reduced the title checkpoint's radius error from
0.657 to 0.003 units. This residual has not passed the visual gate.

Later attract captures identify a separate scripted-camera clock. At swap
3000 its script age is 97; the unconverted 120 Hz script reports 195 at present
6000. At the later checkpoint the ages are 67 and 135. The corresponding image
difference includes a changed camera viewpoint. Script, curve and command-queue
owners are being converted together. A scoped authored clock for the existing
actor update removed half-speed animation, but one checkpoint still has a
one-tick actor phase offset. Full fractional actor advancement remains under
test; secondary physics and animation-event cadence are not accepted.

A private comparison build using the M1 renderer and the same current game
sources reached presents 6000 and 10800 in 49.996424 and 89.997468 seconds.
A separate 600-present profile measured 5.000331 seconds with that renderer
versus 9.981532 seconds with the concurrent renderer work in the shared tree;
world-render CPU time accounted for most of the difference. These comparisons
isolate game-clock diagnosis; they do not establish M1 or M3 for a later shared
binary. Live host input was disabled. Captures, state dumps and logs stay private.

The new game owners retain generated game-math and command-parser neighbors.
The world path also retains its existing generated drawing seams; whole library
replacement remains open. Native owners publish their guest frame before each
such dispatch. No generated C is edited. M2, the requested 240 Hz timing check,
M3 and M4 remain gated; this work is smoke evidence, not play-test acceptance.

## Input edge/repeat owner (2026-10-05)

The opt-in hand scheduler now dispatches the complete input edge/repeat owner
`0x000B5870` to [game_input.c](../../../recomp-runtime/game_input.c).
Polling remains in the existing XAPI input path; button mapping, pressure
thresholds, stick deadzones, per-port edges, aggregate edges and disconnect
clearing retain the guest layout. New presses and releases are derived on each
poll. Repeat delay/reload is 20/5 polls at 60 Hz; the separable model uses 40/10
at 120 Hz, preserving elapsed repeat duration. This does not enable the rejected
whole-game 120 Hz mode.

Public tests cover a one-poll press and release, held-button repeat timing at
both rates, changed-held-mask cancellation, lower-word repeat masks, analog
threshold/deadzone boundaries, aggregate delivery, disconnect clearing and
callee stack cleanup. A deterministic natural attract smoke run with hand input,
scheduler and scene owners produced a byte-identical image to generated at
Swap 3000 and closed normally. Live host input was disabled as described below.
All 17 CTest cases and the tools suite (21 tests, one skip) passed. The public-only
build also succeeds without private inputs.

## Scene/render continuation (2026-10-05)

`RECOMP_GAME_SCENE=1` selects readable hand source for the scene fiber
`0x000A37F0`, scenario fiber/tick `0x0006ADB0` / `0x0006B030`, world-frame
fiber `0x00079D00`, and actor-frame fiber `0x000B42B0`. Use it with
`RECOMP_GAME_SCHEDULER=1` for the retained 60 Hz baseline. Default dispatch is
unchanged. These owners still call generated game neighbours. The world frame
also retains the generated D3D SetTransform seam at `0x001E36D0`; wholesale
library replacement remains open.

The rejected half-sleep experiment has two independently observed causes:

- The main loop clears and swaps every pass, but one-tick fiber sleeps also
  delimit draw production. The title world uses slot 10 (`0x00079D00`); the
  attract scene uses slot 11 (`0x000B42B0`). Slot 13 (`0x0008A970`) supplies UI.
  A bounded draw-count trace found 456 draws in the attract actor-frame slot and
  zero in the title-world slot. Leaving slot 11 asleep produced alternating
  full-scene and black-with-logo images even after title rendering was fixed.
- Input polling (`0x000B57D0`) and edge derivation (`0x000B5870`) run before
  the scheduler on every pass. The scene copies them through `0x000A5030`.
  Its missed pass allows the next poll to replace a one-poll Start edge.

The smallest private cadence experiment kept scene input delivery and frame
owners active on every pass while integer script time advanced every other
pass. Start then reached the title naturally at Swap 73, versus Swap 43 at
60 Hz. All title captures at Swaps 600-603 were filled. Adding the actor-frame
owner removed the alternating missing scene at Swaps 6000-6003: all four
captures contained the scene. This establishes the missing-draw mechanism; it
**does not establish matching animation, camera, lighting or game speed**.

Two translation defects were found before retaining the hand source. The world
camera helper `0x00023B10` takes its camera pointer in EAX as well as the
caller's ESI; omitting EAX let it overwrite scheduler storage. Both failed
receipts stopped at the first world frame with the same corrupted slot address.
The source now supplies the implicit argument, and a public regression test
starts with a stale EAX and checks argument/order/stack/view preservation.
The scene normal-speed branch tests equality with 1.0, rather than greater-than.
Both corrections follow the authenticated generated owner and bounded static
analysis. No generated C was edited.

With a fixed startup seed, hand and generated scene images were byte-identical
at Swaps 300-303, 3000 and 5400. The first four comparisons were from a restored build
that reported SMAA unavailable, so they are image-parity checks only. After
restoring the pinned SMAA submodule, the Swap 3000 comparison again matched
byte-for-byte with scale 4.5, MSAA 8, SMAA enabled and vsync. Generated reached
that checkpoint in **50.497954 s**, hand in **50.299145 s** (-0.394%). The
baseline reported the existing Clear/presenter failure after requested close;
the hand run closed normally. The manual lookup rejects unrelated addresses
before checking its opt-in environment setting; doing that check on every
manual dispatch had reduced the first restored build to roughly 47-56 FPS.
The corrected build sustained about 120 FPS in the sampled title interval.

A private 120 Hz candidate reached physical Swap 6000 in **51.063780 s**, versus
50.497954 s for baseline Swap 3000 (+1.121%). Its visual checkpoint failed.
The later actor-frame fix removed the alternating missing scene, but its
images still had different poses and lighting. Its timing run overlapped
CTest and is excluded from timing acceptance. Physical swaps are not relabeled:
at 120 presentations/second, the old 3000/5400 game-time checkpoints correspond
to approximately 6000/10800 presentations, with startup/transition offsets
requiring separate alignment.

The later isolated pair reached physical Swap 5400 at 60 Hz in **90.491179 s**
and physical Swap 10800 at 120 Hz in **93.726659 s** (+3.576%). In that pair the
3000/6000 checkpoints were 50.474500 s and 51.111271 s (+1.262%). Four consecutive
120 Hz images at 10800-10803 contained the scene, but did not match the 60 Hz
image. Both runs closed normally. Thus neither requested timing tolerance nor
equal-game-time image parity passed. These longer runs were specifically for
the requested second checkpoint; routine iterations remain below one minute.

The incomplete 120 Hz activation is retained only as private experimental
source. Public hand source remains a 60 Hz baseline; `RECOMP_GAME_HZ` is not an
enabled runtime mode. A global half multiplier cannot normalize an actor fiber
that still resumes at 60 Hz, nor integer ages inside render/environment/camera
owners that now execute at 120 Hz. These dependencies must be converted as
coherent owners before the image/timing milestone can pass. Animation and
skeleton work owned by the split-rate effort was not changed here.

During this work the earlier private build/evidence directories disappeared
externally. Their recorded observations above survive, but the deleted raw
receipts cannot be rechecked. Inputs were recovered into a new private directory
and verified against the same XBE SHA below and the current authenticated recipe;
all results in this section came from that restored build.
All 17 CTest cases and 21 tools tests passed (one tools test skipped); public
export verification passed. The tools tests retain the authenticated SHA and
manifest rejection checks. No private inputs,
asset names, captures or logs are public. These are agent smoke observations.

The remaining whole-game estimate stays **7-14 engineer-weeks**. The scheduler
and scene/frame ownership is clearer, but the requested equal-time visual gate
still fails; this does not justify reducing the conversion estimate.

## Deterministic 60 Hz comparison (2026-10-05 follow-up)

The earlier small title-image difference was not a hand-scheduler regression.
Two generated runs, with no live controller source, showed the same variation.
Initialization at `0x0017A7F0` calls the time wrapper `0x001BB36A` and passes its
Unix-seconds result to the RNG seed entry `0x001BA3E0`. The time wrapper obtains
real system time through kernel ordinal 128. This changes ambient animation
between launches. Bounded Ghidra queries used the same binary identity recorded
below; the differing pixels were concentrated on trees, boats and shadows.

In consecutive captures from Swap 35 through 74, the first seed-dependent title
image was **Swap 48**. Swaps 35-40 can still contain the fading opening movie,
which has a separate wall clock. Those media frames are not equal-time images
merely because they have the same Swap number.

`RECOMP_GAME_TEST_CLOCK=1` now fixes only the first system-time read inside game
initialization to Unix second 946684800, for deterministic paired captures.
Subsequent time reads remain live. The fixture works with either generated entry
or `RECOMP_GAME_SCHEDULER=1`; it changes no default behavior, scene state, input
state, scheduler duration or performance-counter clock. The clock is a test
input, not a gameplay-speed fix. A public test checks its one-read scope, manual
lookup and guest stack cleanup.

With that common seed, generated and hand images were byte-identical at every
captured Swap **41-74**, **300-303**, and **3000-3001**. Arrival at Swap 3000 was
50.753193 seconds generated versus 50.610638 seconds hand, a -0.281% difference.
Both reached the same natural attract image. The generated run reported a
Clear/presenter error after the requested window close; the hand run closed
normally. These are smoke comparisons, not gameplay acceptance.

The deterministic harness retains only the two Start pulses at polls 6 and 8
and **omits `--input-host-after-poll`**. `runner.cpp` then keeps the scripted
source's base callback null, so a connected controller cannot inject input.
No live button source is used after the initial pulses. Graphics remain scale
4.5, 8x MSAA, SMAA and vsync. All 15 CTest cases passed; the current shared tools
suite passed 20 tests with one skip. Raw captures and comparisons remain private.

## Hand-source milestone attempt (2026-10-05)

The first conversion attempt did **not** establish speed-correct 120 Hz. The
retained change is an opt-in hand-written game entry, main loop and scheduler in
[game_scheduler.c](../../../recomp-runtime/game_scheduler.c). Set
`RECOMP_GAME_SCHEDULER=1` to exercise them at the existing 60 Hz game cadence.
Unset leaves generated dispatch unchanged. There is no retained `RECOMP_GAME_HZ`
mode. The existing `RECOMP_GUEST_VBLANK_HZ=120` remains a pacing-only probe and
still accelerates unconverted game work.

The complete owners replaced are `0x000BB290`, `0x000BB310` and `0x000B5680`.
The startup adapter already dispatches the first through manual lookup. Replacing
that entry and loop is necessary: the generated loop calls the scheduler
directly, so registering the scheduler alone would never intercept that call.
The hand loop preserves input, feedback, audio, clear/setup, scheduler, post-frame
work, Swap and movie-service ordering. The scheduler preserves all 17 slots,
integer sleeps, suspension, completion/deletion and alternating timestamp banks.
It keeps guest layouts intact and calls existing neighbours through dispatch.

The main loop retains the pre-existing generated D3D vblank/field-status calls
(`0x001E38F0`, `0x001E4830`) and movie-service calls (`0x0018A160`,
`0x0018A560`, `0x00198190`, `0x00197EC0`). These are library seams, not new
library decompilations; wholesale D3D/media replacement remains open. The
existing manual Clear, audio and fiber adapters still own their corresponding
library entries.

### Bounded smoke results

All new runs used scale 4.5, 8x MSAA, SMAA, vsync, Start pulses at polls 6 and 8,
and host input after poll 10. Only one runner was started at a time. Times below
are the in-process vblank-probe arrival times, before the wait and capture.

| Run | Checkpoint | Elapsed seconds | Scope of observation |
| --- | --- | ---: | --- |
| Generated baseline, 60 Hz pacing | Swap 300 | 5.352691 | Title scene; normal close. |
| Hand entry/loop/scheduler, 60 Hz pacing | Swap 300 | 5.396116 | Same scene transition sequence at Swaps 1/2/10/43; normal close. Time difference +0.811%. |
| Rejected half-sleep scheduler, 120 Hz pacing | Swap 600 | 12.399183 | Still in the opening movie; missed the scripted Start input. Swaps 600/602 were entirely black, 601/603 contained movie frames. |

The paired 60 Hz captures at Swaps 300-303 showed the title scene but were not
byte-identical. At Swap 300, the mean absolute RGB differences were
0.577/0.526/0.487 on a 0-255 scale. At that point the source had not been
isolated; the deterministic follow-up above identifies the seed difference. A later unchanged-path repeat
and final hand-loop check were stopped at Swaps 278 and 118 after host-input
traces confirmed live controller button presses. No further runs were attempted
without a quiet input interval. The rejected run reported a
Clear/presenter error during requested shutdown after its four captures.

Two earlier runs reached Swap 3000 at 50.679696 seconds (60 Hz pacing) and
30.189443 seconds (120 Hz pacing), but are invalid attract comparisons. Both
left the title/attract flow for menus following extra A-button edges, first at
Swap 1718 and Swap 836 respectively. Both had enabled live host input and
reported a connected controller. Subsequent checks enabled the existing input
trace and stopped on the first unscripted edge rather than retrying the same
invalid gate. No new valid attract measurement at 3000 or 5400 was obtained.
The earlier research measurements in this document used different graphics/input
settings and must not be presented as results of this conversion.

### Why half-sleeps were rejected

A tested fractional sleep model preserves integer guest counts while consuming
half a tick per 120 Hz pass. Synthetic durations of 1, 2, 5, 20, 30, 3000 and
5400 ticks expire at exactly the same simulated time as at 60 Hz. Tests also
exercise runnable, sleeping, suspended and inactive slots, completed fibers with
and without handles, timestamp order/bank selection and stack/register balance.
These are model tests; 3000/5400 in that test are not observed attract events.

However, one-tick fiber sleeps are also the frame-production boundary. Sleeping
those fibers on alternate main-loop passes leaves the unconditional Clear/Swap
without their draws. Input edges are derived every pass and can disappear before
a sleeping scene fiber consumes them. The rejected run demonstrates both hazards.
The runtime 120 Hz activation was removed; the half-tick model and synthetic
tests remain as groundwork. No generated source or authenticated recipe changed.

The next coherent owner cluster must separate scene/script advancement from
per-pass frame production and define how input edges survive that separation.
Changing only the scene counter or shared multiplier cannot repair blank frames
or recover lost input. Reusing the previous frame would not supply the requested
additional visual samples. Camera, animation advance/root motion, physics,
UI fades, input repeat, feedback and media timing are still unconverted in this
whole-game work. The independent split-rate work is outside this change.

The 7-14 engineer-week planning range remains; this attempt does not justify a
reduction. Revise the first milestone to include the main-loop, scene/frame
production and input-delivery owners together, rather than treating the scheduler
as an independently shippable 120 Hz slice. Pixel parity at 60 Hz, clean natural
attract checkpoints and sustained 120 Hz throughput at the requested settings
remain gates before expanding the conversion.

Validation: all 15 CTest cases in the shared working tree passed (including the
independent animation test), and the tools suite passed 19 tests with one skip.
Wrong function SHA and generated-manifest configurations still fail. Public-only
configuration/build needs no private input. Raw queries, rejected source,
measurements and images remain private. This is smoke/model evidence and a
hand-source foundation, not gameplay progress or a supported 120 Hz mode.

## Finding

The attract/demo and game scheduler are frame-driven. Changing only guest Swap
pacing from 60 to 120 Hz almost halves elapsed time to the same visual state.
There are per-update floating-point speed multipliers, but no measured wall-time
delta in the inspected game scheduler. Media and host audio have separate clocks.
A whole-game conversion requires more than changing vblank or halving one scalar.

Prefer the [split-rate design](high-frame-rate-split.md): retain authoritative
simulation at 60 Hz and evaluate/rasterize additional visual poses and cameras.
That is real rendering from scene state; it does not require image interpolation
or frame generation. The split itself still needs hand-written, separable models.

## Scope and identity

Bounded Ghidra queries were compared with the authenticated local generated
program and hand-written runtime. Ghidra image SHA-256:
`053d44e885fa33c1d15d909a533f39dfbd976e97eeaf67e4fdef8438ea7e5c54`,
image base `0x00010000`. Generated names below are `sub_` plus the uppercase
address. Ghidra functions can span several generated fragments; an entry name is
an investigation anchor, not permission to replace only its first fragment.
Ghidra lacked function definitions for some scene entries; their generated
bodies and the bounded scene-table query supplied those parts of the trace.

This inventory identifies concrete dependencies, not every update in the game.
The attract and opening-movie runs are smoke evidence. Volleyball outcomes,
controller feel, all cutscenes, audio synchronization and long-session behavior
have not been accepted by a user play test.

## Probe and measurement

[Guest pacing](../../../recomp-runtime/d3d_vblank.cpp) accepts
`RECOMP_GUEST_VBLANK_HZ=60` or `120`. It reads the setting at vblank reset and
reports elapsed monotonic seconds before wait 1 and each 300th wait. Unset means
the existing 60 Hz behavior without probe output. Invalid values report a
warning and use 60 Hz. The probe changes pacing only: no generated source,
simulation multiplier, kernel counter, audio rate or movie clock is modified.

One wait occurs in each Swap adapter invocation. Logging precedes that wait and
render-worker submission, so it measures the arrival at a Swap, not scanout.
Late frames discard timing debt as before. The runner's default immediate host
presentation avoids an additional monitor-vsync limit; `--vsync` can impose one.
A 120 Hz pacing request does not promise 120 FPS under load.

The paired runs used the same executable and inputs, native-size output, 8x MSAA,
SMAA off, normal audio gain, and Start pulses at input polls 6 and 8. Those flags
count polls, **not seconds**. No scene or gameplay state was forced.

| Checkpoint | 60 Hz: seconds from vblank reset | 120 Hz: seconds from vblank reset | Observation |
| --- | ---: | ---: | --- |
| Swap 3000 | 50.133 | 25.300 | Sunset demo image; both captured exactly at present 3000, byte-identical images. |
| Swap 5400 | 90.152 | 45.354 | Jungle scene confirmed in nearby captures at presents 5406 and 5404 respectively. |
| Swaps 3000 to 5400 | 40.018 | 20.053 | Same 2400 update/present interval takes approximately half the wall time. |

An external stderr observer placed the first checkpoint at approximately 50.65
and 25.83 seconds from process launch, and the second at 90.72 and 45.87 seconds.
Those boot-relative numbers include startup and log-delivery latency; use the
in-process durations for the pacing comparison. The same scene transition
occurred at Swap 1848 in both runs. Sampling an already-visible scene is not a
measurement of its exact first appearance.

Both runs reached the requested checkpoints and closed normally after about
5700 Swaps. This establishes accelerated attract progression and frame delivery,
not correctness of gameplay at 120 Hz.

## Actual timing path

1. `0x000BB290` enters the perpetual main loop at `0x000BB310`.
   `sub_000BB310` polls input through `0x000B57D0`, derives input edges/repeat
   through `0x000B5870`, services feedback through `0x000B5AA0`, services audio,
   clears/setup-renders, runs the fiber scheduler at `0x000B5680`, performs
   additional work at `0x000BAB70`, then calls `0x00179A90` to Swap.
2. `sub_000B5680` walks 17 fiber slots at `0x005F2C38`, stride 32. It decrements
   each runnable slot's integer sleep count once per main-loop pass and resumes
   the fiber when that count reaches zero. QPC reads bracket execution and write
   alternating timestamp storage; they do not determine the sleep decrement.
3. The scene fiber is `sub_000A37F0`, with its repeating point at `0x000A3820`.
   It yields to the scheduler, advances a scene frame counter at `0x004D967C`,
   and dispatches enter/update/exit callbacks through the table at `0x002F5ED0`.
   `0x000B5570` is the generic fiber callback trampoline, not a physics loop.
4. `sub_00179A90` calls `D3DDevice_Swap` at `0x001E8F30`. The manual
   [Swap adapter](../../../recomp-runtime/d3d_frame_adapter.c) updates the frame
   model, increments synthetic `KeTickCount`, waits in `recomp_d3d_wait_vblank`,
   and submits to the D3D11 render worker. Other Swap callers are initialization
   at `0x00179890` and display-reset helpers at `0x00179E70`/`0x0017A200`.
   Those exceptional Swaps are not necessarily gameplay updates.
5. Movie-active main-loop branches and display resets also call the original
   vertical-blank entry. In this runtime, `KeWaitForSingleObject` and the NT wait
   bridges return immediately. They do not supply a second timed scheduler.
   The operative guest pacing for this experiment is the manual Swap seam.

Clock domains must remain explicit:

| Clock | Current implementation | Effect of this probe |
| --- | --- | --- |
| Guest update/sleep counts | One scheduler pass per ordinary frame | Approximately twice as many passes per wall second. |
| `KeTickCount`, synthetic export `0x00740040` | Adds 16 milliseconds per Swap in the frame adapter | Approximately doubles; even the 60 Hz baseline is 16 rather than 16.667 ms per Swap. This is a runtime approximation, not a claim about Xbox hardware. |
| XAPI QPC `0x001830CF`, frequency `0x001830E0` | [Time adapter](../../../recomp-runtime/xapi_time_adapter.cpp): steady-clock nanoseconds, frequency 1 billion | Wall time remains unchanged. |
| DirectSound/output clock | [Service adapter](../../../recomp-runtime/dsound_service_adapter.c): host milliseconds and a separate 5 ms output-pump wakeup | Playback does not automatically run twice as fast. |
| Host Present | Render-worker DXGI presentation, optionally monitor-vsync-limited | Separate from simulation time; can constrain achieved throughput. |

There is no separate `KeQueryPerformanceCounter` implementation in the inspected
kernel dispatch. The observed title counter path is the XAPI adapter above.
The bounded direct-xref query found its game caller in the fiber scheduler;
other counter consumers lie in D3D/media code. This is not proof that every
indirect timer consumer has been found.

## Conversion inventory

The conversion column describes the remaining 120 Hz work. The opt-in
60 Hz scheduler replacement above does not complete any 120 Hz row.
For the animation and camera clusters, see the independent bounded analysis in
the [split-rate assessment](high-frame-rate-split.md).

| System | Guest address / generated entry | Frame dependence and 120 Hz work |
| --- | --- | --- |
| Fiber sleeps and scene timers | `0x000B5680` / `sub_000B5680`; `0x000A37F0` / `sub_000A37F0` | Integer sleep decrements and scene-counter increments happen once per pass. Use explicit elapsed units or fractional accumulators; simply halving integer decrements truncates to zero. Preserve pause and callback ordering. |
| Shared speed modes | `0x000A4680` / `sub_000A4680`; `0x000A53C0` / `sub_000A53C0`; scalar `0x004D6FC4` | Initialization sets the scalar to 1; speed modes and the scene loop write it again. It is a per-update multiplier, not measured seconds. A normalized half-step must compose with slow motion and all writers, not patch the value once. |
| Match update orchestration | `0x000BB5F0` / `sub_000BB5F0` and continuations | Walks active actors, updates state, runs pose-related work and the ball update. A speed ramp also increments an integer age and adds a per-call ramp amount. Audit ordering and ramp duration, not just the ball's final position. |
| Ball/volleyball simulation | `0x0001B9E0` / `sub_0001B9E0`; `0x0001BB10` / `sub_0001BB10`; `0x0001FF00` / `sub_0001FF00` | Saves position history, advances flight age by the shared scalar, uses trajectory interpolation/analytic flight and per-step integration. In one bounce path position is scalar-scaled but gravity subtraction and horizontal damping are per call. Convert each law and age domain together; halving the scalar alone leaves excessive gravity/damping. |
| Ball contacts and stopping | `0x0001D570`, `0x0001D5E0`, `0x0001E350` / corresponding `sub_` entries | Ground-height contact checks, rebound/position correction, age reset and low-speed stop thresholds are discrete. Twice as many checks can change contact ordering and rally results even after continuous motion is retimed. Requires trajectory and natural rally tests. |
| AI/decision-script candidate | `0x000146D0` / `sub_000146D0` and continuations; `0x00018A40` / `sub_00018A40` | Active-actor decision scripts run until their yield condition; the match-state age uses the shared scalar. Human/automatic branches and pause guards are visible, but every opcode and random consumer is not mapped. Separate decision cadence from motion; doubling decisions may change behavior and random consumption. |
| Animation and root motion | `0x000ADFB0`, `0x000AE240`, `0x000AF330`, `0x000B01E0` / corresponding `sub_` entries | Clip cursor advance multiplies layer/actor speeds by the shared scalar and adds an epsilon; fractional track evaluation exists. Use half-step clip time, preserve endpoint/event crossing and root displacement. Re-evaluating a pose must not apply root motion or cues twice. |
| Secondary physics | `0x00056E90`, `0x00057300` / corresponding `sub_` entries | Stateful prior displacement, damping, forcing and constraints. Re-derive integration for the smaller step or retain a 60 Hz state update. Scaling only primary animation does not retime this cluster. Anatomical ownership of each group remains unverified. |
| Camera smoothing and effects | `0x0002EF20`, `0x000301C0`, `0x00030800` / corresponding `sub_` entries | Per-invocation smoothing, effect ages, phase/sample-pointer advance and random effects. Continuous retiming needs the actual mode-specific laws; doubling the existing update is unsafe. Matrix builders alone do not change camera time. |
| UI/menu transitions and fades | `0x000F27C0` / `sub_000F27C0`; `0x000F38D0` / `sub_000F38D0` | Transition age increments each update; a default limit is 30. Fade age advances toward 30; auxiliary counters wrap at 360. Convert elapsed durations or update these at 60 Hz. Integer thresholds, easing inputs and callbacks must agree. |
| Attract/cutscene control | `0x000A37F0`, `0x000A4B80`, `0x000A4D00`; channel-script helper `0x000B3190` | Scene callbacks and stateful animation commands share frame-driven execution. Paired captures prove attract acceleration. Preserve waits, cuts, animation-event crossings and scene transitions; full cutscene opcode/event inventory remains open. |
| Audio cues and media service | `0x000BB310` calls service per loop; `0x00191080`, `0x001929C0`, `0x001A8260`, `0x001A82B0` are media counter-consumer anchors | Cue requests can arrive earlier while samples remain on wall time. Retain authored cue timing and deduplicate animation events. Media frequency/counter wrappers already use measured time; do not blindly halve their values. Wholesale middleware replacement remains separate work. |
| Input repeat and vibration | `0x000B57D0`, `0x000B5870`, `0x000B5AA0` / corresponding `sub_` entries | Input polls/edge derivation occur per pass; repeat starts at 20 updates and reloads 5, feedback durations decrement per pass. Hand edge/repeat owner now preserves 20/5 at 60 Hz and models 40/10 at 120 Hz. Runtime remains 60 Hz; vibration duration and cross-rate consumers remain open. |

The existing scalar and fractional animation/trajectory sampling are useful
conversion seams. They do **not** establish a variable-wall-delta game loop.
For a normalized half-step, velocity integration needs the half-step in both
position and acceleration terms. Multiplicative damping needs an equivalent
half-step factor, not half its numeric coefficient. Collision impulses and
state-machine transitions need separate treatment.

## Audio and movie checks

The attract runs produced nonzero audio output with zero reported dropped
buffers, underruns and engine glitches at both rates. They submitted different
amounts of audio over their different wall durations; no recorded listening or
sample-accurate cue comparison was performed. These counters cannot establish
lip sync or correct timing relative to doubled animation.

Two additional 20-second smoke runs omitted Start pulses. Both advanced through
the opening movie and closed normally. Each submitted 1,216 audio buffers and
3,648,000 bytes, all nonzero, with zero dropped buffers, underruns or engine
glitches. Movie presentation throughput was not uniformly 60/120: decode/work
also limited it. Sparse captures showed progressing movie imagery, but were not
matched by media timestamp. The checks support a distinct wall-clock audio path;
they do not prove unchanged FMV duration or A/V synchronization. A timestamped
video/audio capture and a full movie completion gate remain necessary.

## Effort, risk and next gate

Whole-game 120 Hz is high risk. It changes scheduling, collision opportunities,
random consumption and script/event ordering across a still-generated program.
The scalar reduces some work, but the confirmed unscaled counters and physics
terms rule out a safe one-line conversion.

Planning estimate for one engineer familiar with the port: 1-2 weeks to complete
clock/owner mapping, 4-8 weeks for coherent hand-written conversion clusters,
and 2-4 weeks for natural gameplay and media regression gates. That is roughly
7-14 engineering weeks, with substantial uncertainty and possible expansion to
several months. It is not an implementation commitment or a measured code count.
Every cluster must preserve guest layouts until all its owners are replaced.
Never modify generated C; use hand source and `recomp_lookup_manual()`.

A useful next whole-game experiment is a narrow, hand-written ball/time model
that compares 60 Hz trajectories against two 120 Hz half-steps, including bounces,
stops and pause/slow-motion transitions. Passing that would cover the model,
not authorize a whole-game switch. For the requested feature, pursue the split
visual evaluation first, then judge actual 120 FPS throughput and gameplay at
60 Hz independently.

The probe build passed all 14 CTest cases and the tools unittest suite (19 tests,
one skipped). Negative configuration checks still reject a wrong function SHA
and a wrong generated-program manifest. Raw queries, captures, measurements and
build/run logs stay private. The probe and this research do not claim gameplay
progress or release acceptance.
