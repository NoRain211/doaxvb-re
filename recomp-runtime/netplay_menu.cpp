/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "netplay_menu.h"
#include "netplay_menu_screens.h"

#include "input_text.h"
#include "input_pulse_source.h"
#include "netplay.h"
#include "netplay_game.h"
#include "netplay_settings.h"
#include "overlay_screen.h"
#include "rollback.h"
#include "runtime.h"
#include "stop_report.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace netplay_menu {

Menu menu;

const std::string AddressCharacters = "0123456789.:";
const std::string HostCharacters = "0123456789.:-abcdefghijklmnopqrstuvwxyz";
// NETPLAY SETTINGS' rows, top to bottom.
enum SettingsRow : int { SettingsDelay, SettingsPort, SettingsDisplay, SettingsUnblock, SettingsMatchmaker, SettingsRows };

// The room's check for a player ID this player blocked.
bool blockedId(std::uint64_t id)
{
    return std::any_of(menu.blocked.begin(), menu.blocked.end(), [id](const auto &entry) { return entry.first == id; });
}

static std::uint16_t keysOf(const RecompInputGamepad &pad)
{
    std::uint16_t keys = pad.buttons & 0x0fu; // the d-pad bits match Up, Down, Left, Right
    if (pad.thumb_ly > 16000) keys |= Up;
    if (pad.thumb_ly < -16000) keys |= Down;
    if (pad.thumb_lx < -16000) keys |= Left;
    if (pad.thumb_lx > 16000) keys |= Right;
    const std::uint16_t face[4] = {A, B, X, Y};
    for (unsigned i = 0u; i < 4u; ++i)
        if (pad.analog_buttons[i] > 0x40u) keys |= face[i];
    if ((pad.buttons & 0x20u) != 0u) keys |= B; // Back
    return keys;
}

static bool exhibitionChosen() { return recomp_netplay_game_exhibition_chosen(); }

#ifdef _WIN32
static std::string pasteText()
{
    std::string text;
    if (!OpenClipboard(nullptr)) return text;
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto *chars = static_cast<const wchar_t *>(GlobalLock(data))) {
            for (const wchar_t *c = chars; *c != 0 && text.size() < 256u; ++c)
                if (*c < 128) text.push_back(static_cast<char>(*c));
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return text;
}

void copyText(const std::string &text)
{
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    if (HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1u) * sizeof(wchar_t))) {
        auto *chars = static_cast<wchar_t *>(GlobalLock(data));
        for (std::size_t i = 0u; i < text.size(); ++i) chars[i] = static_cast<unsigned char>(text[i]);
        chars[text.size()] = 0;
        GlobalUnlock(data);
        if (SetClipboardData(CF_UNICODETEXT, data) == nullptr) GlobalFree(data);
    }
    CloseClipboard();
}
#else
// shortcut: the clipboard is Windows only; use SDL's when the Linux runner draws this menu.
static std::string pasteText() { return {}; }
void copyText(const std::string &) {}
#endif

static void loadSettings()
{
    recomp_netplay_settings_load(menu.storage / "netplay.txt", menu);
}

void saveSettings()
{
    recomp_netplay_settings_save(menu.storage / "netplay.txt", menu);
}

static void close(bool local)
{
    menu.screen = Screen::Closed;
    menu.swallow = true;
    menu.injectA = local ? 3 : 0;
}

// Checks the player's own name before any socket opens.
bool nameAllowed()
{
    menu.name = menu.ownName();
    if (menu.name.empty()) menu.name = "PLAYER";
    if (!recomp_netplay_name_banned(menu.name.c_str())) return true;
    menu.message = "YOUR PROFILE NAME IS NOT ALLOWED ONLINE.";
    return false;
}

// The default matchmaker's host:port, XORed with 0x5a so it is not plain text in the source or the binary.
// shortcut: this only keeps the name out of a casual search; DNS lookups still show it.
static std::string defaultMatchmaker()
{
    static const unsigned char coded[] = {0x37, 0x37, 0x74, 0x34, 0x3f, 0x3c, 0x3c, 0x39, 0x35, 0x28, 0x3e,
        0x74, 0x39, 0x35, 0x37, 0x60, 0x6e, 0x6d, 0x6d, 0x6a, 0x6a};
    std::string server;
    for (const unsigned char c : coded) server.push_back(static_cast<char>(c ^ 0x5au));
    return server;
}

// Looks up the matchmaker from NETPLAY SETTINGS, or the default when none is set. Call after a socket
// opened, which starts Windows sockets.
void lookUpMatchmaker()
{
    const std::string server = menu.matchmaker.empty() ? defaultMatchmaker() : menu.matchmaker;
    auto job = std::make_shared<Lookup>();
    menu.lookup = job;
    std::thread([job, server] {
        const std::size_t colon = server.rfind(':');
        const unsigned long port =
            colon == std::string::npos ? 0u : std::strtoul(server.c_str() + colon + 1u, nullptr, 10);
        const bool found = port != 0u && port <= 65535u &&
            recomp_netplay_match_server(&job->found, server.substr(0u, colon).c_str(), static_cast<std::uint16_t>(port));
        job->state.store(found ? LookupState::Found : LookupState::NotFound);
    }).detach();
}

// Points the socket at the matchmaker once its lookup is done.
LookupState takeLookup()
{
    if (!menu.lookup) return LookupState::None;
    const LookupState state = menu.lookup->state.load();
    if (state == LookupState::Looking) return state;
    if (state == LookupState::Found) {
        menu.net.match.server_address = menu.lookup->found.match.server_address;
        menu.net.match.server_port = menu.lookup->found.match.server_port;
    }
    menu.lookup.reset();
    return state;
}

void stepDelay(std::uint16_t keys)
{
    const int delay = static_cast<int>(menu.delay) + ((keys & Right) ? 1 : -1);
    if (delay >= 0 && delay <= static_cast<int>(RECOMP_ROLLBACK_WINDOW)) menu.delay = static_cast<std::uint32_t>(delay);
}

static void openEditor(Screen screen, const std::string &text)
{
    char stale[64]; // typed before the editor opened, such as the Z that pressed A
    (void)recomp_input_text_take(stale, sizeof stale);
    menu.keyboard = false;
    menu.caret = text.size();
    menu.screen = screen;
}

static void pasteInto(std::string &text, const std::string &characters, std::size_t max)
{
    std::string pasted;
    for (char c : pasteText()) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (characters.find(c) != std::string::npos && pasted.size() < max) pasted.push_back(c);
    }
    if (pasted.empty()) menu.message = "THE CLIPBOARD HAS NO ADDRESS.";
    else {
        text = pasted;
        menu.caret = text.size();
    }
}

// The editor for an address: typed keys insert at the caret, Backspace deletes,
// Ctrl+V pastes, Enter is A and Esc is B. On the pad, up/down change the
// character at the caret, left/right move it, Y deletes and X pastes.
// Returns the keys left for the screen.
static std::uint16_t editText(std::string &text, const std::string &characters, std::size_t max, std::uint16_t keys)
{
    char typed[64];
    const std::size_t typedCount = recomp_input_text_take(typed, sizeof typed);
    if (keys || typedCount) menu.message.clear();
    std::uint16_t typedKeys = 0u;
    for (std::size_t i = 0u; i < typedCount; ++i) {
        char c = typed[i];
        menu.keyboard = true;
        if (c == '\r') typedKeys |= A;
        else if (c == 0x1b) typedKeys |= B;
        else if (c == 0x16) pasteInto(text, characters, max);
        else if (c == '\b') {
            if (menu.caret > 0u) text.erase(--menu.caret, 1u);
        } else {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            if (characters.find(c) != std::string::npos && text.size() < max) text.insert(menu.caret++, 1u, c);
        }
    }
    if (menu.keyboard) keys &= Up | Down | Left | Right; // Z, X, A, S and Backspace are pad buttons too
    keys |= typedKeys;
    if ((keys & Left) && menu.caret > 0u) --menu.caret;
    if ((keys & Right) && menu.caret < text.size()) ++menu.caret;
    if (keys & (Up | Down)) {
        const std::size_t count = characters.size();
        if (menu.caret == text.size() && text.size() < max)
            text.push_back((keys & Up) ? characters.front() : characters.back());
        else if (menu.caret < text.size()) {
            const std::size_t at = characters.find(text[menu.caret]);
            text[menu.caret] = characters[(at + ((keys & Up) ? 1u : count - 1u)) % count];
        }
    }
    if ((keys & Y) && !text.empty()) {
        if (menu.caret == text.size()) --menu.caret;
        text.erase(menu.caret, 1u);
    }
    if (keys & X) pasteInto(text, characters, max);
    return keys;
}

static void update(std::uint16_t keys, bool &launch)
{
    switch (menu.screen) {
    case Screen::Closed:
        return;
    case Screen::Online:
        if (keys) menu.message.clear();
        if (keys & Up) menu.cursor = (menu.cursor + OnlineRows - 1) % OnlineRows;
        if (keys & Down) menu.cursor = (menu.cursor + 1) % OnlineRows;
        if ((keys & (Left | Right)) && menu.cursor == OnlineHost) {
            menu.privateRoom = !menu.privateRoom;
            saveSettings();
        }
        if (keys & B) close(false);
        else if (keys & A) {
            if (menu.cursor == OnlineLocal) close(true);
            else if (menu.cursor == OnlineHost) host(false, !menu.privateRoom);
            else if (menu.cursor == OnlineJoin) {
                menu.address = menu.recent;
                openEditor(Screen::Join, menu.address);
            } else if (menu.cursor == OnlineFind) {
                find();
            } else {
                menu.cursor = SettingsDelay;
                menu.screen = Screen::Settings;
            }
        }
        return;
    case Screen::Settings:
        if (keys) menu.message.clear();
        if (keys & Up) menu.cursor = (menu.cursor + SettingsRows - 1) % SettingsRows;
        if (keys & Down) menu.cursor = (menu.cursor + 1) % SettingsRows;
        if (keys & (Left | Right)) {
            const int step = (keys & Right) ? 1 : -1;
            if (menu.cursor == SettingsDelay) {
                stepDelay(keys);
            } else if (menu.cursor == SettingsPort) {
                const int port = static_cast<int>(menu.port) + step;
                if (port >= 1024 && port <= 65535) menu.port = static_cast<std::uint32_t>(port);
            } else if (menu.cursor == SettingsDisplay) {
                menu.display = (menu.display + 3 + step) % 3;
            }
        }
        if ((keys & A) && menu.cursor == SettingsUnblock) {
            menu.blocked.clear();
            menu.find.blockedRooms.clear();
            saveSettings();
            menu.message = "NO ONE IS BLOCKED NOW.";
        } else if ((keys & A) && menu.cursor == SettingsMatchmaker) {
            openEditor(Screen::Matchmaker, menu.matchmaker);
        } else if (keys & (A | B)) {
            saveSettings();
            menu.cursor = OnlineSettings;
            menu.screen = Screen::Online;
        }
        return;
    case Screen::Join:
        keys = editText(menu.address, AddressCharacters, RecompNetplayMaxAddress, keys);
        if (keys & B) {
            menu.cursor = OnlineJoin;
            menu.screen = Screen::Online;
        } else if (keys & A) {
            join();
        }
        return;
    case Screen::Matchmaker:
        keys = editText(menu.matchmaker, HostCharacters, RecompNetplayMaxMatchmaker, keys);
        if (keys & (A | B)) {
            saveSettings();
            menu.screen = Screen::Settings;
        }
        return;
    case Screen::Find:
        updateFind(keys);
        return;
    case Screen::Room:
        launch = updateRoom(keys);
        return;
    }
}

void title(const char *text)
{
    // Opaque while the game walks back to the main menu behind a reopened room.
    recomp_overlay_rect(-1000.0f, 0.0f, 2640.0f, 480.0f, menu.covering ? 0xff000000u : Backdrop);
    recomp_overlay_rect(40.0f, 38.0f, 6.0f, 52.0f, Red);
    recomp_overlay_text(58.0f, 40.0f, 4.0f, White, text);
    recomp_overlay_text(60.0f, 78.0f, 2.0f, Gray, "DEAD OR ALIVE XTREME BEACH VOLLEYBALL");
}

void footer(const char *text)
{
    if (!menu.message.empty()) recomp_overlay_text(40.0f, 396.0f, 2.0f, Yellow, menu.message.c_str());
    recomp_overlay_rect(40.0f, 428.0f, 560.0f, 2.0f, Dim);
    recomp_overlay_text(40.0f, 442.0f, 2.0f, Gray, text);
}

// An address editor's controls, for the keyboard once a key has been typed.
static void editorHints(const char *padFooter, const char *keyFooter)
{
    if (menu.keyboard) {
        recomp_overlay_text(60.0f, 222.0f, 2.0f, Gray, "TYPE   LEFT/RIGHT MOVE");
        recomp_overlay_text(60.0f, 248.0f, 2.0f, Gray, "BACKSPACE DELETE   CTRL+V PASTE");
        footer(keyFooter);
        return;
    }
    recomp_overlay_text(60.0f, 222.0f, 2.0f, Gray, "UP/DOWN CHANGE   LEFT/RIGHT MOVE");
    recomp_overlay_text(60.0f, 248.0f, 2.0f, Gray, "Y DELETE   X PASTE FROM CLIPBOARD");
    recomp_overlay_text(60.0f, 270.0f, 2.0f, Gray, "OR TYPE IT ON THE KEYBOARD.");
    footer(padFooter);
}

static void item(float y, bool selected, const char *text)
{
    if (selected) recomp_overlay_rect(40.0f, y - 10.0f, 420.0f, 41.0f, Red);
    recomp_overlay_text(60.0f, y, 3.0f, selected ? White : Gray, text);
}

// A name at the largest size that fits a 218-pixel plate line.
void plateName(float x, float y, const std::string &name, std::uint32_t color)
{
    if (name.size() <= 12u) recomp_overlay_text(x, y, 3.0f, color, name.c_str());
    else recomp_overlay_text(x, y + 4.0f, 2.0f, color, name.substr(0u, 18u).c_str());
}

static void draw()
{
    static const char *const items[OnlineRows] = {"LOCAL", "HOST ONLINE", "JOIN ONLINE", "FIND MATCH", "NETPLAY SETTINGS"};
    static const char *const details[OnlineRows] = {
        "TWO CONTROLLERS ON THIS MACHINE.",
        "WAIT FOR A PLAYER. FIND MATCH LISTS THE ROOM.",
        "CONNECT TO A PLAYER WHO IS HOSTING.",
        "PICK AN OPEN ROOM, OR QUICK MATCH.",
        "DELAY, PORT, DISPLAY, BLOCKS, MATCHMAKER.",
    };
    switch (menu.screen) {
    case Screen::Closed:
        return;
    case Screen::Online:
        title("EXHIBITION");
        for (int i = 0; i < OnlineRows; ++i) item(134.0f + 44.0f * static_cast<float>(i), menu.cursor == i, items[i]);
        {
            const char *room = menu.privateRoom ? "< PRIVATE >" : "< PUBLIC >";
            recomp_overlay_text(600.0f - recomp_overlay_text_width(room, 2.0f), 182.0f, 2.0f, White, room);
        }
        recomp_overlay_text(40.0f, 362.0f, 2.0f, White,
            menu.cursor == OnlineHost && menu.privateRoom ? "ONLY PLAYERS WITH YOUR ADDRESS CAN JOIN." : details[menu.cursor]);
        footer(menu.cursor == OnlineHost ? "LEFT/RIGHT ROOM   A SELECT   B BACK" : "A SELECT   B BACK");
        return;
    case Screen::Settings: {
        title("NETPLAY SETTINGS");
        const std::string delay = "< " + std::to_string(menu.delay) + " FRAMES >";
        const std::string port = "< " + std::to_string(menu.port) + " >";
        item(134.0f, menu.cursor == SettingsDelay, "INPUT DELAY");
        recomp_overlay_text(600.0f - recomp_overlay_text_width(delay.c_str(), 2.0f), 138.0f, 2.0f, White, delay.c_str());
        item(178.0f, menu.cursor == SettingsPort, "PORT");
        recomp_overlay_text(600.0f - recomp_overlay_text_width(port.c_str(), 2.0f), 182.0f, 2.0f, White, port.c_str());
        const std::string display = std::string("< ") + RecompNetplayDisplayNames[menu.display] + " >";
        item(222.0f, menu.cursor == SettingsDisplay, "NETWORK DISPLAY");
        recomp_overlay_text(600.0f - recomp_overlay_text_width(display.c_str(), 2.0f), 226.0f, 2.0f, White,
            display.c_str());
        const std::string count = menu.blocked.empty() ? "NONE" : std::to_string(menu.blocked.size()) + " BLOCKED";
        item(266.0f, menu.cursor == SettingsUnblock, "UNBLOCK ALL");
        recomp_overlay_text(600.0f - recomp_overlay_text_width(count.c_str(), 2.0f), 270.0f, 2.0f, White, count.c_str());
        std::string names;
        for (const auto &entry : menu.blocked) names += (names.empty() ? "" : ", ") + entry.second;
        if (names.size() > 46u) names = names.substr(0u, 43u) + "...";
        const std::string server = menu.matchmaker.empty() ? "DEFAULT" : menu.matchmaker;
        item(310.0f, menu.cursor == SettingsMatchmaker, "MATCHMAKER");
        // A value too wide for the 132 pixels right of the highlight goes on its own line below the row.
        const bool fits = recomp_overlay_text_width(server.c_str(), 2.0f) <= 132.0f;
        const float scale = fits ? 2.0f : 1.5f;
        recomp_overlay_text(600.0f - recomp_overlay_text_width(server.c_str(), scale), fits ? 314.0f : 346.0f, scale,
            White, server.c_str());
        const char *const details[SettingsRows] = {
            "LOWER DELAY FEELS SHARPER, MORE ROLLBACK.",
            "THE HOST'S ROUTER MUST LET THIS PORT IN.",
            "PING, OR FULL STATS, IN A SESSION'S CORNER.",
            names.empty() ? "PLAYERS YOU BLOCK IN A ROOM SHOW HERE." : names.c_str(),
            "THE SERVER FIND MATCH USES, HOST:PORT.",
        };
        recomp_overlay_text(40.0f, 364.0f, 2.0f, White, details[menu.cursor]);
        footer(menu.cursor == SettingsMatchmaker ? "A EDIT   B BACK" : menu.cursor == SettingsUnblock ? "A UNBLOCK ALL   B BACK" : "LEFT/RIGHT CHANGE   B BACK");
        return;
    }
    case Screen::Join: {
        title("JOIN ONLINE");
        recomp_overlay_text(60.0f, 132.0f, 2.0f, Gray, "HOST ADDRESS");
        recomp_overlay_rect(56.0f, 154.0f, 528.0f, 44.0f, Panel);
        // Addresses stay off screen, so a stream never shows one; the pad still sees the character it changes.
        std::string masked(menu.address.size(), '*');
        if (menu.caret < masked.size()) masked[menu.caret] = menu.address[menu.caret];
        recomp_overlay_text(68.0f, 165.0f, 3.0f, White, masked.c_str());
        if ((menu.frame / 20u) % 2u == 0u)
            recomp_overlay_rect(68.0f + 18.0f * static_cast<float>(menu.caret), 189.0f, 15.0f, 3.0f, Red);
        if (!menu.recent.empty())
            recomp_overlay_text(60.0f, 296.0f, 2.0f, Dim, "THE LAST HOST IS FILLED IN.");
        editorHints("A CONNECT   B BACK", "ENTER CONNECT   ESC BACK");
        return;
    }
    case Screen::Matchmaker: {
        title("MATCHMAKER");
        recomp_overlay_text(60.0f, 132.0f, 2.0f, Gray, "SERVER  HOST:PORT");
        recomp_overlay_rect(56.0f, 154.0f, 528.0f, 44.0f, Panel);
        recomp_overlay_text(68.0f, 169.0f, 2.0f, White, menu.matchmaker.c_str());
        if ((menu.frame / 20u) % 2u == 0u)
            recomp_overlay_rect(68.0f + 12.0f * static_cast<float>(menu.caret), 186.0f, 10.0f, 2.0f, Red);
        recomp_overlay_text(60.0f, 296.0f, 2.0f, Dim, "LEAVE IT EMPTY FOR THE DEFAULT SERVER.");
        editorHints("A/B DONE", "ENTER/ESC DONE");
        return;
    }
    case Screen::Find:
        drawFind();
        return;
    case Screen::Room:
        drawRoom();
        return;
    }
}

static void onFrame()
{
    bool launch = false;
    {
        std::lock_guard<std::mutex> guard(menu.lock);
        if (menu.screen == Screen::Closed) {
            if (menu.drawn) recomp_overlay_screen_publish(); // nothing queued: clears the screen
            menu.drawn = false;
            return;
        }
        ++menu.frame;
        std::uint16_t down = 0u;
        for (const std::uint16_t held : menu.held) down |= held;
        std::uint16_t keys = menu.pressed;
        menu.pressed = 0u;
        // A held direction repeats after 300 ms, then every 67 ms.
        const std::uint16_t directions = down & (Up | Down | Left | Right);
        if (directions == 0u || (keys & directions) != 0u) menu.repeat = 0u;
        else if (++menu.repeat >= 18u && (menu.repeat - 18u) % 4u == 0u) keys |= directions;
        update(keys, launch);
        if (launch) {
            // The session takes the socket (recomp_netplay_menu_take_launch); the router forward stays for it.
            menu.screen = Screen::Closed;
            menu.launched = true;
            return;
        }
        if (menu.covering && exhibitionChosen()) menu.covering = false;
        draw();
        recomp_overlay_screen_publish();
        menu.drawn = true;
    }
}

} // namespace netplay_menu

using namespace netplay_menu;

void recomp_netplay_menu_install(const std::filesystem::path &storage, const std::string &digest,
    std::function<std::string()> ownName)
{
    // Later boots, back from a session, keep the menu's settings, such as a delay picked in the room.
    if (!menu.installed) {
        menu.storage = storage;
        loadSettings();
    }
    menu.installed = true;
    menu.digest = digest;
    menu.digest.resize(RECOMP_NETPLAY_DIGEST_BYTES, '0');
    menu.ownName = std::move(ownName);
}

void recomp_netplay_menu_frame()
{
    if (!menu.installed) return;
    {
        std::lock_guard<std::mutex> guard(menu.lock);
        if (menu.screen == Screen::Closed && !menu.reopenMessage.empty() && exhibitionChosen()) {
            menu.screen = Screen::Online;
            menu.cursor = OnlineHost;
            menu.message = menu.reopenMessage;
            menu.reopenMessage.clear();
            menu.pressed = 0u;
        }
    }
    onFrame();
}

static std::string endedBecause(const std::string &reason)
{
    return reason == "rollback:peer-left" ? "THE OTHER PLAYER LEFT THE MATCH."
        : reason == "rollback:peer-timeout" ? "THE OTHER PLAYER STOPPED ANSWERING."
        : reason == "rollback:desync" ? "THE TWO GAMES WENT OUT OF STEP."
        : reason == "netplay:session-failed" ? "THE MATCH COULD NOT START."
        : "THE MATCH ENDED.";
}

void recomp_netplay_menu_reopen(const std::string &reason)
{
    std::lock_guard<std::mutex> guard(menu.lock);
    menu.reopenMessage = endedBecause(reason);
}

void recomp_netplay_menu_filter(std::uint32_t port, RecompInputGamepad *pad)
{
    if (!menu.installed || port >= RECOMP_INPUT_PORT_COUNT) return;
    std::lock_guard<std::mutex> guard(menu.lock);
    const std::uint16_t keys = keysOf(*pad);
    const std::uint16_t edges = keys & ~menu.held[port];
    menu.held[port] = keys;
    if (menu.screen == Screen::Closed) {
        if (menu.swallow) {
            bool down = false;
            for (const std::uint16_t held : menu.held) down = down || held != 0u;
            if (down) {
                *pad = RecompInputGamepad{};
                return;
            }
            menu.swallow = false;
        }
        if (menu.injectA > 0 && port == menu.openedBy) {
            --menu.injectA;
            pad->analog_buttons[RECOMP_INPUT_ANALOG_A] = RECOMP_INPUT_ANALOG_PRESSED;
            return;
        }
        if ((edges & A) == 0u || !exhibitionChosen()) return;
        menu.screen = Screen::Online;
        menu.cursor = OnlineLocal;
        menu.message.clear();
        menu.openedBy = port;
        menu.pressed = 0u;
    } else {
        menu.pressed |= edges;
    }
    *pad = RecompInputGamepad{};
}

bool recomp_netplay_menu_take_launch(std::uint32_t &hostPort, std::uint32_t &delay, std::string &join,
    RecompNetplayRoomState &room, RecompNetplay &socket)
{
    std::lock_guard<std::mutex> guard(menu.lock);
    if (!menu.launched) return false;
    menu.launched = false;
    hostPort = menu.lobby.hosting ? menu.port : 0u;
    delay = menu.delay;
    join = menu.lobby.hosting ? std::string() : menu.lobby.peerAddress;
    room = {menu.lobby.hosting, menu.lobby.listing, menu.lobby.found, menu.lobby.peerAddress};
    socket = menu.net; // the menu reopens nothing until the session hands it back
    return true;
}

int recomp_netplay_menu_display(const std::filesystem::path &storage)
{
    // A copy, so a delay the host picked in the room survives the session.
    RecompNetplaySettings settings;
    recomp_netplay_settings_load(storage / "netplay.txt", settings);
    return settings.display;
}

void recomp_netplay_menu_return_to_room(const RecompNetplay &socket, const RecompNetplayRoomState &room,
    const std::string &reason)
{
    std::lock_guard<std::mutex> guard(menu.lock);
    menu.lobby.hosting = room.hosting;
    menu.lobby.listing = room.listing;
    menu.lobby.found = room.found;
    menu.lobby.peerAddress = room.peerAddress;
    menu.openedBy = 0u;
    menu.message.clear();
    menu.covering = true;
    // The socket, its peer and how it reaches the peer carry over; the host's room greets the last joiner first.
    menu.net = RecompNetplay{};
    menu.net.socket = socket.socket;
    menu.net.peer_address = socket.peer_address;
    menu.net.peer_port = socket.peer_port;
    menu.net.match = socket.match;
    menu.net.match.punching = false;
    // A failure leaves the online screen up with its message.
    menu.screen = Screen::Online;
    menu.cursor = menu.lobby.hosting ? OnlineHost : menu.lobby.found ? OnlineFind : OnlineJoin;
    if (menu.lobby.hosting) {
        host(true, menu.lobby.listing);
    } else if (menu.lobby.found) {
        if (nameAllowed()) enterRoom(false); // a matched joiner has no address to call
    } else {
        menu.address = menu.lobby.peerAddress;
        join(true);
    }
    if (menu.screen != Screen::Room) recomp_netplay_close(&menu.net);
    else menu.message = endedBecause(reason);
}
