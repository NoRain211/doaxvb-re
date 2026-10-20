#include "viewer_scene.h"

#include <stddef.h>

/* Each place's stage by time; 0 where the place has none. Found by loading
   every stage in the model viewer (docs/model-viewer.md). */
typedef struct Place {
    uint8_t stage[RECOMP_VIEWER_TIME_COUNT];
} Place;

enum { BEACH_A, ROCK_ARCH, JUNGLE, BEACH_B, PLACE_COUNT };

static const Place places[PLACE_COUNT] = {
    [BEACH_A] = {{0u, 4u, 5u, 39u}},
    [ROCK_ARCH] = {{0u, 6u, 7u, 40u}},
    [JUNGLE] = {{0u, 8u, 9u, 41u}},
    [BEACH_B] = {{0u, 11u, 12u, 0u}},
};

/* Exhibition's courts in court select's order: dusk, day, sunset of each
   of the three places, then the last beach by day. */
static const uint8_t court_place[RECOMP_VIEWER_COURTS] = {
    BEACH_A, BEACH_A, BEACH_A, ROCK_ARCH, ROCK_ARCH, ROCK_ARCH,
    JUNGLE, JUNGLE, JUNGLE, BEACH_B,
};

const char *recomp_viewer_time_name(RecompViewerTime time)
{
    switch (time) {
    case RECOMP_VIEWER_TIME_COURT: return "As the court";
    case RECOMP_VIEWER_TIME_DAY: return "Day";
    case RECOMP_VIEWER_TIME_SUNSET: return "Sunset";
    case RECOMP_VIEWER_TIME_DUSK: return "Dusk";
    case RECOMP_VIEWER_TIME_COUNT: break;
    }
    return "?";
}

RecompViewerTime recomp_viewer_time_step(RecompViewerTime time, int step)
{
    int next = ((int)time + (step > 0 ? 1 : step < 0 ? -1 : 0)) % RECOMP_VIEWER_TIME_COUNT;
    if (next < 0) next += RECOMP_VIEWER_TIME_COUNT;
    return (RecompViewerTime)next;
}

int recomp_viewer_court_stage(uint32_t court, RecompViewerTime time, uint8_t own_stage)
{
    if (court >= RECOMP_VIEWER_COURTS) return RECOMP_VIEWER_NO_STAGE;
    if (time <= RECOMP_VIEWER_TIME_COURT || time >= RECOMP_VIEWER_TIME_COUNT) return own_stage;
    uint8_t stage = places[court_place[court]].stage[time];
    return stage != 0u ? stage : own_stage;
}
