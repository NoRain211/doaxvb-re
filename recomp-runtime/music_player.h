#ifndef DOAXBV_RECOMP_MUSIC_PLAYER_H
#define DOAXBV_RECOMP_MUSIC_PLAYER_H

#include "runtime.h"

/* Music_Play's argument that starts the next playlist song. */
#define RECOMP_MUSIC_PLAY_PLAYLIST 0x7fffffffu

/* The game's playlist index rules. clamp bounds index to 0..count-1;
   step moves to the next entry and wraps after the last one. step needs a
   positive count. */
int recomp_music_playlist_clamp(int index, int count);
int recomp_music_playlist_step(int index, int count);

/* Guest 0x000B8F70, Music_Play. EAX is the player; the one stack argument
   is a track id or RECOMP_MUSIC_PLAY_PLAYLIST, and the callee pops it. */
void recomp_music_play(void);

#endif
