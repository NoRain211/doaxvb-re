# Swimsuit unlock

An opt-in cheat. With **Unlock swimsuits** ticked in the launcher (or
`RECOMP_UNLOCK_SWIMSUITS=1` set before the runner starts), the Collection of
the girl being played holds every swimsuit she can wear: 307 of the game's
328, the rest being the other girls' three gift-only suits each. It is off by
default, and with it off the runtime changes nothing.

## Behavior

- The suits are listed under **View Collection** in the hotel at night. **Take
  out** moves one to the inventory, where it can be worn; **Keep** puts it
  back.
- Only the girl being played is affected. Partners keep their own Collections,
  so they do not start wearing suits nobody gave them.
- Lent suits are not written to the save, neither the Collection entries nor
  the plain suits taken out of it. Turning the option off and loading the save
  shows the real Collection and inventory again.
- A lent suit that the girl is wearing, or that has been wrapped as a gift, is
  saved, because the game refers to it afterwards.
- A suit she holds in her inventory is not lent while it is there, so the
  game still lets a bought suit be kept.
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
| `0x9D77E8` | Save image, `0x2178` bytes; the ownership table is part of it |

An ownership word gives each girl two bits, at twice her inventory index. The
low bit says she has had the item, the high bit that it is in her Collection.
An item definition holds its type (2 for a swimsuit) and, at offset `0x10`,
the mask of girls who can wear it. Swimsuits are ids 1 to `0x148`.

An inventory slot holds the item id (`0xFFFF` when empty), two model handles,
a state byte (0 plain, 2 worn, other values wrapped), the acquisition order, a
wrap item, a pointer to the item definition and a counter. The save image
keeps 8 bytes of each slot, packed by `0x000E6BA0`.

`unlock_swimsuits.c` sets both bits for every wearable swimsuit the girl holds
in neither place and remembers which bits it set. When the game clears a lent
Collection bit, the suit was taken out, and its slot is pointed at a private
copy of the item definition. The game rewrites that pointer whenever it loads,
clears or fills a slot, so the pointer alone marks a lent suit.

## Seams

- `recomp_d3d_swap_adapter` calls `recomp_unlock_swimsuits_frame` once per
  presented frame.
- The `XcSHAUpdate` adapter in `kernel_crypto.c` calls
  `recomp_unlock_swimsuits_hash` when the game hashes a save image, the one
  point where that image passes through the runtime before it is written or
  after it is read. The pack routine and its callers are reached by direct
  calls, which a manual lookup cannot intercept; replacing them by hand is
  open work.
- The save hooks in `save_adapter.c` bracket the game's save routine with
  `recomp_unlock_swimsuits_saving`, so nothing is lent between the hash and
  the write.

## Checks

A scripted run on macOS against a copy of a save: **View Collection** listed
the suits over 13 pages and one was taken out. A save made afterwards from the
hotel had the same ownership table as before and no entry for the suit taken
out, and it loaded with the option off. Wearing a suit taken out, **Keep**,
the shops and a second girl's vacation have not been checked with the option
on.
