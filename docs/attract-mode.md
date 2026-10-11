# Attract mode without the movies

An opt-in change to what the game shows when left idle. With **Skip attract
movies** ticked in the launcher (or `RECOMP_ATTRACT_NO_MOVIES=1` set before
the runner starts), the idle loop alternates between the title screen and the
real-time volleyball demo. It is off by default, and with it off the runtime
changes nothing.

## Behavior

- Unchanged, the idle loop is movie, title screen, real-time demo, title
  screen, and round again. The title screen stays for 30 seconds.
- With the option on, the loop is title screen, real-time demo. The title
  screen stays for 10 seconds, the last of them its usual fade.
- The movie still plays once after the logos when the game starts, as it
  always has. START skips it. It does not come back while the game idles.
- The demo is the game's own: the same girls and courts in the same order
  each time, with swimsuits picked afresh on each pass.
- START works as before, from the demo to the title screen and from the
  title screen to the main menu.
- The movie that opens a vacation, the ending and the gallery's movies are
  not part of the idle loop and are not affected.

## Guest contract

Addresses are for the verified USA executable.

| Address | Meaning |
|---|---|
| `0x328C14` | Attract step table: 7 steps of 4 bytes (kind, pad, 16-bit parameter) |
| `0x328C30` | Handlers for each kind, 16 bytes a kind: prepare, enter, update, exit |
| `0x4D9944` | Step state: active at `+0`, finished at `+1`, kind at `+4`, step index at `+5`, frame counter at `+8` |
| `0x000A4920` | Starts the sequence at boot, at the first step of kind 5 |
| `0x0006B290` | Update of the title step (kind 2) |

| Step | Kind | Parameter | |
|---|---|---|---|
| 0 | 0 | 0 | Wait 300 frames |
| 1 | 1 | 0 | Movie |
| 2 | 2 | 0 | Title screen for 1800 frames, fading from frame 1740 |
| 3 | 5 | 4 | Movie; the one shown after the logos |
| 4 | 2 | 0 | Title screen |
| 5 | 3 | 0 | Real-time demo |
| 6 | `0xFF` | 2 | Jump to step 2 |

Steps 0 and 1 are read from the table and were not seen to run: every
observed start entered at step 3 and looped from step 6 to step 2.

The table is ordinary writable data. `attract_sequence.c` changes one value
in it, the parameter of the jump, from 2 to 4, so the sequence returns to the
title screen that precedes the demo and never reaches the movie between the
two title screens again. The step kinds are checked first; if they are not
the seven above the table is left alone and a line is written to stderr.

The boot path needs a step of kind 5 to exist: `0x000A4920` advances through
the table until it finds one. Removing the movie steps outright leaves that
search without an end, which is why the table keeps them.

## Seams

- `recomp_d3d_swap_adapter` calls `recomp_attract_sequence_frame` once per
  presented frame. It changes the table on the first call and does nothing
  afterwards.
- `recomp_lookup_manual` routes `0x0006B290` to a wrapper that, with the
  option on, moves the step's frame counter from 540 to 1740 and then calls
  the generated update. The generated function still owns the title music
  and the fade. Writing the title step by hand is open work.

## Checks

`attract_sequence_test.c` runs the table change and the title hold on
synthetic guest memory, and with `--unexpected-table` checks that a table
holding another sequence, and the title's hold with it, is left alone.

Scripted runs on macOS with step changes logged and a screenshot every 200
frames:

- Option on, no input for five minutes: logos, movie, title screen, demo,
  title screen, demo.
- Option on, START during the first movie, then no input: title screen for
  about 600 frames, demo for about 6800, twice over, with no movie frame in
  between. The two passes of the demo showed the same girls in different
  swimsuits.
- Option on, START during the demo and again at the title screen: title
  screen, then the main menu.
- Option off, START during the first movie, then no input: title screen
  for 1800 frames, demo, title screen for 1800 frames, then the movie.

Windows and Linux builds, and the launcher checkboxes on those systems, have
not been run.
