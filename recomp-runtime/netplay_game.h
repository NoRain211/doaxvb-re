#ifndef DOAXBV_RECOMP_NETPLAY_GAME_H
#define DOAXBV_RECOMP_NETPLAY_GAME_H

#include "input_model.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The game's side of a netplay session, read from guest RAM. */

/* A rally is live: the match scene runs and stays, unpaused, before its
   results menu. Only then may a session predict the other player. */
bool recomp_netplay_game_rally_live(void);
/* The pad on port 0 that walks the menus from boot to Two Players
   character select. Both runners call it at the same frame with the same
   guest state. True once character select shows; the pad is then empty. */
bool recomp_netplay_game_walk(uint32_t frame, RecompInputGamepad *pad);
/* Exhibition character select shows. */
bool recomp_netplay_game_character_select(void);
/* The main menu runs with its cursor on EXHIBITION, where the runner's
   online menu opens on A. */
bool recomp_netplay_game_exhibition_chosen(void);

#ifdef __cplusplus
}
#endif

#endif
