/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOAXBV_RECOMP_NETPLAY_LAUNCH_H
#define DOAXBV_RECOMP_NETPLAY_LAUNCH_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

/* How a full-program runner plays: a lobby session (--netplay-host or
   --netplay-join, typed or from a room), a --replay, a RECOMP_ROLLBACK test
   session, a RECOMP_SYNCTEST run, or offline with the online menu. */

/* Takes the netplay arguments out of argv, keeping the rest for
   recomp_runner_relaunch. False with a message when one is malformed. */
bool recomp_netplay_launch_parse(int &argc, char **argv);
/* Before recomp_save_initialize: settles the mode from the arguments and the
   environment, checks it, and pairs with the other runner or reads the
   replay. digest is this build's, disc the play folder. Returns 0, or the
   runner's exit code. */
int recomp_netplay_launch_configure(const std::string &digest, const std::filesystem::path &disc);
/* Before the guest starts, after the runner's own input sources: installs
   the mode over guest RAM. */
void recomp_netplay_launch_install(std::uint8_t *ram, std::size_t size, bool writeWatch);
/* Starts this runner again with its own arguments plus extra, and exits. */
[[noreturn]] void recomp_runner_relaunch(const std::vector<std::string> &extra);

#endif
