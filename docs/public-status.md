# Public Status

## Fully playable (0.4 Beta)

0.4.5 Beta packages the same generation recipe with bundled Git and CMake and
an installation offer for missing Microsoft build tools. 0.4.6 Beta fixes
swimsuits, such as Diamond, whose fabric did not render (#62). 0.4.7 Beta
fixes the View Collection crash in the hotel room menu (#64) with a new
generation recipe. 0.4.8 Beta keeps that recipe and reads controllers through
SDL3, adding PlayStation 4/5 controllers, up to four controllers and vibration
(#67, #68, #70). Its PlayStation mapping passes a virtual-controller check and
an Xbox controller smoke run; no physical PlayStation controller or
two-controller match has been tested. 0.4.9 Beta fixes deleting a save (#71)
with a new generation recipe; its first start upgrades the save journal,
which earlier builds then reject. 0.5.0 Beta "Mixtape" adds custom
soundtracks from `UserMusic` with a new generation recipe (#73); 0.5.1 moves
that folder to `private/UserMusic` and adds a launcher button for it (#75);
0.5.2 adds a launcher volume choice that defaults to 100% (#76); 0.5.3 adds
Radio Station shuffle with a new generation recipe (#78). 0.6.0 Beta RC1, a
prerelease, adds the rendering fixes, texture replacement and opt-in high
refresh presentation in #79–#98 with a new generation recipe. #79, #92–#96
and #98 were each accepted in the user's play test of its own branch build;
RC1 itself (tag `v0.6.0-rc1`) has not had a full play session. None of
these releases expands the play-test coverage below.

The local build published as 0.4 Beta (tag `v0.4`, commit `3848a67`,
generated-program manifest `9558380…`) is fully playable. Long player
sessions with that generated program covered every stage, the Casino, Hotel
and Shops, saving and reloading, and the end of a vacation carrying into a new
one. They ran at a steady 60 FPS with no audio underruns and closed normally.
The vacation-end run started from an edited day-14 save. Days 11–13 are
accepted by inference from shared code, not a full natural run. See the
[playability decision](https://github.com/NoRain211/doaxbv-re/issues/16#issuecomment-5840313062).

Known issues are [#54](https://github.com/NoRain211/doaxbv-re/issues/54)
(second Hopping Game playable before it loads),
[#55](https://github.com/NoRain211/doaxbv-re/issues/55) (upside-down map flash
leaving the pool) and [#56](https://github.com/NoRain211/doaxbv-re/issues/56)
(possibly missing underwater textures). Formal acceptance of every offline row,
and original lighting and camera fidelity, remain open in #16.

## Public tree

The public tree proves that the tracked runtime, model, and adapter tests build
without private game input. It does not prove that the game boots, reaches a
menu, renders correctly, or is playable.

The active implementation includes:

- guest memory and register dispatch;
- kernel/device models, checked RAM aliases and fiber-stack recycling;
- controller mapping, scripted input handover and file-backed input pulses;
- transactional saves with interrupted-write recovery;
- owned native PCM/ADPCM audio output and cooperative movie service;
- a `UserMusic` MP3/WAV/FLAC catalog and native PCM decoder, with local
  codec/packet checks; natural menu selection and audible playback await a
  named-build play test (see [native audio](native-audio.md#custom-soundtracks));
- D3D8 model/adapter seams, render targets, mutable textures and D3D11 VSync flip presentation;
- paired monotonic movie timing, color conversion and native SSE helpers;
- XBE parsing and hashing for authenticated local runners.

Generated game source, private execution receipts, frozen internal reference
code, research archives, and extracted symbol datasets are intentionally not
part of the initial public export.

Progress claims must identify a natural observed event and the exact local
build identity. Public CTest success is regression evidence, not gameplay
progress.

The local runtime has separately passed representative save/reload (#17),
audible output (#15) and movie playback (#19) gates. Movie natural endings
return to title or character selection, and permitted skips repeat. Boot
delivery measured approximately 29.4 and 29.6 fps with stable captured audio
timing. Occasional frame drops, broader rendering gaps and untested offline
routes remain. These local results depend on authenticated generated inputs;
this source export does not bundle those inputs or make the pinned lifter alone
sufficient to reproduce the accepted game run.


The release branch also includes the local portrait, compressed-mipmap and
60 Hz gameplay-pacing changes. A pipeline-cache regression that dropped
character geometry after portrait rendering is fixed: new layouts replace
old cache entries, and evicted layouts rebuild when needed. A native
Exhibition run verified all four selection portraits and character geometry
in a match and closeup. Lighting remains incomplete; this does not establish
all character, outfit, court or offline-mode parity. See the
[verified progress report](https://github.com/NoRain211/doaxbv-re/issues/16#issuecomment-5594275984).

New and older saves default to Digital unless an explicit PC control-mode
preference exists; later Analog and Digital choices are remembered. See [controls](recomp-controls.md) for tested mappings
and limitations. Stop logs distinguish normal exits, diagnostic boundaries,
runtime errors and unhandled host crashes; an expectation-adjusted exit code
alone is not a gameplay result. See [log interpretation](../recomp-runtime/README.md#reading-stop-and-crash-logs).

The latest source update includes reflective item previews, virtual disk/cache
queries, controller routing fixes, and authenticated variable chunk enumeration.
Two natural local store runs restored visor surfaces and lotion previews;
the final guarded candidate passed nine runtime tests. Inventory equip/use,
full lighting fidelity, camera parity and the remaining offline matrix rows
remain unproven. See the [preview evidence](https://github.com/NoRain211/doaxbv-re/issues/28#issuecomment-5647654899)
and [acceptance dashboard](https://github.com/NoRain211/doaxbv-re/issues/16#issuecomment-5647656786).

The pinned rollback dependencies and their seal repair are available as source.
They are not linked into the game runtime; see the
[dependency audit](research/rollback-engine-audit.md) for limitations and checks.

The 0.3 Alpha source and recipe include subsequent inventory action-text,
picture-setting, activity-transition and pool-rendering repairs. Daytime water
shows its floor and reflections in two independent natural runs; one corrected
manual dusk run reaches gameplay and the retry screen with positive feedback.
The dusk run ends normally at its supervision limit, so it does not prove map
return or the complete activity row. Character/platform shading and shader-cache
reuse have bounded evidence. See the [water progress report](https://github.com/NoRain211/doaxbv-re/issues/16#issuecomment-5656165854).
All offline acceptance rows and original rendering parity remain subject to #16.

The current source recipe also preserves the recovered Radio Station entry and
input-history expiry correction. A manual check of local candidate r484 reached
Radio Station. Two controls using local candidate r501 reproduced nine delayed
rally-start crouches after the action button was released; two corrected matches
using local candidate r504 completed fifteen rallies without recurrence, retained
fresh serving, and returned naturally to the map.
These were muted scripted-controller comparisons using identical initial save
copies. See the [controller comparison report](https://github.com/NoRain211/doaxbv-re/issues/16#issuecomment-5708858572).
The 0.35 Alpha retains the input-history and Radio repairs, and adds bounded
activity recoveries alongside the tested local glare correction and FPS logging.
Local package tests found two split Rest predicates. Both now retain their
true/false returns and have synthetic generation coverage. The reported Tina
midday branch also fails in an isolated test of the preserved r504 executable;
the earlier successful Rest run covered a different predicate. Matching build
inputs did not establish coverage of all Rest variants. In the R4 local test,
the user reports Rest, Hopping Game and midday Jungle completed; dusk Jungle
failed. A diagnostic repeat connected the crash to guest allocation exhaustion
before sound-buffer creation. Freed allocation reuse is repaired and covered by
regression tests. In the corrected R6 runtime, retained unchanged for 0.35, the
player reports Jungle, Beach, Niki Beach, Private Beach, Take a Rest and Hopping
Game working. The approximately 27-minute session closed normally, with final
performance samples at 60 FPS. The player also reports occasional 1–4 second
volleyball slowdowns; their cause and any allocation connection are unverified.
The earlier shop-exit crash and audio crackle have no new confirmation.
Other action outcomes, repeated court/time variants and full offline acceptance
remain open in #16.
