# Decompilation progress

This page tracks how much of the program runs hand-written source in place of
generated code. Progress is measured the way decompilation projects measure
it, adapted to a recompilation that is replaced function by function instead
of byte-matched.

- **Units** in [`tools/decomp/units.json`](../tools/decomp/units.json) split the
  executable into game code and libraries. Game code is the part of `.text`
  before the XDK libraries start. Libraries are the rest of `.text` and the
  XBE's library sections (D3D, DSOUND, WMADEC, XPP and others).
- **Functions and sizes** come from the generation recipe's function bounds
  and recoveries, the same list the lifter translates.
- **The ledger** in [`tools/decomp/functions.csv`](../tools/decomp/functions.csv)
  lists every guest function that a manual lookup routes to runtime source,
  with its status and the file that holds it.

Game code is decompiled into readable source. Library code (D3D, DirectSound,
XAPI, the C runtime, CRI middleware) is replaced wholesale at its API; the
same ledger tracks both, and the report keeps them apart.

## Statuses

| Status | Meaning |
|---|---|
| `replaced` | Hand-written code runs and the generated body never does. This is the coverage that counts. |
| `wrapped` | Hand-written code runs but still calls the generated body: a hook, a probe or a library seam. |
| `optional` | Hand-written code exists but runs only behind a diagnostic or opt-in setting. |

Functions without a ledger row run generated code.

## Keeping it current

Every change that adds, removes or rewrites a manual lookup updates the ledger
in the same commit and refreshes the report below:

```powershell
python tools/decomp_progress.py
```

`python -m unittest discover -s tools -p "test_*.py"` runs
`tools/test_decomp_progress.py`, which fails when a ledger row names a missing
source file, an address no runtime source mentions, an address outside every
unit, or a `replaced` function whose generated `sub_` body the runtime still
calls.

## Current report

Percentages are shares of each unit's bytes.

| Unit | Kind | Functions | Bytes | Replaced | Wrapped | Optional |
|---|---|---:|---:|---:|---:|---:|
| game | game | 10100 | 1442226 | 1 (0.00%) | 10 (0.42%) | 12 (0.18%) |
| **all game** | game | 10100 | 1442226 | 1 (0.00%) | 10 (0.42%) | 12 (0.18%) |
| xdk-text | library | 4184 | 385819 | 15 (0.63%) | 9 (0.21%) | 0 (0.00%) |
| D3D | library | 522 | 52977 | 21 (4.76%) | 3 (1.46%) | 0 (0.00%) |
| D3DX | library | 4 | 752 | 0 (0.00%) | 0 (0.00%) | 0 (0.00%) |
| XGRPH | library | 17 | 817 | 0 (0.00%) | 0 (0.00%) | 0 (0.00%) |
| DSOUND | library | 674 | 36738 | 7 (0.85%) | 9 (0.72%) | 0 (0.00%) |
| PSGSFD | library | 69 | 17605 | 0 (0.00%) | 0 (0.00%) | 0 (0.00%) |
| WMADEC | library | 431 | 46458 | 8 (1.31%) | 0 (0.00%) | 0 (0.00%) |
| XPP | library | 541 | 29736 | 9 (2.28%) | 0 (0.00%) | 0 (0.00%) |
| **all library** | library | 6442 | 570902 | 60 (1.15%) | 21 (0.32%) | 0 (0.00%) |

Ledger entries without recipe bounds (counted with no bytes): 0x000C9CC0, 0x0018322D, 0x00189CB0, 0x0021926C

The `xdk-text` unit holds the XAPI, C runtime and CRI code linked into
`.text` and is not split by library yet.

## Game clusters

Game code is replaced in clusters that own their data, following the
decompilation loop in `AGENTS.md`.

| Cluster | State | Notes |
|---|---|---|
| Music player (`0x000B8C70`-`0x000BA4AA`) | wrapped | `Music_Play` and `Music_Update` carry the radio shuffle. |
| View Collection (`0x000C9CC0`) | replaced | Task init the lifter could not find (#64). |
| Controller settings (`0x0001B340`, `0x000E8100`) | wrapped | PC control-mode preference. |
| Profile saves (`0x0001ADB0`, `0x000E0410`) | wrapped | Save transactions around the original writers. |
| Title idle (`0x0006B290`, `0x000696D0`) | wrapped | CPU Exhibition and demo cut hold. |
| Animation curves and pose (`0x000AEA60`-`0x000B01E0`) | optional | See [the high-frame-rate research](wayfinder/research/high-frame-rate-split.md). |
| Volleyball match | mapped | See [volleyball](volleyball.md). |

