#ifndef DOAXBV_RECOMP_VIEWER_SCENE_H
#define DOAXBV_RECOMP_VIEWER_SCENE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The model viewer's places and times of day. The game bakes a time of day
   into each stage, so one place is several stages: Exhibition's ten courts
   are three places at day, sunset and dusk, and one more beach by day. */
typedef enum RecompViewerTime {
    RECOMP_VIEWER_TIME_COURT = 0, /* whatever the chosen court shows */
    RECOMP_VIEWER_TIME_DAY,
    RECOMP_VIEWER_TIME_SUNSET,
    RECOMP_VIEWER_TIME_DUSK,
    RECOMP_VIEWER_TIME_COUNT
} RecompViewerTime;

enum {
    RECOMP_VIEWER_COURTS = 10,
    RECOMP_VIEWER_NO_STAGE = -1,
};

const char *recomp_viewer_time_name(RecompViewerTime time);
/* The next (step > 0) or previous time, wrapping. */
RecompViewerTime recomp_viewer_time_step(RecompViewerTime time, int step);
/* The stage court `court` loads at `time`: its own stage for
   RECOMP_VIEWER_TIME_COURT, or for a time its place has no stage for, given
   the court's own stage. RECOMP_VIEWER_NO_STAGE for a court out of range. */
int recomp_viewer_court_stage(uint32_t court, RecompViewerTime time, uint8_t own_stage);

#ifdef __cplusplus
}
#endif

#endif
