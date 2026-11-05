# Current local generation recipe

`recipe.json` pins the lifter revision and authenticates every generation input. The
builder checks this recipe hash before using it and checks every generated
file against the current proven local program before compilation.

The function shards contain addresses and analysis metadata. The 61 recovery
files must retain their listed order. Neither contains machine instructions or
game assets. Manual call targets preserve native adapter routing.

The wide-string comparison target routes both direct callers through the native
CRT adapter. This preserves equality when validating a profile name before
deletion; the split generated comparator lost the flags for its equal return.
Regeneration changes only those two call sites.

The casino recoveries follow the Radio input. They restore the Blackjack, Poker,
and Slots handlers and the split bodies that crashed in play. The stub-class
recovery also joins the same split defect in twelve other functions, including
D3D and DirectSound code; only 0x60DC0 and 0x13DF80 were crash targets. Three
earlier inputs also change: the r82 tail gap starts at the Poker double-up
callback (0x4B460), the r83 gap at the fade initializer it overlapped (0x34EE0),
and the r329 set joins 22 casino callback fragments into five bodies.

The hotel input, last in the list, extends two callbacks, 0xD8160 and 0xDE700,
to their final return. The lifter ended 0xDE700 at 0xDE7B8, which stopped the
game when a gift tape was played.

The lifter's casino flag corrections also change 40 generated functions
outside the recovery ranges, including D3D, DirectSound, WMA decoder, and XPP
controller code. All four casino games were played before the hotel input,
which changes only those two bodies, and gift tapes play with it. Volleyball,
the pool, Radio music, and save and load still need local retesting.

The cross-block carry correction changes 17 generated functions and adds only
carry writes. Two are the island map's name comparators (0xF2160, 0xF2250),
which never returned "less" and so sorted wrongly and slowly.

The Radio recovery input restores the input-history owner and the reached Radio
Station callback. These bodies match the locally tested program;
the expiry correction keeps comparisons and their branches in the same body.

The Rest recoveries supply a missing callback and its two dependencies, and join
each of two reached predicates' true/false returns into one owner. Other generated
bodies remain unchanged. The corrected Take a Rest variants still need local testing.

The setup suite generates synthetic expiry logic with this recovery and checks
that the input-history comparisons and branches share one body, and that the
Radio callback retains its full range. It also exercises both local returns of
both Rest predicates. It requires no game files. Run:

```sh
git submodule update --init tools/xboxrecomp
python -m pip install capstone==5.0.9
python -m unittest discover -s tools -p 'test_*.py'
```

The lifter is the `tools/xboxrecomp` submodule at fork revision
`4500b88f25275cfcbc6c413842d9f84e102333e4` (branch `codex/doaxbv-recipe`): upstream
`32da23872a552b12b4a932c9d5a6e952bb3f24bb` plus the preserved working local lifter,
including the explicit x87 destination correction used by camera
calculations, the cross-block carry fix, the jump-table slot-one retry and the
LOOP/LOOPE/LOOPNE lift. It also gives switch arms that fall outside their split
owner their own `jump_table_arm` entry, such as the collection screen exit (#64).
The x87 transcendentals lift as calls to `recomp_sin` and the rest, declared in
`recomp-runtime/portable_math.h` and defined by the `recomp_portable_math`
library that `recomp-runtime/CMakeLists.txt` builds from musl
(`recomp-runtime/musl_math/`), so every host computes the same results.
It is not an upgrade to the latest upstream release:
the fork's `main` follows upstream, and pinning it froze player movement
animation, broke an island map texture and sent the hotel to the pool scene.

Changing the recipe requires regenerating from the supported user-owned XBE,
reviewing output differences, and validating the affected player-visible flows.
Do not update expected hashes merely to make a mismatched build pass.

`tools/update_lifter_pin.py --imported private/<import>` does the mechanical
part: it proves the current pin still reproduces the recipe, checks out the
target revision (default: the fork's `codex/doaxbv-recipe`), regenerates, rewrites the recipe,
`build_game.py` and `public-export.json` identities, stages the submodule and
writes `changed-functions.txt` beside the new program. It does not commit.
