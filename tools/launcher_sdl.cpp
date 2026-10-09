// SPDX-License-Identifier: GPL-3.0-or-later
// Linux launcher: pick resolution, anti-aliasing, volume and music options,
// remember them in private/launcher.json, then exit 0 so run_game.py starts
// the game. Mirrors tools/launcher.ps1 without SMAA (Direct3D 11 only) and the
// Music folder button (custom soundtracks need Media Foundation). Gamepad,
// keyboard, mouse and touch all work, so it runs from Steam Game Mode.
// Exits 1 when closed without Play.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

namespace {

// Everything is laid out on a 640x400 canvas and scaled by whole numbers,
// so SDL's 8x8 debug font stays sharp: 2x on the Steam Deck's 1280x800.
constexpr int kWidth = 640, kHeight = 400;
constexpr float kRowX = 120, kRowW = 400, kRowH = 26, kFirstRowY = 128, kRowGap = 32;
constexpr float kButtonY = kFirstRowY + 4 * kRowGap + 12, kButtonW = 194, kButtonH = 30;

const int kHeights[] = {480, 720, 1080, 1440, 2160};
const int kSamples[] = {1, 2, 4, 8};
const int kVolumes[] = {100, 75, 50, 25, 0};

enum Item { RESOLUTION, MSAA, VOLUME, SHUFFLE, PLAY, QUIT, ITEM_COUNT };

struct Settings {
    int height = 0;
    int msaa = 1;
    int volume = 100;
    bool shuffle = false;
};

struct Color { Uint8 r, g, b; };
constexpr Color kBackground{11, 22, 34}, kPanel{19, 38, 58}, kSelected{31, 77, 110};
constexpr Color kAccent{255, 179, 71}, kText{232, 241, 248}, kMuted{127, 155, 179};

// Reads the flat object launcher.ps1, launcher_macos.swift and this file write.
bool jsonValue(const std::string &text, const char *key, std::string &value)
{
    const auto at = text.find(std::string("\"") + key + "\"");
    if (at == std::string::npos) return false;
    auto start = text.find(':', at);
    if (start == std::string::npos) return false;
    start = text.find_first_not_of(" \t\r\n", start + 1);
    if (start == std::string::npos) return false;
    value = text.substr(start, text.find_first_of(",}\r\n", start) - start);
    return true;
}

Settings loadSettings(const std::string &path)
{
    Settings settings;
    std::ifstream file(path);
    const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    std::string value;
    if (jsonValue(text, "height", value)) settings.height = std::atoi(value.c_str());
    if (jsonValue(text, "msaa", value)) settings.msaa = std::atoi(value.c_str());
    if (jsonValue(text, "volume", value)) settings.volume = std::atoi(value.c_str());
    if (jsonValue(text, "shuffle", value)) settings.shuffle = value == "true";
    return settings;
}

bool saveSettings(const std::string &path, const Settings &settings)
{
    std::ofstream file(path, std::ios::trunc);
    file << "{\n  \"height\": " << settings.height << ",\n  \"msaa\": " << settings.msaa
         << ",\n  \"smaa\": false,\n  \"volume\": " << settings.volume
         << ",\n  \"shuffle\": " << (settings.shuffle ? "true" : "false") << "\n}\n";
    return static_cast<bool>(file);
}

template <size_t N>
int indexOf(const int (&values)[N], int value, int fallback)
{
    const auto it = std::find(std::begin(values), std::end(values), value);
    return it == std::end(values) ? fallback : static_cast<int>(it - std::begin(values));
}

SDL_FRect itemRect(int item)
{
    if (item == PLAY) return {kRowX, kButtonY, kButtonW, kButtonH};
    if (item == QUIT) return {kRowX + kRowW - kButtonW, kButtonY, kButtonW, kButtonH};
    return {kRowX, kFirstRowY + item * kRowGap, kRowW, kRowH};
}

struct Launcher {
    SDL_Renderer *renderer = nullptr;
    int choice[4] = {};
    int selected = PLAY;

    std::string valueText(int item) const
    {
        switch (item) {
        case RESOLUTION: {
            const int height = kHeights[choice[item]];
            return height == 480 ? "480p (original)" : std::to_string(height) + "p";
        }
        case MSAA: return kSamples[choice[item]] == 1 ? "Off" : std::to_string(kSamples[choice[item]]) + "x";
        case VOLUME: return kVolumes[choice[item]] == 0 ? "Mute" : std::to_string(kVolumes[choice[item]]) + "%";
        default: return choice[item] ? "On" : "Off";
        }
    }

    void change(int item, int step)
    {
        static const int counts[] = {static_cast<int>(std::size(kHeights)),
            static_cast<int>(std::size(kSamples)), static_cast<int>(std::size(kVolumes)), 2};
        if (item < PLAY) choice[item] = (choice[item] + counts[item] + step) % counts[item];
    }

    // Play and Quit share the bottom row.
    void move(int dy, int dx)
    {
        if (dy < 0 && selected > RESOLUTION) selected = selected >= PLAY ? SHUFFLE : selected - 1;
        if (dy > 0 && selected < PLAY) ++selected;
        if (dx != 0) {
            if (selected >= PLAY) selected = dx > 0 ? QUIT : PLAY;
            else change(selected, dx);
        }
    }

    void fill(const SDL_FRect &rect, Color color, Uint8 alpha = 255)
    {
        SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, alpha);
        SDL_RenderFillRect(renderer, &rect);
    }

    // scale 2 draws 16x16 glyphs; coordinates stay on the 640x400 canvas.
    void text(float x, float y, const std::string &value, Color color, float scale = 1)
    {
        SDL_SetRenderScale(renderer, scale, scale);
        SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, 255);
        SDL_RenderDebugText(renderer, x / scale, y / scale, value.c_str());
        SDL_SetRenderScale(renderer, 1, 1);
    }

    void centered(float y, const std::string &value, Color color, float scale = 1)
    {
        text((kWidth - value.size() * 8 * scale) / 2, y, value, color, scale);
    }

    void draw()
    {
        fill({0, 0, kWidth, kHeight}, kBackground);
        fill({0, 0, kWidth, 3}, kAccent);
        centered(40, "DEAD OR ALIVE", kAccent);
        centered(56, "XTREME BEACH VOLLEYBALL", kText, 2);
        centered(82, "Native port settings", kMuted);

        static const char *labels[] = {"Resolution", "Anti-aliasing", "Volume", "Shuffle music"};
        for (int item = RESOLUTION; item < PLAY; ++item) {
            const SDL_FRect row = itemRect(item);
            const bool active = item == selected;
            fill(row, active ? kSelected : kPanel);
            if (active) fill({row.x, row.y, 3, row.h}, kAccent);
            const float y = row.y + (row.h - 8) / 2;
            text(row.x + 14, y, labels[item], active ? kText : kMuted);
            const std::string value = active ? "<  " + valueText(item) + "  >" : valueText(item);
            text(row.x + row.w - 14 - value.size() * 8, y, value, active ? kAccent : kText);
        }
        for (int item = PLAY; item <= QUIT; ++item) {
            const SDL_FRect button = itemRect(item);
            const bool active = item == selected;
            fill(button, active ? (item == PLAY ? kAccent : kSelected) : kPanel);
            const std::string label = item == PLAY ? "PLAY" : "QUIT";
            text(button.x + (button.w - label.size() * 16) / 2, button.y + (button.h - 16) / 2,
                 label, active && item == PLAY ? kBackground : kText, 2);
        }
        centered(kHeight - 28, "A select   B quit   D-pad or stick move   left/right change", kMuted);
        SDL_RenderPresent(renderer);
    }

    // Returns the item under a point on the canvas, or -1.
    int hit(float x, float y) const
    {
        for (int item = 0; item < ITEM_COUNT; ++item) {
            const SDL_FRect r = itemRect(item);
            if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return item;
        }
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
}

} // namespace

int main(int argc, char **argv)
{
    const std::string root = argc > 1 ? argv[1] : ".";
    const std::string settings_path = root + "/private/launcher.json";
    Settings settings = loadSettings(settings_path);

    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "launcher: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    if (settings.height == 0) {
        const SDL_DisplayMode *mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
        settings.height = mode != nullptr ? mode->h : 720;
    }

    // Steam sets these in Game Mode and on the Deck, where a window would only
    // be letterboxed by gamescope anyway.
    const bool fullscreen = SDL_getenv("SteamTenfoot") != nullptr || SDL_getenv("SteamDeck") != nullptr;
    SDL_Window *window = nullptr;
    Launcher launcher;
    if (!SDL_CreateWindowAndRenderer("DOAXBV", kWidth * 2, kHeight * 2,
            fullscreen ? SDL_WINDOW_FULLSCREEN : SDL_WINDOW_RESIZABLE, &window, &launcher.renderer)) {
        std::fprintf(stderr, "launcher: cannot open a window: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderLogicalPresentation(launcher.renderer, kWidth, kHeight, SDL_LOGICAL_PRESENTATION_INTEGER_SCALE);
    SDL_SetRenderVSync(launcher.renderer, 1);

    const int nearest = static_cast<int>(std::upper_bound(std::begin(kHeights), std::end(kHeights),
        settings.height) - std::begin(kHeights)) - 1;
    launcher.choice[RESOLUTION] = std::max(0, nearest);
    launcher.choice[MSAA] = indexOf(kSamples, settings.msaa, 0);
    launcher.choice[VOLUME] = indexOf(kVolumes, settings.volume, 0);
    launcher.choice[SHUFFLE] = settings.shuffle ? 1 : 0;

    int result = -1;
    int held_dx = 0, held_dy = 0;
    Uint64 next_repeat = 0;
    while (result < 0) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            SDL_ConvertEventToRenderCoordinates(launcher.renderer, &event);
            switch (event.type) {
            case SDL_EVENT_QUIT:
                result = 1;
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
                SDL_OpenGamepad(event.gdevice.which);
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (event.gbutton.button == SDL_GAMEPAD_BUTTON_SOUTH ||
                    event.gbutton.button == SDL_GAMEPAD_BUTTON_START) {
                    if (launcher.selected == PLAY) result = 0;
                    else if (launcher.selected == QUIT) result = 1;
                    else launcher.change(launcher.selected, 1);
                } else if (event.gbutton.button == SDL_GAMEPAD_BUTTON_EAST) {
                    result = 1;
                }
                break;
            case SDL_EVENT_KEY_DOWN:
                switch (event.key.key) {
                case SDLK_UP: launcher.move(-1, 0); break;
                case SDLK_DOWN: launcher.move(1, 0); break;
                case SDLK_LEFT: launcher.move(0, -1); break;
                case SDLK_RIGHT: launcher.move(0, 1); break;
                case SDLK_RETURN:
                case SDLK_SPACE:
                    if (launcher.selected == PLAY) result = 0;
                    else if (launcher.selected == QUIT) result = 1;
                    else launcher.change(launcher.selected, 1);
                    break;
                case SDLK_ESCAPE: result = 1; break;
                default: break;
                }
                break;
            case SDL_EVENT_MOUSE_MOTION: {
                const int item = launcher.hit(event.motion.x, event.motion.y);
                if (item >= 0) launcher.selected = item;
                break;
            }
            case SDL_EVENT_MOUSE_BUTTON_DOWN: {
                // Touch arrives as mouse input. Options step down on their left
                // half and up on their right half.
                const int item = launcher.hit(event.button.x, event.button.y);
                if (item < 0) break;
                launcher.selected = item;
                if (item == PLAY) result = 0;
                else if (item == QUIT) result = 1;
                else {
                    const SDL_FRect row = itemRect(item);
                    launcher.change(item, event.button.x < row.x + row.w / 2 ? -1 : 1);
                }
                break;
            }
            default:
                break;
            }
        }

        // D-pad and stick: one step on press, then repeat while held.
        int dx, dy;
        heldDirection(dx, dy);
        const Uint64 now = SDL_GetTicks();
        if (dx != held_dx || dy != held_dy) {
            held_dx = dx;
            held_dy = dy;
            next_repeat = now + 350;
            if (dx || dy) launcher.move(dy, dx);
        } else if ((dx || dy) && now >= next_repeat) {
            next_repeat = now + 120;
            launcher.move(dy, dx);
        }
        launcher.draw();
    }

    if (result == 0) {
        settings.height = kHeights[launcher.choice[RESOLUTION]];
        settings.msaa = kSamples[launcher.choice[MSAA]];
        settings.volume = kVolumes[launcher.choice[VOLUME]];
        settings.shuffle = launcher.choice[SHUFFLE] != 0;
        if (!saveSettings(settings_path, settings)) {
            std::fprintf(stderr, "launcher: cannot write %s\n", settings_path.c_str());
            result = 1;
        }
    }
    SDL_DestroyRenderer(launcher.renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return result;
}
