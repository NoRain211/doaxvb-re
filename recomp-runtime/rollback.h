#ifndef DOAXBV_RECOMP_ROLLBACK_H
#define DOAXBV_RECOMP_ROLLBACK_H

#include "input_model.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Input history and prediction for a two-player rollback session. Frames
   count from the session start. Each side's pads arrive in frame order; a
   remote frame that has not arrived runs with the last remote pad. */
enum {
    /* Frames the local side may run past the last remote pad. */
    RECOMP_ROLLBACK_WINDOW = 8u,
    RECOMP_ROLLBACK_HISTORY = 64u,
};
#define RECOMP_ROLLBACK_NONE UINT32_MAX

typedef struct RecompRollbackSession {
    uint32_t frame;        /* next frame to run */
    uint32_t local_count;  /* local pads known for frames [0, local_count) */
    uint32_t remote_count;
    uint32_t rewind_to;    /* earliest frame that ran with a wrong prediction */
    /* Frames the local side may run past the last remote pad, at most
       RECOMP_ROLLBACK_WINDOW; 0 is lockstep. */
    uint32_t window;
    RecompInputGamepad local[RECOMP_ROLLBACK_HISTORY];
    RecompInputGamepad remote[RECOMP_ROLLBACK_HISTORY];
    RecompInputGamepad used[RECOMP_ROLLBACK_HISTORY]; /* remote pad each frame ran with */
} RecompRollbackSession;

/* Both sides must use the same input delay; its first frames are neutral.
   The window starts at RECOMP_ROLLBACK_WINDOW. */
void recomp_rollback_init(RecompRollbackSession *session, uint32_t input_delay);
/* False when the pad is too far ahead of the frame being run. */
bool recomp_rollback_add_local(RecompRollbackSession *session, const RecompInputGamepad *pad);
bool recomp_rollback_add_remote(RecompRollbackSession *session, const RecompInputGamepad *pad);
bool recomp_rollback_can_run(const RecompRollbackSession *session);
/* The frame to load and run again, or RECOMP_ROLLBACK_NONE. The session
   continues from that frame. */
uint32_t recomp_rollback_take_rewind(RecompRollbackSession *session);
/* The pads for the next frame; advances it. */
void recomp_rollback_run(RecompRollbackSession *session,
    RecompInputGamepad *local, RecompInputGamepad *remote);

#ifdef __cplusplus
}
#endif

#endif
