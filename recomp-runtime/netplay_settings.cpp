/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "netplay_settings.h"

#include "rollback.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>

const char *const RecompNetplayDisplayNames[3] = {"off", "ping", "full"};

namespace {

// Sixteen hex digits, not all zero.
bool parseId(const std::string &text, std::uint64_t &id)
{
    if (text.size() != 16u || text.find_first_not_of("0123456789abcdef") != std::string::npos) return false;
    id = std::strtoull(text.c_str(), nullptr, 16);
    return id != 0u;
}

std::string idText(std::uint64_t id)
{
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(id));
    return text;
}

} // namespace

void recomp_netplay_settings_load(const std::filesystem::path &path, RecompNetplaySettings &settings)
{
    std::ifstream file(path);
    settings.blocked.clear();
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream words(line);
        std::string key, value;
        if (!(words >> key >> value)) continue;
        const unsigned long number = std::strtoul(value.c_str(), nullptr, 10);
        std::uint64_t id = 0u;
        if (key == "delay" && number <= RECOMP_ROLLBACK_WINDOW) settings.delay = static_cast<std::uint32_t>(number);
        else if (key == "port" && number >= 1024u && number <= 65535u) settings.port = static_cast<std::uint32_t>(number);
        else if (key == "recent" && value.size() <= RecompNetplayMaxAddress) settings.recent = value;
        else if (key == "matchmaker" && value.size() <= RecompNetplayMaxMatchmaker) settings.matchmaker = value;
        else if (key == "room") settings.privateRoom = value == "private";
        else if (key == "id" && parseId(value, id)) settings.id = id;
        else if (key == "block" && parseId(value, id)) {
            std::string name;
            std::getline(words >> std::ws, name);
            settings.blocked.emplace_back(id, name);
        } else if (key == "display")
            for (int i = 0; i < 3; ++i)
                if (value == RecompNetplayDisplayNames[i]) settings.display = i;
    }
    if (settings.id != 0u) return;
    std::random_device random;
    while (settings.id == 0u) settings.id = static_cast<std::uint64_t>(random()) << 32 | random();
    file.close();
    recomp_netplay_settings_save(path, settings);
}

void recomp_netplay_settings_save(const std::filesystem::path &path, const RecompNetplaySettings &settings)
{
    std::ofstream file(path, std::ios::trunc);
    file << "delay " << settings.delay << "\nport " << settings.port << "\ndisplay "
         << RecompNetplayDisplayNames[settings.display] << '\n';
    if (!settings.recent.empty()) file << "recent " << settings.recent << '\n';
    if (!settings.matchmaker.empty()) file << "matchmaker " << settings.matchmaker << '\n';
    file << "room " << (settings.privateRoom ? "private" : "public") << '\n';
    file << "id " << idText(settings.id) << '\n';
    for (const auto &[id, name] : settings.blocked) {
        std::string shown = name; // a name from the network, kept to one line
        for (char &c : shown)
            if (static_cast<unsigned char>(c) < 0x20u || c == 0x7f) c = '?';
        file << "block " << idText(id) << ' ' << shown << '\n';
    }
}
