#include "save_transaction.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

#ifdef _WIN32
static DWORD WINAPI release_delete_blocker(void *handle)
{
    Sleep(100);
    return CloseHandle(static_cast<HANDLE>(handle)) ? 0 : 1;
}
#endif

static void put(const fs::path &path, const std::string &value)
{
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << value;
    file.close();
    assert(!file.fail());
}

static std::string get(const fs::path &path)
{
    std::ifstream file(path, std::ios::binary);
    assert(file);
    return std::string(std::istreambuf_iterator<char>(file), {});
}

static void append_u64(std::string &image, uint64_t value)
{
    image.append(reinterpret_cast<const char *>(&value), sizeof value);
}

static void append_times(std::string &image, char kind, uint64_t base)
{
    image.push_back(kind);
    append_u64(image, base);
    append_u64(image, base + 1000u);
    append_u64(image, base + 2000u);
}

/* A root directory and one file. Only v2 records attributes. */
static std::string undo_image(bool v2, uint64_t file_times, uint64_t file_attributes)
{
    std::string image(v2 ? "rsundo02" : "rsundo01", 8u);
    append_u64(image, 0u);
    append_times(image, 'D', 0u);
    if (v2) append_u64(image, 0u);
    append_u64(image, 0u);
    append_times(image, 'F', file_times);
    if (v2) append_u64(image, file_attributes);
    const auto name = fs::path("legacy").native();
    append_u64(image, name.size() * sizeof name[0]);
    image.append(reinterpret_cast<const char *>(name.data()), name.size() * sizeof name[0]);
    append_u64(image, 11u);
    image += "legacy save";
    const uint64_t size = image.size();
    image.replace(8u, sizeof size, reinterpret_cast<const char *>(&size), sizeof size);
    return image;
}

int main()
{
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path root = fs::temp_directory_path() /
        ("recomp-save-test-" + std::to_string(stamp));
    assert(fs::create_directory(root));
    const auto live = root / ".recomp-storage" / "partition1" / "UDATA";
    const auto journal = root / ".recomp-storage" / "save-undo-v1";
    const auto payload = live / "title" / "profile" / "payload";
    const std::string root_name = root.string();

    assert(recomp_save_initialize(root_name.c_str()));
#ifdef _WIN32
    HANDLE competing = CreateFileW((journal / "lock").c_str(),
        GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(competing == INVALID_HANDLE_VALUE);
    assert(GetLastError() == ERROR_SHARING_VIOLATION);
#else
    int competing = open((journal / "lock").c_str(), O_RDWR | O_CLOEXEC);
    assert(competing >= 0);
    assert(flock(competing, LOCK_EX | LOCK_NB) != 0);
    assert(close(competing) == 0);
#endif
    assert(!recomp_save_pending());
    assert(recomp_save_begin(7));
    assert(recomp_save_active(7) && recomp_save_pending());
    put(payload, "first incomplete save");
    assert(!recomp_save_end(7, false));
    assert(!fs::exists(live));
    assert(!recomp_save_pending());

    assert(recomp_save_begin(7));
    put(payload, "first save interrupted");
    assert(recomp_save_initialize(root_name.c_str()));
    assert(!fs::exists(live));

    assert(recomp_save_begin(7));
    put(payload, "first complete save");
    fs::create_directory(live / "empty");
    assert(recomp_save_end(7, true));
    assert(get(payload) == "first complete save");
    assert(!fs::exists(journal / "undo"));

#ifdef _WIN32
    /* Distinct times catch a swapped restore. */
    const FILETIME creation{1u, 0x01D00000u}, access{2u, 0x01D10000u}, write{3u, 0x01D20000u};
    HANDLE timed = CreateFileW(payload.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(timed != INVALID_HANDLE_VALUE);
    assert(SetFileTime(timed, &creation, &access, &write) && CloseHandle(timed));
    const DWORD original_attributes = GetFileAttributesW(payload.c_str());
    assert(original_attributes != INVALID_FILE_ATTRIBUTES);
    const auto change_time = [](const fs::path &path) {
        HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        assert(handle != INVALID_HANDLE_VALUE);
        FILE_BASIC_INFO info{};
        assert(GetFileInformationByHandleEx(handle, FileBasicInfo, &info, sizeof info));
        assert(CloseHandle(handle));
        return info.ChangeTime.QuadPart;
    };
    const auto original_change = change_time(payload);
    assert(recomp_save_begin(7));
    const DWORD changed_attributes =
        (original_attributes & ~FILE_ATTRIBUTE_NORMAL) | FILE_ATTRIBUTE_READONLY;
    assert(SetFileAttributesW(payload.c_str(), changed_attributes));
    assert(!recomp_save_end(7, false));
    assert(GetFileAttributesW(payload.c_str()) == original_attributes);
    assert(change_time(payload) == original_change);
    WIN32_FILE_ATTRIBUTE_DATA restored_times;
    assert(GetFileAttributesExW(payload.c_str(), GetFileExInfoStandard, &restored_times));
    assert(CompareFileTime(&restored_times.ftCreationTime, &creation) == 0);
    assert(CompareFileTime(&restored_times.ftLastAccessTime, &access) == 0);
    assert(CompareFileTime(&restored_times.ftLastWriteTime, &write) == 0);
#endif

    assert(recomp_save_begin(7));
    assert(!recomp_save_begin(9));
    assert(!recomp_save_end(9, true));
    assert(recomp_save_active(7));
    assert(recomp_save_begin(7));
    put(payload, "nested partial update");
    put(live / "new-file", "must disappear");
    fs::remove(live / "empty");
    recomp_save_note_failure(7);
    assert(!recomp_save_end(7, true));
    assert(recomp_save_active(7));
    assert(!recomp_save_end(7, true));
    assert(get(payload) == "first complete save");
    assert(fs::is_directory(live / "empty"));
    assert(!fs::exists(live / "new-file"));

    /* A rejected foreign mutation aborts whichever operation is pending. */
    recomp_save_note_pending_failure();
    assert(!recomp_save_pending());
    assert(recomp_save_begin(7));
    assert(recomp_save_end(7, true));
    assert(recomp_save_begin(7));
    put(payload, "rejected by another owner");
    recomp_save_note_failure(9);
    recomp_save_note_pending_failure();
    assert(!recomp_save_end(7, true));
    assert(get(payload) == "first complete save");

    assert(recomp_save_begin(7));
    put(payload, "interrupted operation");
    assert(fs::is_regular_file(journal / "undo"));
#ifdef _WIN32
    assert(!recomp_save_initialize(nullptr));
    competing = CreateFileW((journal / "lock").c_str(),
        GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(competing != INVALID_HANDLE_VALUE);
    assert(!recomp_save_initialize(root_name.c_str()));
    assert(get(payload) == "interrupted operation");
    assert(fs::is_regular_file(journal / "undo"));
    assert(CloseHandle(competing) != 0);
#else
    assert(!recomp_save_initialize(nullptr));
    competing = open((journal / "lock").c_str(), O_RDWR | O_CLOEXEC);
    assert(competing >= 0 && flock(competing, LOCK_EX | LOCK_NB) == 0);
    assert(!recomp_save_initialize(root_name.c_str()));
    assert(get(payload) == "interrupted operation");
    assert(fs::is_regular_file(journal / "undo"));
    assert(close(competing) == 0);
#endif
    /* Reinitialization models a fresh process: volatile ownership is lost. */
    assert(recomp_save_initialize(root_name.c_str()));
    assert(get(payload) == "first complete save");

    assert(recomp_save_begin(7));
    /* Interruption after recovery removed live and copied only one file. */
    fs::remove_all(live);
    put(live / "recovery-partial", "not the prior generation");
    assert(recomp_save_initialize(root_name.c_str()));
    assert(get(payload) == "first complete save");
    assert(!fs::exists(live / "recovery-partial"));

#ifdef _WIN32
    assert(recomp_save_begin(7));
    put(payload, "locked interrupted save");
    HANDLE locked = CreateFileW(payload.c_str(), GENERIC_READ, 0, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(locked != INVALID_HANDLE_VALUE);
    assert(!recomp_save_initialize(root_name.c_str()));
    assert(fs::is_regular_file(journal / "undo"));
    assert(!recomp_save_begin(7));
    assert(CloseHandle(locked) != 0);
    assert(recomp_save_initialize(root_name.c_str()));
    assert(get(payload) == "first complete save");
#endif

    assert(recomp_save_begin(7));
    put(payload, "committed generation");
    /* Deleting the undo image is the commit point. */
    fs::remove(journal / "undo");
    assert(recomp_save_initialize(root_name.c_str()));
    assert(get(payload) == "committed generation");

    /* An image cut short never reached live data and is discarded. */
    assert(recomp_save_begin(7));
    const std::string image = get(journal / "undo");
    assert(recomp_save_end(7, true));
    put(payload, "newer than the image");
    put(journal / "undo", image.substr(0, image.size() - 1));
    assert(recomp_save_initialize(root_name.c_str()));
    assert(get(payload) == "newer than the image");
    assert(!fs::exists(journal / "undo"));

    /* An image longer than its recorded size is malformed and kept. */
    put(journal / "undo", image + "x");
    assert(!recomp_save_initialize(root_name.c_str()));
    assert(get(payload) == "newer than the image");
    assert(fs::exists(journal / "undo"));
    fs::remove(journal / "undo");

    /* A directory record from the previous journal format fails closed. */
    fs::create_directory(journal / "pending");
    assert(!recomp_save_initialize(root_name.c_str()));
    fs::remove(journal / "pending");

    put(journal / "unknown", "leave this alone");
    assert(!recomp_save_initialize(root_name.c_str()));
    assert(get(journal / "unknown") == "leave this alone");
    assert(get(payload) == "newer than the image");
    fs::remove(journal / "unknown");
    assert(recomp_save_initialize(root_name.c_str()));

    /* Recover the previous known image format, then upgrade its marker past
       a temporary marker left by an interrupted upgrade. */
    const uint64_t legacy_times = 132000000000000000u;
    put(journal / "version", "recomp-save-undo-v2\nx");
    assert(!recomp_save_initialize(root_name.c_str()));
    put(journal / "version", "recomp-save-undo-v1\n");
    put(journal / "version.tmp", "recomp-sa");
    put(journal / "undo", undo_image(false, legacy_times, 0u));
    assert(recomp_save_initialize(root_name.c_str()));
    assert(!fs::exists(payload));
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA legacy;
    assert(GetFileAttributesExW((live / "legacy").c_str(), GetFileExInfoStandard, &legacy));
    const auto ticks = [](FILETIME time) {
        return (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    };
    assert(ticks(legacy.ftCreationTime) == legacy_times);
    assert(ticks(legacy.ftLastAccessTime) == legacy_times + 1000u);
    assert(ticks(legacy.ftLastWriteTime) == legacy_times + 2000u);
#else
    assert(uint64_t(fs::last_write_time(live / "legacy").time_since_epoch().count()) ==
        legacy_times + 2000u);
#endif
    assert(get(live / "legacy") == "legacy save");
    assert(get(journal / "version") == "recomp-save-undo-v2\n");
    assert(!fs::exists(journal / "version.tmp"));
    assert(!fs::exists(journal / "undo"));

#ifdef _WIN32
    /* Storage attributes rollback cannot rebuild are refused before any change. */
    const auto sparse = live / "sparse";
    put(sparse, "sparse");
    HANDLE sparse_handle = CreateFileW(sparse.c_str(), GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(sparse_handle != INVALID_HANDLE_VALUE);
    DWORD returned;
    assert(DeviceIoControl(sparse_handle, FSCTL_SET_SPARSE, nullptr, 0,
        nullptr, 0, &returned, nullptr));
    assert(CloseHandle(sparse_handle));
    assert(!recomp_save_begin(7));
    assert(!fs::exists(journal / "undo"));
    fs::remove(sparse);
    assert(recomp_save_initialize(root_name.c_str()));

    put(journal / "undo", undo_image(true, legacy_times, FILE_ATTRIBUTE_ENCRYPTED));
    assert(!recomp_save_initialize(root_name.c_str()));
    assert(get(live / "legacy") == "legacy save");
    assert(fs::is_regular_file(journal / "undo"));
    fs::remove(journal / "undo");
    assert(recomp_save_initialize(root_name.c_str()));
#endif

#ifndef _WIN32
    const auto sparse = live / "sparse";
    put(sparse, "");
    fs::resize_file(sparse, 1024 * 1024);
    assert(!recomp_save_begin(7));
    assert(!fs::exists(journal / "undo"));
    fs::remove(sparse);
    assert(recomp_save_initialize(root_name.c_str()));
#endif

    const auto outside = root / "outside";
    put(outside / "untouched", "outside data");
#ifdef _WIN32
    const std::wstring command = L"cmd /c mklink /J \"" +
        (live / "linked").wstring() + L"\" \"" + outside.wstring() + L"\" >NUL";
    assert(_wsystem(command.c_str()) == 0);
    assert((GetFileAttributesW((live / "linked").c_str()) &
        FILE_ATTRIBUTE_REPARSE_POINT) != 0);
#else
    fs::create_directory_symlink(outside, live / "linked");
#endif
    assert(!recomp_save_begin(7));
    assert(get(outside / "untouched") == "outside data");
    fs::remove(live / "linked");
    assert(recomp_save_initialize(root_name.c_str()));

    assert(recomp_save_begin(0));
    put(payload, "last generation");
    assert(recomp_save_begin(0));
    assert(recomp_save_end(0, true));
    assert(recomp_save_end(0, true));
    assert(recomp_save_initialize(root_name.c_str()));
    assert(get(payload) == "last generation");
#ifdef _WIN32
    assert(recomp_save_begin(7));
    put(payload, "delete released");
    HANDLE blocker = CreateFileW((journal / "undo").c_str(),
        GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(blocker != INVALID_HANDLE_VALUE);
    assert(!DeleteFileW((journal / "undo").c_str()));
    assert(GetLastError() == ERROR_SHARING_VIOLATION);
    HANDLE release = CreateThread(nullptr, 0, release_delete_blocker,
        blocker, 0, nullptr);
    assert(release != nullptr);
    assert(recomp_save_end(7, true));
    assert(WaitForSingleObject(release, 1000) == WAIT_OBJECT_0);
    DWORD thread_status;
    assert(GetExitCodeThread(release, &thread_status) && thread_status == 0);
    assert(CloseHandle(release));
    assert(get(payload) == "delete released");
    assert(!fs::exists(journal / "undo"));

    assert(recomp_save_begin(7));
    put(payload, "delete persistently blocked");
    blocker = CreateFileW((journal / "undo").c_str(),
        GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    assert(blocker != INVALID_HANDLE_VALUE);
    DWORD before = GetTickCount();
    assert(!recomp_save_end(7, true));
    assert(GetTickCount() - before < 2000);
    assert(fs::exists(journal / "undo"));
    assert(!recomp_save_begin(7));
    assert(get(payload) == "delete persistently blocked");
    assert(CloseHandle(blocker));
    assert(recomp_save_initialize(root_name.c_str()));
    assert(get(payload) == "delete released");
#endif
#ifndef _WIN32
    /* A differently cased store must be the same tree the guest opens. */
    fs::rename(live, live.parent_path() / "udata");
    const auto lower_payload = live.parent_path() / "udata" / "title" / "profile" / "payload";
    assert(recomp_save_initialize(root_name.c_str()));
    const timespec stamps[2] = {{1700000000, 123456789}, {1700000001, 987654321}};
    assert(utimensat(AT_FDCWD, lower_payload.c_str(), stamps, 0) == 0);
    assert(chmod(lower_payload.c_str(), 0640) == 0);
    assert(recomp_save_begin(7));
    put(lower_payload, "partial POSIX save");
    assert(chmod(lower_payload.c_str(), 0400) == 0);
    assert(!recomp_save_end(7, false));
    struct stat restored{};
    assert(stat(lower_payload.c_str(), &restored) == 0);
    assert((restored.st_mode & 0777) == 0640);
    assert(restored.st_atim.tv_sec == stamps[0].tv_sec && restored.st_atim.tv_nsec == stamps[0].tv_nsec);
    assert(restored.st_mtim.tv_sec == stamps[1].tv_sec && restored.st_mtim.tv_nsec == stamps[1].tv_nsec);
    assert(get(lower_payload) == "last generation");
    assert(!fs::exists(live));
#endif
    /* Release the lifetime lock before removing this disposable fixture. */
    assert(!recomp_save_initialize(nullptr));
    fs::remove_all(root);
    std::cout << "save transaction checks passed\n";
}
