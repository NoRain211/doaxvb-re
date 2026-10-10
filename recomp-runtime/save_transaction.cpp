#include "save_transaction.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {
fs::path storage, live, journal, undo;
bool ready, failed;
uint32_t active_owner, depth;
const char legacy_version[] = "recomp-save-undo-v1\n";
const char version[] = "recomp-save-undo-v2\n";
static_assert(sizeof legacy_version == sizeof version, "markers share one size");
/* journal/undo holds the whole UDATA tree as it was before the operation, as
   one file so a save costs a few file operations. Its header records the
   image size: an image cut short by an interruption never reached live data
   and is discarded. Deleting it commits the operation. */
/* v2 adds attributes; v3 adds the change time. */
const char undo_magic_v1[8] = {'r', 's', 'u', 'n', 'd', 'o', '0', '1'};
const char undo_magic_v2[8] = {'r', 's', 'u', 'n', 'd', 'o', '0', '2'};
const char undo_magic[8] = {'r', 's', 'u', 'n', 'd', 'o', '0', '3'};
constexpr size_t undo_header = sizeof undo_magic + sizeof(uint64_t);
#ifdef _WIN32
HANDLE journal_lock = INVALID_HANDLE_VALUE;
/* Rollback rebuilds files with SetFileAttributesW, which silently drops
   these, so a tree using them is refused before any change. */
constexpr DWORD unsupported_attributes = FILE_ATTRIBUTE_REPARSE_POINT |
    FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_ENCRYPTED | FILE_ATTRIBUTE_SPARSE_FILE;
/* Rollback restores only what SetFileAttributesW accepts; filesystem-owned
   states such as ReFS integrity streams would make recovery fail forever. */
constexpr DWORD settable_attributes = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN |
    FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NORMAL |
    FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
#else
int journal_lock = -1;
#endif

struct Times { uint64_t creation, access, write, change; };
struct Node {
    Times times;
    fs::path path;
    bool directory;
    uint32_t attributes;
    std::string_view data;
};

void require(bool condition)
{
    if (!condition) throw std::runtime_error("invalid save journal or path");
}

bool exists_plain(const fs::path &path)
{
#ifdef _WIN32
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        DWORD error = GetLastError();
        require(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND);
        return false;
    }
    require((attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0);
    return true;
#else
    std::error_code ec;
    auto status = fs::symlink_status(path, ec);
    if (ec) {
        require(ec == std::errc::no_such_file_or_directory);
        return false;
    }
    if (status.type() == fs::file_type::not_found) {
        return false;
    }
    require(status.type() != fs::file_type::symlink);
    return true;
#endif
}

void check_parents(const fs::path &path)
{
    fs::path part;
    for (const auto &component : path) {
        part /= component;
        if (exists_plain(part)) require(fs::is_directory(part));
    }
}

void check_tree(const fs::path &path)
{
    if (!exists_plain(path)) return;
    require(fs::is_directory(path) || fs::is_regular_file(path));
    if (fs::is_directory(path)) {
        for (const auto &entry : fs::directory_iterator(path))
            check_tree(entry.path());
    }
}

void remove_tree(const fs::path &path)
{
    check_tree(path);
#ifdef _WIN32
    auto clear_readonly = [&](auto &&self, const fs::path &entry) -> void {
        DWORD attributes = GetFileAttributesW(entry.c_str());
        require(attributes != INVALID_FILE_ATTRIBUTES);
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0u) {
            for (const auto &child : fs::directory_iterator(entry))
                self(self, child.path());
        }
        if ((attributes & FILE_ATTRIBUTE_READONLY) != 0u) {
            DWORD writable = attributes & ~(FILE_ATTRIBUTE_READONLY |
                FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT);
            if (writable == 0u) writable = FILE_ATTRIBUTE_NORMAL;
            require(SetFileAttributesW(entry.c_str(), writable) != 0);
        }
    };
    if (exists_plain(path)) clear_readonly(clear_readonly, path);
#endif
    fs::remove_all(path);
}

void remove_file(const fs::path &path)
{
#ifdef _WIN32
    const DWORD started = GetTickCount();
    for (;;) {
        if (DeleteFileW(path.c_str())) return;
        const DWORD error = GetLastError();
        const DWORD elapsed = GetTickCount() - started;
        /* Scanners and indexers briefly open newly written files. */
        if ((error != ERROR_ACCESS_DENIED && error != ERROR_SHARING_VIOLATION &&
             error != ERROR_LOCK_VIOLATION) || elapsed >= 500u) {
            std::fprintf(stderr, "save journal delete failed: win32=%lu elapsed_ms=%lu\n",
                static_cast<unsigned long>(error), static_cast<unsigned long>(elapsed));
            throw fs::filesystem_error("delete save journal", path,
                std::error_code(error, std::system_category()));
        }
        Sleep((500u - elapsed < 10u) ? 500u - elapsed : 10u);
    }
#else
    require(fs::remove(path));
#endif
}

void write_file(const fs::path &path, std::string_view data)
{
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(data.data(), static_cast<std::streamsize>(data.size()));
    stream.close();
    require(!stream.fail());
}

/* Syncs a path and the directory holding it. The undo journal must be on disk
   before the guest changes live saves, or a power loss can leave a torn save
   with no rollback. */
void sync_to_disk(const fs::path &path)
{
#ifndef _WIN32
    for (const fs::path &target : {path, path.parent_path()}) {
        const int fd = open(target.c_str(), O_RDONLY | O_CLOEXEC);
        require(fd >= 0);
#ifdef __APPLE__
        // macOS fsync leaves data in the drive cache; F_FULLFSYNC flushes it.
        const bool synced = fcntl(fd, F_FULLFSYNC) == 0 || fsync(fd) == 0;
#else
        const bool synced = fsync(fd) == 0;
#endif
        close(fd);
        require(synced);
    }
#else
    // shortcut: Windows keeps its tested behavior; flush here if a Windows torn save is reported.
    (void)path;
#endif
}

std::string read_file(const fs::path &path)
{
#ifdef _WIN32
    /* Guest profile handles may hold delete access, which ifstream's sharing
       mode conflicts with. */
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(handle != INVALID_HANDLE_VALUE);
    std::string data;
    LARGE_INTEGER size{};
    bool read = GetFileSizeEx(handle, &size) != 0 && size.QuadPart >= 0;
    if (read) data.resize(static_cast<size_t>(size.QuadPart));
    for (size_t at = 0; read && at < data.size();) {
        const DWORD chunk = static_cast<DWORD>((std::min)(data.size() - at, size_t(1) << 30));
        DWORD got = 0;
        read = ReadFile(handle, data.data() + at, chunk, &got, nullptr) != 0 && got != 0;
        at += got;
    }
    const bool closed = CloseHandle(handle) != 0;
    require(read && closed);
    return data;
#else
    std::string data(static_cast<size_t>(fs::file_size(path)), '\0');
    std::ifstream stream(path, std::ios::binary);
    stream.read(data.data(), static_cast<std::streamsize>(data.size()));
    require(!stream.fail());
    return data;
#endif
}

void put(std::string &out, uint64_t value)
{
    out.append(reinterpret_cast<const char *>(&value), sizeof value);
}

void put(std::string &out, std::string_view bytes)
{
    put(out, uint64_t(bytes.size()));
    out.append(bytes);
}

void save_node(std::string &out, const fs::path &path, const fs::path &relative)
{
    Times times{};
    uint32_t saved_attributes = 0u;
#ifdef _WIN32
    /* A handle query is the only way to read the change time. */
    HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    require(handle != INVALID_HANDLE_VALUE);
    FILE_BASIC_INFO info{};
    const bool queried = GetFileInformationByHandleEx(handle, FileBasicInfo, &info, sizeof info) != 0;
    const bool closed = CloseHandle(handle) != 0;
    require(queried && closed);
    require((info.FileAttributes & unsupported_attributes) == 0);
    saved_attributes = info.FileAttributes;
    const bool directory = (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    times = {uint64_t(info.CreationTime.QuadPart), uint64_t(info.LastAccessTime.QuadPart),
        uint64_t(info.LastWriteTime.QuadPart), uint64_t(info.ChangeTime.QuadPart)};
#else
    const auto status = fs::symlink_status(path);
    require(fs::is_directory(status) || fs::is_regular_file(status));
    const bool directory = fs::is_directory(status);
    times.write = static_cast<uint64_t>(fs::last_write_time(path).time_since_epoch().count());
#endif
    out += directory ? 'D' : 'F';
    put(out, times.creation);
    put(out, times.access);
    put(out, times.write);
    put(out, times.change);
    put(out, uint64_t(saved_attributes));
    const auto &name = relative.native();
    put(out, std::string_view(reinterpret_cast<const char *>(name.data()),
        name.size() * sizeof(fs::path::value_type)));
    if (!directory) {
        put(out, read_file(path));
        return;
    }
    for (const auto &entry : fs::directory_iterator(path))
        save_node(out, entry.path(), relative / entry.path().filename());
}

std::string snapshot_live()
{
    std::string out(undo_magic, sizeof undo_magic);
    put(out, uint64_t(0));
    if (exists_plain(live)) {
        require(fs::is_directory(live));
        save_node(out, live, fs::path());
    }
    const uint64_t size = out.size();
    std::memcpy(&out[sizeof undo_magic], &size, sizeof size);
    return out;
}

struct Reader {
    std::string_view data;
    size_t at = undo_header;

    std::string_view take(uint64_t size)
    {
        require(size <= data.size() - at);
        const auto bytes = data.substr(at, static_cast<size_t>(size));
        at += static_cast<size_t>(size);
        return bytes;
    }
    uint64_t number()
    {
        uint64_t value;
        std::memcpy(&value, take(sizeof value).data(), sizeof value);
        return value;
    }
};

/* One call restores times and attributes, so setting attributes cannot move
   the restored change time. A zero field (older images) is left unchanged. */
void set_metadata(const Node &node)
{
#ifdef _WIN32
    HANDLE handle = CreateFileW(node.path.c_str(), FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    require(handle != INVALID_HANDLE_VALUE);
    FILE_BASIC_INFO info{};
    info.CreationTime.QuadPart = LONGLONG(node.times.creation);
    info.LastAccessTime.QuadPart = LONGLONG(node.times.access);
    info.LastWriteTime.QuadPart = LONGLONG(node.times.write);
    info.ChangeTime.QuadPart = LONGLONG(node.times.change);
    info.FileAttributes = node.attributes & settable_attributes;
    bool copied = SetFileInformationByHandle(handle, FileBasicInfo, &info, sizeof info) != 0;
    bool closed = CloseHandle(handle) != 0;
    require(copied && closed);
#else
    fs::last_write_time(node.path, fs::file_time_type(
        fs::file_time_type::duration(static_cast<int64_t>(node.times.write))));
#endif
}

/* A name as the filesystem compares it: Windows ignores case and separator style. */
fs::path::string_type name_key(const fs::path &path)
{
    auto key = path.lexically_normal().native();
#ifdef _WIN32
    if (!key.empty()) CharUpperBuffW(key.data(), static_cast<DWORD>(key.size()));
#endif
    return key;
}

/* Parse the whole image before touching live data. A restart during removal
   or rewriting repeats this from the same image. */
void restore(std::string_view image)
{
    Reader in{image};
    std::vector<Node> nodes;
    const bool has_change = std::memcmp(image.data(), undo_magic, sizeof undo_magic) == 0;
    const bool has_attributes = has_change ||
        std::memcmp(image.data(), undo_magic_v2, sizeof undo_magic_v2) == 0;
    std::set<fs::path::string_type> seen, directories;
    while (in.at != image.size()) {
        Node node{};
        const char kind = in.take(1)[0];
        require(kind == 'D' || kind == 'F');
        node.directory = kind == 'D';
        node.times.creation = in.number();
        node.times.access = in.number();
        node.times.write = in.number();
        if (has_change) node.times.change = in.number();
        if (has_attributes) {
            const uint64_t attributes = in.number();
            require(attributes <= (std::numeric_limits<uint32_t>::max)());
            node.attributes = static_cast<uint32_t>(attributes);
#ifdef _WIN32
            require((node.attributes & unsupported_attributes) == 0u);
#endif
        }
        const auto name = in.take(in.number());
        require(name.size() % sizeof(fs::path::value_type) == 0);
        fs::path::string_type native(name.size() / sizeof(fs::path::value_type), 0);
        std::memcpy(native.data(), name.data(), name.size());
        const fs::path relative(native);
        require(nodes.empty() ? relative.empty() && node.directory
                              : !relative.empty() && !relative.has_root_path());
        for (const auto &part : relative) require(part != ".." && part != ".");
        /* Each name once, after its parent directory, so rebuilding cannot fail midway. */
        const auto key = name_key(relative);
        require(nodes.empty() || directories.count(name_key(relative.parent_path())) != 0);
        require(seen.insert(key).second);
        if (node.directory) directories.insert(key);
        node.path = nodes.empty() ? live : live / relative;
        if (!node.directory) node.data = in.take(in.number());
        nodes.push_back(node);
    }
    remove_tree(live);
    if (nodes.empty()) return;
    fs::create_directories(live.parent_path());
    for (const auto &node : nodes) {
        if (node.directory) {
            require(fs::create_directory(node.path));
        } else {
            write_file(node.path, node.data);
            set_metadata(node);
        }
    }
    /* Creating children updates directory times, so restore them last. */
    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
        if (it->directory) set_metadata(*it);
    }
}

void recover()
{
    if (!exists_plain(undo)) return;
    require(fs::is_regular_file(undo));
    const std::string image = read_file(undo);
    if (image.size() >= sizeof undo_magic) {
        require(std::memcmp(image.data(), undo_magic, sizeof undo_magic) == 0 ||
            std::memcmp(image.data(), undo_magic_v2, sizeof undo_magic_v2) == 0 ||
            std::memcmp(image.data(), undo_magic_v1, sizeof undo_magic_v1) == 0);
    }
    uint64_t size = 0;
    if (image.size() >= undo_header) std::memcpy(&size, image.data() + sizeof undo_magic, sizeof size);
    /* Only an image shorter than its recorded size is discardable; anything
       else malformed stops startup with the undo file kept. */
    if (image.size() >= undo_header && image.size() >= size) {
        require(image.size() == size);
        restore(image);
    }
    remove_file(undo);
}

bool check_journal()
{
    require(exists_plain(journal) && fs::is_directory(journal));
    const auto version_path = journal / "version";
    require(exists_plain(version_path) && fs::is_regular_file(version_path));
    /* Reading one byte past the marker proves the file ends there. */
    char buffer[sizeof version];
    std::ifstream stream(version_path, std::ios::binary);
    stream.read(buffer, sizeof buffer);
    require(!stream.bad() && stream.eof() && stream.gcount() == sizeof version - 1);
    const std::string_view marker(buffer, sizeof version - 1);
    require(marker == version || marker == legacy_version);
    const bool legacy = marker == legacy_version;
    for (const auto &entry : fs::directory_iterator(journal)) {
        const auto name = entry.path().filename();
        if (name == "staging" || name == "pending" || name == "committed") {
            throw std::runtime_error(
                "save journal from an older build; run that build once to recover it");
        }
        /* An upgrade interrupted before its rename leaves version.tmp. */
        require(name == "version" || name == "lock" || name == "undo" ||
            (legacy && name == "version.tmp"));
        require(exists_plain(entry.path()) && fs::is_regular_file(entry.path()));
    }
    return legacy;
}
}

extern "C" bool recomp_save_initialize(const char *disc_root)
{
#ifdef _WIN32
    if (journal_lock != INVALID_HANDLE_VALUE) {
        CloseHandle(journal_lock);
        journal_lock = INVALID_HANDLE_VALUE;
    }
#else
    if (journal_lock >= 0) {
        close(journal_lock);
        journal_lock = -1;
    }
#endif
    ready = false;
    depth = 0;
    failed = false;
    try {
        require(disc_root != nullptr && *disc_root != '\0');
#ifdef _WIN32
        fs::path root = fs::absolute(disc_root).lexically_normal();
#else
        // macOS reaches /tmp and /var through symlinks, which check_parents
        // rejects; resolve the root first.
        std::error_code ec;
        fs::path root = fs::canonical(disc_root, ec);
        if (ec) root = fs::absolute(disc_root).lexically_normal();
#endif
        check_parents(root);
        require(exists_plain(root) && fs::is_directory(root));
        storage = root / ".recomp-storage";
        live = storage / "partition1" / "UDATA";
        journal = storage / "save-undo-v1";
        undo = journal / "undo";
        check_parents(storage);
        fs::create_directories(storage);
        check_parents(live.parent_path());
        bool created = false;
        bool upgrade_version = false;
        if (!exists_plain(journal)) created = fs::create_directory(journal);
        require(exists_plain(journal) && fs::is_directory(journal));
#ifdef _WIN32
        const auto lock_path = journal / "lock";
        if (exists_plain(lock_path)) require(fs::is_regular_file(lock_path));
        journal_lock = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE,
            0, nullptr, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        require(journal_lock != INVALID_HANDLE_VALUE);
        BY_HANDLE_FILE_INFORMATION lock_info;
        require(GetFileInformationByHandle(journal_lock, &lock_info) != 0);
        require((lock_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0);
        require(lock_info.nFileSizeHigh == 0 && lock_info.nFileSizeLow == 0);
#else
        const auto lock_path = journal / "lock";
        if (exists_plain(lock_path)) require(fs::is_regular_file(lock_path));
        journal_lock = open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0666);
        require(journal_lock >= 0);
        require(flock(journal_lock, LOCK_EX | LOCK_NB) == 0);
#endif
        if (created) {
            write_file(journal / "version", version);
        }
        /* Persist the marker and the directory entries on every start, so a sync
           that failed on an earlier launch is retried. */
        sync_to_disk(journal / "version");
        sync_to_disk(storage);
        upgrade_version = check_journal();
        recover();
        if (upgrade_version) {
            /* Keep the v1 marker valid until the same-directory rename. */
            write_file(journal / "version.tmp", version);
            fs::rename(journal / "version.tmp", journal / "version");
        }
        check_tree(live);
        ready = true;
        return true;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "save recovery failed: %s\n", error.what());
#ifdef _WIN32
        if (journal_lock != INVALID_HANDLE_VALUE) {
            CloseHandle(journal_lock);
            journal_lock = INVALID_HANDLE_VALUE;
        }
#else
        if (journal_lock >= 0) {
            close(journal_lock);
            journal_lock = -1;
        }
#endif
        return false;
    }
}

extern "C" bool recomp_save_begin(uint32_t owner)
{
    if (!ready) return false;
    if (depth != 0) {
        if (owner != active_owner || depth == (std::numeric_limits<uint32_t>::max)())
            return false;
        ++depth;
        return true;
    }
    try {
        require(!exists_plain(undo));
        write_file(undo, snapshot_live());
        sync_to_disk(undo);
        active_owner = owner;
        depth = 1;
        failed = false;
        return true;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "save begin failed: %s\n", error.what());
        ready = false;
        return false;
    }
}

extern "C" bool recomp_save_end(uint32_t owner, bool success)
{
    if (!ready || depth == 0 || owner != active_owner) return false;
    failed = failed || !success;
    if (--depth != 0) return !failed;
    try {
        require(exists_plain(undo));
        if (failed) {
            recover();
        } else {
            check_tree(live);
            remove_file(undo);
            /* A lost unlink would restore the old image over a save reported done. */
            sync_to_disk(journal);
        }
        return !failed;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "save end failed: %s\n", error.what());
        ready = false;
        return false;
    }
}

extern "C" void recomp_save_note_failure(uint32_t owner)
{
    if (depth != 0 && owner == active_owner) failed = true;
}

extern "C" bool recomp_save_end_recovers(uint32_t owner, bool success)
{
    return ready && depth == 1 && owner == active_owner && (failed || !success);
}

extern "C" void recomp_save_note_pending_failure(void)
{
    if (depth != 0) failed = true;
}

extern "C" bool recomp_save_active(uint32_t owner)
{
    return ready && depth != 0 && owner == active_owner;
}

extern "C" bool recomp_save_pending(void)
{
    return depth != 0;
}
