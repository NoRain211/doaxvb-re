#include "game_frame_replay.h"

#include <stdlib.h>
#include <string.h>

bool recomp_game_frame_replay_record(RecompGameFrameReplay *replay,
    const RecompD3dPresenterDrawCommand *draw)
{
    if (replay->count == replay->capacity) {
        const unsigned capacity = replay->capacity ? replay->capacity * 2u : 64u;
        RecompD3dPresenterDrawCommand *draws = realloc(replay->draws, capacity * sizeof *draws);
        if (!draws) return false;
        replay->draws = draws;
        void **bytes = realloc(replay->bytes, capacity * sizeof *bytes);
        if (!bytes) return false;
        replay->bytes = bytes;
        replay->capacity = capacity;
    }
    const size_t vertices = (size_t)draw->vertex_count * draw->vertex_stride;
    const size_t indices = draw->index_bytes ? (size_t)draw->index_count * 2u : 0u;
    uint8_t *owned = malloc(vertices + indices + 1u);
    if (!owned) return false;
    RecompD3dPresenterDrawCommand *copy = replay->draws + replay->count;
    *copy = *draw;
    if (draw->vertex_bytes) memcpy(owned, draw->vertex_bytes, vertices);
    if (indices) memcpy(owned + vertices, draw->index_bytes, indices);
    copy->vertex_bytes = draw->vertex_bytes ? owned : NULL;
    copy->index_bytes = indices ? owned + vertices : NULL;
    copy->pose_replay = NULL;
    copy->pose_vertex_bytes = NULL;
    copy->split_pose = NULL;
    copy->split_pose_size = 0;
    replay->bytes[replay->count++] = owned;
    return true;
}

void recomp_game_frame_replay_clear(RecompGameFrameReplay *replay)
{
    for (unsigned i = 0; i < replay->count; ++i) free(replay->bytes[i]);
    replay->count = 0;
}
