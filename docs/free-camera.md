# Free camera

The free camera takes over the game's camera and flies it anywhere in the
scene, for screenshots or a closer look at a model. It is off by default. Tick
**Free camera** in the launcher, or set `RECOMP_FREE_CAMERA=1` before the
runner starts. Then press **F1**, or hold **Back** and click the right stick,
to switch the free camera on and off in any scene.

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
| Freeze / unfreeze the scene | P | Start |
| Hide / show the overlay while frozen | H | Left shoulder |
| Advance one frame while frozen | . (period) | Right shoulder |
| Previous / next animation | [ / ] | D-pad left / right |
| Next character to animate (the camera turns to her) | `\` (backslash) | Left stick click |

Switching the free camera off also unfreezes the scene, hands every
animation back to the game, and the game carries on where it stopped. Music keeps playing while frozen; the scene's own sounds
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

Not yet verified: mouse look, freezing and animation select in a play test,
animation select outside the demo, physical controller flight, scenes beyond the demo, the island map and Exhibition matches (the
hotel, the shops, the pool), and Windows and Linux builds.

## Debugging

`RECOMP_FREE_CAMERA_TRACE=<frame>` logs every camera build from that frame
on, up to 600 lines, with the object, whether the free camera drove it, and
the game's own eye, target and field of view.
