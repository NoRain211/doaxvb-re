#ifndef DOAXBV_GAME_FRAME_REPLAY_H
#define DOAXBV_GAME_FRAME_REPLAY_H

#include "d3d_presenter.h"

#include <stdbool.h>

/* Draws one frame owner submitted during an authored tick. Held substeps
   resubmit them so the owner's state advances once per tick while its
   output stays on every present. Vertex and index bytes are owned copies;
   texture and palette pointers stay in guest memory, which the held
   substep does not advance. */
typedef struct RecompGameFrameReplay {
    RecompD3dPresenterDrawCommand *draws;
    void **bytes;
    unsigned count, capacity;
} RecompGameFrameReplay;

bool recomp_game_frame_replay_record(RecompGameFrameReplay *replay,
    const RecompD3dPresenterDrawCommand *draw);
void recomp_game_frame_replay_clear(RecompGameFrameReplay *replay);

#endif
