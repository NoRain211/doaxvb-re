/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOAXBV_RECOMP_NETPLAY_SESSION_H
#define DOAXBV_RECOMP_NETPLAY_SESSION_H

#include "input_model.h"

#include <cstddef>
#include <cstdint>

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
bool recomp_netplay_session_enabled();
/* Turns on the frame clock, the snapshot modules and the frame hook. Call
   before the guest starts. */
void recomp_netplay_session_install(const RecompNetplaySessionHost &host);
/* True when the session decides the guest port's pad, with present set. */
bool recomp_netplay_session_pad(std::uint32_t port, RecompInputGamepad *pad, bool *present);
// At exit: tells the other player.
void recomp_netplay_session_exit();

#endif
