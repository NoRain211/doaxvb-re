# Rollback netcode

Status: in progress. The rollback core from
[doa2u-re](https://github.com/NoRain211/doa2u-re) (branch
`codex/rollback-netcode`) is ported: input prediction, the snapshot ring,
SyncTest, replays, the UDP transport and the matchmaker client. The
matchmaker server is doa2u-re's `tools/matchmaker.py`, shared by both
games. Players pair through the runner's online menu, or with
`--netplay-host` and `--netplay-join`, and play a Two Players Exhibition
match over UDP with rollback.

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
`musl_math/` (MIT, see NOTICE) and their tests.
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

Two more causes showed up in one-frame checks from frame 900, which span
the menu and the match's loading:

- At frame 1,942 the player records at `0x4D6FF0` (the `+5` byte of each),
  the court byte and the people bytes differed. CPU Exhibition
  (`cpu_exhibition.c`) casts the next match after its own count of menu
  frames, and that count was host state a rewind did not restore, so only
  one pass cast the match. The test mode's counters and cast choices are
  now a SyncTest module. This was the test route, not the game.
- At frame 2,194 the heap cursor differed by six 16 KB fiber stacks. A
  rewind past a fiber's creation kept its guest stack for the next create
  to reuse, while the heap module rewound the cursor under it. A rewind now
  frees those stacks with the heap. The high frame rate display passes
  (`game_rollback.c`) also rewind the heap now, so a fiber a display pass
  creates cannot leak its stack.

With both fixed, a Windows clang build ran 8,411 one-frame checks from
frame 900 with no mismatch.

A check during the boot's loading (frame 200) differed in 2.5 MB of guest
RAM and the fiber state. Sessions roll back only inside a match, so loading
runs in lockstep, as in doa2u-re.

Two machines must also compute the same floating point results. The lifter
(`4500b88` on the recipe branch) emits the x87 transcendentals (fsin,
fcos, fsincos, fptan, fpatan, fyl2x, fyl2xp1 and f2xm1, as
`expm1(x ln 2)`) as calls to `recomp_sin` and the rest, which
`recomp_types.h` maps to musl's (`musl_math/`), built the same on every
host.
125 game functions changed. RCPSS and RSQRTSS already lift as exact
division and square root. A UDP session from boot on this program matched
guest RAM to frame 4,000.

## Sessions

`netplay_session.cpp` is doa2u-re's session driver on this runtime's
frame boundary. `RECOMP_ROLLBACK=loopback` plays a generated remote player
on port 0 whose pads arrive `RECOMP_ROLLBACK_LAG` frames late, so every
frame is predicted and most are run again. `RECOMP_ROLLBACK=udp:<local
port>:<peer host>:<peer port>` plays a second runner, with
`RECOMP_ROLLBACK_PLAYER` (0 or 1, the guest port) and
`RECOMP_ROLLBACK_DELAY` matching on both sides. `RECOMP_ROLLBACK_START`
is the loop pass the session starts at, `RECOMP_ROLLBACK_FRAMES` where it
stops with every pad confirmed, and `RECOMP_ROLLBACK_DUMP` receives guest
RAM there. `RECOMP_ROLLBACK_GENERATED=1` gives each UDP player a
generated stream for scripted runs: new pads every frame in live rallies,
and A now and then in character select. Frames run again are not presented
and skip the vblank wait, and the scheduler's own 60 Hz pacing is off while
the session paces frames.

Two runners must read the same host values. While the frame clock runs,
`KeQuerySystemTime` returns 2003-01-01 plus frame-locked guest time, and
directory listings and save-file queries return that date as every file
time. Before this, two runners in different play folders differed in 20
pages after 3,000 frames: CPU Exhibition picks its cast from the date. The
last difference was 20 bytes of stale stack holding the `\Fonts` folder's
host times.

A UDP session predicts only while a rally is live
(`recomp_netplay_game_rally_live` in `netplay_game.c`): the match scene
(`0x5DECE0` 0) runs and has not asked for another (`0x5DED55`), the
game is not paused (`0x4D56EF`), and neither the results menu
(`0x5D89EC`) nor the end of the match (`0x4D6FC8`) has come. Menus,
loads, pauses and results run in lockstep. A profile write waits, inside
its frame, until the other player's pads through that frame have arrived
(`recomp_kernel_profile_mutation_hook`), and a mispredicted frame ends
the session before the write. SyncTest and the snapshot ring refuse to
rewind past a save (`save_storage` in `synctest_modules.c`).

A UDP session that starts at boot walks the menus for both sides with
port 0's pad, computed from guest state, so both runners press the same
button on the same frame: START through the movies and the title,
EXHIBITION on the main menu (list 0) and Two Players on Exhibition's setup
(list 2), which lists it because the session connects both ports. Character
select (menu screen 4) ends the walk, and each player picks with their own
pad.
Sessions from a lobby, and replays, cover the walk with a black STARTING
MATCH screen drawn each Swap and mute host audio until it ends
(`recomp_dsound_service_adapter_set_muted`), as doa2u-re hides its walks
to Versus. Both are host output only: a lobby session with them ran the same
number of kernel calls to frame 3,000 as one without, with guest RAM
identical on both sides.

Results on one Windows machine, clang Release runner, two play folders:

| Run | Frames | Rollbacks | Guest RAM at the end |
|---|---|---|---|
| Loopback, CPU match from pass 2400, lag 8 against lag 0 | 600 | 590 | identical |
| Loopback from boot with random remote pads in the menus, lag 8 against lag 0 | 3,000 | 2,984 | identical |
| UDP, CPU match from pass 2400, delay 0 | 1,200 | 600 per side | identical |
| UDP from boot: walk, both pick, Two Players match, delay 0 | 4,000 | about 1,760 per side | identical |
| UDP from boot through a relay adding 50 ms each way, 10 ms jitter and 2% loss, delay 3 | 4,000 | about 1,220 per side | identical |

In the last run character select showed at frame 182 and the match at 441;
slots 0 and 1 were people on ports 0 and 1, slots 2 and 3 their CPU
partners. Every 30th frame's guest RAM hash matched the other side's.

## Hosting and joining

`--netplay-host <port>` waits up to 10 minutes for a joiner, with
`--netplay-delay <frames>` (default 2, at most 8), and
`--netplay-join <host>:<port>` says hello for up to 2 minutes. The hello
carries the joiner's build: a SHA-256 of the source commit, the generated
program's manifest and the XBE's SHA-256. The commit comes from
`RECOMP_NETPLAY_VERSION`, else a release ZIP's `package.json`
(`source_base`), else Git, so every platform's ZIP of one release and a
checkout of that commit pair. The host refuses any other build and answers with its input delay
and the settings that change guest RAM, which the joiner adopts: the
swimsuit unlock, widescreen and music shuffle. Both sides then run at
60 Hz from the game's own title with no free camera and no custom
soundtracks, which the game would list in guest RAM.

The game keeps all three vacations in one save
(`UDATA/54430007/<save>/doaxuser.dat`), so both players' saves would
collide. The host recovers its save journal, packs its saves and title
saves with doa2u-re's `netplay_profile.cpp` (at most 1 MiB, with a
SHA-256), and the joiner sends none. Each side clears
`.recomp-storage/netplay`, writes the host's copy there after checking the
hash and every path, and plays the session from it with its own journal
(`recomp_save_set_session`), so neither player's own saves change. A save
folder may hold only what the game writes there: `doaxuser.dat` at the
11,176 bytes of every real save, and a UTF-16 `SaveMeta.xbx` of at most
4 KiB that names the save once. Anything else, including a folder inside
it, refuses the copy, so a modified host cannot hand the game a crafted
save.

The host asks the router to forward its UDP port with UPnP (miniupnpc
2.3.3) and removes the forward when the runner exits. The forward is leased
for a day, so one left by a crash expires, except on routers that take only
permanent forwards. The log names the public address the other player
joins, or says to forward the port by hand.

Two runners on one machine paired this way at delay 2, with the joiner's
own saves moved away: both walked to character select at frame 182 and
played the match to frame 3,000 with guest RAM identical at the end and
every check matching. The router accepted the forward, and its removal at
exit logged no error.

## The online menu

Pressing A on EXHIBITION in the game's main menu opens doa2u-re's menu
(`netplay_menu.cpp`, `netplay_menu_room.cpp`, `netplay_menu_find.cpp`)
over the game: LOCAL, HOST ONLINE, JOIN ONLINE, FIND MATCH and NETPLAY
SETTINGS. LOCAL hands the game the A press, so Exhibition starts as before;
B closes the menu. The menu opens while the main menu runs with its cursor
on EXHIBITION (`0x9D9E0C` 1, `viewer_menu_adapter.c`) and list 0 is open.
The rooms, chat, blocks, FIND MATCH, the shared matchmaker and
`.recomp-storage/netplay.txt` work as in doa2u-re
([matchmaking](https://github.com/NoRain211/doa2u-re/blob/codex/rollback-netcode/docs/matchmaking.md)).

The menu draws with doa2u-re's quads and 5 x 7 font (`overlay_screen.c`).
The game thread publishes each frame's quads, and the presenter's thread
draws them with Dear ImGui into a 640 x 480 area centered on the window,
in the same pass as the overlay text (`overlay_imgui_panel.cpp`). Typed
characters come from the window's `WM_CHAR` (`input_text.c`); the SDL
presenter feeds its text events and Enter, Backspace and Esc the same way.
Pasting uses the Windows clipboard only.

When both players are ready, doa2u-re reboots the game in the same process.
Here each runner starts again instead (`recomp_runner_relaunch`) with its
own arguments plus `--netplay-host` or `--netplay-join` and the room
socket's port, peer and matchmaker (`--netplay-room-socket`), so the path
the routers opened and a relay stay in use. Resetting every adapter that
keeps state between boots would be needed for an in-process reboot. The
window closes and opens again. A session started from a room that ends
(the other player left or went quiet, a desync, a failed lobby) starts the
runner offline again with `--netplay-return`, the room socket and the
room's state (`--netplay-room`: hosting, listed, found by FIND MATCH, the
peer's address), which the room's runner passed to the session. The room
opens again at once over the game on the same port, and the reason shows in
it; the host lists it again and waits, and the joiner calls the host again.
A runner that cannot have the port back within 2 s opens the menu with the
reason once the main menu has EXHIBITION chosen. Leaving the room leaves
the game where it booted to.

Two runners on one machine, each driven by an input script, went through
the menu: the host opened a private room, the joiner joined it from its
recent address, both pressed READY, and both runners started again into the
session, walked to character select at frame 182 and played to frame 1,500
with guest RAM identical at the end. Killing the joiner's session ended the
host's after 15 s (`rollback:peer-timeout`), and the host's runner came
back to the menu.

With the room reopening, the same test went on: the host's runner came
back into its room, a new joiner joined it from the menu, both pressed
READY and the second session played with every check matching. Killing the
host instead brought the joiner back into its room, and it joined a new
host's room on the same port for a second session.

## Replays and the network display

Each session started by `--netplay-host` or `--netplay-join`, and so
each room's session, records a replay in the player's own
`.recomp-storage/replays` (doa2u-re's `replay.c`, tag `DXBR`). It holds
the build digest, the side, the input delay, the host's options and saves,
the loop pass the session started at and both ports' confirmed pads, so it
stays on this computer. `--replay <file>` plays it again at 60 Hz from the
same saves and options, walks the menus as the session did, and stops at
the end of the file; another build's replay is refused.

A 3,000-frame session recorded on both sides; the joiner's replay, played
in its own play folder, ended with the same guest RAM as both live runners.

NETPLAY SETTINGS' network display draws in the window's bottom right
corner during a session: the ping, or also the input delay, the deepest
rewind of the last second and how many frames this side runs ahead.
A player who has used a gamepad in the session sees CONTROLLER
DISCONNECTED over the game while it is unplugged; the other side and both
guests keep the session's pads.

When the other player goes quiet for a second at a frame boundary, a
WAITING FOR OPPONENT panel (CONNECTING before they were first heard)
counts down the grace over the last frame. The frame adapter presents that
frame again ten times a second with the panel
(`recomp_d3d_frame_adapter_present_again`), which also keeps the window
answering, and leaves guest state alone. OPPONENT LEFT shows for 2 s before
the session ends on a goodbye. A wait inside a frame, for a profile write,
only logs. With one runner frozen for 3 s, the other kept presenting at
10 fps, both went on to frame 3,000 and guest RAM matched; with one killed,
the other presented at 10 fps until its 15 s grace ended the session.

## Decisions

- **Interpolation.** `game_rollback.c` undoes high frame rate display
  passes with the snapshot ring's own engine (`guest_snapshot.c`), which
  refuses a second write-watch user, so the two cannot run together.
  Sessions run at 60 Hz: installing the frame boundary hook at any other
  rate stops the runner, and SyncTest turns on the hand-written loop
  without high frame rate.
- **Guest threads.** Every guest thread is a fiber on the game thread and
  runs at fixed points in the scheduler, so no guest work runs on its own
  timing. The only other host thread that touches game state is the
  DirectSound output pump (above).

## Still to do

- A full match played with real input.
- Sessions between two machines and between operating systems.
