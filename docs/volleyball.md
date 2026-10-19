# Volleyball match code

A map of the game-owned code that runs a volleyball match, for decompiling it
in clusters. Addresses refer to the retail image; sizes come from the
generation recipe and bounded Ghidra queries. Nothing here is hand-written yet.

## How a match runs

The top-level loop at `0x000A37F0` runs one scene at a time. The scene table
at `0x002F5ED0` holds three handlers per scene (enter, update, leave), and
the match is scene 0:

| Handler | Address | Role |
|---|---|---|
| Enter | `0x000A4940` | Clears match flags and starts the root task. |
| Update | `0x000A4970` | Acts on the results menu's choice and shows the controller-disconnect notice. |
| Leave | `0x000A4A80` | Kills the match tasks (`0x000991C0`). |

Gameplay runs in cooperative tasks, each a fiber. Task slots are 32 bytes at
`0x005F2C38` (flags, fiber, callback); `0x000B5570` is the shared fiber entry
and `0x000B55F0` kills a slot. Every task's loop does one step and switches
back to the main fiber.

## Root task and phases

`0x00099160` starts the root task (slot 2, `0x00099100`). After a one-time
init it runs, each frame unless the match is paused:

1. `0x00099500`, the match phase machine.
2. `0x0009B4C0`, per-player distances and side flags derived from positions.
3. The gameplay update counter `0x005D8984`.
4. `0x0009C390`.

The phase machine keeps the phase at `0x005D89BC` and a requested phase at
`0x005D89C0`, and runs the enter, update and leave handlers from the table at
`0x00317640`. There are ten phases. Phase 0 sets up the match
(`0x00099580`) and its leave handler starts the worker tasks; phase 9 is the
results screen (`0x0009AE10`). The roles of phases 1 to 8 are still to be
named.

## Worker tasks

`0x00099320` starts six workers when phase 0 ends:

| Slot | Callback | Reach | Role |
|---|---|---|---|
| 5 | `0x00014440` | 203 functions, 41.8 KB | Player behaviour: per-player command queues (`0x00410E60`, 0x38 bytes each) and updates. 158 of its functions sit together in `0x00011000`-`0x0001B000`. |
| 6 | `0x000BB420` | 208 functions, 47.6 KB | Not yet identified. |
| 7 | `0x000AF160` | 129 functions, 37.0 KB | Animation; overlaps the pipeline in the high-frame-rate research. |
| 4 | `0x0006B720` | small | Not yet identified. |
| 11 | `0x000B42B0` | 204 functions, 70.9 KB | Not yet identified. |
| 14 | `0x000E6F00` | 3 functions | Not yet identified. |

Reach counts every game function a callback can call, so clusters overlap.

## Match data

| Address | Contents |
|---|---|
| `0x004D6FF0` | Four players, 0xBC bytes each (`cpu_exhibition.c` names +0 CPU and +5 present). |
| `0x005DECE4`, `0x005DECE8` | Per-slot human flag and controller port. |
| `0x005D8978` | Results menu choice. |
| `0x004D6FD5` | Match paused. |
| `0x00410E60` | Player command queues. |

## Order of work

1. **Root task and phase machine.** The scene handlers, root task, phase
   machine and 30 phase handlers are 34 functions (8.1 KB); the 297 functions
   they reach (65.7 KB) can stay generated at first. It is the smallest
   cluster, it names every phase, and an explicit per-frame step here is the
   boundary #24's rally rewind needs.
2. **Player task** (`0x00011000`-`0x0001B000`). The rally logic; replace it
   once the phase data it reads is named.
3. **Workers 6 and 11.** Identify them before scoping.

The animation worker stays with the animation pipeline work.

