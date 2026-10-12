# Title screen idle behavior

What the game does when its title screen is left idle. The launcher's
**Title screen idle behavior** choice, or `RECOMP_TITLE_IDLE` set before the
runner starts, picks one of three:

| Launcher | `RECOMP_TITLE_IDLE` | Left idle, the game shows |
|---|---|---|
| Default | unset or `default` | Its own attract sequence: movie, title screen, real-time demo |
| No cutscene | `no-cutscene` | The attract sequence without the movies |
| CPU Exhibition | `cpu-exhibition` | Exhibition matches played by the CPU, one after another |

The launcher saves the choice as `title_idle` in `private/launcher.json`.
With Default the runtime changes nothing.

## Behavior

All three start the same way: the logos, then the movie, then the title
screen. START skips the movie.

### Default

The idle loop is movie, title screen, real-time demo, title screen, and round
again. The title screen stays for 30 seconds.

### No cutscene

- The loop is title screen, real-time demo. The title screen stays for 10
  seconds, the last of them its usual fade.
- The movie that plays once after the logos does not come back while the
  game idles.
- The demo is the game's own: the same girls and courts in the same order
  each time, with swimsuits picked afresh on each pass.
- START works as before, from the demo to the title screen and from the
  title screen to the main menu.

### CPU Exhibition

- When the title screen has been idle for 29 seconds, just before it would
  fade to the attract sequence, an Exhibition match loads in which the CPU
  plays all four girls. The main menu shows for a moment on the way.
- Each match has four different girls, chosen at random. Each wears a
  swimsuit chosen at random from all the swimsuits she can wear, 307 of the
  game's 328, whether or not a save owns it. The court is one of the ten
  Exhibition courts, chosen at random.
- A match is a full 7 point match, the Exhibition default.
- When a match ends, its results stay up for five seconds and the next match
  loads with a new cast, court and swimsuits.
- START on any controller ends the matches and shows the title screen, and
  the game is then played as usual. Left idle there, the matches start again.
- The swimsuits are only worn for the match; nothing is added to a save.

The movie that opens a vacation, the ending and the gallery's movies are not
part of the idle loop and are not affected by either choice.

## Guest contract

Addresses are for the verified USA executable.

### Attract sequence

| `0x328C14` | Attract step table: 7 steps of 4 bytes (kind, pad, 16-bit parameter) |
| `0x328C30` | Handlers for each kind, 16 bytes a kind: prepare, enter, update, exit |
| `0x4D9944` | Step state: active at `+0`, finished at `+1`, kind at `+4`, step index at `+5`, frames left before leaving at `+6`, frame counter at `+8` |
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

The table is ordinary writable data. For No cutscene, `title_idle.c` changes
one value in it, the parameter of the jump, from 2 to 4, so the sequence
returns to the title screen that precedes the demo and never reaches the
movie between the two title screens again. The step kinds are checked first;
if they are not the seven above the table is left alone and a line is written
to stderr.

The boot path needs a step of kind 5 to exist: `0x000A4920` advances through
the table until it finds one. Removing the movie steps outright leaves that
search without an end, which is why the table keeps them.

The title scene the game shows after a match is quit runs the same title
step, with the same update and frame counter.

### CPU Exhibition

| `0x5DECE0` | Scene running: 0 Exhibition match, 2 attract sequence, 3 menus, 4 title |
| `0x5DED55` | Scene asked for next |
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

`title_idle.c` starts the matches from the title step's update when its frame
counter reaches 1738 and the step is not already leaving after START. `cpu_exhibition.c` then runs once per presented frame
until START:

- **START.** If START is held, it puts back the bytes that say which
  players are people and, in a match, stores Quit as the results menu's
  choice with the match over byte. The match scene's update then leaves for
  the title as it does after Quit. A scene the matches never ask for, which
  only a person can have chosen, ends them the same way.
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

- `recomp_d3d_swap_adapter` calls `recomp_title_idle_frame` once per
  presented frame. For No cutscene it changes the table on the first call;
  it also runs `recomp_cpu_exhibition_frame`.
- `recomp_lookup_manual` routes `0x0006B290` to a wrapper in `title_idle.c`
  that then calls the generated update. For No cutscene it first moves the
  step's frame counter from 540 to 1740; for CPU Exhibition it starts the
  matches at frame 1738. The generated function still owns the title music
  and the fade. Writing the title step by hand is open work.
- `leave_menu` in `cpu_exhibition.c` calls the generated game routine at
  `0x000F2C10` through `recomp_dispatch`, with the guest registers and FPU
  state saved around the call. The routine runs the current screen's own
  leaving handler, which is what clears the main menu from the screen; a
  hand-written menu would replace this call.

## Checks

`title_idle_test.c` runs the table change and the title hold on synthetic
guest memory; with `--unexpected-table` it checks that a table holding
another sequence, and the title's hold with it, is left alone, and with
`--cpu-exhibition` that the matches are started at the idle frame and the
table is untouched. `cpu_exhibition_test.c` runs the title, menu and results
steps on synthetic guest memory, including that nothing is done before the
matches are started, that the cast is four different girls in swimsuits each
can wear, and that START ends the matches until they are started again.

A scripted run on macOS with CPU Exhibition chosen, against a copy of a
save, with scene changes logged: START skipped the logos and the movie, the
title screen was then left idle, and after 1744 frames the menus showed for
two seconds and a match loaded. That match ran for about five minutes and
returned to the menus, and a second match loaded two and a half seconds
later. The run had no screenshots, so the casts were not compared.

Before the three choices were one setting, scripted runs on macOS showed the
No cutscene loop (title screen for about 600 frames, demo, twice over, with
no movie frame in between) and a CPU Exhibition match played to 7 points with
its results shown and a second match on another court with another cast.

Not checked: the attract sequence's own title screen (reached by letting the
movie end) leading to CPU Exhibition, START ending the matches in a run of
the game, sound, sessions longer than thirteen minutes, and Windows and
Linux builds. The launcher choice has not been seen on screen in any of the
three launchers.
