/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOAXBV_RECOMP_NETPLAY_PROFILE_H
#define DOAXBV_RECOMP_NETPLAY_PROFILE_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

/* The profiles a lobby session exchanges. A blob is part of partition 1:
   the SHA-256 of the rest as 64 hex digits, then per entry a kind (0 folder,
   1 file), a 2-byte path length, the path below partition 1 and, for a
   file, a 4-byte size and its bytes. Numbers are little-endian. */

// The save name on a profile's SaveMeta.xbx "Name=" line (UTF-16), as UTF-8.
std::string recomp_netplay_profile_name(const std::uint8_t *bytes, std::size_t size);
// The profile this player saved last, as UDATA/<title>/<save>, or empty without one.
std::string recomp_netplay_latest_profile(const std::filesystem::path &root);
// The name on the profile this player saved last, or empty without one.
std::string recomp_netplay_own_profile_name(const std::filesystem::path &partition1);
/* Packs profile's tree below root and, with title, the title saves (TDATA)
   and the files beside the profiles. False when a file is unreadable, over
   RECOMP_NETPLAY_MAX_PROFILE or not a plain file or folder. */
bool recomp_netplay_pack_profile(const std::filesystem::path &root, const std::string &profile, bool title,
    std::vector<std::uint8_t> &blob);
/* Writes a blob from the other player below root, adding its save names.
   False for a damaged blob or one whose paths leave UDATA and TDATA, use
   anything but plain names or name a Windows device; it may have written
   part of the blob by then. */
bool recomp_netplay_unpack_profile(const std::uint8_t *blob, std::size_t size, const std::filesystem::path &root,
    std::vector<std::string> &names);

#endif
