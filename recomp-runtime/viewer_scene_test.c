#include "viewer_scene.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(bool condition, const char *what)
{
    if (!condition) {
        fprintf(stderr, "viewer scene test failed: %s\n", what);
        ++failures;
    }
}

int main(void)
{
    /* The game's own court table: stage byte of each court. */
    static const uint8_t own[RECOMP_VIEWER_COURTS] = {39, 4, 5, 40, 6, 7, 41, 8, 9, 11};

    for (uint32_t court = 0u; court < RECOMP_VIEWER_COURTS; ++court) {
        check(recomp_viewer_court_stage(court, RECOMP_VIEWER_TIME_COURT, own[court]) == own[court],
            "the court's own time keeps its stage");
    }
    check(recomp_viewer_court_stage(0u, RECOMP_VIEWER_TIME_DAY, 39u) == 4, "beach A by day");
    check(recomp_viewer_court_stage(1u, RECOMP_VIEWER_TIME_DUSK, 4u) == 39, "beach A at dusk");
    check(recomp_viewer_court_stage(5u, RECOMP_VIEWER_TIME_DAY, 7u) == 6, "the rock arch by day");
    check(recomp_viewer_court_stage(7u, RECOMP_VIEWER_TIME_SUNSET, 8u) == 9, "the jungle at sunset");
    check(recomp_viewer_court_stage(9u, RECOMP_VIEWER_TIME_SUNSET, 11u) == 12, "beach B at sunset");
    check(recomp_viewer_court_stage(9u, RECOMP_VIEWER_TIME_DUSK, 11u) == 11, "beach B has no dusk: its own stage");
    check(recomp_viewer_court_stage(10u, RECOMP_VIEWER_TIME_DAY, 4u) == RECOMP_VIEWER_NO_STAGE, "courts past the table");

    check(recomp_viewer_time_step(RECOMP_VIEWER_TIME_DUSK, 1) == RECOMP_VIEWER_TIME_COURT, "next wraps");
    check(recomp_viewer_time_step(RECOMP_VIEWER_TIME_COURT, -1) == RECOMP_VIEWER_TIME_DUSK, "previous wraps");
    check(strcmp(recomp_viewer_time_name(RECOMP_VIEWER_TIME_SUNSET), "Sunset") == 0, "names");

    if (failures == 0) printf("viewer scene test passed\n");
    return failures == 0 ? 0 : 1;
}
