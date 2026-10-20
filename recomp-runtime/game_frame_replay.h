#ifndef DOAXBV_GAME_FRAME_REPLAY_H
#define DOAXBV_GAME_FRAME_REPLAY_H

#include "d3d_presenter.h"

#include <stdbool.h>

/* Draws and clears one frame owner submitted during an authored tick, in
   order. Held substeps resubmit them so the owner's state advances once per
   tick while its output stays on every present; its depth clears must come
   back too, or its 3D draws test against the scene's depth. Vertex and
   index bytes are owned copies; texture and palette pointers stay in guest
   memory, which the held substep does not advance. */
typedef struct RecompGameFrameReplay {
    RecompD3dPresenterCommand *commands;
    void **bytes;
    unsigned count, capacity;
} RecompGameFrameReplay;

bool recomp_game_frame_replay_record(RecompGameFrameReplay *replay,
    const RecompD3dPresenterCommand *command);
void recomp_game_frame_replay_clear(RecompGameFrameReplay *replay);

#endif
