# CPU Exhibition

A way to leave the game playing by itself. **CPU Exhibition** in the launcher
(or `RECOMP_CPU_EXHIBITION=1` set before the runner starts, or
`./run_game.sh --cpu-exhibition`) starts Exhibition matches in which the CPU
plays all four girls, one match after another. It is off by default, and with
it off the runtime changes nothing.

## Behavior

- The game starts with its logos as usual. The opening movie is not played:
  the title follows the logos and is left after a second, and a match loads.
  No controller is needed.
- Each match has four different girls, chosen at random. Each wears a
  swimsuit chosen at random from all the swimsuits she can wear, 307 of the
  game's 328, whether or not a save owns it.
- The court is one of the ten Exhibition courts, chosen at random.
- A match is a full 7 point match, the Exhibition default.
- When a match ends, its results stay up for five seconds and the next match
  loads with a new cast.
- START on any controller ends the mode. During a match it leaves for the
  title screen, and the game is then played as usual. Otherwise the mode
  lasts until the game is closed.
- The swimsuits are only worn for the match; nothing is added to a save.

## Guest contract

Addresses are for the verified USA executable.

| Address | Meaning |
|---|---|
| `0x5DECE0` | Scene running: 0 Exhibition match, 2 attract sequence, 3 menus, 4 title |
| `0x5DED55` | Scene asked for next |
| `0x000A4920` | Routine the logos leave through: asks for the attract sequence and starts it at its movie |
| `0x5F3138` | Buttons held on any controller; START is `0x0010` |
| `0x4D994C` | Frames the title or attract step now showing has run |
| `0x4D994A` | Frames left before that step is left; START sets it |
| `0x9D9D80` | Menu state machine: one screen at a time |
| `0x9D9D9C` | Its exit code, `0xFFFFFFFF` for none, `0x20` to start what was chosen |
| `0x9D9DA0` | Its phase, 1 while a screen is running |
| `0x9D9DB4` | Callback given the exit code; `0x000F3540` is the main menu's |
| `0x9D9E0C` | What the main menu starts: 0 a vacation, 1 Exhibition |
| `0x000F2C10` | Routine a screen calls to leave for another, or for none |
| `0x9DAA68` | Character select's four choices, `0xB8` bytes each: `+1` girl, `+4` swimsuit item id |
| `0x9DADD9` | Its court, an index into the ten 6-byte entries at `0x2A7A40` |
| `0x4D92F0` | Court now loaded, indexed by the byte at `0x4D56E4` |
| `0x4D6FF0` | Four players, `0xBC` bytes each: `+0` played by the CPU, `+5` taking part |
| `0x5DECE4` | Four bytes: the player is a person |
| `0x5DECE8` | Four bytes: that person's controller port |
| `0x5DECF4` | Four bytes: the player on each port |
| `0x4D9658` | A team has a person in it; the other team's byte is 9 further on |
| `0x5D89EC` | Results menu open |
| `0x5D89C4` | Frames the results menu still waits for a choice |
| `0x5D8978` | Its choice: 0 none, 2 Character Select, 3 Quit |
| `0x4D6FC8` | Match over, set when that choice is stored |
| `0x3003E8` | Table of pointers to 28-byte item definitions, as in `docs/swimsuit-unlock.md` |
| `0x2A7B00` | Character id of each of the 8 girls |

The players are ordered so that a team is `slot & 1`: players 0 and 2 against
1 and 3.

`cpu_exhibition.c` runs once per presented frame:

- **START.** If START is held, it puts back the bytes that say which
  players are people and, in a match, stores Quit as the results menu's
  choice with the match over byte. The match scene's update then leaves for
  the title as it does after Quit. Nothing more is done for the session.
- **Title or attract sequence.** Once the step showing has run for a second,
  it stores one frame left in `0x4D994A`. The game's own sequence code then
  ends the step as it does after START and moves on to the menus.
- **Menus.** Once the main menu has been up for a second and a half, it marks
  the four players as the CPU's, fills in character select's choices and
  court, stores the start exit code and calls `0x000F2C10` for no screen. The
  menu then leaves as it does when character select is confirmed: its
  callback copies the choices to the match and asks for the match scene.
- **Match.** When the results menu has waited five seconds, it ends the wait.
  The game stores an empty choice, which it replaces with Character Select;
  the match scene returns to the menus and the cycle repeats.

## Seams

- `recomp_cpu_exhibition_lookup_manual`, called from
  `recomp_lookup_manual`, replaces the routine at `0x000A4920` with
  `logos_leave`. With the mode on it asks for the title scene, whose own
  enter routine starts the title step; with the mode off it calls the
  generated routine.
- `recomp_d3d_swap_adapter` calls `recomp_cpu_exhibition_frame` once per
  presented frame.
- `leave_menu` in `cpu_exhibition.c` calls the generated game routine at
  `0x000F2C10` through `recomp_dispatch`, with the guest registers and FPU
  state saved around the call. The routine runs the current screen's own
  leaving handler, which is what clears the main menu from the screen; a
  hand-written menu would replace this call.

## Checks

`cpu_exhibition_test.c` runs the title, menu and results steps on synthetic
guest memory, including that the cast is four different girls in swimsuits
each can wear.

Scripted runs on macOS with no input, against a copy of a save: a match was
on screen about 30 seconds after the game started, with no menu left over
it. In a ten minute run the first match was played to 7 points with rallies,
replays and the match point banner; its results showed, and a second match
started on another court with another cast. With the movie skipped, the
title followed the logos directly; START pressed during a match showed the
title, and from there START and A opened the main menu and the saved game
list. Save storage was byte-identical
after a 400 second run. Sound, sessions longer than ten minutes, and Windows and Linux have not
been checked.
