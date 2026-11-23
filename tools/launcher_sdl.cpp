// SPDX-License-Identifier: GPL-3.0-or-later
// macOS and Linux launcher: the counterpart of tools/launcher.ps1, with its
// sections, look and controls. It opens before the game is built (launcher.sh
// builds it on its own with tools/build_launcher.py), so its Build page finds
// the player's ISO and runs build_game.sh. Play saves private/launcher.json,
// which run_game.sh applies, and exits 0; closing exits 1.
//
// The Direct3D 11 options (SMAA, VRR, sun shadows, per-pixel lighting, smooth
// normals, RTGI, texture packs) and the music folder (Media Foundation) are
// Windows-only, so they are left out; their saved values are kept untouched.
// Gamepad, keyboard, mouse and touch all work, so it runs in Steam Game Mode.
#include <SDL3/SDL.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <stb_image.h>

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

extern char **environ;

namespace {

namespace fs = std::filesystem;

// One 640x400 canvas, letterboxed into the window: 2x fills the Steam Deck's
// 1280x800 in Game Mode. Positions follow the Windows launcher's 864x576
// layout at three quarters.
constexpr int kWidth = 640, kHeight = 400;

const int kHeights[] = {480, 720, 1080, 1440, 2160};
const int kSamples[] = {1, 2, 4, 8};
const int kVolumes[] = {100, 75, 50, 25, 0};
// What the game does when its title screen is left idle; see docs/title-idle.md.
const char *const kIdleValues[] = {"default", "no-cutscene", "cpu-exhibition"};
const char *const kIdleNames[] = {"Default", "No cutscene", "CPU Exhibition"};

enum Section { MAIN, BUILD, GRAPHICS, AUDIO, GAME, HELP, SECTION_COUNT };
const char *const kSections[] = {"Main Menu", "Build", "Graphics", "Audio", "Game", "Help"};
// The SDL GPU presenter runs on Metal on macOS and on Vulkan elsewhere.
#ifdef __APPLE__
constexpr const char *kRenderer = "Metal";
#else
constexpr const char *kRenderer = "Vulkan";
#endif
const char *const kSubtitles[] = {"Your next visit to Zack Island", "Build the game from your ISO",
    "Display and image quality", "Volume and music", "Swimsuits, demo and camera", "Keys while playing"};

// The game's island-map menu colours, as in launcher.ps1.
struct Color { Uint8 r, g, b, a = 255; };
constexpr Color kInk{255, 255, 255}, kSoft{217, 246, 247}, kField{13, 79, 99}, kLogBack{6, 47, 62};
constexpr Color kOutline{29, 79, 184}, kDarkOutline{11, 58, 102}, kReady{154, 245, 200}, kWarn{255, 210, 122};

Color hex(unsigned value, Uint8 alpha = 255)
{
    return {Uint8(value >> 16), Uint8(value >> 8), Uint8(value), alpha};
}

// --- private/launcher.json -------------------------------------------------
// A flat object shared with launcher.ps1. Keys this launcher does not use, such
// as the Direct3D 11 options, keep their saved raw values.
struct Json {
    std::vector<std::pair<std::string, std::string>> items;

    static size_t skipString(const std::string &text, size_t at)
    {
        for (++at; at < text.size(); ++at) {
            if (text[at] == '\\') ++at;
            else if (text[at] == '"') return at + 1;
        }
        return text.size();
    }

    void parse(const std::string &text)
    {
        size_t at = text.find('{');
        while (at != std::string::npos && at < text.size()) {
            const size_t key = text.find('"', at);
            if (key == std::string::npos) return;
            const size_t key_end = skipString(text, key);
            const size_t colon = text.find(':', key_end);
            if (colon == std::string::npos) return;
            size_t value = text.find_first_not_of(" \t\r\n", colon + 1);
            if (value == std::string::npos) return;
            size_t end = value;
            int depth = 0;
            while (end < text.size()) {
                const char c = text[end];
                if (c == '"') { end = skipString(text, end); continue; }
                if (c == '[' || c == '{') ++depth;
                if ((c == ']' || c == '}') && depth-- == 0) break;
                if (c == ',' && depth == 0) break;
                ++end;
            }
            std::string raw = text.substr(value, end - value);
            raw.erase(raw.find_last_not_of(" \t\r\n") + 1);
            items.emplace_back(unquote(text.substr(key, key_end - key)), raw);
            at = end < text.size() && text[end] == ',' ? end + 1 : std::string::npos;
        }
    }

    const std::string *raw(const std::string &key) const
    {
        for (const auto &item : items)
            if (item.first == key) return &item.second;
        return nullptr;
    }
    void set(const std::string &key, const std::string &raw)
    {
        for (auto &item : items)
            if (item.first == key) { item.second = raw; return; }
        items.emplace_back(key, raw);
    }

    int number(const char *key, int fallback) const
    {
        const std::string *value = raw(key);
        return value && !value->empty() && (std::isdigit(static_cast<unsigned char>((*value)[0])) ||
            (*value)[0] == '-') ? std::atoi(value->c_str()) : fallback;
    }
    bool flag(const char *key, bool fallback) const
    {
        const std::string *value = raw(key);
        return value == nullptr ? fallback : *value == "true" ? true : *value == "false" ? false : fallback;
    }
    std::string text(const char *key) const
    {
        const std::string *value = raw(key);
        return value && !value->empty() && (*value)[0] == '"' ? unquote(*value) : std::string();
    }

    static std::string unquote(const std::string &quoted)
    {
        std::string out;
        for (size_t i = 1; i + 1 < quoted.size(); ++i) {
            char c = quoted[i];
            if (c == '\\' && i + 2 < quoted.size()) {
                c = quoted[++i];
                if (c == 'n') c = '\n';
                else if (c == 't') c = '\t';
                else if (c == 'u' && i + 4 < quoted.size()) {  // only ASCII escapes occur here
                    c = static_cast<char>(std::strtol(quoted.substr(i + 1, 4).c_str(), nullptr, 16));
                    i += 4;
                }
            }
            out += c;
        }
        return out;
    }
    static std::string quote(const std::string &value)
    {
        std::string out = "\"";
        for (const char c : value) {
            if (c == '"' || c == '\\') out += '\\';
            if (static_cast<unsigned char>(c) < 0x20) {
                char escape[8];
                std::snprintf(escape, sizeof escape, "\\u%04x", c);
                out += escape;
            } else {
                out += c;
            }
        }
        return out + "\"";
    }

    std::string dump() const
    {
        std::string out = "{\n";
        for (size_t i = 0; i < items.size(); ++i)
            out += "  " + quote(items[i].first) + ": " + items[i].second + (i + 1 < items.size() ? ",\n" : "\n");
        return out + "}\n";
    }
};

std::string readFile(const fs::path &path)
{
    std::ifstream file(path, std::ios::binary);
    std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.erase(0, 3);  // Windows PowerShell 5 writes a BOM
    return text;
}

template <size_t N>
int indexOf(const int (&values)[N], int value, int fallback)
{
    const auto it = std::find(std::begin(values), std::end(values), value);
    return it == std::end(values) ? fallback : static_cast<int>(it - std::begin(values));
}

std::string lower(std::string text)
{
    for (char &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string upper(std::string text)
{
    for (char &c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

// The end of a long path or name, so the distinguishing part stays visible.
std::string tail(const std::string &text, size_t chars)
{
    return text.size() <= chars ? text : "..." + text.substr(text.size() - (chars - 3));
}

// --- Finding the player's ISO ----------------------------------------------
// Where players keep disc images: home folders, EmuDeck's Xbox folder, and
// removable drives such as the Deck's SD card. Shallow, so it stays quick.
std::vector<std::string> findIsos()
{
    std::vector<std::pair<fs::path, int>> roots;
    const char *home = std::getenv("HOME");
    if (home != nullptr) {
        roots.push_back({home, 3});
        roots.push_back({fs::path(home) / "Emulation/roms/xbox", 1});
    }
    std::error_code error;
    for (const char *base : {"/run/media", "/media", "/mnt", "/Volumes"}) {
        for (const auto &entry : fs::directory_iterator(base, error)) {
            if (!entry.is_directory(error)) continue;
            // /run/media/<user>/<drive> and /run/media/<drive> both occur.
            roots.push_back({entry.path(), 3});
            roots.push_back({entry.path() / "Emulation/roms/xbox", 1});
        }
    }
    std::set<std::string> seen;
    std::vector<std::string> found;
    for (const auto &[root, depth] : roots) {
        int visited = 0;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end;
        for (; !error && it != end && visited < 20000; it.increment(error), ++visited) {
            const std::string name = it->path().filename().string();
            if (it->is_directory(error)) {
                if (it.depth() + 1 >= depth || name.empty() || name[0] == '.') it.disable_recursion_pending();
                continue;
            }
            const std::string ext = lower(it->path().extension().string());
            // Xbox discs are several GB; smaller images are other systems' games.
            if ((ext != ".iso" && ext != ".xiso") || it->file_size(error) < (1ull << 30) || error) {
                error.clear();
                continue;
            }
            const std::string path = fs::weakly_canonical(it->path(), error).string();
            if (!error && seen.insert(path).second) found.push_back(path);
            error.clear();
        }
        error.clear();
    }
    // Images named like this game first.
    std::stable_sort(found.begin(), found.end(), [](const std::string &a, const std::string &b) {
        auto likely = [](const std::string &path) {
            const std::string name = lower(fs::path(path).filename().string());
            return name.find("volleyball") != std::string::npos || name.find("doax") != std::string::npos;
        };
        return likely(a) && !likely(b);
    });
    return found;
}

// True when dotted version a, such as "0.8.0", is later than b. A part that
// is not a number, as in "0.8.0-rc1", ends that version there.
bool newerVersion(const std::string &a, const std::string &b)
{
    int x[3] = {}, y[3] = {};
    std::sscanf(a.c_str(), "%d.%d.%d", &x[0], &x[1], &x[2]);
    std::sscanf(b.c_str(), "%d.%d.%d", &y[0], &y[1], &y[2]);
    return std::lexicographical_compare(y, y + 3, x, x + 3);
}

// --- Art -------------------------------------------------------------------
SDL_Texture *loadArt(SDL_Renderer *renderer, const fs::path &path)
{
    int width = 0, height = 0, channels = 0;
    stbi_uc *pixels = stbi_load(path.string().c_str(), &width, &height, &channels, 4);
    if (pixels == nullptr) return nullptr;
    SDL_Surface *surface = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32, pixels, width * 4);
    SDL_Texture *texture = surface ? SDL_CreateTextureFromSurface(renderer, surface) : nullptr;
    if (texture) SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    SDL_DestroySurface(surface);
    stbi_image_free(pixels);
    return texture;
}

// --- Pages and their controls ----------------------------------------------
enum Id { RESOLUTION, MSAA, FOG, VOLUME, SHUFFLE, UNLOCK, DEMO_CUT, FREE_CAMERA, IDLE, ISO, BROWSE, START_BUILD, PLAY, UPDATE, MUSIC };
enum class Kind { Choice, Toggle, Button };

struct Control {
    Id id;
    Kind kind;
    std::string label;
    SDL_FRect rect;
    bool enabled = true;
};

constexpr float kContentX = 272, kContentW = 340, kRowH = 20, kFirstRow = 112, kRowGap = 26;
constexpr SDL_FRect kPlayRect{440, 314, 172, 34};

SDL_FRect row(int index) { return {kContentX, kFirstRow + index * kRowGap, kContentW, kRowH}; }
SDL_FRect sectionRect(int index) { return {16, 132 + index * 34.0f, 210, 30}; }

struct Launcher {
    std::string root;
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    Json json;
    int choice[IDLE + 1] = {};
    int section = MAIN;
    int focus = 0;  // index into controls(section)
    SDL_Texture *backdrop = nullptr, *logo = nullptr, *buttonA = nullptr, *buttonB = nullptr;
    bool customBackdrop = false;

    std::vector<std::string> isos;
    int isoIndex = 0;
    bool searched = false;
    std::mutex pickedMutex;
    std::string picked;  // set by the file dialog's callback thread

    pid_t buildPid = -1;
    std::string buildState = "Pick your ISO, then build once.";
    Color buildStateColor = kSoft;
    std::deque<std::string> logLines;
    std::string logPartial;
    std::streamoff logRead = 0;

    pid_t updatePid = -1;
    std::string current;      // this ZIP's version, from package.json
    std::string latest;       // set when GitHub has a newer release
    std::string releaseUrl;

    std::string path(const char *relative) const { return root + "/" + relative; }
    bool building() const { return buildPid > 0; }

    // Same checks as Test-Built in launcher.ps1; run_game.py still validates the build.
    bool built() const
    {
        std::error_code error;
        if (fs::exists(path("private/active-build.json"), error)) return true;
        for (const auto &entry : fs::directory_iterator(path("private"), error)) {
            const std::string text = readFile(entry.path() / "build-receipt.json");
            if (entry.path().filename().string().rfind("setup-", 0) == 0 &&
                text.find("\"built-unverified\"") != std::string::npos) return true;
        }
        return false;
    }

    void load()
    {
        json.parse(readFile(path("private/launcher.json")));
        int height = json.number("height", 0);
        if (height == 0) {
            const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
            height = mode != nullptr ? mode->h : 720;
        }
        choice[RESOLUTION] = std::max(0, static_cast<int>(std::upper_bound(std::begin(kHeights),
            std::end(kHeights), height) - std::begin(kHeights)) - 1);
        choice[MSAA] = indexOf(kSamples, json.number("msaa", 1), 0);
        choice[FOG] = json.flag("fog", true);
        choice[VOLUME] = indexOf(kVolumes, json.number("volume", 100), 0);
        choice[SHUFFLE] = json.flag("shuffle", false);
        choice[UNLOCK] = json.flag("unlock_swimsuits", false);
        choice[DEMO_CUT] = json.flag("demo_cut_hold", false);
        choice[FREE_CAMERA] = json.flag("free_camera", false);
        const std::string idle = json.text("title_idle");
        for (int i = 0; i < static_cast<int>(std::size(kIdleValues)); ++i)
            if (idle == kIdleValues[i]) choice[IDLE] = i;
    }

    bool save()
    {
        auto flag = [](int value) { return std::string(value ? "true" : "false"); };
        json.set("height", std::to_string(kHeights[choice[RESOLUTION]]));
        json.set("msaa", std::to_string(kSamples[choice[MSAA]]));
        json.set("fog", flag(choice[FOG]));
        json.set("volume", std::to_string(kVolumes[choice[VOLUME]]));
        json.set("shuffle", flag(choice[SHUFFLE]));
        json.set("unlock_swimsuits", flag(choice[UNLOCK]));
        json.set("demo_cut_hold", flag(choice[DEMO_CUT]));
        json.set("free_camera", flag(choice[FREE_CAMERA]));
        json.set("title_idle", Json::quote(kIdleValues[choice[IDLE]]));
        if (!isos.empty()) json.set("iso", Json::quote(isos[isoIndex]));
        std::error_code error;
        fs::create_directories(path("private"), error);
        std::ofstream file(path("private/launcher.json"), std::ios::trunc);
        file << json.dump();
        return static_cast<bool>(file);
    }

    void loadArt()
    {
        // A player's own island map frame wins over the beach preview setup copies.
        backdrop = ::loadArt(renderer, path("private/launcher/background.png"));
        customBackdrop = backdrop != nullptr;
        if (!backdrop) backdrop = ::loadArt(renderer, path("private/launcher/beach.png"));
        logo = ::loadArt(renderer, path("private/launcher/logo.png"));
        buttonA = ::loadArt(renderer, path("private/launcher/button-a.png"));
        buttonB = ::loadArt(renderer, path("private/launcher/button-b.png"));
    }

    void searchIsos()
    {
        if (searched) return;
        searched = true;
        isos = findIsos();
        // The last ISO built from stays chosen, even outside the searched folders.
        const std::string last = json.text("iso");
        if (!last.empty() && fs::exists(last)) {
            const auto it = std::find(isos.begin(), isos.end(), last);
            if (it == isos.end()) isos.insert(isos.begin(), last);
            isoIndex = it == isos.end() ? 0 : static_cast<int>(it - isos.begin());
        }
        if (isos.empty()) buildState = "No ISO found. Browse, or drop it on this window.";
    }

    void addIso(const std::string &iso)
    {
        const auto it = std::find(isos.begin(), isos.end(), iso);
        if (it == isos.end()) isos.insert(isos.begin(), iso);
        isoIndex = it == isos.end() ? 0 : static_cast<int>(it - isos.begin());
        buildState = "Pick your ISO, then build once.";
        buildStateColor = kSoft;
        select(BUILD);
    }

    std::vector<Control> controls(int page) const
    {
        std::vector<Control> list;
        auto add = [&](Id id, Kind kind, const char *label, SDL_FRect rect, bool enabled = true) {
            list.push_back({id, kind, label, rect, enabled});
        };
        switch (page) {
        case BUILD:
            add(ISO, Kind::Choice, "ISO", row(0), !building() && isos.size() > 1);
            add(BROWSE, Kind::Button, "Browse...", {kContentX, kFirstRow + kRowGap, 110, 22}, !building());
            add(START_BUILD, Kind::Button, "Build game", {kContentX + 120, kFirstRow + kRowGap, 130, 22},
                !building() && !isos.empty());
            break;
        case GRAPHICS:
            add(RESOLUTION, Kind::Choice, "Resolution", row(0));
            add(MSAA, Kind::Choice, "MSAA", row(1));
            add(FOG, Kind::Toggle, "Fog", row(2));
            break;
        case AUDIO:
            add(VOLUME, Kind::Choice, "Volume", row(0));
            add(SHUFFLE, Kind::Toggle, "Shuffle music", row(1));
            add(MUSIC, Kind::Button, "Open music folder", {kContentX, kFirstRow + 2 * kRowGap, 150, 22});
            break;
        case GAME:
            add(UNLOCK, Kind::Toggle, "Unlock swimsuits", row(0));
            add(DEMO_CUT, Kind::Toggle, "Shorter demo cuts", row(1));
            add(FREE_CAMERA, Kind::Toggle, "Free camera (F1)", row(2));
            add(IDLE, Kind::Choice, "Title screen idle", row(3));
            break;
        default:
            break;
        }
        add(PLAY, Kind::Button, "PLAY", kPlayRect);
        // After PLAY, so A on the Main Menu still plays by default.
        if (page == MAIN && !latest.empty())
            add(UPDATE, Kind::Button, "Open release page", {kContentX, 274, 150, 22});
        return list;
    }

    std::string valueText(Id id) const
    {
        switch (id) {
        case RESOLUTION: {
            const int height = kHeights[choice[id]];
            return height == 480 ? "480p (windowed)" : std::to_string(height) + "p";
        }
        case MSAA: return kSamples[choice[id]] == 1 ? "Off" : std::to_string(kSamples[choice[id]]) + "x";
        case VOLUME: return kVolumes[choice[id]] == 0 ? "Mute" : std::to_string(kVolumes[choice[id]]) + "%";
        case IDLE: return kIdleNames[choice[id]];
        case ISO: return isos.empty() ? "None found" : tail(fs::path(isos[isoIndex]).filename().string(), 30);
        default: return choice[id] ? "On" : "Off";
        }
    }

    void change(Id id, int step)
    {
        const int counts[] = {static_cast<int>(std::size(kHeights)), static_cast<int>(std::size(kSamples)), 2,
            static_cast<int>(std::size(kVolumes)), 2, 2, 2, 2, static_cast<int>(std::size(kIdleValues))};
        if (id == ISO) {
            if (!isos.empty()) isoIndex = (isoIndex + static_cast<int>(isos.size()) + step) % static_cast<int>(isos.size());
        } else if (id <= IDLE) {
            choice[id] = (choice[id] + counts[id] + step) % counts[id];
        }
    }

    void select(int index)
    {
        section = (index + SECTION_COUNT) % SECTION_COUNT;
        focus = 0;
        if (section == BUILD) searchIsos();
    }

    // Returns 0 to play, 1 to quit, -1 to stay open.
    int activate(const Control &control, int step = 1)
    {
        if (!control.enabled) return -1;
        switch (control.id) {
        case PLAY:
            if (building()) { select(BUILD); return -1; }
            if (!built()) { select(BUILD); return -1; }
            if (!save()) { buildState = "Cannot write private/launcher.json."; buildStateColor = kWarn; return -1; }
            return 0;
        case BROWSE: browse(); return -1;
        case START_BUILD: startBuild(); return -1;
        case UPDATE: SDL_OpenURL(releaseUrl.c_str()); return -1;
        case MUSIC: openMusicFolder(); return -1;
        default: change(control.id, step); return -1;
        }
    }

    // run_game.py passes this folder to the runner as RECOMP_USER_MUSIC.
    void openMusicFolder() const
    {
        // Absolute, because root may be "." and file://./ names a host.
        std::error_code error;
        const fs::path folder = fs::absolute(path("private/UserMusic"), error);
        if (folder.empty()) return;  // a bare file:// would open the filesystem root
        fs::create_directories(folder, error);
        std::string url = "file://";
        for (unsigned char c : folder.string()) {
            if (std::isalnum(c) || std::strchr("/-._~", c)) url += static_cast<char>(c);
            else { char hex[4]; std::snprintf(hex, sizeof hex, "%%%02X", c); url += hex; }
        }
        SDL_OpenURL(url.c_str());
    }

    int quit()
    {
        if (!building()) return 1;
        buildState = "Setup is still building. Wait for it to finish.";
        buildStateColor = kWarn;
        select(BUILD);
        return -1;
    }

    // --- The build ----------------------------------------------------------
    static void SDLCALL picked_callback(void *userdata, const char *const *files, int)
    {
        auto *self = static_cast<Launcher *>(userdata);
        std::lock_guard<std::mutex> lock(self->pickedMutex);
        if (files == nullptr) self->picked = "\n";  // the system has no file picker
        else if (files[0] != nullptr) self->picked = files[0];
    }

    void browse()
    {
        static const SDL_DialogFileFilter filter{"Xbox disc image", "iso;xiso"};
        const char *home = std::getenv("HOME");
        SDL_ShowOpenFileDialog(picked_callback, this, window, &filter, 1, home, false);
    }

    void startBuild()
    {
        if (building() || isos.empty()) return;
        save();
        const std::string log = path("private/launcher-build.log");
        const std::string script = path("build_game.sh");
        const std::string iso = isos[isoIndex];
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&actions, 1, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        posix_spawn_file_actions_adddup2(&actions, 1, 2);
        setenv("PYTHONUNBUFFERED", "1", 1);
        const char *argv[] = {"/usr/bin/env", "bash", script.c_str(), iso.c_str(), nullptr};
        const int failed = posix_spawn(&buildPid, argv[0], &actions, nullptr, const_cast<char **>(argv), environ);
        posix_spawn_file_actions_destroy(&actions);
        logLines.clear();
        logPartial.clear();
        logRead = 0;
        if (failed != 0) {
            buildPid = -1;
            buildState = "Cannot start build_game.sh.";
            buildStateColor = kWarn;
            return;
        }
        buildState = "Building... this can take a while.";
        buildStateColor = kSoft;
    }

    void readLog()
    {
        std::ifstream file(path("private/launcher-build.log"), std::ios::binary);
        if (!file) return;
        file.seekg(0, std::ios::end);
        const std::streamoff size = file.tellg();
        if (size <= logRead) return;
        file.seekg(logRead);
        std::string text(static_cast<size_t>(size - logRead), '\0');
        file.read(text.data(), static_cast<std::streamsize>(text.size()));
        logRead = size;
        for (const char c : text) {
            if (c == '\n' || c == '\r') {
                if (!logPartial.empty()) logLines.push_back(logPartial);
                logPartial.clear();
            } else if (c == '\t') {
                logPartial += "    ";
            } else if (static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x7f) {
                logPartial += c;
            }
        }
        while (logLines.size() > 200) logLines.pop_front();
    }

    void poll()
    {
        {
            std::lock_guard<std::mutex> lock(pickedMutex);
            if (picked == "\n") {
                buildState = "No file picker here. Drop the ISO on this window.";
                buildStateColor = kWarn;
            } else if (!picked.empty() && !building()) {
                // A picker opened before Build game was pressed must not change the
                // ISO or the status under a running build.
                addIso(picked);
            }
            picked.clear();
        }
        if (!building()) return;
        int status = 0;
        const pid_t done = waitpid(buildPid, &status, WNOHANG);
        readLog();
        if (done != buildPid) return;
        buildPid = -1;
        if (!logPartial.empty()) logLines.push_back(logPartial), logPartial.clear();
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            buildState = "Build complete. Press Play.";
            buildStateColor = kReady;
            select(MAIN);
        } else {
            buildState = "Build failed. Log: private/launcher-build.log";
            buildStateColor = kWarn;
        }
    }

    // --- Update check -------------------------------------------------------
    // Release ZIPs carry package.json, so a source checkout never asks. GitHub
    // redirects releases/latest to releases/tag/vX.Y.Z; curl prints that final URL.
    void startUpdateCheck()
    {
        Json package;
        package.parse(readFile(path("package.json")));
        current = package.text("version");
        if (current.empty()) return;
        std::error_code error;
        fs::create_directories(path("private"), error);
        const std::string out = path("private/launcher-update.txt");
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&actions, 1, out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
        const char *argv[] = {"curl", "-fsIL", "--max-time", "10", "-o", "/dev/null", "-w", "%{url_effective}",
            "https://github.com/NoRain211/doaxbv-re/releases/latest", nullptr};
        // Without curl or a network, the launcher says nothing.
        if (posix_spawnp(&updatePid, argv[0], &actions, nullptr, const_cast<char **>(argv), environ) != 0)
            updatePid = -1;
        posix_spawn_file_actions_destroy(&actions);
    }

    void pollUpdateCheck()
    {
        int status = 0;
        if (updatePid <= 0 || waitpid(updatePid, &status, WNOHANG) != updatePid) return;
        updatePid = -1;
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return;
        // Only github.com/NoRain211/<repo>/releases/tag/vX.Y.Z, whatever the repository is called now.
        const std::string url = readFile(path("private/launcher-update.txt"));
        const std::string owner = "https://github.com/NoRain211/", tag = "/releases/tag/v";
        const size_t at = url.rfind(tag);
        if (url.rfind(owner, 0) != 0 || at == std::string::npos || url.find('/', owner.size()) != at) return;
        const std::string version = url.substr(at + tag.size());
        if (version.empty() || version.find_first_not_of("0123456789.") != std::string::npos ||
            !newerVersion(version, current)) return;
        latest = version;
        // Built here, so the button only ever opens this GitHub page.
        releaseUrl = "https://github.com/NoRain211/doaxbv-re/releases/tag/v" + version;
    }

    // --- Drawing ------------------------------------------------------------
    void color(Color c) { SDL_SetRenderDrawColor(renderer, c.r, c.g, c.b, c.a); }

    void fill(const SDL_FRect &rect, Color c)
    {
        color(c);
        SDL_RenderFillRect(renderer, &rect);
    }

    // The game's glass plate: a rounded vertical gradient with a gloss and a white edge.
    void plate(SDL_FRect r, Color top, Color bottom, bool gloss = true)
    {
        const float radius = std::min(10.0f, r.h / 2);
        std::vector<SDL_FPoint> outline;
        const float cx[] = {r.x + r.w - radius, r.x + radius, r.x + radius, r.x + r.w - radius};
        const float cy[] = {r.y + radius, r.y + radius, r.y + r.h - radius, r.y + r.h - radius};
        for (int corner = 0; corner < 4; ++corner) {
            for (int step = 0; step <= 6; ++step) {
                const float angle = (corner * 90 + step * 15) * 3.14159265f / 180;
                outline.push_back({cx[corner] + radius * std::cos(angle), cy[corner] - radius * std::sin(angle)});
            }
        }
        auto shade = [&](float y, Color a, Color b) {
            const float t = std::clamp((y - r.y) / r.h, 0.0f, 1.0f);
            return SDL_FColor{(a.r + (b.r - a.r) * t) / 255, (a.g + (b.g - a.g) * t) / 255,
                (a.b + (b.b - a.b) * t) / 255, (a.a + (b.a - a.a) * t) / 255};
        };
        std::vector<SDL_Vertex> vertices{{{r.x + r.w / 2, r.y + r.h / 2}, shade(r.y + r.h / 2, top, bottom), {}}};
        for (const SDL_FPoint &p : outline) vertices.push_back({p, shade(p.y, top, bottom), {}});
        std::vector<int> indices;
        for (int i = 1; i < static_cast<int>(vertices.size()); ++i)
            indices.insert(indices.end(), {0, i, i + 1 < static_cast<int>(vertices.size()) ? i + 1 : 1});
        SDL_RenderGeometry(renderer, nullptr, vertices.data(), static_cast<int>(vertices.size()),
                           indices.data(), static_cast<int>(indices.size()));
        if (gloss) fill({r.x + radius / 2, r.y + 2, r.w - radius, r.h * 0.4f}, {255, 255, 255, 50});
        outline.push_back(outline.front());
        color({255, 255, 255, 210});
        SDL_RenderLines(renderer, outline.data(), static_cast<int>(outline.size()));
    }

    // scale 2 draws 16x16 glyphs; coordinates stay on the 640x400 canvas.
    void text(float x, float y, const std::string &value, Color c, float scale = 1)
    {
        SDL_SetRenderScale(renderer, scale, scale);
        color(c);
        SDL_RenderDebugText(renderer, x / scale, y / scale, value.c_str());
        SDL_SetRenderScale(renderer, 1, 1);
    }

    // White text outlined like the game's menu captions.
    void outlined(float x, float y, const std::string &value, float scale = 2, Color line = kOutline)
    {
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
                if (dx || dy) text(x + dx, y + dy, value, line, scale);
        text(x, y, value, kInk, scale);
    }

    float width(const std::string &value, float scale = 1) const { return value.size() * 8 * scale; }

    void image(SDL_Texture *texture, SDL_FRect box, bool cover)
    {
        float w = 0, h = 0;
        SDL_GetTextureSize(texture, &w, &h);
        const float scale = cover ? std::max(box.w / w, box.h / h) : std::min(box.w / w, box.h / h);
        const SDL_FRect at{box.x + (box.w - w * scale) / 2, box.y + (box.h - h * scale) / 2, w * scale, h * scale};
        SDL_RenderTexture(renderer, texture, nullptr, &at);
    }

    void icon(SDL_Texture *texture, const char *letter, Color c, float x, float y, float size)
    {
        if (texture) { image(texture, {x, y, size, size}, false); return; }
        plate({x, y, size, size}, c, c, false);
        outlined(x + (size - 8) / 2, y + (size - 8) / 2, letter, 1, {32, 48, 64});
    }

    void drawControl(const Control &control, bool focused)
    {
        const SDL_FRect r = control.rect;
        if (control.id == PLAY) {
            plate(r, hex(0x9af5c8), hex(0x17886a));
            if (focused) { color(kInk); SDL_RenderRect(renderer, &r); }
            icon(buttonA, "A", hex(0x3fc43f), r.x + 10, r.y + 5, 24);
            outlined(r.x + 44 + (r.w - 54 - width("PLAY", 2)) / 2, r.y + 9, "PLAY");
            return;
        }
        const Color ink = control.enabled ? kInk : Color{140, 170, 180};
        if (control.kind == Kind::Button) {
            fill(r, focused ? hex(0x17886a) : kField);
            color(focused ? kReady : kSoft);
            SDL_RenderRect(renderer, &r);
            text(r.x + (r.w - width(control.label)) / 2, r.y + (r.h - 8) / 2, control.label, ink);
            return;
        }
        fill(r, focused ? kField : Color{13, 79, 99, 120});
        if (focused) fill({r.x, r.y, 3, r.h}, kReady);
        text(r.x + 8, r.y + 6, control.label, kSoft);
        if (control.kind == Kind::Toggle) {
            const SDL_FRect box{r.x + r.w - 20, r.y + 4, 12, 12};
            color(ink);
            SDL_RenderRect(renderer, &box);
            if (choice[control.id]) fill({box.x + 3, box.y + 3, 6, 6}, kReady);
        } else {
            const std::string value = focused && control.enabled ? "<  " + valueText(control.id) + "  >"
                                                                 : valueText(control.id);
            text(r.x + r.w - 8 - width(value), r.y + 6, value, ink);
        }
    }

    void note(float y, const std::string &value, Color c = kSoft) { text(kContentX, y, value, c); }

    void drawPage()
    {
        switch (section) {
        case MAIN: {
            const bool ready = built();
            text(kContentX, kFirstRow, building() ? "Building..." : ready ? "Ready to play" : "Build the game first",
                 ready && !building() ? kReady : kWarn, 2);
            const std::string display = valueText(RESOLUTION) + " / " +
                (choice[MSAA] ? valueText(MSAA) + " MSAA" : std::string("No MSAA"));
            const std::string render = std::string(kRenderer) + " / " + (choice[FOG] ? "Fog on" : "Fog off");
            std::vector<std::string> extras;
            if (choice[UNLOCK]) extras.push_back("Swimsuits");
            if (choice[FREE_CAMERA]) extras.push_back("Free camera");
            if (choice[DEMO_CUT]) extras.push_back("Short demo cuts");
            std::string extra = extras.empty() ? "None" : extras[0];
            for (size_t i = 1; i < extras.size(); ++i) extra += " / " + extras[i];
            const char *keys[] = {"Display", "Rendering", "Extras"};
            const std::string values[] = {display, render, extra};
            for (int i = 0; i < 3; ++i) {
                text(kContentX, 150 + i * 16, keys[i], kSoft);
                text(kContentX + 88, 150 + i * 16, tail(values[i], 31), kInk);
            }
            if (!ready && !building()) note(214, "Open Build to make the game from your ISO.");
            if (!latest.empty()) {
                note(242, "Version " + latest + " is out. Extract it into a", kWarn);
                note(254, "new folder and build again.", kWarn);
            }
            break;
        }
        case BUILD: {
            text(kContentX, kFirstRow + 2 * kRowGap + 2, tail(buildState, 42), buildStateColor);
            if (!isos.empty()) note(kFirstRow + 2 * kRowGap + 14, tail(isos[isoIndex], 42), {150, 200, 210});
            const SDL_FRect box{kContentX, 196, kContentW, 110};
            fill(box, kLogBack);
            const int lines = 9;
            const int first = std::max(0, static_cast<int>(logLines.size()) - lines);
            for (int i = first; i < static_cast<int>(logLines.size()); ++i)
                text(box.x + 4, box.y + 4 + (i - first) * 11, logLines[i].substr(0, 41), kSoft);
            if (building() && !logPartial.empty())
                text(box.x + 4, box.y + 4 + std::min(lines - 1, static_cast<int>(logLines.size()) - first) * 11,
                     logPartial.substr(0, 41), kSoft);
            break;
        }
        case GRAPHICS:
            note(kFirstRow + 3 * kRowGap + 6, "Resolution sets the render height; above");
            note(kFirstRow + 3 * kRowGap + 18, "480p the game fills the screen.");
            note(kFirstRow + 3 * kRowGap + 38, "SMAA, RTGI, sun shadows and texture packs", {150, 200, 210});
            note(kFirstRow + 3 * kRowGap + 50, "need Direct3D 11 on Windows.", {150, 200, 210});
            break;
        case AUDIO:
            note(kFirstRow + 3 * kRowGap + 6, "Shuffle plays Radio Station songs in a");
            note(kFirstRow + 3 * kRowGap + 18, "random order. MP3, WAV and FLAC files in");
            note(kFirstRow + 3 * kRowGap + 30, "the music folder play at the Radio Station.");
            break;
        case HELP: {
            const char *const lines[] = {"While playing", "  F1          Fly the camera (when on)", "",
                "In this launcher", "  LB / RB     Switch section", "  D-pad       Move between options",
                "  Left/Right  Change an option", "  A           Tick or press", "  Start       Play",
                "  B           Quit", "", "Logs are in the private folder."};
            for (int i = 0; i < static_cast<int>(std::size(lines)); ++i)
                text(kContentX, kFirstRow + i * 13, lines[i], i == 0 || i == 3 ? kReady : kSoft);
            break;
        }
        default:
            break;
        }
    }

    void draw()
    {
        // Clear the whole window, bars around the canvas included: they are
        // otherwise left over from earlier frames and flicker.
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
        fill({0, 0, kWidth, kHeight}, hex(0x178ab8));
        if (backdrop) {
            image(backdrop, {0, 0, kWidth, kHeight}, true);
        } else {
            for (int y = 0; y < kHeight; y += 4) {  // a plain sea, as in launcher.ps1
                const float t = y / static_cast<float>(kHeight);
                fill({0, static_cast<float>(y), kWidth, 4}, {Uint8(127 - 104 * t), Uint8(216 - 78 * t), Uint8(255 - 71 * t)});
            }
        }
        // The map art carries the game's own island sign, so the logo only fills in without it.
        if (!customBackdrop) {
            if (logo) {
                image(logo, {8, 4, 226, 104}, false);
            } else {
                outlined(18, 30, "DEAD OR ALIVE");
                outlined(20, 52, "Xtreme Beach Volleyball", 1);
            }
        }
        for (int i = 0; i < SECTION_COUNT; ++i) {
            const SDL_FRect r = sectionRect(i);
            if (i == section) plate(r, hex(0x9af5c8, 245), hex(0x17886a, 245));
            else plate({r.x, r.y + 3, r.w - 22, r.h - 6}, hex(0xc4f6fa, 150), hex(0x3a9ec0, 150));
            outlined(r.x + 12, r.y + 7, upper(kSections[i]));
        }
        // The settings frame matches the location preview frame on the island map.
        plate({252, 78, 380, 286}, hex(0xd4fffa, 235), hex(0x53bcc8, 235));
        plate({260, 86, 364, 270}, hex(0x073e52, 225), hex(0x073e52, 225), false);
        plate({262, 34, 360, 36}, hex(0x9af5c8, 245), hex(0x17886a, 245));
        const std::string title = upper(kSections[section]);
        outlined(262 + (360 - width(title, 2)) / 2, 44, title);
        text(kContentX, 92, kSubtitles[section], kSoft);

        drawPage();
        const std::vector<Control> list = controls(section);
        for (int i = 0; i < static_cast<int>(list.size()); ++i) drawControl(list[i], i == focus);

        outlined(16, 380, "Settings apply on next launch", 1, kDarkOutline);
        // Button prompts, right-aligned: LB/RB Section, (A) Select, (B) Quit.
        struct Hint { SDL_Texture *art; const char *letter; Color color; const char *text; };
        const Hint hints[] = {{nullptr, nullptr, {}, "LB/RB Section"},
            {buttonA, "A", hex(0x3fc43f), "Select"}, {buttonB, "B", hex(0xd83a3a), "Quit"}};
        float x = kWidth - 16;
        for (int i = static_cast<int>(std::size(hints)) - 1; i >= 0; --i) {
            x -= width(hints[i].text);
            outlined(x, 380, hints[i].text, 1, kDarkOutline);
            if (hints[i].letter) {
                x -= 18;
                icon(hints[i].art, hints[i].letter, hints[i].color, x, 376, 15);
            }
            x -= 16;
        }
    }

    // DOAXBV_LAUNCHER_SCREENSHOTS=<prefix> saves every section as <prefix>-<n>.bmp
    // and exits, so the look can be checked without a display (SDL_VIDEO_DRIVER=offscreen).
    bool screenshots(const char *prefix)
    {
        for (int i = 0; i < SECTION_COUNT; ++i) {
            select(i);
            draw();
            SDL_Surface *shot = SDL_RenderReadPixels(renderer, nullptr);
            const std::string file = std::string(prefix) + "-" + std::to_string(i) + ".bmp";
            const bool saved = shot != nullptr && SDL_SaveBMP(shot, file.c_str());
            SDL_DestroySurface(shot);
            SDL_RenderPresent(renderer);
            if (!saved) return false;
        }
        return true;
    }

    // The control under a canvas point, or -1; sections answer with -2 - index.
    int hit(float x, float y) const
    {
        auto inside = [&](const SDL_FRect &r) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; };
        for (int i = 0; i < SECTION_COUNT; ++i)
            if (inside(sectionRect(i))) return -2 - i;
        const std::vector<Control> list = controls(section);
        for (int i = 0; i < static_cast<int>(list.size()); ++i)
            if (inside(list[i].rect)) return i;
        return -1;
    }

    // Returns 0 to play, 1 to quit, -1 to stay open.
    int press(int dy, int dx)
    {
        const std::vector<Control> list = controls(section);
        const int count = static_cast<int>(list.size());
        if (dy) focus = (focus + count + dy) % count;
        if (dx && list[focus].kind == Kind::Choice && list[focus].enabled) change(list[focus].id, dx);
        return -1;
    }
};

// Direction from every open gamepad's d-pad and left stick.
void heldDirection(int &dx, int &dy)
{
    dx = dy = 0;
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    for (int i = 0; ids != nullptr && i < count; ++i) {
        SDL_Gamepad *pad = SDL_GetGamepadFromID(ids[i]);
        if (pad == nullptr) continue;
        const int x = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX);
        const int y = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP) || y < -20000) dy = -1;
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN) || y > 20000) dy = 1;
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT) || x < -20000) dx = -1;
        if (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || x > 20000) dx = 1;
    }
    SDL_free(ids);
    if (dy != 0) dx = 0;  // a diagonal moves a row without also changing a value
}

} // namespace

int main(int argc, char **argv)
{
    Launcher launcher;
    launcher.root = argc > 1 ? argv[1] : ".";

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "launcher: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    launcher.load();

    // Steam sets these in Game Mode and on the Deck, where a window would only
    // be letterboxed by gamescope anyway.
    const bool fullscreen = SDL_getenv("SteamTenfoot") != nullptr || SDL_getenv("SteamDeck") != nullptr;
    // A window that fills the screen gets maximised by the desktop, as on the
    // Deck's 1280x800 in Desktop Mode, so open at the largest of 2x, 1.5x and 1x
    // that leaves room for its frame and the panel.
    float scale = 1;
    SDL_Rect usable{0, 0, kWidth, kHeight};
    if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable)) {
        for (const float candidate : {2.0f, 1.5f}) {
            if (kWidth * candidate <= usable.w * 0.9f && kHeight * candidate <= usable.h * 0.9f) {
                scale = candidate;
                break;
            }
        }
    }
    if (!SDL_CreateWindowAndRenderer("DOAXBV", static_cast<int>(kWidth * scale), static_cast<int>(kHeight * scale),
            fullscreen ? SDL_WINDOW_FULLSCREEN : SDL_WINDOW_RESIZABLE, &launcher.window, &launcher.renderer)) {
        std::fprintf(stderr, "launcher: cannot open a window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    // Letterbox scaling fills any window size, maximised or resized, keeping the shape.
    SDL_SetRenderLogicalPresentation(launcher.renderer, kWidth, kHeight, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_SetRenderVSync(launcher.renderer, 1);
    launcher.loadArt();
    if (const char *prefix = SDL_getenv("DOAXBV_LAUNCHER_SCREENSHOTS")) {
        const bool saved = launcher.screenshots(prefix);
        SDL_Quit();
        return saved ? 1 : 2;
    }
    // Without a build, open on the Build page, as DOAXBV.exe does on Windows.
    if (!launcher.built()) launcher.select(BUILD);
    launcher.startUpdateCheck();

    int result = -1;
    int held_dx = 0, held_dy = 0;
    Uint64 next_repeat = 0;
    while (result < 0) {
        SDL_Event event;
        while (result < 0 && SDL_PollEvent(&event)) {
            SDL_ConvertEventToRenderCoordinates(launcher.renderer, &event);
            const std::vector<Control> list = launcher.controls(launcher.section);
            const Control &focused = list[std::min(launcher.focus, static_cast<int>(list.size()) - 1)];
            switch (event.type) {
            case SDL_EVENT_QUIT:
                result = launcher.quit();
                break;
            case SDL_EVENT_DROP_FILE:
                if (!launcher.building() && event.drop.data != nullptr) launcher.addIso(event.drop.data);
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                SDL_OpenGamepad(event.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                switch (event.gbutton.button) {
                case SDL_GAMEPAD_BUTTON_SOUTH: result = launcher.activate(focused); break;
                case SDL_GAMEPAD_BUTTON_START:
                    result = launcher.activate(*std::find_if(list.begin(), list.end(),
                        [](const Control &control) { return control.id == PLAY; }));
                    break;
                case SDL_GAMEPAD_BUTTON_EAST: result = launcher.quit(); break;
                case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: launcher.select(launcher.section - 1); break;
                case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: launcher.select(launcher.section + 1); break;
                default: break;
                }
                break;
            case SDL_EVENT_KEY_DOWN:
                switch (event.key.key) {
                case SDLK_UP: launcher.press(-1, 0); break;
                case SDLK_DOWN: launcher.press(1, 0); break;
                case SDLK_LEFT: launcher.press(0, -1); break;
                case SDLK_RIGHT: launcher.press(0, 1); break;
                case SDLK_TAB:
                    launcher.select(launcher.section + ((event.key.mod & SDL_KMOD_SHIFT) ? -1 : 1));
                    break;
                case SDLK_PAGEUP: launcher.select(launcher.section - 1); break;
                case SDLK_PAGEDOWN: launcher.select(launcher.section + 1); break;
                case SDLK_RETURN:
                case SDLK_SPACE: result = launcher.activate(focused); break;
                case SDLK_ESCAPE: result = launcher.quit(); break;
                default: break;
                }
                break;
            case SDL_EVENT_MOUSE_MOTION: {
                const int item = launcher.hit(event.motion.x, event.motion.y);
                if (item >= 0) launcher.focus = item;
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                // Touch arrives as mouse input. Lists step down on their left
                // half and up on their right half.
                const int item = launcher.hit(event.button.x, event.button.y);
                if (item <= -2) {
                    launcher.select(-2 - item);
                } else if (item >= 0) {
                    launcher.focus = item;
                    const Control &control = list[item];
                    result = launcher.activate(control,
                        event.button.x < control.rect.x + control.rect.w / 2 ? -1 : 1);
                }
                break;
            }
            default:
                break;
            }
        }
        launcher.poll();
        launcher.pollUpdateCheck();

        // D-pad and stick: one step on press, then repeat while held.
        int dx, dy;
        heldDirection(dx, dy);
        const Uint64 now = SDL_GetTicks();
        if (dx != held_dx || dy != held_dy) {
            held_dx = dx;
            held_dy = dy;
            next_repeat = now + 350;
            if (dx || dy) launcher.press(dy, dx);
        } else if ((dx || dy) && now >= next_repeat) {
            next_repeat = now + 120;
            launcher.press(dy, dx);
        }
        launcher.draw();
        SDL_RenderPresent(launcher.renderer);
    }

    SDL_DestroyRenderer(launcher.renderer);
    SDL_DestroyWindow(launcher.window);
    SDL_Quit();
    return result;
}
