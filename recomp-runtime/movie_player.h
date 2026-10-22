#ifndef DOAXBV_RECOMP_MOVIE_PLAYER_H
#define DOAXBV_RECOMP_MOVIE_PLAYER_H

#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Native replacement for the CRI Sofdec player calls the game makes; see
   docs/research/sofdec-player-api.md. */
RecompFunction recomp_movie_player_lookup_manual(uint32_t guest_address);

#ifdef __cplusplus
}
#endif

#endif
