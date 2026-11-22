#include "controller_settings.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

/* Host preferences the game does not keep itself, each a 4-byte tag and a
   few bytes no greater than a limit, in a file below .recomp-storage. */
static fs::path settings_path(const char *root, const char *name)
{
    return fs::path(root) / ".recomp-storage" / name;
}

// 1 loaded, 0 no file, -1 unreadable or invalid.
static int load_bytes(const char *root, const char *name, const char tag[4],
    uint8_t *values, unsigned count, uint8_t limit)
{
    try {
        const auto path = settings_path(root, name);
        if (!fs::exists(path)) return 0;
        std::ifstream file(path, std::ios::binary);
        unsigned char bytes[4 + 8];
        if (count > 8u ||
            !file.read(reinterpret_cast<char *>(bytes), 4 + count) ||
            file.peek() != std::char_traits<char>::eof() ||
            std::memcmp(bytes, tag, 4) != 0) return -1;
        for (unsigned i = 0; i < count; ++i) if (bytes[4 + i] > limit) return -1;
        std::memcpy(values, bytes + 4, count);
        return 1;
    } catch (...) { return -1; }
}

static bool save_bytes(const char *root, const char *name, const char tag[4],
    const uint8_t *values, unsigned count, uint8_t limit)
{
    for (unsigned i = 0; i < count; ++i) if (values[i] > limit) return false;
    try {
        const auto path = settings_path(root, name);
        fs::create_directories(path.parent_path());
        auto temporary = path;
        temporary += ".tmp";
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(tag, 4);
        file.write(reinterpret_cast<const char *>(values), count);
        file.close();
        if (!file) return false;
#ifdef _WIN32
        return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
        fs::rename(temporary, path);
        return true;
#endif
    } catch (...) { return false; }
}

bool recomp_controller_settings_load(const char *root, uint8_t modes[4])
{
    if (root == nullptr || modes == nullptr) return false;
    const int loaded = load_bytes(root, "controller-mode-v1", "DCP1", modes, 4, 1);
    if (loaded == 0) std::memset(modes, 0, 4); // Digital until the player chooses otherwise.
    return loaded >= 0;
}

bool recomp_controller_settings_save(const char *root, const uint8_t modes[4])
{
    return root != nullptr && modes != nullptr &&
        save_bytes(root, "controller-mode-v1", "DCP1", modes, 4, 1);
}

int recomp_language_settings_load(const char *root, uint8_t languages[2])
{
    if (root == nullptr || languages == nullptr) return -1;
    return load_bytes(root, "language-v1", "DLG1", languages, 2, 1);
}

bool recomp_language_settings_save(const char *root, const uint8_t languages[2])
{
    return root != nullptr && languages != nullptr &&
        save_bytes(root, "language-v1", "DLG1", languages, 2, 1);
}
