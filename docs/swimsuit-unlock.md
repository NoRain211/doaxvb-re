# Swimsuit unlock

An opt-in cheat for **Exhibition**. With **Unlock swimsuits** ticked in the
launcher (or `RECOMP_UNLOCK_SWIMSUITS=1` set before the runner starts), the
swimsuit picker on Exhibition's character select screen offers every girl all
the suits she can wear: 307 of the game's 328, the rest being the other girls'
three gift-only suits each. It is off by default, and with it off the runtime
changes nothing.

## Behavior

- Only Exhibition changes. Vacations, the hotel Collection, the shops, the
  inventories and the save files are left exactly as the game keeps them.
- A suit bought, kept, taken out, sold or thrown away during a vacation has
  no effect on the unlock: Exhibition still offers it next time.
- Turning the option off leaves nothing behind. Exhibition then offers the
  suits the saves hold, as before.

## Guest contract

Addresses are for the verified USA executable.

| Address | Meaning |
|---|---|
| `0x3003E8` | Table of pointers to 28-byte item definitions, indexed by item id |
| `0x9DAE08` | Exhibition's ownership table, 708 words indexed by item id |
| `0x9DB3C8` | Eight words counting each girl's suits in that table |

`0x000F82F0` rebuilds Exhibition's table from the default suits and all three
save files. It is not part of any save. An ownership word gives each girl two
bits, at twice her inventory index: the low bit says she has had the item, the
high bit that it is in her Collection. An item definition holds its type (2
for a swimsuit) and, at offset `0x10`, the mask of girls who can wear it.
Swimsuits are ids 1 to `0x148`.

`unlock_swimsuits.c` sets both bits for every girl who can wear each swimsuit,
keeps any bits already there, and writes the matching counts, once a frame.

## Seams

- `recomp_d3d_swap_adapter` calls `recomp_unlock_swimsuits_frame` once per
  presented frame.

## Checks

`unlock_swimsuits_test.c` marks the table on synthetic guest memory, checks
the counts and a bit kept from a save, and checks that the vacation ownership
table is unchanged.

A scripted run on macOS with an earlier build that also lent suits in
vacations: in Exhibition the picker went from one suit to the full list and a
match was played. This Exhibition-only build has not been played.
