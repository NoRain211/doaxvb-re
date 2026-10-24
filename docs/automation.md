# Driving the game by script

`tools/drive_game.py` plays the game without a person at the pad. It sends
scripted presses through the runner's `--input-paused-analog-file` and, after
each press, waits for the [`[auto:ui]` log line](ui-log.md) that shows the
press landed. It does not sleep a fixed time and hope. A press the game
ignored, for example while the island map loads, is sent again.

Use it to check a change in the running game: reach a screen, look at the log,
and take a screenshot. Everything it writes goes into its session folder
(`private/drive/` by default): the runner's log, screenshots and the save
backup. Run evidence stays private (see `AGENTS.md`).

## Commands

Each command is one step. The session keeps the running game between
commands, so steps can run one at a time from a shell:

```text
python tools/drive_game.py route shop          # start, walk to the Sports Shop
python tools/drive_game.py until down "shop 0 cursor 3"
python tools/drive_game.py shot after-down     # private/drive/shots/after-down.png
python tools/drive_game.py stop
```

| Command | Does |
|---|---|
| `start` | starts the game with `RECOMP_AUTO_LOG=1`, muted, at render scale 1 |
| `route shop\|map\|pool\|items\|hotel [--vacation N]` | starts the game, continues Vacation N (default 1) and walks to the Sports Shop, the island map, the pool's menu, the items screen or the hotel's menu at night |
| `route new [--vacation N] [--girl NAME]` | starts a new vacation in the empty Vacation N (default 2) as the girl (default Kasumi), through to the end of its welcome |
| `press BUTTON [-n N] [--gap S]` | `a b x y black white lt rt up down left right start back ls rs` (`ls`, `rs`: thumbstick clicks) |
| `wait REGEX [--timeout S]` | waits for a log line after the mark |
| `until BUTTON REGEX` | presses the button until a line after the mark matches |
| `goto LOCATION` | steps the island map to a location, the shorter way round |
| `select LABEL` | moves the open list menu onto the item and confirms it |
| `buy [--wrap]` | buys the item under the shop's cursor, gift wrapped or not |
| `rest` | takes a rest at the pool, which uses up a part of the day |
| `shot NAME` | saves the next frame, without the runtime's overlay |
| `log [-n N]` | the last `[auto:...]` lines |
| `stop [--keep-save]` | stops the game |

`start` and `route` find the runner and game the way `run_game.py` does, from
the selected build receipt. `--runner` and `--xbe` choose others, and
`--env NAME=VALUE` adds runner settings such as `RECOMP_SHOP_PANEL=1`.

**The mark.** `wait`, `until` and the steps built on them look only at log
lines after the mark. The mark is the line the previous wait matched, or
the end of the log when a press was made since: a press moves it there
first, so a wait after a press only sees what followed it. If a press lands
before its wait starts, the wait still finds its line. `until` takes a match
logged since the last press without pressing; a line an earlier `press`
command caused cannot end it.

A screen often drops a press made while it opens, such as the shop list's
first Down. Use `until` to move a cursor: it presses again until the log shows
the move, so a dropped press costs one more try.

**Failures.** A step that times out, or a game that exits, ends the command
with exit code 1. The tool saves a `fail-HHMMSS` screenshot and prints the last
`[auto:...]` lines.

**Saves.** `start` copies the saves (`.recomp-storage/partition1/UDATA` beside
the game executable). `stop` puts them back if the run changed them, and keeps
the written copy in the session folder. The same goes for the Analog/Digital
preference (`.recomp-storage/controller-mode-v1`), which Controller Settings
writes outside the saves. Pass `--keep-save` to keep the changes. If a run
ended without `stop` (it crashed or was killed), the next `start` puts both
back first, so the backup always holds the player's own saves. A game that
exits before it polls the pad, such as one that cannot open the saves, has
changed nothing, and `start` keeps no backup of it. On Linux `start` refuses
to run while another runner has the same game open: a person may be playing
it, and its saves are in use.

The driver prints its own lines as `[auto:driver] ...`, in the same prefix
family as the game's `[auto:ui]` lines.

## The game's flow

The routes follow the game's own structure. Community guides on
[GameFAQs](https://gamefaqs.gamespot.com/xbox/561527-dead-or-alive-xtreme-beach-volleyball/faqs)
describe it:

- A vacation day has four parts: morning, daytime, evening and night. The
  places on the map open by part of the day. The Casino opens only at night,
  Bass Island only in the daytime, and Zack of All Trades from the fourth day.
- The pool's menu offers Leave the poolside, Take a rest, Play Hopping game,
  View items and Quit the game. Resting or the hopping game uses up a part of
  the day. View items opens the items screen with its Swimsuit, Accessory,
  Item and Present tabs.
- Going to a place on the map uses up a part of the day too. After the
  evening the player goes to the hotel. Each gift that arrives that night is
  a dialog that A moves on; B does not. Then the hotel's menu opens: Go to
  the Casino, Make a gift, Watch video, View items, View Collection, Go to
  bed and Quit the game. A pressed once the menu is up takes the Casino, so
  `route hotel` stops pressing as soon as the menu's line appears, and walks
  back from the Casino if a press got there first.
- The night writes the save. Go to bed starts the next day, on the island
  map. `stop` puts the saves back unless `--keep-save` is given.

`route hotel` rests at the pool until night (two or three rests from a
morning) and takes about 45 s. One A skips each rest's scene; the driver
presses nothing more until the pool's menu is back, because the menu would
take the next A as Leave the poolside.

### The pool's menu hides itself

When no button is pressed for a while, the pool's menu hides. In one run it
hid 39 s after it opened. The next press only shows the menu again, and the
game takes the press after that. The log shows the hide as `list 0 closed`.
`select` wakes the menu before it moves the cursor. It wakes the menu with
Down, because a run showed that Down only brings the menu back. A on a menu
that is showing confirms the item under the cursor.

### A new vacation

`route new` chooses an empty vacation, whose prompt reads New Game and
Options, and refuses one that reads Continue. New Game starts the opening
scene, which START skips, then the character select: the girl is chosen
by her name in the `pick cursor` line. A welcome follows at the pool, a
conversation of many lines, then Lisa's tour of the map, which ends with
"Then let's take a look at the Accessory Shop." The driver presses A only
while the log says the dialog is open (`dialog open`), one press a line,
so no press goes past the end: the tour's last A enters the Accessory
Shop and the dialog closes there. A run takes about 70 s. A new vacation
writes the save, which `stop` puts back.

## Platforms

The tool needs only Python. It starts the runner in its own process group and
stops it with `taskkill` on Windows or a signal elsewhere. Screenshots come
from the runtime (`RECOMP_SCREENSHOT_TRIGGER`; see
[free-camera.md](free-camera.md)), so they need no window capture, but they
leave out the runtime's overlay. Linux is the only platform tested so far.
