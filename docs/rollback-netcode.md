# Rollback netcode

Status: in progress. SyncTest (`synctest.c`, ported from
[doa2u-re](https://github.com/NoRain211/doa2u-re), branch
`codex/rollback-netcode`) saves a frame, runs it, rewinds and runs it again,
then compares the runtime's modules and guest RAM. It finds host state a
rollback would miss before online play depends on it.

## What SyncTest needs from this runtime

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
- Decoded audio in two guest RAM ranges (`0xCAE000` to `0xCB7000` and
  `0x1259000` to `0x125B000`, 44 KB together, of which about 13 KB
  differed) followed DirectSound play cursors on host time. With the frame
  clock on, buffers play in game ticks, as
  `RECOMP_DSOUND_GAME_CLOCK=1` already did, and the guest-visible buffer
  clocks are saved under the lock the output pump shares. The pump only
  copies audio the guest wrote into host output, so it advances nothing the
  guest reads.

With both, a Windows clang build ran 887 two-frame checks from frame 2,400
and then 770 eight-frame checks from frame 1,700 to about 14,000, over three
minutes of a CPU match, with no check skipped. Each check compares every
module except the clock and save storage, which SyncTest ignores, and all
guest RAM outside parked fibers' stacks. Saving took 6 to 10 ms and
comparing 7 to 9 ms, most of it copying 64 MB for the comparison, which
sessions do not do.

One mismatch is still open and predates this work: at frame 1,942, after
the end-of-match flag is set, 64 to 66 bytes of guest RAM from `0x4D6FF5`
differ between the two passes. Until it is found, this range is not proven
deterministic.

A check during the boot's loading (frame 200) differed in 2.5 MB of guest
RAM and the fiber state. Sessions roll back only inside a match, so loading
runs in lockstep, as in doa2u-re.
