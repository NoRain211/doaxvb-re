# Whole-game 120 Hz timing assessment

Date: 2026-10-03. Research and an opt-in pacing probe; not a supported 120 Hz
mode or gameplay acceptance.

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
0.577/0.526/0.487 on a 0-255 scale. The source of that difference has not been
isolated; this is **not** image-parity acceptance. A later unchanged-path repeat
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
| Input repeat and vibration | `0x000B57D0`, `0x000B5870`, `0x000B5AA0` / corresponding `sub_` entries | Input polls/edge derivation occur per pass; repeat starts at 20 updates and reloads 5, feedback durations decrement per pass. Convert to durations or equivalent 40/10 counts at 120 Hz. Preserve single-consumption edges and analog pressure behavior. |

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
