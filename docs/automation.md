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
| `route shop\|map\|pool\|items` | starts the game and walks to the Sports Shop, the island map, the pool's menu or the items screen |
| `press BUTTON [-n N] [--gap S]` | `a b x y black white lt rt up down left right start back` |
| `wait REGEX [--timeout S]` | waits for a log line after the mark |
| `until BUTTON REGEX` | presses the button until a line after the mark matches |
| `goto LOCATION` | steps the island map to a location, the shorter way round |
| `select LABEL` | moves the open list menu onto the item and confirms it |
| `buy [--wrap]` | buys the item under the shop's cursor, gift wrapped or not |
| `shot NAME` | saves the next frame, without the runtime's overlay |
| `log [-n N]` | the last `[auto:...]` lines |
| `stop [--keep-save]` | stops the game |

`start` and `route` find the runner and game the way `run_game.py` does, from
the selected build receipt. `--runner` and `--xbe` choose others, and
`--env NAME=VALUE` adds runner settings such as `RECOMP_SHOP_PANEL=1`.

**The mark.** `wait`, `until` and the steps built on them look only at log
lines after the mark. The mark is the line the previous wait matched. If a
press lands before its wait starts, the wait still finds its line.

A screen often drops a press made while it opens, such as the shop list's
first Down. Use `until` to move a cursor: it presses again until the log shows
the move, so a dropped press costs one more try.

**Failures.** A step that times out, or a game that exits, ends the command
with exit code 1. The tool saves a `fail-HHMMSS` screenshot and prints the last
`[auto:...]` lines.

**Saves.** `start` copies the saves (`.recomp-storage/partition1/UDATA` beside
the game executable). `stop` puts them back if the run changed them, and keeps
the written copy in the session folder. Pass `--keep-save` to keep the changes.

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
- At night the player goes to the hotel. Its menu has View items, the
  Collection, gifts, sleep and the Casino. The hotel's screens are not in the
  log yet.

### The pool's menu hides itself

When no button is pressed for a while, the pool's menu hides. In one run it
hid 39 s after it opened. The next press only shows the menu again, and the
game takes the press after that. The log shows the hide as `list closed`.
`select` wakes the menu before it moves the cursor. It wakes the menu with
Down, because a run showed that Down only brings the menu back. A on a menu
that is showing confirms the item under the cursor.

## Platforms

The tool needs only Python. It starts the runner in its own process group and
stops it with `taskkill` on Windows or a signal elsewhere. Screenshots come
from the runtime (`RECOMP_SCREENSHOT_TRIGGER`; see
[free-camera.md](free-camera.md)), so they need no window capture, but they
leave out the runtime's overlay. Linux is the only platform tested so far.
