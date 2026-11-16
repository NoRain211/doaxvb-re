/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef DOAXBV_RECOMP_NETPLAY_MENU_SCREENS_H
#define DOAXBV_RECOMP_NETPLAY_MENU_SCREENS_H

/* The online menu's state and what its screens share. netplay_menu.cpp runs
   the menu with its online, settings and address screens,
   netplay_menu_room.cpp the lobby room and netplay_menu_find.cpp FIND MATCH. */

#include "input_model.h"
#include "netplay.h"
#include "netplay_menu.h"
#include "netplay_settings.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace netplay_menu {

using Clock = std::chrono::steady_clock;

enum Key : std::uint16_t { Up = 1u, Down = 2u, Left = 4u, Right = 8u, A = 0x10u, B = 0x20u, X = 0x40u, Y = 0x80u };
enum class Screen { Closed, Online, Settings, Join, Matchmaker, Find, Room };
// The online screen's rows, top to bottom; a screen left for it puts the cursor on its own row.
enum OnlineRow : int { OnlineLocal, OnlineHost, OnlineJoin, OnlineFind, OnlineSettings, OnlineRows };

constexpr std::uint32_t White = 0xffffffffu, Gray = 0xffa8a8b0u, Dim = 0xff686870u, Red = 0xffd8283cu,
    Green = 0xff48d070u, Yellow = 0xffffd040u, Panel = 0xe8181820u, Backdrop = 0xd8000000u;

enum class LookupState { None, Looking, Found, NotFound };

// A matchmaker lookup on its own thread, so a slow DNS answer never holds the menu.
struct Lookup {
    std::atomic<LookupState> state{LookupState::Looking};
    RecompNetplay found{}; // only its matchmaker address and port are set
};

struct ChatLine {
    std::string name, text;
    bool mine;
};

/* The lobby room. The room state part outlives a session, which reopens the
   same room: listing is a host that asked to be listed, found a joiner whose
   room came from FIND MATCH. */
struct LobbyState : RecompNetplayRoomState {
    bool ready = false, starting = false;
    RecompNetplayRoom room{};
    Clock::time_point opened, startAt, saidAt, checkedIn;
    // Room chat: the last few lines as name and text, and the line being typed.
    std::vector<ChatLine> chat;
    bool typing = false;
    std::string draft;
    // Y hides the chat of the player with mutedId while they stay in the room.
    bool muting = false;
    std::uint64_t mutedId = 0u;
    Clock::time_point blockAsked; // X asks to block the other player; X again within 3 s blocks
    bool showAddress = false;     // Y shows the host's own address until someone joins
};

// FIND MATCH: the listed rooms and a join being asked, which stays set in the joined room.
struct FindState {
    bool listOk = false, quick = false;
    int choice = 0;
    std::uint32_t joining = 0u;
    std::vector<std::uint32_t> blockedRooms; // listed rooms of hosts this player blocked, this run
    Clock::time_point listedAt, joinStarted, askedAt, probedAt;
};

// The settings part is what netplay.txt keeps.
struct Menu : RecompNetplaySettings {
    bool installed = false;
    std::filesystem::path storage;
    std::string digest;
    std::function<std::string()> ownName;
    std::mutex lock; // the game may sample pads on another guest thread than it presents from
    Screen screen = Screen::Closed;
    std::uint16_t held[RECOMP_INPUT_PORT_COUNT]{};
    std::uint16_t pressed = 0u;
    std::uint16_t aliased = 0u; // the pressed face buttons that came from the keyboard's Z, X, A, S and Backspace
    unsigned repeat = 0u, frame = 0u;
    bool swallow = false; // after closing, the game waits until every button is up
    int injectA = 0;      // polls of A the game still gets, so LOCAL enters Exhibition
    std::uint32_t openedBy = 0u;
    int cursor = 0;
    std::string message;
    std::string reopenMessage; // shown when the menu opens again after a room's session
    std::string address;
    std::size_t caret = 0u;
    bool keyboard = false; // a key was typed in this editor, so the keyboard's pad aliases are ignored
    bool launched = false, covering = false, drawn = false;
    std::string name; // this player's profile name, read when a room opens
    RecompNetplay net{};
    std::shared_ptr<Lookup> lookup; // the matchmaker, for a listed host or FIND MATCH
    LobbyState lobby;
    FindState find;
};
extern Menu menu;

// netplay_menu.cpp
void saveSettings();
bool blockedId(std::uint64_t id);
bool nameAllowed();
void lookUpMatchmaker();
LookupState takeLookup();
void copyText(const std::string &text);
void stepDelay(std::uint16_t keys);
void title(const char *text);
void footer(const char *text);
void plateName(float x, float y, const std::string &name, std::uint32_t color);

// netplay_menu_room.cpp
void enterRoom(bool hosting);
void host(bool bound, bool listed);
void join(bool bound = false);
bool updateRoom(std::uint16_t keys);
void drawRoom();

// netplay_menu_find.cpp
void find();
void updateFind(std::uint16_t keys);
void drawFind();

} // namespace netplay_menu

#endif
