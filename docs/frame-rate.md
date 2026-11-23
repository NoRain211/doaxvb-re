# Frame rate (D3D11)

The launcher's Frame rate choice (Game page) sets `RECOMP_SPLIT_RATE`.
Gameplay, physics and sound always keep the game's 60 Hz tick; above 60 the
render worker presents each tick several times, interpolating the camera,
characters, the ball and rigid props, one tick behind (see
[the split-rate notes](wayfinder/research/high-frame-rate-split.md)).

| Choice | Value | Presents |
| --- | --- | --- |
| 60 FPS (original) | unset | The game's own 60 Hz path |
| 120 FPS | `120` | 120 per second; even only on a 120 Hz or VRR display |
| Match display refresh | `auto` | The primary display's refresh rate |
| Unlocked | `unlocked` | As fast as the GPU allows (up to 1000), with tearing |

`unlocked` presents through the VRR path (`DXGI_PRESENT_ALLOW_TEARING`, no
vblank wait) whatever the VRR toggle says. Where the GPU or driver does not
support tearing, it still presents without waiting for vblank, and the display
shows the newest finished frame at each refresh; the run log says which.

A tick never takes more presents than fit in it. The worker times a tick's
first present and its later ones separately and spaces presents evenly by how
many fit in 80% of a tick, never faster than the chosen rate; it also stops
early when a present would end past the tick, or when the game has already
queued the next tick. Without this, a slow
present delayed the next packet, the game waited on the full queue, and play
ran in slow motion (45 ticks per second) instead of at fewer frames.

With sun shadows on, the first replay of a tick draws the sun map and the later
ones reuse it, its projection and caster flags, so shadows move at 60 Hz while
the bodies casting them are interpolated. Each replay still selects the camera
so receivers match the interpolated view.

Observed 2026-11-01, RTX 4060 Laptop and i5-13420H, 144 Hz panel, DLSS
Quality, per-pixel lighting and smooth normals, CPU Exhibition court 2: sun
shadows off, `auto` presents 120 per second and `unlocked` 146 to 184, the game
at 60 ticks either way. With sun shadows on, a present costs about 8.5 ms there
and only one fits a tick, so it stays near 60; plain settings reach 144.
