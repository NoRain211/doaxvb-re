/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The lobby room: two players ready up, chat, and the host starts the session. */
#include "netplay_menu_screens.h"

#include "input_text.h"
#include "overlay_screen.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace netplay_menu {

// The router forward, asked on its own thread.
enum class Upnp { Idle, Asking, Forwarded, NotForwarded };
static std::atomic<Upnp> upnpState{Upnp::Idle};
static char upnpAddress[80];
static std::string lanAddress;

static void openRouterPort(std::uint16_t port)
{
    static bool registered;
    if (!registered) {
        std::atexit(recomp_netplay_close_router_port);
        registered = true;
    }
    char lan[64];
    lanAddress = recomp_netplay_route_address(lan, sizeof lan) ? lan : "";
    Upnp idle = Upnp::Idle;
    if (!upnpState.compare_exchange_strong(idle, Upnp::Asking)) return; // a forward is open or still being asked for
    std::thread([port] {
        char address[sizeof upnpAddress];
        const bool forwarded = recomp_netplay_open_router_port(port, address, sizeof address);
        std::memcpy(upnpAddress, address, sizeof address);
        upnpState.store(forwarded ? Upnp::Forwarded : Upnp::NotForwarded);
    }).detach();
}

void enterRoom(bool hosting)
{
    menu.lobby.hosting = hosting;
    menu.lobby.ready = menu.lobby.starting = false;
    menu.lobby.room = RecompNetplayRoom{};
    menu.lobby.room.hosting = hosting;
    menu.lobby.room.id = menu.id;
    menu.lobby.room.blocked = blockedId;
    menu.lobby.opened = Clock::now();
    menu.message.clear();
    menu.lobby.chat.clear();
    menu.lobby.typing = false;
    menu.lobby.showAddress = false;
    menu.lobby.blockAsked = Clock::time_point{};
    menu.lobby.checkedIn = Clock::time_point{}; // a host lists its room on the first tick
    char stale[64]; // typed before the room opened, such as a game's Enter
    (void)recomp_input_text_take(stale, sizeof stale);
    menu.screen = Screen::Room;
}

// bound: the socket is already listening, handed back by a session. listed: ask the matchmaker to list it.
void host(bool bound, bool listed)
{
    if (!nameAllowed()) return;
    if (!bound && !recomp_netplay_listen(&menu.net, static_cast<std::uint16_t>(menu.port))) {
        recomp_netplay_close(&menu.net);
        menu.message = "UDP PORT " + std::to_string(menu.port) + " IS IN USE.";
        return;
    }
    if (!bound) {
        menu.lobby.listing = listed;
        if (listed) lookUpMatchmaker();
    }
    openRouterPort(static_cast<std::uint16_t>(menu.port));
    menu.lobby.found = false;
    enterRoom(true);
}

// Switches a hosting room between public and private in place, and keeps the choice for HOST ONLINE.
// A private room drops the matchmaker; if its LEAVE is lost, the server drops the room after 20 s.
static void setListed(bool listed)
{
    menu.lobby.listing = listed;
    menu.privateRoom = !listed;
    saveSettings();
    menu.lobby.checkedIn = Clock::time_point{};
    if (listed) {
        lookUpMatchmaker();
        return;
    }
    recomp_netplay_match_leave(&menu.net);
    menu.lookup.reset();
    RecompNetplayMatch &match = menu.net.match;
    match.server_port = 0u;
    match.relay = match.punching = false;
}

void join(bool bound)
{
    const std::size_t colon = menu.address.rfind(':');
    const std::string address = menu.address.substr(0u, colon);
    const unsigned long port = colon == std::string::npos ? menu.port
                                                          : std::strtoul(menu.address.c_str() + colon + 1u, nullptr, 10);
    if (address.empty() || port == 0u || port > 65535u) {
        menu.message = "ENTER AN ADDRESS LIKE 203.0.113.7:47701";
        return;
    }
    if (!nameAllowed()) return;
    if (!bound && !recomp_netplay_open(&menu.net, 0u, address.c_str(), static_cast<std::uint16_t>(port))) {
        recomp_netplay_close(&menu.net);
        menu.message = "THAT ADDRESS CANNOT BE USED.";
        return;
    }
    menu.lobby.peerAddress = address + ':' + std::to_string(port);
    menu.recent = menu.lobby.peerAddress;
    saveSettings();
    menu.lobby.found = false;
    enterRoom(false);
}

static void leaveRoom()
{
    if (menu.lobby.hosting) recomp_netplay_match_leave(&menu.net);
    recomp_netplay_close(&menu.net);
    menu.lookup.reset();
    Upnp forwarded = Upnp::Forwarded;
    if (menu.lobby.hosting && upnpState.compare_exchange_strong(forwarded, Upnp::Idle)) recomp_netplay_close_router_port();
    else if (upnpState.load() == Upnp::NotForwarded) upnpState.store(Upnp::Idle);
    menu.lobby.muting = false;
    menu.screen = Screen::Online;
    menu.cursor = menu.lobby.hosting ? OnlineHost : menu.lobby.found ? OnlineFind : OnlineJoin;
}

// A joiner leaves a host it blocked, and FIND MATCH stops offering that host's room.
static void leaveBlockedHost(const std::string &message)
{
    if (menu.lobby.found) menu.find.blockedRooms.push_back(menu.find.joining);
    leaveRoom();
    menu.message = message;
}

// Keeps the other player's ID and name; then a host drops them and keeps hosting, and a joiner leaves.
static void blockPeer()
{
    RecompNetplayRoom &room = menu.lobby.room;
    const std::string name = room.name;
    menu.lobby.blockAsked = Clock::time_point{};
    if (!blockedId(room.peer_id)) {
        menu.blocked.emplace_back(room.peer_id, name);
        saveSettings();
    }
    if (!menu.lobby.hosting) return leaveBlockedHost("BLOCKED " + name + ".");
    recomp_netplay_room_drop(&menu.net, &room);
    menu.lobby.chat.clear();
    menu.message = "BLOCKED " + name + ".";
}

static bool peerMuted()
{
    return menu.lobby.muting && menu.lobby.room.joined && menu.lobby.room.peer_id == menu.lobby.mutedId;
}

static std::string shownAddress()
{
    if (upnpState.load() == Upnp::Forwarded && upnpAddress[0] != '\0') return upnpAddress;
    return lanAddress.empty() ? "UNKNOWN" : lanAddress + ':' + std::to_string(menu.port);
}

static std::string routerStatus()
{
    const std::string port = std::to_string(menu.port);
    switch (upnpState.load()) {
    case Upnp::Asking: return "OPENING UDP " + port + "...";
    case Upnp::Forwarded: return upnpAddress[0] != '\0' ? "UDP " + port + " IS OPEN" : "BEHIND CARRIER NAT; LAN ONLY";
    default: return "NO UPNP; FORWARD UDP " + port + " BY HAND";
    }
}

static void addChat(const std::string &name, const std::string &text, bool mine)
{
    menu.lobby.chat.push_back({name.substr(0u, 12u), text, mine});
    if (menu.lobby.chat.size() > 4u) menu.lobby.chat.erase(menu.lobby.chat.begin());
}

// One line a second, checked as names are.
static void say()
{
    const auto now = Clock::now();
    if (menu.lobby.draft.empty()) {
        menu.lobby.typing = false;
    } else if (recomp_netplay_name_banned(menu.lobby.draft.c_str())) {
        menu.message = "THAT MESSAGE IS NOT ALLOWED.";
    } else if (now - menu.lobby.saidAt < std::chrono::seconds(1)) {
        menu.message = "WAIT A MOMENT BEFORE THE NEXT LINE.";
    } else {
        recomp_netplay_room_say(&menu.lobby.room, menu.lobby.draft.c_str());
        addChat(menu.name, menu.lobby.draft, true);
        menu.lobby.saidAt = now;
        menu.lobby.typing = false;
        menu.message.clear();
    }
}

// Enter or Tab opens a line, Enter sends it, Esc drops it; printable ASCII only.
static void typeChat(bool peerAllowed)
{
    if (menu.lobby.typing) recomp_input_text_focus(); // Enter, which opens the line, comes without text input
    char typed[64];
    const std::size_t count = recomp_input_text_take(typed, sizeof typed);
    for (std::size_t i = 0u; i < count; ++i) {
        const char c = typed[i];
        if (!menu.lobby.typing) {
            if ((c == '\r' || c == '\t') && peerAllowed) {
                menu.lobby.typing = true;
                menu.lobby.draft.clear();
            }
        } else if (c == 0x1b) {
            menu.lobby.typing = false;
            menu.message.clear();
        } else if (c == '\b') {
            if (!menu.lobby.draft.empty()) menu.lobby.draft.pop_back();
        } else if (c == '\r') {
            say();
        } else if (c >= 0x20 && c < 0x7f && menu.lobby.draft.size() < RECOMP_NETPLAY_CHAT_BYTES) {
            menu.lobby.draft.push_back(c);
        }
    }
}

// Returns true when the runner should restart into the session.
bool updateRoom(std::uint16_t keys)
{
    RecompNetplayRoom &room = menu.lobby.room;
    recomp_netplay_room_tick(&menu.net, &room, menu.digest.data(), menu.name.c_str(), menu.lobby.ready, menu.lobby.starting);
    if (room.refused) {
        room.refused = false;
        if (!menu.lobby.hosting) { // a host says nothing to a blocked joiner
            leaveBlockedHost("YOU BLOCKED THIS HOST.");
            return false;
        }
    }
    const bool peerAllowed = room.joined && !room.mismatch && !recomp_netplay_name_banned(room.name);
    if (menu.lobby.hosting) (void)takeLookup();
    if (menu.lobby.hosting && menu.net.match.server_port != 0u && Clock::now() - menu.lobby.checkedIn >= std::chrono::seconds(5)) {
        menu.lobby.checkedIn = Clock::now();
        recomp_netplay_match_host(&menu.net, menu.digest.data(), static_cast<std::uint8_t>(menu.delay),
            !room.joined && !menu.lobby.starting, menu.name.c_str());
    }
    if (room.heard_new) {
        room.heard_new = false;
        if (peerAllowed && !peerMuted()) addChat(room.name, room.heard, false);
    }
    typeChat(peerAllowed);
    if (menu.lobby.typing) keys = 0u; // typed letters are pad buttons too
    if (keys & ~X) {
        menu.message.clear();
        menu.lobby.blockAsked = Clock::time_point{};
    }
    if (keys & B) {
        leaveRoom();
        return false;
    }
    const auto now = Clock::now();
    if (room.joined) menu.lobby.showAddress = false;
    else if ((keys & Y) && menu.lobby.hosting) menu.lobby.showAddress = !menu.lobby.showAddress;
    if ((keys & Y) && peerAllowed) {
        menu.lobby.muting = !peerMuted();
        menu.lobby.mutedId = room.peer_id;
        if (menu.lobby.muting)
            menu.lobby.chat.erase(std::remove_if(menu.lobby.chat.begin(), menu.lobby.chat.end(), [](const ChatLine &line) { return !line.mine; }),
                menu.lobby.chat.end());
    }
    if ((keys & X) && room.joined && !menu.lobby.starting) {
        if (now - menu.lobby.blockAsked < std::chrono::seconds(3)) {
            blockPeer();
            return false;
        }
        menu.lobby.blockAsked = now;
        menu.message = "PRESS X AGAIN TO BLOCK " + std::string(room.name) + ".";
    } else if ((keys & X) && menu.lobby.hosting) {
        copyText(shownAddress());
        menu.message = "ADDRESS COPIED.";
    }
    // The host's left/right switches the room's listing until someone joins, then the input delay.
    if ((keys & (Left | Right)) && menu.lobby.hosting && !menu.lobby.starting) {
        if (!room.joined) {
            setListed(!menu.lobby.listing);
        } else {
            stepDelay(keys);
            menu.message = "INPUT DELAY " + std::to_string(menu.delay) + " FRAMES."; // chat may hide its line
        }
    }
    if (!peerAllowed) menu.lobby.ready = false;
    else if ((keys & A) && !menu.lobby.starting) menu.lobby.ready = !menu.lobby.ready;
    if (menu.lobby.hosting && !menu.lobby.starting && menu.lobby.ready && room.ready && peerAllowed) {
        menu.lobby.starting = true;
        menu.lobby.startAt = now;
    }
    // The host keeps saying start for a moment so the joiner hears it.
    if (menu.lobby.hosting && menu.lobby.starting && now - menu.lobby.startAt >= std::chrono::milliseconds(600)) return true;
    return !menu.lobby.hosting && room.start && menu.lobby.ready && peerAllowed;
}

static void plate(float x, const char *label, const std::string &name, bool present, const std::string &detail, bool ready)
{
    recomp_overlay_rect(x, 116.0f, 250.0f, 156.0f, Panel);
    recomp_overlay_rect(x, 116.0f, 250.0f, 4.0f, present && ready ? Green : Red);
    recomp_overlay_text(x + 16.0f, 134.0f, 2.0f, Gray, label);
    plateName(x + 16.0f, 164.0f, present ? name : "WAITING...", present ? White : Dim);
    recomp_overlay_text(x + 16.0f, 204.0f, 2.0f, Gray, detail.c_str());
    if (present) recomp_overlay_text(x + 16.0f, 240.0f, 2.0f, ready ? Green : Gray, ready ? "READY" : "NOT READY");
}

void drawRoom()
{
    const RecompNetplayRoom &room = menu.lobby.room;
    title("ONLINE LOBBY ROOM");
    const std::string ping = room.pinged ? std::to_string(room.ping_ms) + " MS" + (menu.net.match.relay ? "  RELAY" : "") : "";
    const bool peer = room.joined;
    const std::string peerName = room.mismatch ? "OTHER BUILD" : room.name;
    if (menu.lobby.hosting) {
        plate(40.0f, "P1  HOST", menu.name, true, "YOU", menu.lobby.ready);
        plate(350.0f, "P2", peerName, peer, ping, room.ready);
    } else {
        plate(40.0f, "P1  HOST", peerName, peer, peer ? ping : "CONNECTING...", room.ready);
        plate(350.0f, "P2", menu.name, true, "YOU", menu.lobby.ready);
    }
    if (peerMuted()) {
        const float right = (menu.lobby.hosting ? 350.0f : 40.0f) + 234.0f;
        recomp_overlay_text(right - recomp_overlay_text_width("CHAT MUTED", 1.5f), 136.0f, 1.5f, Yellow, "CHAT MUTED");
    }
    recomp_overlay_text(304.0f, 180.0f, 3.0f, Red, "VS");
    if (!menu.lobby.chat.empty() || menu.lobby.typing) {
        // Chat takes the address lines' place once it starts.
        for (std::size_t i = 0u; i < menu.lobby.chat.size(); ++i) {
            const float y = 290.0f + 20.0f * static_cast<float>(i);
            const float x = 40.0f + recomp_overlay_text(40.0f, y, 1.5f, Gray, (menu.lobby.chat[i].name + ":").c_str());
            recomp_overlay_text(x + 9.0f, y, 1.5f, White, menu.lobby.chat[i].text.c_str());
        }
        if (menu.lobby.typing) {
            const std::string line = "> " + menu.lobby.draft + ((menu.frame / 20u) % 2u == 0u ? "_" : "");
            recomp_overlay_text(40.0f, 372.0f, 2.0f, White, line.c_str());
        }
    } else if (menu.lobby.hosting) {
        const std::string shown = menu.lobby.showAddress ? shownAddress() : "HIDDEN, Y SHOWS IT";
        recomp_overlay_text(40.0f, 296.0f, 2.0f, Gray, ("YOUR ADDRESS  " + shown).c_str());
        recomp_overlay_text(40.0f, 322.0f, 2.0f, Gray, ("ROUTER  " + routerStatus()).c_str());
        const std::string frames = std::to_string(menu.delay) + " FRAMES";
        std::string delay = "INPUT DELAY  " + (peer ? "< " + frames + " >" : frames);
        if (room.pinged) {
            const std::uint32_t suggested = recomp_netplay_suggested_delay(room.ping_ms);
            delay += suggested == menu.delay ? "  SUGGESTED" : "  SUGGESTED " + std::to_string(suggested);
        }
        recomp_overlay_text(40.0f, 348.0f, 2.0f, Gray, delay.c_str());
        const char *const kinds[2] = {"PUBLIC", "PRIVATE"};
        const std::string kind = kinds[menu.lobby.listing ? 0 : 1];
        recomp_overlay_text(40.0f, 374.0f, 2.0f, Gray, ("ROOM  " + (peer ? kind : "< " + kind + " >")).c_str());
    } else {
        recomp_overlay_text(40.0f, 296.0f, 2.0f, Gray, menu.lobby.found ? "HOST  FOUND ON FIND MATCH" : "HOST  JOINED BY ADDRESS");
        recomp_overlay_text(40.0f, 322.0f, 2.0f, Gray, "INPUT DELAY  SET BY THE HOST");
    }
    if (menu.message.empty()) {
        const char *status = nullptr;
        std::string waiting;
        if (room.mismatch) status = "THE OTHER PLAYER RUNS ANOTHER BUILD.";
        else if (peer && recomp_netplay_name_banned(room.name)) status = "THE OTHER PLAYER'S NAME IS NOT ALLOWED.";
        else if (menu.lobby.starting || (room.start && menu.lobby.ready)) status = "BOTH READY: STARTING THE SESSION...";
        else if (peer && !menu.lobby.ready) status = "PRESS A WHEN YOU ARE READY.";
        else if (peer) status = (waiting = "WAITING FOR " + std::string(room.name) + " TO READY UP.").c_str();
        else if (menu.lobby.hosting && menu.net.match.room_id != 0u) status = "LISTED ON FIND MATCH. OR SEND YOUR ADDRESS.";
        else if (menu.lobby.hosting && menu.lobby.listing && (menu.lookup || menu.net.match.server_port != 0u))
            status = "LISTING ON FIND MATCH...";
        else if (menu.lobby.hosting && !menu.lobby.listing) status = "PRIVATE: SEND YOUR ADDRESS TO A FRIEND.";
        else if (menu.lobby.hosting) status = "SEND YOUR ADDRESS TO THE OTHER PLAYER.";
        else if (Clock::now() - menu.lobby.opened > std::chrono::seconds(menu.lobby.found ? 8 : 5))
            status = menu.lobby.found ? "NO ANSWER YET." : "NO ANSWER YET. CHECK THE ADDRESS.";
        if (status != nullptr) recomp_overlay_text(40.0f, 396.0f, 2.0f, Yellow, status);
    }
    footer(menu.lobby.typing ? "ENTER SEND   ESC CANCEL"
        : peer && !room.mismatch ? (peerMuted() ? "A READY  B LEAVE  Y UNMUTE  X BLOCK  ENTER CHAT"
                                                : "A READY  B LEAVE  Y MUTE  X BLOCK  ENTER CHAT")
        : peer ? "A READY   B LEAVE   X BLOCK"
        : menu.lobby.hosting ? "A READY  B LEAVE  X COPY  LEFT/RIGHT ROOM" : "A READY   B LEAVE");
}

} // namespace netplay_menu
