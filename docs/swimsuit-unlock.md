# Swimsuit unlock

An opt-in cheat. With **Unlock swimsuits** ticked in the launcher (or
`RECOMP_UNLOCK_SWIMSUITS=1` set before the runner starts), every girl's free
inventory slots hold the swimsuits of her own collection that she does not
own. It is off by default, and with it off the runtime changes nothing.

## Behavior

- The suits appear wherever the game lists a girl's inventory, such as
  **View items**, and can be worn.
- They are not written to the save. Turning the option off and loading the
  save shows the real inventory again.
- A granted suit that a girl is wearing, or that has been wrapped as a gift,
  is saved, because the game refers to it afterwards.
- The game gives each girl 51 slots and its own add routine uses the first
  48, shared by swimsuits, accessories and other items. The unlock uses those
  48 and keeps three free so shops and gifts still work. A collection that
  does not fit is filled from the lowest item id; the largest collection has
  54 suits, so it never fits whole.
- Discarding a granted suit does not free its slot for long: the next frame
  fills it again.

## Guest contract

Addresses are for the verified USA executable.

| Address | Meaning |
|---|---|
| `0x317634` | Pointer to 8 inventories of 51 slots, 16 bytes a slot |
| `0x3003E8` | Table of pointers to 28-byte item definitions, indexed by item id |
| `0x2758A4` | Each girl's default swimsuit id, the first of her group |
| `0x9D77E8` | Save image, `0x2178` bytes, packed from the slots by `0x000E6BA0` |

A slot holds the item id (`0xFFFF` when empty), two model handles, a state
byte (0 plain, 2 worn, other values wrapped), the acquisition order, a wrap
item, a pointer to the item definition and a counter. Swimsuits are ids 1 to
`0x148`.

`unlock_swimsuits.c` writes granted slots as the game's add routine at
`0x0009CEA0` would, except that the definition pointer leads to a private
copy of the definition. The game rewrites that pointer whenever it loads,
clears or fills a slot, so the pointer alone marks a granted suit.

## Seams

- `recomp_d3d_swap_adapter` calls `recomp_unlock_swimsuits_frame` once per
  presented frame.
- The `XcSHAUpdate` adapter in `kernel_crypto.c` calls
  `recomp_unlock_swimsuits_strip` when the game hashes its save image, the
  one point where that image passes through the runtime before it is written.
  The pack routine and its callers are reached by direct calls, which a
  manual lookup cannot intercept; replacing them by hand is open work.

## Checks

Scripted runs on macOS against a copy of a save: the item viewer listed the
granted suits and one could be worn; a save made with the option on kept the
same inventory entries as before and loaded with the option off. Shops, gifts
and other modes have not been checked with the option on.
