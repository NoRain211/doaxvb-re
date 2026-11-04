# Rollback netcode

Status: in progress. The rollback core from
[doa2u-re](https://github.com/NoRain211/doa2u-re) (branch
`codex/rollback-netcode`) is ported: input prediction, the snapshot ring,
SyncTest, replays, the UDP transport, the matchmaker client and the shared
matchmaker server. Online sessions are not wired into the game yet. SyncTest
rewinds and replays live Exhibition rallies with matching state.

Game addresses below come from static analysis of the verified USA
executable in Ghidra unless a section says otherwise.

## The mode online play drives

Exhibition's setup screen (menu screen 2, handlers at `0x2A78F0`) lists One
Player, Two Players, the points of the match and Controller Settings.
`0x0F56E0` builds the list and adds Two Players only when two or more
controllers are connected (`0x5F3080`, a bit per port). The chosen entry
sits in a table at `0x9D9F58`, indexed by the cursor at `0x9D9F6D`.

`0x0F9390` assigns players when the setup is confirmed. The controller that
confirmed takes slot `port & 1`. With Two Players (entry 1) and exactly one
other controller connected, that controller takes the other team's slot,
`~slot & 1`. Slots 2 and 3 stay with the CPU. Teams are `slot & 1`, so
Two Players is one person against another, each with a CPU partner.
Character select (screen 4) also lets an unassigned controller join a slot
with START (`0x0F94F0`).

The bytes that say who plays are listed in [title-idle.md](title-idle.md):
`0x5DECE4` (a person), `0x5DECE8` (their port) and `0x5DECF4` (the
player on each port). Online play uses Two Players with the host on port 0
and the joiner on port 1. The remote player's pad reaches the game through
the input adapter's sample source for that port, as in doa2u-re.

## What came over from doa2u-re

Copied with their GPL-3.0-or-later notices and with header guards and the
replay tag (`DXBR`, `.dxbr`) renamed: `rollback.c`, `replay.c`,
`netplay.c`, `netplay_names.c`, `netplay_settings.cpp`,
`netplay_upnp.c`, `guest_snapshot.c`, `synctest.c`, `native_fiber.c`,
`musl_math/` (MIT, see NOTICE), `tools/matchmaker.py` and their tests.
The matchmaker's packets keep doa2u-re's `D2MM` tag, so both games use one
server; rooms pair only runners whose build digest matches, and the digest
includes the XBE hash.

`synctest.c` no longer checks which routine called Present. doa2u-re's
frame boundary is the Swap, where that routine decides the main thread's
native stack. Here the boundary is the top of the hand-written main loop in
`game_scheduler.c`, where every guest fiber is parked in the scheduler and
the main thread's stack is always the same.

Ported, because this runtime differs:

- **Fibers.** `fiber_adapter.c` switched Windows fibers, whose suspended
  context the OS keeps out of reach. It now uses `native_fiber.c`, which
  keeps each fiber's context and live stack as plain data, so a snapshot
  saves every parked fiber's native continuation. A deleted fiber stays
  parked, so a rewind can bring it back. POSIX hosts switch the same way but
  do not capture stacks yet, so SyncTest skips there.
- **Frame clock.** `xapi_time_adapter.cpp` gained doa2u-re's frame-locked
  clock: guest time advances 1/60 s per frame boundary, plus 1 us per read
  so polling loops move on. The scheduler's own 60 Hz pacing reads host time
  (`recomp_xapi_host_ns`).
- **Module state.** Each module saves its own host state for snapshots: CPU
  and FPU registers, fibers, the scheduler's pass and tick counters, events,
  DPCs, file cursors, allocations, the heap cursors, input, the D3D frame
  state and the DirectSound buffer clocks (`synctest_modules.c`).

## Determinism found so far

`RECOMP_SYNCTEST=loop` saves a frame at the loop top, runs
`RECOMP_SYNCTEST_FRAMES` frames, rewinds, runs them again and compares the
modules and guest RAM. The test route is CPU Exhibition
([title-idle.md](title-idle.md)) on court 2, with START pressed at polls 400
and 800 to skip the opening movie; the match loads at about frame 1,500.

The first checks inside a match differed in guest RAM. Each cause was a host
value the guest reads that a rewind did not restore:

- Present returns the D3D frame adapter's swap counter, which the game
  stores in two bytes of the D3D library's data (`0x1F569C`, `0x1F5D30`).
  A probe at each frame boundary showed them climbing through the rewind.
  The frame state is now saved.
- About 13 KB of decoded audio in guest RAM (`0xCAE000` to `0xCB7000`
  and `0x1259000` to `0x125B000`) followed DirectSound play cursors on
  host time. With the frame clock on, buffers play in game ticks, as
  `RECOMP_DSOUND_GAME_CLOCK=1` already did, and the guest-visible buffer
  clocks are saved under the lock the output pump shares. The pump only
  copies audio the guest wrote into host output, so it advances nothing the
  guest reads.

With both, a Windows clang build ran 887 two-frame checks from frame 2,400
and then 770 eight-frame checks from frame 1,700 to about 14,000, over three
minutes of a CPU match, with every module and all of guest RAM matching and
no check skipped. Saving took 6 to 10 ms and comparing 7 to 9 ms, most of it
copying 64 MB for the comparison, which sessions do not do.

A check during the boot's loading (frame 200) differed in 2.5 MB of guest
RAM and the fiber state. Sessions roll back only inside a match, so loading
runs in lockstep, as in doa2u-re.

## Decisions

- **Interpolation.** `game_rollback.c` undoes high frame rate display
  passes with the same `GetWriteWatch` reset tracking the snapshot ring
  uses, so the two cannot run together. Sessions run at 60 Hz: the frame
  boundary hook only runs when every pass is a tick, and SyncTest turns on
  the hand-written loop without high frame rate.
- **Guest threads.** Every guest thread is a fiber on the game thread and
  runs at fixed points in the scheduler, so no guest work runs on its own
  timing. The only other host thread that touches game state is the
  DirectSound output pump (above).

## Still to do

- Route the lifter's x87 transcendentals through `recomp_sin` and the
  rest on this game's recipe branch, as doa2u-re's lifter commit `f122d6d`
  does, and link `musl_math` into the runner. This recipe already lifts
  RCPSS and RSQRTSS as exact division and square root, so they need no
  change. Moving the lifter pin needs a play test (AGENTS.md).
- Session driver, the lobby and room menus, the walk to Two Players
  character select, and the conditions for rollback (a live rally) and for
  lockstep (menus, pauses, results, saves).
- Two runners on one machine, scripted, comparing guest RAM hashes; then a
  lag relay.