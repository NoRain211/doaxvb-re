# Model viewer

With **Free camera** ticked in the launcher (`RECOMP_FREE_CAMERA=1`), the
main menu has a third item, **MODEL VIEWER**, under GO TO ZACK ISLAND and
EXHIBITION. It leads through Exhibition's own screens: the viewer picks One
Player, you choose the girl and her swimsuit as Player 1, the viewer accepts
the partner and opponents the game offers, and you choose the court. When the match has
started, the viewer takes it over:

- the free camera comes on in front of Player 1's girl,
- the scene freezes,
- the other three players and the game's on-screen overlay are hidden,
- a panel names her, shows the animation and swimsuit and lists the controls.

Every free camera control then works as described in
[`free-camera.md`](free-camera.md): fly around her, step or unfreeze the
scene, play another of the scene's animation clips on her, or change her
swimsuit with `-` and `=` (Back with the d-pad's down and up). Put the
free camera away (F1, or Back with the right stick) to leave: the scene is
unpaused, the match is quit and the game returns to the main menu.

## Time of day

On court select, `[` and `]` (or LT and RT) choose the time of day: **As the
court**, **Day**, **Sunset** or **Dusk**. The game bakes the time of day into
each stage, and Exhibition's ten courts are three places at dusk, day and
sunset, then one more beach by day. The viewer swaps the chosen court's stage
for its place's stage at the chosen time; a place without that time keeps the
court's own. Places and stages (`recomp-runtime/viewer_scene.c`, tested by
`viewer_scene_test.c`):

| Place (courts) | Day | Sunset | Dusk |
|---|---|---|---|
| Beach A (0-2) | 4 | 5 | 39 |
| Rock arch (3-5) | 6 | 7 | 40 |
| Jungle court (6-8) | 8 | 9 | 41 |
| Beach B (9) | 11 | 12 | - |

The court table at `0x2A7A40` holds six bytes a court, the stage first, and
the match loads the chosen court's stage from it. While court select is up
the viewer writes the swapped stages into the table, and puts the game's own
back once the viewer is done. A shifted system clock changes nothing, so the
time of day comes only from the stage.

## How it works

`recomp-runtime/viewer_menu_adapter.c` does the menu and the hand-over.

The menu scene runs one screen at a time from the table at `0x2A78B0`,
32-byte records of seven handlers and a data block; the main menu is screen
0. Its setup, `0x000F3B30`, creates the list widget at `0x5C57F8` through
`0x0008D800`, which takes the number of rows, and fills its items: for each
string number at `0x2A7A90` it appends the text, for the language in
`0x3A4C45`, from the string table at `0x31F7C8` to the item list at
`0x5C5860`, counting at `0x5C585C`. The widget draws each item in the game's
own font and button frames. After the setup the viewer makes the widget
again with three rows, through the same `0x0008D800`, and fills it with the
two items and its own, whose text is in a small pool block.

The screen's update, `0x000F3E40`, keeps the cursor in the screen's data
block at `0x9D9E0C`. Confirming item 0 goes to screen 1 and item 1,
Exhibition, to screen 2; any other item does nothing. When item 2 is
confirmed the viewer runs the update as if item 1 were chosen, so the game
plays its sound and leaves for Exhibition, and the viewer notes that it was
asked for. The cursor stays on Exhibition, because the menu hands its value
on as the mode to start; 2 would start a vacation.

Only Player 1's pick matters to the viewer. Character select keeps the pick
under way in its data block at `0x9DAA48`: `+0` the pick (0 to 3, Player 1
first) and `+8` the step within it (girl, swimsuit, attitude), 4 once the
court is being chosen. While the menu is on character select (screen 4, in
`0x9D9D80`), past the first pick and short of the court, the viewer presses
A on the first controller every 12 polls, so the game accepts the girl,
swimsuit and attitude it offers for the partner and both opponents.
Meanwhile only Player 1's column is shown. Character select draws its four
columns as 2D quads at fixed places in its 720 x 480 screen, Player 1's at x
56 to 202. The draw adapter's screen mask
(`recomp_d3d_draw_set_screen_mask`) leaves out every pre-transformed quad
drawn to the back buffer that lies wholly inside a given rectangle: x 200 to
720, y 60 to 362 while picking, and only y 60 to 250 once the court grid
fills the lower half. The last opponent's attitude list overlaps the court
grid, so it can be seen fading out for a moment. It
confirms Exhibition's first screen (screen 2) the same way while its cursor,
the byte at `0x9D9F6D`, is on One Player.

The match loads as usual. After it has run 240 frames the viewer chooses the
first player for the animation controls, hides the other three, turns on
and frames the free camera, freezes the scene and hides the overlay. A
character is hidden by moving her skeleton 100 units below the ground after
it is built (`0x000B01E0`), and at once when hiding starts, since a frozen
scene builds no skeletons. The camera is placed 2.5 units in front of her
hips, along the third axis of her hip bone, which points the way she faces.

When the free camera is put away the viewer quits the match the way the
results menu's Quit does (`0x5D8978` = 3, then `0x4D6FC8` = 1). Quit leads to
the title, which the viewer moves on to the main menu the way START would,
as CPU Exhibition does: once the title step has run 60 frames (`0x4D994C`)
it sets the step's leaving count at `0x4D994A`.

## Verification

Scripted runs on an M-series Mac showed MODEL VIEWER in the main menu in the
game's own style, under the other two items, and moved the cursor onto it.
Confirming it led to Exhibition's setup and character select; after Player
1's girl (the third one) and swimsuit were picked, the other three picks were
made by themselves and court select came up.
After the match started the viewer showed only Player 1's girl, from the
front, frozen, with no overlay. Switching the free camera off quit the match
and the game came back to the main menu.

The third girl, Hitomi, was also chosen and framed the same way.

On court 0, Day and Sunset loaded stages 4 and 5 in place of 39, with the
matching sky, and the table read 39 again after leaving the viewer.

Not yet verified: a play test, every court,
Windows and Linux builds, and the Japanese language setting.
