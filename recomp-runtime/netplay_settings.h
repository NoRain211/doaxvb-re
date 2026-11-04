/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOAXBV_RECOMP_NETPLAY_SETTINGS_H
#define DOAXBV_RECOMP_NETPLAY_SETTINGS_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

constexpr std::size_t RecompNetplayMaxAddress = 21u;    // 255.255.255.255:65535
constexpr std::size_t RecompNetplayMaxMatchmaker = 40u; // what the editor's panel shows
extern const char *const RecompNetplayDisplayNames[3];

/* What the Versus menu keeps in .recomp-storage/netplay.txt. */
struct RecompNetplaySettings {
    std::uint32_t delay = 2u, port = 47701u;
    int display = 1; // the session's network display: off, ping, full
    std::string recent, matchmaker;
    bool privateRoom = false; // HOST ONLINE's choice
    std::uint64_t id = 0u;    // this install's player ID, random; never 0 once loaded
    // Blocked players' IDs, with the name each had for display. Never sent anywhere.
    std::vector<std::pair<std::uint64_t, std::string>> blocked;
};

/* Reads path over settings, skipping lines it does not accept, and the
   block list replaces settings'. Without an ID it makes one and saves it. */
void recomp_netplay_settings_load(const std::filesystem::path &path, RecompNetplaySettings &settings);
void recomp_netplay_settings_save(const std::filesystem::path &path, const RecompNetplaySettings &settings);

#endif
