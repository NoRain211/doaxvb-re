# Screen and cursor log

`RECOMP_AUTO_LOG=1` makes the runner print an `[auto:ui]` line to the console
each time the game changes screen or moves a cursor. It is meant for scripts
that drive the game with scripted pad input: instead of sleeping a fixed time
after a press, a script waits for the line that shows the press landed.
[`tools/drive_game.py`](automation.md) does this. Everything is read-only: no
save or game state is changed.

Every line meant for automation starts with `[auto:<source>]`, so a tool can
pick them out of the runner's other output with `^\[auto:`: `ui` is this log,
and `driver` is what `drive_game.py` prints about its own steps. An
`[auto:ui]` line goes on with `frame N: ` and the event.

## Log output

```text
[auto:ui] frame 134: scene title
[auto:ui] frame 253: scene menu
[auto:ui] frame 253: menu screen main
[auto:ui] frame 267: list 0 open
[auto:ui] frame 267: list 0 cursor 0 of 2: GO TO ZACK ISLAND
[auto:ui] frame 280: list 0 closed
[auto:ui] frame 309: menu screen save-select
[auto:ui] frame 341: save vacation 1
[auto:ui] frame 355: list 2 open
[auto:ui] frame 355: list 2 cursor 0 of 3: Continue
[auto:ui] frame 400: menu screen -1
[auto:ui] frame 401: scene island
[auto:ui] frame 401: island map
[auto:ui] frame 401: map location 0 Niki Beach
[auto:ui] frame 840: map location 5 Sports Shop
[auto:ui] frame 879: island shop
[auto:ui] frame 913: shop 0 cursor 2 item 178
```

Frames count from the first logged frame. A value without a name is printed
as a number, such as `scene 8` or `island 9`.

| Line | Meaning |
|---|---|
| `scene title\|menu\|island\|match` | the running scene changed; logged from the first scene after boot |
| `menu screen main\|save-select\|exhibition-setup\|character-select` | the menus' screen changed; `-1` while they change screens or leave |
| `list K open`, `list K closed` | the game's list widget K opened or closed (below) |
| `list K cursor C of N: TEXT` | an open list's cursor and the game's text for its item; `?` when the item has no text |
| `list K cursor C` | the cursor of an open list without texts |
| `save vacation V` | the save select's cursor is on Vacation V |
| `island pick`, `pick cursor C NAME` | a new vacation's character select and the girl under its cursor |
| `dialog open`, `dialog closed` | a conversation began or ended (below) |
| `day part morning\|daytime\|evening\|night` | the part of the day changed; logged on the island |
| `island opening\|pick\|map\|shop\|items\|pool\|hotel\|collection\|rest\|casino` | where the player is on the island; `pool` and `hotel` are the same place value, named by the part of the day |
| `map location L NAME` | the island map points at location L |
| `items tab T NAME cursor C` | the items screen's tab (0 Swimsuit, 1 Accessory, 2 Item, 3 Present) and its grid cursor, three columns |
| `collection tab T NAME page P cursor C` | the Collection's tab (0 Swimsuit, 1 Accessory, 2 Item), its page, and the cursor counted over all pages, 25 cells a page in five columns |
| `shop S cursor C item I` | in shop S (0 Sports, 1 Accessory) the cursor is on entry C, item id I |
| `shop step list\|buy\|wrap` | the shop's list, its Buy/No prompt, or the Wrap/No prompt after Buy |
| `shop prompt cursor C: Buy\|Wrap\|No` | the open prompt's cursor |

Map locations: 0 Niki Beach, 1 Private Beach, 2 Jungle, 3 Bass Island,
4 Accessory Shop, 5 Sports Shop, 6 Zack of All Trades, 7 Poolside,
9 Radio Station. The map skipped 8 on a morning; it is printed as `8 ?`.

The game's menus are list widgets, numbered in the log:

| List | Used by |
|---|---|
| 0 | main menu, the pool's menu, the hotel's and the Casino's menus |
| 1 | the save select's row of vacations, and a new vacation's character select (no texts) |
| 2 | the prompt under a vacation: Continue, Options, Delete a saved game; or New Game and Options for an empty one |
| 3 | the shop's Buy/No and Wrap/No prompts; its phase reads open only for the few frames as a choice is confirmed, so follow the `shop step` and `shop prompt` lines instead |

The Casino's chip price is drawn without a text and shows as `?`.

`dialog open` comes with the first line of a conversation and `dialog closed`
after the last, so a conversation of many lines is one open and one close.
A guided step keeps it open too: the welcome of a new vacation ends with
"Then let's take a look at the Accessory Shop.", the box closes, and the
flag stays on until the next A has entered the shop.

The girls in a new vacation's character select, in order: Kasumi, Hitomi,
Helena, Leifang, Christie, Lisa, Tina, Ayane. Its first cursor can still be
the save select's for a few frames, so `pick cursor` waits until the
widget is set up for eight girls. Making a gift at the hotel shows the items grid on its Present tab
without changing the place; the girl it goes to is not in the log yet.

## Guest contract

USA executable. Each address below was found by comparing guest RAM dumps
taken at successive screens or cursor positions and checked against a
screenshot of the same moment.

| Address | Meaning |
|---|---|
| `0x5DECE0` | running scene: 0 match, 1 island, 3 menus, 4 title (state machine at `0x000A3820`) |
| `0x9D9D80` | menus' screen (signed); the screen table at `0x2A78B0` holds 32-byte records of seven handlers and a data block |
| `0x9DA120` | save select's data block: `+0xD` vacation under the cursor, `+0x10` step (3 choosing a vacation, 4 prompt open) |
| `0x5C5800` | list widgets, 0xD0 bytes each: `+0x0D` columns, `+0x1A` cursor (word), `+0x5C` item count, `+0x60` item text pointers, `+0x88` phase (1 opening, 2 open, 4 closing, 5 closed) |
| `0x5C5FED` | set while a conversation is going on (in the message window's block) |
| `0x4D72E3` | island place: 0 a new vacation's character select, 17 its opening scene, 1 map, 3 shop, 4 items screen, 5 a place with a menu (the pool by day, the hotel at night), 6 Collection, 7 a rest at the pool, 8 Casino; 255 off the island |
| `0x41EFEB` | part of the day: 0 morning, 1 daytime, 2 evening, 3 night |
| `0x3AF874` | a shop's screen block, the context of its row draw `0x000C8840`; `+0x18` points to the list state: `+0x22` highlighted row, `+0x24` entry at the top (wraps), `+0x26` entry count |
| `0x3AF894` | shop step (block `+0x20`): 1 list, 2 Buy/No, 3 Wrap/No |
| `0x5C5A8A` | the shop prompts' cursor (word), 0 the first button; list 3's cursor |
| `0x3AF8A1` | shop number (signed) |
| `0x317638` | points to the shops' entries: 16 bytes each, 15 a shop, the item id first |
| `0x31BD69` | items screen's tab; RT and LT step through them |
| `0x31BE04` | items screen's grid cursor (word); also the hotel's gift grid |
| `0x31CC7D` | Collection's tab |
| `0x31CE0C` | Collection's cursor over all its pages (word) |
| `0x3AF875` | location the map points at |

A closed list keeps its items and cursor, so the cursor is logged only while
the phase reads open.

Open: the place values of the beaches and the other venues; the day number;
the items screen's scrolling past its twelve visible cells; the gift's
recipient.
