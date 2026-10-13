#ifndef DOAXBV_GAME_PACING_H
#define DOAXBV_GAME_PACING_H

#include <stdbool.h>
#include <stdint.h>

/* A negative delay retains elapsed game time until subsequent ticks catch up.
   The presentation-only policy instead discards a missed deadline. */
static inline int64_t recomp_game_pacing_delay_ns(
    int64_t until_previous_deadline, int64_t tick_ns, bool preserve_elapsed)
{
    const int64_t delay = until_previous_deadline + tick_ns;
    return preserve_elapsed || delay > 0 ? delay : 0;
}

/* Display passes per 60 Hz tick. */
typedef struct {
    unsigned passes, fitting_ticks, held_ticks, dwell_ticks;
    int32_t missed; /* Ticks that missed a pass, in 1/65536ths, averaged over 128 ticks. */
} RecompGamePacing;

static inline void recomp_game_pacing_init(RecompGamePacing *pacing, unsigned most)
{
    const RecompGamePacing initial = {most, 0, 0, 120, 0};
    *pacing = initial;
}

/* A pass drops when passes keep missing their slot (one tick in four over
   about two seconds, so a camera cut's burst does not count) or the render
   cost of each cannot fit. One returns after ticks without misses whose game
   work and render cost fit with margin for a dwell of two seconds, doubled
   (to a minute) each time a returned pass dropped within 5 s. Render cost
   excludes the output pass, so outcomes decide drops. */
static inline unsigned recomp_game_pacing_display_passes(RecompGamePacing *pacing, unsigned most,
    unsigned missed, int64_t tick_ns, int64_t pass_ns, int64_t render_ns)
{
    const int64_t tick = 1000000000 / 60;
    const unsigned passes = pacing->passes;
    pacing->missed += ((missed ? 65536 : 0) - pacing->missed) / 128;
    ++pacing->held_ticks;
    if (passes > 1 && (pacing->missed > 16384 || passes * render_ns > tick * 95 / 100)) {
        pacing->dwell_ticks = pacing->held_ticks >= 300 ? 120 :
            pacing->dwell_ticks < 3840 ? pacing->dwell_ticks * 2 : 3840;
        pacing->passes = passes - 1;
        pacing->fitting_ticks = pacing->held_ticks = 0;
        pacing->missed = 0;
    } else if (passes < most && missed == 0 && tick_ns + (passes + 1) * pass_ns <= tick * 80 / 100 &&
               (passes + 1) * render_ns <= tick * 80 / 100) {
        if (++pacing->fitting_ticks >= pacing->dwell_ticks) {
            pacing->passes = passes + 1;
            pacing->fitting_ticks = pacing->held_ticks = 0;
            pacing->missed = 0;
        }
    } else {
        pacing->fitting_ticks = 0;
    }
    return pacing->passes;
}

#endif
