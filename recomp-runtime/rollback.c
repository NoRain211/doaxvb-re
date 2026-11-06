#include "rollback.h"

#include <string.h>

enum { HISTORY = RECOMP_ROLLBACK_HISTORY };

void recomp_rollback_init(RecompRollbackSession *s, uint32_t input_delay)
{
    memset(s, 0, sizeof *s);
    s->local_count = s->remote_count = input_delay;
    s->rewind_to = RECOMP_ROLLBACK_NONE;
    s->window = RECOMP_ROLLBACK_WINDOW;
}

/* A rewind reaches at most WINDOW frames back, so older history can be
   overwritten. */
static bool fits(const RecompRollbackSession *s, uint32_t frame)
{
    return frame + RECOMP_ROLLBACK_WINDOW < s->frame + HISTORY;
}

bool recomp_rollback_add_local(RecompRollbackSession *s, const RecompInputGamepad *pad)
{
    if (!fits(s, s->local_count)) return false;
    s->local[s->local_count++ % HISTORY] = *pad;
    return true;
}

bool recomp_rollback_add_remote(RecompRollbackSession *s, const RecompInputGamepad *pad)
{
    const uint32_t frame = s->remote_count;
    if (!fits(s, frame)) return false;
    if (frame < s->frame && memcmp(&s->used[frame % HISTORY], pad, sizeof *pad) != 0 &&
        (s->rewind_to == RECOMP_ROLLBACK_NONE || frame < s->rewind_to))
        s->rewind_to = frame;
    s->remote[frame % HISTORY] = *pad;
    ++s->remote_count;
    return true;
}

bool recomp_rollback_can_run(const RecompRollbackSession *s)
{
    return s->frame < s->local_count && s->frame < s->remote_count + s->window;
}

uint32_t recomp_rollback_take_rewind(RecompRollbackSession *s)
{
    const uint32_t frame = s->rewind_to;
    s->rewind_to = RECOMP_ROLLBACK_NONE;
    if (frame != RECOMP_ROLLBACK_NONE) s->frame = frame;
    return frame;
}

void recomp_rollback_run(RecompRollbackSession *s,
    RecompInputGamepad *local, RecompInputGamepad *remote)
{
    const uint32_t frame = s->frame++;
    *local = s->local[frame % HISTORY];
    if (frame < s->remote_count)
        *remote = s->remote[frame % HISTORY];
    else if (s->remote_count != 0u)
        *remote = s->remote[(s->remote_count - 1u) % HISTORY];
    else
        memset(remote, 0, sizeof *remote);
    s->used[frame % HISTORY] = *remote;
}
