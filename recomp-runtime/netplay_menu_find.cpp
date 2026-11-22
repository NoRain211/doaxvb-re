/* SPDX-License-Identifier: GPL-3.0-or-later */
/* FIND MATCH: the matchmaker's open rooms, a join by pick, and quick match. */
#include "netplay_menu_screens.h"

#include "overlay_screen.h"

#include <algorithm>
#include <cstdio>

namespace netplay_menu {

// A new socket has no list yet: it asks on the first tick after the matchmaker's address is known.
static void awaitList()
{
    menu.find.listOk = false;
    menu.find.listedAt = Clock::time_point{};
}

// FIND MATCH asks from a socket that becomes this joiner's room socket, so the
// endpoint the matchmaker sees is the one the host punches to.
void find()
{
    if (!nameAllowed()) return;
    if (!recomp_netplay_listen(&menu.net, 0u)) {
        recomp_netplay_close(&menu.net);
        menu.message = "NO UDP PORT IS FREE.";
        return;
    }
    lookUpMatchmaker();
    awaitList();
    menu.find.quick = false;
    menu.find.choice = 0;
    menu.find.chosen = 0u;
    menu.find.joining = 0u;
    menu.message.clear();
    menu.screen = Screen::Find;
}

// The listed rooms whose names pass the name check, newest first as the server lists them, as many as fit.
static std::vector<const RecompNetplayListedRoom *> shownRooms(std::size_t limit = 6u)
{
    std::vector<const RecompNetplayListedRoom *> rooms;
    const RecompNetplayMatch &match = menu.net.match;
    for (std::uint32_t i = 0u; i < match.room_count && rooms.size() < limit; ++i)
        if (!recomp_netplay_name_banned(match.rooms[i].name) &&
            std::find(menu.find.blockedRooms.begin(), menu.find.blockedRooms.end(), match.rooms[i].id) == menu.find.blockedRooms.end())
            rooms.push_back(&match.rooms[i]);
    return rooms;
}

static void askToJoin(std::uint32_t id, Clock::time_point now)
{
    menu.find.joining = id;
    menu.find.joinStarted = menu.find.askedAt = now;
    recomp_netplay_match_join(&menu.net, id, menu.digest.data(), menu.name.c_str());
}

// A late PEER for a stopped join would still pair this socket, and PEER names no room, so the
// join is dropped with the socket. False when no new socket opens.
static bool forgetJoin()
{
    menu.find.joining = 0u;
    const std::uint32_t serverAddress = menu.net.match.server_address;
    const std::uint16_t serverPort = menu.net.match.server_port;
    recomp_netplay_close(&menu.net);
    if (!recomp_netplay_listen(&menu.net, 0u)) {
        recomp_netplay_close(&menu.net);
        menu.lookup.reset();
        menu.screen = Screen::Online;
        menu.cursor = OnlineFind;
        menu.message = "NO UDP PORT IS FREE.";
        return false;
    }
    menu.net.match.server_address = serverAddress;
    menu.net.match.server_port = serverPort;
    awaitList();
    return true;
}

void updateFind(std::uint16_t keys)
{
    RecompNetplayMatch &match = menu.net.match;
    if (takeLookup() == LookupState::NotFound) {
        recomp_netplay_close(&menu.net);
        menu.screen = Screen::Online;
        menu.cursor = OnlineFind;
        menu.message = "THE MATCHMAKER CANNOT BE FOUND.";
        return;
    }
    recomp_netplay_match_poll(&menu.net);
    const auto now = Clock::now();
    if (keys) menu.message.clear();
    if (match.gone) {
        match.gone = false;
        menu.find.joining = 0u;
        menu.find.quick = false;
        menu.message = "THAT ROOM IS GONE.";
        menu.find.listedAt = Clock::time_point{};
    }
    if (match.listed) {
        match.listed = false;
        menu.find.listOk = true;
    }
    if (keys & B) {
        if (menu.find.joining == 0u) {
            recomp_netplay_close(&menu.net);
            menu.lookup.reset();
            menu.screen = Screen::Online;
            menu.cursor = OnlineFind;
        } else {
            (void)forgetJoin();
        }
        menu.find.quick = false;
        return;
    }
    if (menu.find.joining != 0u && now - menu.find.joinStarted > std::chrono::seconds(3)) {
        if (forgetJoin()) menu.message = "NO ANSWER FROM THE MATCHMAKER.";
        return;
    }
    if (match.met && menu.find.joining == 0u) match.met = false; // a pairing after STOP or a timeout opens nothing
    if (match.met) {
        match.met = false;
        char text[64] = "RELAY";
        if (match.candidate_port[0] != 0u) {
            const std::uint8_t *at = reinterpret_cast<const std::uint8_t *>(&match.candidate_address[0]);
            const std::uint8_t *port = reinterpret_cast<const std::uint8_t *>(&match.candidate_port[0]);
            std::snprintf(text, sizeof text, "%u.%u.%u.%u:%u", at[0], at[1], at[2], at[3], port[0] << 8 | port[1]);
        }
        menu.lobby.peerAddress = text;
        menu.lobby.found = true;
        enterRoom(false);
        return;
    }
    if (menu.find.joining != 0u) {
        // The server answers a repeated join again, so a lost answer costs one retry.
        if (now - menu.find.askedAt >= std::chrono::milliseconds(500)) {
            menu.find.askedAt = now;
            recomp_netplay_match_join(&menu.net, menu.find.joining, menu.digest.data(), menu.name.c_str());
        }
        return;
    }
    if (match.server_port != 0u && now - menu.find.listedAt >= std::chrono::seconds(2)) {
        if (menu.find.listedAt == Clock::time_point{}) menu.find.askingSince = now; // the wait counts from the first ask
        menu.find.listedAt = now;
        recomp_netplay_match_list(&menu.net, menu.digest.data());
    }
    const auto rooms = shownRooms();
    const int count = static_cast<int>(rooms.size());
    // A newer room pushes the others down the list, so the highlight follows its room.
    const auto kept = std::find_if(rooms.begin(), rooms.end(),
        [](const RecompNetplayListedRoom *room) { return room->id == menu.find.chosen; });
    if (kept != rooms.end()) menu.find.choice = static_cast<int>(kept - rooms.begin());
    if (menu.find.choice >= count) menu.find.choice = count > 0 ? count - 1 : 0;
    if ((keys & Up) && count > 0) menu.find.choice = (menu.find.choice + count - 1) % count;
    if ((keys & Down) && count > 0) menu.find.choice = (menu.find.choice + 1) % count;
    if (count > 0) menu.find.chosen = rooms[static_cast<std::size_t>(menu.find.choice)]->id;
    if ((keys & A) && count > 0) askToJoin(rooms[static_cast<std::size_t>(menu.find.choice)]->id, now);
    if (keys & Y) menu.find.quick = true;
    if (menu.find.quick && menu.find.listOk) {
        menu.find.quick = false;
        // Quick match takes the listed room with the lowest estimated ping, the oldest when no host's
        // round trip is known, or hosts one when none is open. It looks past the six shown.
        const auto listed = shownRooms(RECOMP_NETPLAY_LISTED_ROOMS);
        const RecompNetplayListedRoom *nearest = nullptr;
        for (const RecompNetplayListedRoom *room : listed)
            if (room->pinged && (nearest == nullptr || room->ping_ms < nearest->ping_ms)) nearest = room;
        if (!listed.empty()) {
            // Newest first, so the last of rooms equally old is the oldest.
            const auto oldest = std::max_element(listed.rbegin(), listed.rend(),
                [](const RecompNetplayListedRoom *a, const RecompNetplayListedRoom *b) { return a->age_s < b->age_s; });
            askToJoin((nearest != nullptr ? nearest : *oldest)->id, now);
        } else {
            recomp_netplay_close(&menu.net);
            menu.screen = Screen::Online; // where a failed host leaves its message
            menu.cursor = OnlineFind;
            host(false, true);
        }
    }
}

void drawFind()
{
    title("FIND MATCH");
    const auto rooms = shownRooms();
    for (std::size_t i = 0u; i < rooms.size(); ++i) {
        const float y = 134.0f + 40.0f * static_cast<float>(i);
        const bool selected = static_cast<int>(i) == menu.find.choice && menu.find.joining == 0u;
        if (selected) recomp_overlay_rect(40.0f, y - 10.0f, 560.0f, 37.0f, Red);
        plateName(60.0f, y - 2.0f, rooms[i]->name, selected ? White : Gray);
        const std::string ping = rooms[i]->pinged ? "~" + std::to_string(rooms[i]->ping_ms) + " MS  " : "";
        const std::string detail = ping + "DELAY " + std::to_string(rooms[i]->delay) + "  " +
            std::to_string(rooms[i]->age_s / 60u) + " MIN";
        recomp_overlay_text(588.0f - recomp_overlay_text_width(detail.c_str(), 2.0f), y + 2.0f, 2.0f,
            selected ? White : Gray, detail.c_str());
    }
    // A slow address lookup is not the matchmaker's silence.
    const bool silent = menu.find.listedAt != Clock::time_point{} &&
        Clock::now() - menu.find.askingSince > std::chrono::seconds(6);
    const char *asking = silent
        ? "NO ANSWER FROM THE MATCHMAKER. STILL ASKING..." : "ASKING THE MATCHMAKER...";
    const char *status = menu.find.joining != 0u ? "JOINING..."
        : !menu.find.listOk ? asking
        : rooms.empty() ? "NO OPEN ROOMS. Y HOSTS ONE FOR OTHERS TO FIND."
        : menu.find.quick ? "FINDING THE NEAREST ROOM..." : nullptr;
    if (status != nullptr && menu.message.empty()) recomp_overlay_text(40.0f, 396.0f, 2.0f, Yellow, status);
    footer(menu.find.joining != 0u ? "B STOP" : "A JOIN   Y QUICK MATCH   B BACK");
}

} // namespace netplay_menu
