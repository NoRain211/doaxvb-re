#include "game_rollback.h"
#include "fiber_adapter.h"
#include "guest_snapshot.h"
#include "runtime.h"
#include "stop_report.h"

#include <stdlib.h>

#ifdef _WIN32
static RecompGuestSnapshots ram;
static void *fibers;
static RecompFpuContext fpu;

bool recomp_game_rollback_begin(void)
{
    if (fibers != NULL) return true;
    if (recomp_runtime.memory_region_count != 1u) return false;
    fibers = malloc(recomp_fiber_adapter_state_size());
    if (fibers == NULL ||
        !recomp_snapshots_init(&ram, recomp_runtime.memory_regions[0].data,
            recomp_runtime.memory_regions[0].size, true, 1u)) {
        free(fibers);
        fibers = NULL;
        return false;
    }
    recomp_fiber_adapter_state_save(fibers);
    recomp_fpu_context_save(&fpu);
    return true;
}

void recomp_game_rollback_commit(void)
{
    /* A failed save still leaves the live RAM as the newest save point. */
    (void)recomp_snapshots_save(&ram);
    recomp_fiber_adapter_state_save(fibers);
    recomp_fpu_context_save(&fpu);
}

void recomp_game_rollback_restore(void)
{
    if (!recomp_fiber_adapter_state_restorable(fibers)) {
        recomp_stop(2, "game:rollback-fibers");
    }
    if (!recomp_snapshots_load(&ram, 0u)) {
        recomp_stop(1, "game:rollback-write-watch");
    }
    recomp_fiber_adapter_state_restore(fibers);
    recomp_fpu_context_restore(&fpu);
}
#else
bool recomp_game_rollback_begin(void) { return false; }
void recomp_game_rollback_commit(void) {}
void recomp_game_rollback_restore(void) {}
#endif
