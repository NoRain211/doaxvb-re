# Runtime options

The runner reads its settings from command-line flags and `RECOMP_*`
environment variables at startup. This page lists them as the code on `main`
reads them. Feature pages linked below explain behavior in more detail.

`RunGame.cmd` passes `--xbe` and `--vsync`, sets `RECOMP_USER_MUSIC` to
`private/UserMusic`, and defaults `RECOMP_AUDIO_GAIN` and
`RECOMP_PERF_COUNTER` to 1 unless they are already set. The launcher sets
`RECOMP_D3D_SCALE`, `RECOMP_D3D_MSAA`, `RECOMP_D3D_SMAA`,
`RECOMP_AUDIO_GAIN` and `RECOMP_MUSIC_SHUFFLE`. Set any other variable in
the shell before starting either one.

## Runner flags

| Flag | Effect |
|---|---|
| `--xbe <path>` | The user-owned XBE to run. Required. |
| `--vsync` | Pace presentation to the display. Use it for play and for split rate. |
| `--expect-stop <id>`, `--stop-at <id>` | Stop at a named diagnostic boundary and report whether it was the expected one. |
| `--milestone-log <path>` | Write milestone events to a file. |
| `--input-host-after-poll <poll>` | Turn on keyboard and controller input after that input poll. Scripted runs leave it out so attract demos play the same way. |
| `--input-start-pulse-at <poll>`, `--input-a-pulse-at <poll>` | Press Start or A once at that poll. |
| `--input-buttons-at <poll>:<hexmask>`, `--input-analog-at <poll>:<index>:<value>` | Scripted button and analog input. |
| `--input-wait-after-poll <poll>` with `--input-resume-file <path>` | Pause input at a poll until the file appears. |
| `--input-paused-analog-file <path>` | Read analog values from a file while paused. |

## Player settings

| Variable | Values | Effect |
|---|---|---|
| `RECOMP_AUDIO_GAIN` | 0 to 1 | Output volume. Unset means muted when the runner starts directly. |
| `RECOMP_MUSIC_SHUFFLE` | `1` | Shuffle Radio Station playlists. See [native audio](native-audio.md#radio-shuffle). |
| `RECOMP_USER_MUSIC` | folder | Custom soundtrack folder. Unset means `UserMusic` beside the XBE. See [custom soundtracks](custom-soundtracks.md). |
| `RECOMP_D3D_WIDESCREEN` | `0` | 4:3 in a 640x480 window. The default is 16:9 in 854x480. |
| `RECOMP_D3D_SCALE` | 1 to 8, fractions allowed | Render height multiplier, such as 3 for 1440p or 4.5 for 2160p. Above 1 the window is borderless and capped at the screen height. |
| `RECOMP_D3D_MSAA` | sample count | Uses the highest supported power of two up to this count. |
| `RECOMP_D3D_SMAA` | any value except `0` | SMAA 1x Ultra, if the `third_party/smaa` submodule was present at build time. |
| `RECOMP_D3D_FOG` | `0` | Turn off the game's fog, which is on by default. See [fog](fog.md). |
| `RECOMP_D3D_VRR` | `1` | Variable refresh presentation, if the system supports tearing; the log prints `vrr on` or `vrr unsupported`. The swap chain matches the window, and frames are presented on a 60 Hz grid. See [VRR pacing](wayfinder/research/vrr-frame-pacing.md). |
| `RECOMP_SPLIT_RATE` | `auto` or 60 to 1000 | Present between 60 Hz ticks at that rate; `auto` uses the display's refresh rate. Gameplay stays at 60 Hz, and the picture runs one tick behind. See [split rate](wayfinder/research/high-frame-rate-split.md). |
| `RECOMP_TEXTURES` | `1` or folder | Load replacement textures from `private/textures/replace`, or from the given folder. See [texture replacement](texture-replacement.md). |
| `RECOMP_TEXTURE_DUMP` | `1` | Also write each distinct original texture once. |
| `RECOMP_TEXTURE_DUMP_DIR` | folder | Root folder for texture dumps. The default is `private/textures`. |
| `RECOMP_PERF_COUNTER` | `1` | FPS in the window title and one-second performance samples in the log. |

Without VRR, presentation at 120 Hz or 240 Hz holds each 60 Hz frame for
exactly 2 or 4 refreshes. Other refresh rates use interval 1. The runner
rechecks the monitor's refresh rate once per second.

With texture replacement or dumping on, **F12** writes the textures used by
the current frame to `private/textures/frame-<present>/`. **Ctrl+F12**
reloads replacements.

## Diagnostics

These are for agent gates and investigations. They change logging or capture,
not gameplay, unless noted.

| Variable | Effect |
|---|---|
| `RECOMP_D3D_FOG_FACTOR=1` | Draw the fog factor instead of the fogged color. |
| `RECOMP_D3D_PRESENT_LOG=<file.csv>` | One row per present with sync interval, flags, DXGI frame statistics and QPC times before and after `Present`. Use it with PresentMon's `--qpc_time`. |
| `RECOMP_D3D_FRAME_DUMP=<path>` | Save backbuffer images. `_AT` takes a present number or comma list, `_COUNT` a burst length, `_INTERVAL_MS` a minimum spacing, `_TRIGGER` a file whose presence starts a burst, and `_DEFER=1` allocates the burst ahead of time. |
| `RECOMP_D3D_DRAW_CAPTURE=<path>` with `RECOMP_D3D_DRAW_CAPTURE_AT=<present>` | Record every draw's state in one frame. `_TRIGGER` starts a capture when its file appears, and `_FVF` filters by vertex format. |
| `RECOMP_D3D_PROGRAM_TEXTURE_DUMP=<prefix>` | With a draw capture trigger, save textures bound to vertex-program draws. |
| `RECOMP_TEXTURE_FRAME_AT=<present>` | Dump the current frame's textures once at that present. |
| `RECOMP_SPLIT_STRICT=1` | Stop on any unexpected split-rate state instead of falling back to the 60 Hz frame. |
| `RECOMP_SPLIT_TRACE=1` or `present` | Log per-tick camera and actor steps, or buffer one record per present. `RECOMP_SPLIT_TRACE_LIMIT` sets the buffered count; the default is 1200. |
| `RECOMP_SPLIT_FAILURE=<path>` | Write the inputs of a split-rate palette mismatch. |
| `RECOMP_ANIMATION_TRACKS=1` or `verify` | Use the hand-written animation track builders, or compare them with the generated ones. |
| `RECOMP_POSE_EXPERIMENT_FRAME`, `RECOMP_POSE_EXPERIMENT_ACTOR`, `RECOMP_POSE_CAPTURE`, `RECOMP_POSE_STATE_CAPTURE`, `RECOMP_POSE_STATE_CAPTURE_AT`, `RECOMP_POSE_VERTEX_CAPTURE`, `RECOMP_SKELETON_VERIFY`, `RECOMP_ANIMATION_DISPATCH_TRACE` | Pose and skeleton experiments and captures used to verify split rate. |
| `RECOMP_REPLAY_VERIFY_AT=<present>` | Compare 120 replayed frames with the game's frames from that present. |
| `RECOMP_INPUT_TRACE` | Log host input. |
| `RECOMP_WATCH=<hex address>` | Report writes to one guest address. `RECOMP_WATCH_HITS` sets the limit; the default is 40. |
| `RECOMP_FAULT_TRACE` | Add a host stack trace to fault reports. |
| `RECOMP_SAVE_SHORT_WRITE_AT`, `RECOMP_SAVE_INTERRUPT_AFTER_WRITE` | Fault the given save write to test recovery. **Changes save behavior.** |
| `RECOMP_XFORM_DUMP_AT=<present>` | Present at which the one-time transform report is written. The default is 550. |
| `RECOMP_D3D_SLOTTRACE`, `RECOMP_D3D_RTTRACE`, `RECOMP_D3D_VIEWSEL`, `RECOMP_D3D_VIEWARG`, `RECOMP_D3D_WHOCALLS`, `RECOMP_D3D_YTRACE` (with `_DRAW`) | Bounded D3D traces of transform slots, render targets, view selection, view arguments, frame-adapter callers and one draw per present. |
| `RECOMP_ACTPROBE`, `RECOMP_SPINPROBE`, `RECOMP_REPOST`, `RECOMP_FIBER_FPU` | Bounded probes for activity transitions, fiber spin waits and FPU state at fiber switches. `RECOMP_REPOST` re-posts a stalled load request. **It changes behavior.** |
