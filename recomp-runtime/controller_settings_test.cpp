#include "controller_settings.h"
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <string>

int main()
{
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("recomp-controls-test-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (fs::exists(root)) return 1;
    const auto path = root.string();
    uint8_t modes[4] = {1,1,1,1};
    const uint8_t digital[4] = {};
    const uint8_t chosen[4] = {1,0,1,0};
    bool ok = recomp_controller_settings_load(path.c_str(), modes) &&
        std::memcmp(modes, digital, 4) == 0;
    ok &= recomp_controller_settings_save(path.c_str(), chosen);
    std::memset(modes, 0, 4);
    ok &= recomp_controller_settings_load(path.c_str(), modes) &&
        std::memcmp(modes, chosen, 4) == 0;
    const uint8_t invalid[4] = {2,0,0,0};
    ok &= !recomp_controller_settings_save(path.c_str(), invalid);
    ok &= recomp_controller_settings_load(path.c_str(), modes) &&
        std::memcmp(modes, chosen, 4) == 0;
    ok &= recomp_controller_settings_save(path.c_str(), digital);
    ok &= recomp_controller_settings_load(path.c_str(), modes) &&
        std::memcmp(modes, digital, 4) == 0;
    std::ofstream(root / ".recomp-storage" / "controller-mode-v1", std::ios::binary) << "DCP";
    ok &= !recomp_controller_settings_load(path.c_str(), modes);

    uint8_t languages[2] = {7, 7};
    const uint8_t japanese[2] = {0, 0};
    const uint8_t mixed[2] = {1, 0};
    ok &= recomp_language_settings_load(path.c_str(), languages) == 0 &&
        languages[0] == 7 && languages[1] == 7;
    ok &= recomp_language_settings_save(path.c_str(), japanese);
    ok &= recomp_language_settings_load(path.c_str(), languages) == 1 &&
        std::memcmp(languages, japanese, 2) == 0;
    ok &= recomp_language_settings_save(path.c_str(), mixed);
    ok &= recomp_language_settings_load(path.c_str(), languages) == 1 &&
        std::memcmp(languages, mixed, 2) == 0;
    const uint8_t korean[2] = {2, 0};
    ok &= !recomp_language_settings_save(path.c_str(), korean);
    std::ofstream(root / ".recomp-storage" / "language-v1", std::ios::binary) << "DLG1" << '\2' << '\0';
    ok &= recomp_language_settings_load(path.c_str(), languages) == -1;
    fs::remove_all(root);
    std::puts(ok ? "PASS Digital default, explicit Analog persistence, replacement, invalid data, language" : "FAIL controller preferences");
    return ok ? 0 : 1;
}
