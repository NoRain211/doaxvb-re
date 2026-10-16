# Free camera

The free camera takes over the game's camera and flies it anywhere in the
scene, for screenshots or a closer look at a model. It is off by default. Tick
**Free camera** in the launcher, or set `RECOMP_FREE_CAMERA=1` before the
runner starts. Then press **F1**, or hold **Back** and click the right stick,
to switch the free camera on and off in any scene.
The setting also adds MODEL VIEWER to the main menu, which sets up a scene
with one girl for the free camera; see [`model-viewer.md`](model-viewer.md).

While it flies, the game reads a neutral controller on the first port, so
the game ignores what moves the camera. The game itself keeps running until
you freeze it.

| Action | Keyboard | Controller |
|---|---|---|
| Move | W A S D | Left stick |
| Down / up | Q / E | Left / right trigger |
| Look | Arrow keys, or drag with the right mouse button | Right stick |
| Zoom in / out | Z / X | D-pad up / down |
| Faster / slower | Shift / Ctrl (held) | A / B (held) |
| Back to the game's view | R | Y |
| Next camera | C | X |
| Pause / resume the game (P works anywhere, camera on or off) | P | Start (while flying) |
| Clean view: hide the panel and the game's overlay | H | Left shoulder |
| Advance one frame while frozen | . (period) | Right shoulder |
| Previous / next animation; hold to scroll, faster the longer held, and let go to play | [ / ] | D-pad left / right |
| Previous / next swimsuit on the chosen character | - / = | Back + d-pad down / up |
| Screenshot (anywhere; the controller while flying) | F12 | Back + A |
| Next character to animate (the camera turns to her) | `\` (backslash) | Left stick click |

While the camera flies, a panel at the top left names the chosen character,
the animation playing on her (the game's own, or the held clip as its place
in the scene's list), her swimsuit and whether the scene is frozen, and lines along
the bottom list the controls: the keyboard's after a key was last used, the
controller's after a controller was (and from the start on a Steam Deck).

P pauses the game in any scene even without the free camera; a small PAUSED
panel says so, and . and H work as above. Start pauses only while the camera
flies, since the game uses it. Switching the free camera off keeps a pause
and hands every animation back to the game; P resumes, and the game carries
on where it stopped. Music keeps playing while frozen; the scene's own sounds
stop.

Linux and macOS save the launcher choice in `private/launcher.json` as
`free_camera`, and `run_game.py` turns it into the variable. The Windows
launcher sets the variable itself.

## How it works

The game keeps five camera objects of `0xAE0` bytes from `0x41A800`. Each
frame the game-owned function at `0x00023B10` builds one, from the object in
EAX: it reads the eye at `+0x180`, the target at `+0x190` and the vertical
field of view in radians at `+0x1B0`, and writes the view and projection
matrices at `+0x50` and `+0x90`. The frame's draws, vertex programs and
culling all use those matrices.

`recomp-runtime/free_camera_adapter.c` is the manual lookup for
`0x00023B10`. While the free camera flies, it stores its own eye, target and
field of view in the chosen object, lets the generated build run, then
restores the game's values. The game's camera logic carries on undisturbed,
and the view returns to it when the free camera is switched off. The fly
camera itself is a plain model in `recomp-runtime/free_camera.c`, tested by
`free_camera_test.c`.

The main view is not always the same object. The real-time demo builds only
the first. The island map builds the first for the map and three more,
including the two portrait cameras. The free camera starts on the first, and
**Next camera** moves it to the next object the game built in the previous
frame.

The game's world is right-handed with +Y up: looking down +Z, +X is on the
left. The camera starts where the game's camera was. It moves at half the
game's eye-to-target distance per second, and keeps the target that distance
ahead, in case the game reads it as a focus distance.

## Freezing the scene

The game runs its systems as cooperative tasks, one turn each per frame on
its own fiber. The task table at `0x5F2C38` holds 32-byte records: `+0` the
state, `+4` set once the task has yielded, `+8` its fiber and `+0xC` its
function. The scheduler stores the index of the task it is about to run at
`0x5F2E5C` and switches to its fiber; the task switches back when its turn is
over. Only some tasks draw, and which ones depends on the scene: the island
map draws from `0x00079D00` and `0x0008A970`, the real-time demo from
`0x000B42B0` and `0x0008A970`. The game's own camera, and so the free
camera's build, runs inside a drawing task.

`recomp-runtime/scene_freeze_adapter.c` installs a hook in the fiber
adapter's `SwitchToFiber` replacement. It counts the draws each task issues
per turn; the plain model in `scene_freeze.c`, tested by
`scene_freeze_test.c`, keeps the last eight turns of each task. While frozen,
a turn of a task that has not drawn in that history is declined: the switch
returns at once and the record is left as a yield leaves it, with bit `0x10`
of the state clear and `+4` set. The drawing tasks keep their turns, so the
last game state is drawn again every frame from wherever the free camera is.
Hiding the overlay declines the turns of `0x0008A970`, which draws the 2D
text, portraits, menus and the demo's logo. In a match the score is drawn by
the scene's own task, so it stays. A frame step lets every task take its turns
until the next frame is presented, then holds the scene again.

The game polls the controller from a task the freeze skips, so while frozen
the free camera samples the controller itself once a frame, from the freeze's
own hook in the scheduler. That works in any scene, including 2D screens with
no camera, so a frozen scene can always be thawed.

## Choosing an animation

A press of `[` or `]` (d-pad left or right) moves to the previous or next
clip; holding it keeps moving, one clip every 60 ms after the first 350 ms,
3 at a time after 1.5 s and 10 at a time after 3.5 s. The panel shows the
clip being looked for, and the clip plays only when the key is let go.

Up to four characters animate at once; `0x4D6FD0 + n` is nonzero while
character `n` is active. Each has two animation layers of `0x80` bytes, the
first at `0x5E0800 + n * 0x100`. Each frame `0x000AC100` updates a
character's layers. When the first layer's byte `+0x74` is set, it clears it
and calls `0x000AEC80`, which starts the clip numbered at `+0x62`. Bit
`0x2000` of the number picks a shared bank; otherwise the layer's byte
`+0x6E` picks the character's bank. The bank table at `0x5DD844` has 12-byte
entries whose first word is the bank, the shared bank fifth. A bank is 1000
clip pointers, and a clip the scene did not load points at a placeholder
`0xFA4` bytes into the bank.

`recomp-runtime/actor_pose_adapter.c` lists the clips the scene loaded for
the chosen character and the shared ones (the plain model in
`actor_pose.c`, tested by `actor_pose_test.c`), and starts the chosen one the
same way the game does. It then holds it: a clip the game's own logic starts
in its place is replaced again. A clip carries its own root position, so the
character's position at `+8` of its root block, `0x4D6FF0 + n * 0xBC`, is put
back each frame while a clip is held. Many clips also move the body away from
the root, so the adapter wraps the skeleton build at `0x000B01E0` as well.
It writes 32 row-vector world matrices of 64 bytes at `0x4D3650 + n * 0x800`,
the first being the hips. After a held character's skeleton is built, every
bone is moved across the ground so the hips stay where they stood when the
hold began; height is left alone, so sitting and standing clips keep their
own. The character's shadow is not moved with her and can sit apart.

Choosing a character turns the free camera to face her hips. A scene can
have active characters that are not shown, such as the second one in the
demo's first scene; choosing one of those turns the camera to wherever the
game keeps her.

Only clips the scene loaded can be chosen: 45 for the girl in the demo's
first scene, including 3 shared ones. The clips have no names, only numbers,
which the log reports (`recomp pose: character 0 clip 02b0 (36 of 45)`).

## Changing the swimsuit

`-` and `=`, or Back with the d-pad's down and up, put the chosen
character's previous or next swimsuit on her, in a running or frozen scene.
The picture holds for the few frames the load takes, then the scene goes on
with her in the new suit and the panel names it.

The game loads characters through a loader task, `0x000A0530`. `0x000A2060`
asks it for a suit (EAX the suit, EBX the character number at `+1` of her
root block, the slot on the stack) and `0x000A2260` lets a slot's suit go.
The request record at `0x5DEBD0 + n * 16` holds the phase at `+0`, `0xFF`
when idle, and the loaded suit at `+4`; once the suit has been read the
loader binds it to the character (`0x00062A70`) and sets both. The game only
does this between scenes, because a suit loads into the same memory as the
one it replaces: nothing may draw her while it loads.

`recomp-runtime/suit_switch_adapter.c` starts the switch in the camera build
the free camera already wraps, in the middle of a drawing task's turn.
Letting the old suit go only marks its memory free, so the rest of that turn
still draws it intact. From then on the scene freeze runs only the loader
task and Swap shows nothing new, so the last picture stays on screen. When
the record shows the new suit loaded, the scene waits one more frame and
runs again. Direct3D still holds the last texture on each stage and the last
vertex buffer on each stream, pointing into the reloaded memory, so they are
cleared first without dropping their references (`0x1F2978 + 0xB38`,
`0x1F2E28`): a seam into the game's statically linked Direct3D, to go when it
is replaced wholesale. The phases are in `suit_switch.c`, tested by
`suit_switch_test.c`.

Every swimsuit item can be chosen: the item table at `0x3003E8` points at
28-byte records whose first byte is 2 for a swimsuit, and each record's
wearer mask at `+16` allows every girl. The names come from `0x3095F8`, one
pointer every 8 bytes per item, Japanese first and then each other language,
indexed by the language byte at `0x4D56D9`; the panel shows the English name
for Japanese, which its font lacks.

## Verification

Scripted runs on an M-series Mac flew the camera in the real-time demo and on
the island map, and a user play test on the Mac confirmed the camera flies.
Back with the right stick switched it on, and the frames showed the view
leaving the game's own. Turning and strafing right moved the
scene left in the frame. **Next camera** moved it to the island map's second
object. Each run used a constant debug input, `RECOMP_FREE_CAMERA_DRIFT`
(right, up, forward, turn and pitch, comma-separated), because scripted input
cannot move a stick.

Freezing was checked the same way. In the demo, Start froze the scene with
the girl on screen; with the camera held still, frames 120 apart matched
except for the sea, and turning the camera showed the frozen scene from new
angles. Switching the camera off thawed it and the demo went on to its next
scene. On the island map the frozen dialog stayed put, the overlay hid, and
the camera flew down to the beach. A recording of the sound through a freeze
showed the music going on at a lower level with no repeating buffer.

In a CPU Exhibition match, the freeze held the ball and a jumping player in
mid-air while the camera flew around them. A frame step changed the frozen
picture once and it then held again. Start unfroze the match and it went on.
On the title screen, a 2D scene with no camera, Start froze and unfroze it.

Animation select was checked in the demo's first scene: each press of the
d-pad started a different clip on the girl (bending, walking, standing,
resting a hand on her knee) and the scene's script did not take it back.
With the scene frozen, a press changed her pose once and it then held.
Across four clips her hips stayed in the middle of the view, where before
the hips were kept only through the root they drifted to the edge. Choosing
her with the left stick click, after the second character, turned the
camera back to her.

The Windows drawer has been built with llvm-mingw but not yet run.

Swimsuit changes were checked in a CPU Exhibition match, where the first
player's suit changed twice while the match played on, and in the model
viewer, where frozen Kasumi went from Tulip to Lily, Hyacinth and Olive in
the same pose with the panel naming each. One change in the real-time demo
loaded and bound with no fault. Nothing happened on the island map or in the
hotel room, where no character is active. A scripted debug setting,
`RECOMP_SUIT_SWITCH_AT=<frame>[,<frame>...]`, changes the first character's
suit at those frames.

Not yet verified: mouse look, freezing, animation select and swimsuit changes in a play test,
swimsuit changes in vacation scenes with a character in them (the beach, the pool),
animation select outside the demo, physical controller flight, scenes beyond the demo, the island map and Exhibition matches (the
hotel, the shops, the pool), and Windows and Linux builds.

## The overlay

The panel is plain text in `recomp-runtime/overlay_text.c` (tested by
`overlay_text_test.c`), which the free camera fills and hands over once a
frame; a presenter, on Windows on its own thread, takes a copy under a short
lock. `overlay_imgui_panel.cpp` lays it out in Dear ImGui. On macOS and
Linux, `overlay_sdl_gpu.cpp` draws it with ImGui's SDL GPU backend onto the
window after the game's frame is copied there; on Windows,
`overlay_d3d11.cpp` draws it with ImGui's D3D11 backend into the swap
chain's back buffer, after the front buffer is copied. Neither draws into a
buffer the game reads back. In a
match the character's name is read from the game: character `n` is player
`n`, whose choice at `0x9DAA68 + n * 0xB8` holds her character number at
`+1`, and the English names are pointed to from `0x3245BC` by that number.

## Screenshots

F12 saves the game's frame, without the runtime's panel, as a PNG in
`Pictures/DOAXBV` in your home folder, or in `RECOMP_SCREENSHOT_DIR` when it
is set; the log names the file. H first hides the game's own overlay too.
The presenter takes the request when it presents: the SDL GPU one downloads
the frame it rendered, and the D3D11 one copies the window's back buffer
before the panel is drawn on it. `recomp-runtime/screenshot.c` writes the
PNG itself, uncompressed, so no image library is needed (tested by
`screenshot_test.c`). A 720p screenshot is about 2.7 MB. The Windows path
is built but not yet run.

## Debugging

`RECOMP_FREE_CAMERA_TRACE=<frame>` logs every camera build from that frame
on, up to 600 lines, with the object, whether the free camera drove it, and
the game's own eye, target and field of view.
