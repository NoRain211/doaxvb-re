#include "controller_settings.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#ifdef _WIN32
#include <windows.h>
#else
#include "host_file_posix.h"
#endif

namespace fs = std::filesystem;

static fs::path settings_path(const char *root)
{
    return fs::path(root) / ".recomp-storage" / "controller-mode-v1";
}

bool recomp_controller_settings_load(const char *root, uint8_t modes[4])
{
    if (root == nullptr || modes == nullptr) return false;
    try {
        const auto path = settings_path(root);
        if (!fs::exists(path)) {
            std::memset(modes, 0, 4); // Digital until the player chooses otherwise.
            return true;
        }
        std::ifstream file(path, std::ios::binary);
        unsigned char bytes[8];
        if (!file.read(reinterpret_cast<char *>(bytes), sizeof bytes) ||
            file.peek() != std::char_traits<char>::eof() ||
            std::memcmp(bytes, "DCP1", 4) != 0) return false;
        for (unsigned i = 4; i < 8; ++i) if (bytes[i] > 1) return false;
        std::memcpy(modes, bytes + 4, 4);
        return true;
    } catch (...) { return false; }
}

bool recomp_controller_settings_save(const char *root, const uint8_t modes[4])
{
    if (root == nullptr || modes == nullptr) return false;
    for (unsigned i = 0; i < 4; ++i) if (modes[i] > 1) return false;
    try {
        const auto path = settings_path(root);
        fs::create_directories(path.parent_path());
        auto temporary = path;
        temporary += ".tmp";
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write("DCP1", 4);
        file.write(reinterpret_cast<const char *>(modes), 4);
        file.close();
        if (!file) return false;
#ifdef _WIN32
        return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
        return recomp_posix_replace(temporary.c_str(), path.c_str()) != 0;
#endif
    } catch (...) { return false; }
}
