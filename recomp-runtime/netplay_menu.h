#ifndef DOAXBV_RECOMP_NETPLAY_MENU_H
#define DOAXBV_RECOMP_NETPLAY_MENU_H

#include "input_model.h"
#include "netplay.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

/* The online entries the runner shows when Exhibition is chosen on the
   game's main menu, and the lobby room, drawn over the game. When both
   players are ready, the room closes and each runner takes the launch and
   starts again into the session. */

/* Turns the menu on for an offline run. storage is the play folder's
   .recomp-storage, digest the runner build's hex digest, and ownName reads
   the player's profile name when a room opens. */
void recomp_netplay_menu_install(const std::filesystem::path &storage, const std::string &digest,
    std::function<std::string()> ownName);
/* Once a frame, on the game's thread: runs an open menu and publishes its screen. */
void recomp_netplay_menu_frame();
/* After a room's session ended (its stop reason): the menu opens by itself
   once the main menu has EXHIBITION chosen, and says why. */
void recomp_netplay_menu_reopen(const std::string &reason);
/* Sees each sampled pad first; while the menu is open the game gets an idle pad. */
void recomp_netplay_menu_filter(std::uint32_t port, RecompInputGamepad *pad);
/* What a room keeps through its session, so it can open again after it. */
struct RecompNetplayRoomState {
    bool hosting = false, listing = false, found = false;
    std::string peerAddress;
};
/* The state as --netplay-room carries it: <hosting>,<listing>,<found>,<peer address>. */
std::string recomp_netplay_room_state_format(const RecompNetplayRoomState &room);
bool recomp_netplay_room_state_parse(const std::string &text, RecompNetplayRoomState &room);
/* Takes a room's launch, once: the room's state, the port and input delay a
   host plays with, and the room's open socket, which the caller now owns. */
bool recomp_netplay_menu_take_launch(RecompNetplayRoomState &room, std::uint32_t &port, std::uint32_t &delay,
    RecompNetplay &socket);
/* Opens the room a session came from again on the session's socket, which
   the menu takes back: the host listens, the joiner calls the host. reason
   is the session's stop reason, shown in the room. */
void recomp_netplay_menu_return_to_room(const RecompNetplay &socket, const RecompNetplayRoomState &room,
    const std::string &reason);

#endif
