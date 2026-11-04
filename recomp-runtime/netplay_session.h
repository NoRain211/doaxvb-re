/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOAXBV_RECOMP_NETPLAY_SESSION_H
#define DOAXBV_RECOMP_NETPLAY_SESSION_H

#include "input_model.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

/* Rollback sessions over the running game (docs/rollback-netcode.md),
   adapted from doa2u-re's netplay_session.cpp. The session runs at the top
   of the hand-written main loop, where every guest fiber is parked. */

struct RecompNetplaySessionHost {
    std::uint8_t *guestRam;  // for RECOMP_ROLLBACK_DUMP
    std::size_t guestRamSize;
    bool writeWatch;         // guest RAM reports its written pages
};

/* RECOMP_ROLLBACK=loopback or udp:<local port>:<peer host>:<peer port>. False
   with a message when it is malformed or the socket cannot open. */
bool recomp_netplay_session_read_env();

/* --netplay-host <port> or --netplay-join <host>:<port>, with the host's
   --netplay-delay. */
struct RecompNetplayLaunch {
    std::uint32_t hostPort = 0u;
    std::string join;
    std::uint32_t delay = 2u;
    /* Set when a room started this runner (recomp_netplay_session_room_launch):
       the room socket's port, peer and matchmaker, so the path the room's
       routers opened stays; a session that ends goes back to the menu. */
    std::string roomSocket;
    std::string digest;         // this build; the other runner must match it
    std::filesystem::path disc; // the play folder
};
/* Pairs with the other runner before the guest boots and readies the
   session: both sides play from a copy of the host's saves, with the host's
   settings that change the simulation. Call before recomp_save_initialize.
   False with a message when the lobby fails. */
bool recomp_netplay_session_connect(const RecompNetplayLaunch &launch);
/* Once a frame offline: when the menu's room starts a match, the arguments
   that start this runner again as the room's session. The room's socket and
   router forward are closed, so the new runner can take the port. */
bool recomp_netplay_session_room_launch(std::vector<std::string> &args);
/* Starts this runner again with its own arguments plus extra, and exits.
   The runner implements it. */
[[noreturn]] void recomp_runner_relaunch(const std::vector<std::string> &extra);
bool recomp_netplay_session_enabled();
/* Turns on the frame clock, the snapshot modules and the frame hook. Call
   before the guest starts. */
void recomp_netplay_session_install(const RecompNetplaySessionHost &host);
/* True when the session decides the guest port's pad, with present set. */
bool recomp_netplay_session_pad(std::uint32_t port, RecompInputGamepad *pad, bool *present);
// At exit: tells the other player.
void recomp_netplay_session_exit();

#endif
