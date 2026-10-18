# Swimsuit unlock

An opt-in cheat. With **Unlock swimsuits** ticked in the launcher (or
`RECOMP_UNLOCK_SWIMSUITS=1` set before the runner starts), the Collection of
the girl being played holds every swimsuit she can wear: 307 of the game's
328, the rest being the other girls' three gift-only suits each. It is off by
default, and with it off the runtime changes nothing. A suit taken out while
it is on stays in the save for good, also after the option is turned off.

## Behavior

- The suits are listed under **View Collection** in the hotel at night. **Take
  out** moves one to the inventory, where it can be worn.
- A suit taken out is the player's from then on. It is saved with the rest of
  the inventory, and **Keep** puts it in the saved Collection.
- The Collection entries that were only lent are not written to the save.
  Turning the option off and loading the save shows the Collection the player
  built, plus whatever was taken out.
- In **Exhibition**, the swimsuit picker on the character select screen
  offers every girl all the suits she can wear.
- In a vacation only the girl being played is affected. Partners keep their
  own Collections, so they do not start wearing suits nobody gave them.
- A suit she has held in her inventory is not lent again until the vacation
  ends or another girl is played, so the game still lets a bought suit be
  kept.
- The shops report a lent suit as already in the Collection.

## Guest contract

Addresses are for the verified USA executable.

| Address | Meaning |
|---|---|
| `0xB5B244` | Pointer to the ownership table, 708 words indexed by item id |
| `0x317634` | Pointer to 8 inventories of 51 slots, 16 bytes a slot |
| `0x3003E8` | Table of pointers to 28-byte item definitions, indexed by item id |
| `0x9D9939` | Character id of the girl being played |
| `0x2A7B00` | Character id of each of the 8 inventory indices |
| `0x5DECE0` | Scene byte, 1 during a vacation |
| `0x9DAE08` | Exhibition's ownership table, same shape, rebuilt by `0x000F82F0` |
| `0x9DB3C8` | Eight words counting each girl's suits in that table |
| `0x9D77E8` | Save image, `0x2178` bytes; the ownership table is part of it |

An ownership word gives each girl two bits, at twice her inventory index. The
low bit says she has had the item, the high bit that it is in her Collection.
An item definition holds its type (2 for a swimsuit) and, at offset `0x10`,
the mask of girls who can wear it. Swimsuits are ids 1 to `0x148`. An
inventory slot starts with the item id, `0xFFFF` when empty.

`unlock_swimsuits.c` sets both bits for every wearable swimsuit the girl holds
in neither place and remembers which bits it set. A lent Collection bit that
turns up clear was either taken out or lost to a reloaded table. Either way
both its bits are forgotten, so a had bit the save holds is kept. A suit found
in the inventory has both its bits forgotten, so they are saved, and the slot
the game filled is left as it is.
Both cases mark the suit held, and a held suit is not lent again until the
vacation ends or another girl is played. Keep and Take out can move a suit
between the inventory and the Collection over two frames, and a suit lent in
between would be saved in the wrong place.

Exhibition reads a second table that `0x000F82F0` fills from the default
suits and all three save files. It is not part of any save, so the unlock
marks every wearable suit in it each frame and writes the matching counts.

## Seams

- `recomp_d3d_swap_adapter` calls `recomp_unlock_swimsuits_frame` once per
  presented frame.
- The `XcSHAUpdate` adapter in `kernel_crypto.c` calls
  `recomp_unlock_swimsuits_hash` when the game hashes a save image, the one
  point where that image passes through the runtime before it is written or
  after it is read. The remembered bits are cleared from the live table
  there; nothing else in the image is touched.
- The save hooks in `save_adapter.c` bracket the game's save routine with
  `recomp_unlock_swimsuits_saving`, so nothing is lent between the hash and
  the write.

## Checks

`unlock_swimsuits_test.c` runs the lending, take-out, Keep and save paths on
synthetic guest memory.

A scripted run on macOS against a copy of a save, made before taken-out suits
were kept: **View Collection** listed the suits over 13 pages, one was taken
out, and a save made afterwards from the hotel had the same ownership table as
before and loaded with the option off. In Exhibition the picker went from one
suit to the full list and a match was played. A save holding a suit taken out,
**Keep**, the shops and a second girl's vacation have not been checked with
the option on.
