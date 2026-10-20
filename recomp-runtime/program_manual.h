#ifndef DOAXBV_RECOMP_PROGRAM_MANUAL_H
#define DOAXBV_RECOMP_PROGRAM_MANUAL_H

#include "runtime.h"

RecompFunction recomp_lookup_manual(uint32_t guest_address);

/* Radio shuffle: a random playlist index other than playing whose location
   mask allows location (a location outside 0..7 allows every entry), or -1
   when there is none. random is any host random value. */
int recomp_music_shuffle_pick(const uint16_t *masks, int count, int playing,
                              int location, uint32_t random);

/* Sun shadows: whether a bounding sphere (object-space x, y, z, radius) lies
   near enough to the camera to cast into the sun map. world and view are the
   game's row-vector matrices. */
int recomp_sun_caster_near(const float sphere[4], const float world[16],
                           const float view[16]);

#endif
