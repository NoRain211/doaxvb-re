#include "kernel_abi.h"
#include "runtime.h"
#include "directory_model.h"
#include "device_model.h"
#include "symbolic_link_model.h"
#include "save_transaction.h"
#include "stop_report.h"
#ifdef RECOMP_FULL_PROGRAM
#include "fiber_adapter.h"
#endif

#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include "win32_compat.h"
#endif

static const uint32_t RECOMP_STATUS_SUCCESS = 0x00000000u;
static const uint32_t RECOMP_STATUS_INVALID_HANDLE = 0xc0000008u;
static const uint32_t RECOMP_STATUS_INVALID_PARAMETER = 0xc000000du;
static const uint32_t RECOMP_STATUS_OBJECT_NAME_NOT_FOUND = 0xc0000034u;
static const uint32_t RECOMP_STATUS_NO_MEMORY = 0xc0000017u;
static const uint32_t RECOMP_STATUS_DELETE_PENDING = 0xc0000056u;

enum {
    MAX_FILE_HANDLES = 256,
    MAX_PATH_LEN = 512,
    MAX_SEGMENT_LEN = 128,
};

typedef enum FileHandleKind {
    FILE_HANDLE_HOST_FILE,
    FILE_HANDLE_DIRECTORY,
    FILE_HANDLE_PSEUDO,
} FileHandleKind;

typedef struct FileHandleEntry {
    HANDLE host_handle;
    uint32_t guest_handle;
    uint64_t cursor;
    RecompDirectoryModel directory;
    FileHandleKind kind;
    int active;
    int is_writable;
    int save_owned; /* Profile mutation handles block transaction end. */
    int save_write; /* Data writers also need flushing on close. */
    int delete_access;
    uint32_t sharing_access; /* FILE_SHARE_* bits this handle's access conflicts with. */
    uint32_t share_mode;
    int write_attributes;
    int delete_on_close;
    RecompReadFilter read_filter;
    uint32_t save_owner;
    uint32_t delete_owner;
    char host_path[MAX_PATH_LEN];
} FileHandleEntry;

static FileHandleEntry file_handles[MAX_FILE_HANDLES];
static uint32_t next_guest_handle = 1u;
static RecompSymbolicLinkModel symbolic_links;
static int symbolic_links_initialized;
static bool is_profile_path(const char *path);

static void ensure_symbolic_links_initialized(void)
{
    if (!symbolic_links_initialized) {
        recomp_symbolic_link_reset(&symbolic_links);
        symbolic_links_initialized = 1;
    }
}

/* Supplied by runner.cpp from the directory containing the XBE. */
const char *recomp_disc_root_path = NULL;

/* Private proving runs use these fail-loud checkpoints to interrupt real
   save writes. They are inactive unless explicitly selected in the environment. */
static unsigned save_write_number;

static unsigned save_fault_write(const char *name)
{
    const char *text = getenv(name);
    char *end;
    if (text == NULL) return 0u;
    unsigned long value = strtoul(text, &end, 10);
    if (*text == '\0' || *end != '\0' || value == 0u || value > 1024u) {
        recomp_stop(64, "save:invalid-fault-setting");
    }
    return (unsigned)value;
}

static uint32_t current_save_owner(void)
{
#ifdef RECOMP_FULL_PROGRAM
    return recomp_fiber_adapter_model()->current_handle;
#else
    return 0u;
#endif
}

static FileHandleEntry *find_file_handle(uint32_t guest_handle)
{
    for (size_t i = 0; i < MAX_FILE_HANDLES; ++i) {
        if (file_handles[i].active && file_handles[i].guest_handle == guest_handle) {
            return &file_handles[i];
        }
    }
    return NULL;
}

static uint32_t sharing_access_of(uint32_t desired_access)
{
    uint32_t access = 0u;
    if ((desired_access & (GENERIC_READ | GENERIC_EXECUTE | 0x1u | 0x20u)) != 0u) {
        access |= FILE_SHARE_READ;
    }
    if ((desired_access & (GENERIC_WRITE | 0x2u | 0x4u)) != 0u) access |= FILE_SHARE_WRITE;
    if ((desired_access & DELETE) != 0u) access |= FILE_SHARE_DELETE;
    return access;
}

/* Guest sharing on profile paths is checked here. Native profile handles
   always share reads so the save snapshot can copy a file the guest holds
   exclusively, and directories have no native handle at all. */
static bool profile_sharing_conflict(
    const char *host_path, uint32_t desired_access, uint32_t share_access)
{
    const uint32_t access = sharing_access_of(desired_access);
    share_access &= FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    for (size_t i = 0; i < MAX_FILE_HANDLES; ++i) {
        const FileHandleEntry *entry = &file_handles[i];
        if (entry->active && entry->kind != FILE_HANDLE_PSEUDO &&
            _stricmp(entry->host_path, host_path) == 0 &&
            ((access & ~entry->share_mode) != 0u ||
             (entry->sharing_access & ~share_access) != 0u)) {
            return true;
        }
    }
    return false;
}

/* Rollback deletes and rebuilds the profile tree, which an open native handle
   without delete sharing blocks. The runtime stops after a rollback, so the
   released handles only need to fail cleanly if used. */
void recomp_kernel_release_profile_handles(void)
{
    for (size_t i = 0; i < MAX_FILE_HANDLES; ++i) {
        FileHandleEntry *entry = &file_handles[i];
        if (entry->active && entry->host_handle != INVALID_HANDLE_VALUE &&
            is_profile_path(entry->host_path)) {
            CloseHandle(entry->host_handle);
            entry->host_handle = INVALID_HANDLE_VALUE;
        }
    }
}

bool recomp_kernel_save_handles_closed(uint32_t owner)
{
    for (size_t i = 0; i < MAX_FILE_HANDLES; ++i) {
        if (file_handles[i].active &&
            ((file_handles[i].save_owned && file_handles[i].save_owner == owner) ||
             (file_handles[i].delete_on_close &&
              file_handles[i].delete_owner == owner))) {
            return false;
        }
    }
    return true;
}

static bool save_write_allowed(const FileHandleEntry *entry, uint32_t owner)
{
    if (entry->save_write && recomp_save_pending() && !recomp_save_active(owner)) {
        recomp_stop(2, "save:non-owner-write");
    }
    if (recomp_save_active(owner) &&
        (!entry->save_write || entry->save_owner != owner)) {
        recomp_save_note_failure(owner);
        return false;
    }
    return true;
}

static uint32_t register_file_handle(
    HANDLE host_handle,
    FileHandleKind kind,
    const char *host_path,
    uint32_t desired_access,
    uint32_t share_access,
    int is_writable,
    uint32_t *status)
{
    const bool profile_path = kind != FILE_HANDLE_PSEUDO &&
        host_path != NULL && is_profile_path(host_path);
    const uint32_t sharing_access = sharing_access_of(desired_access);
    if (profile_path && profile_sharing_conflict(host_path, desired_access, share_access)) {
        *status = 0xc0000043u; /* STATUS_SHARING_VIOLATION */
        return 0u;
    }
    share_access &= FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    for (size_t i = 0; i < MAX_FILE_HANDLES; ++i) {
        if (!file_handles[i].active) {
            file_handles[i].active = 1;
            file_handles[i].is_writable = is_writable;
            file_handles[i].save_write = profile_path && kind == FILE_HANDLE_HOST_FILE &&
                (desired_access & GENERIC_WRITE) != 0u;
            file_handles[i].delete_access =
                (desired_access & 0x00010000u) != 0u;
            file_handles[i].sharing_access = sharing_access;
            file_handles[i].share_mode = share_access;
            file_handles[i].write_attributes =
                (desired_access & (GENERIC_WRITE | FILE_WRITE_ATTRIBUTES)) != 0u;
            file_handles[i].save_owned = profile_path &&
                (file_handles[i].delete_access || file_handles[i].write_attributes);
            file_handles[i].delete_on_close = 0;
            file_handles[i].read_filter = NULL;
            file_handles[i].save_owner = file_handles[i].save_owned ? current_save_owner() : 0u;
            file_handles[i].delete_owner = 0u;
            file_handles[i].host_handle = host_handle;
            file_handles[i].guest_handle = next_guest_handle;
            file_handles[i].cursor = 0u;
            recomp_directory_reset(&file_handles[i].directory);
            file_handles[i].kind = kind;
            file_handles[i].host_path[0] = '\0';
            if (host_path != NULL) {
                strncpy(file_handles[i].host_path, host_path,
                    sizeof file_handles[i].host_path - 1u);
                file_handles[i].host_path[
                    sizeof file_handles[i].host_path - 1u] = '\0';
            }
            if (kind == FILE_HANDLE_DIRECTORY && host_path != NULL) {
                char pattern[MAX_PATH_LEN];
                WIN32_FIND_DATAA data;
                HANDLE find;

                snprintf(pattern, sizeof pattern, "%s\\*", host_path);
                find = FindFirstFileA(pattern, &data);
                if (find != INVALID_HANDLE_VALUE) {
                    do {
                        RecompDirectoryEntry entry = {0};
                        uint64_t size;

                        if (strcmp(data.cFileName, ".") == 0 ||
                            strcmp(data.cFileName, "..") == 0) {
                            continue;
                        }
                        if (strlen(data.cFileName) >= sizeof entry.name) {
                            continue;
                        }
                        strcpy(entry.name, data.cFileName);
                        entry.creation_time =
                            (uint64_t)data.ftCreationTime.dwLowDateTime |
                            (uint64_t)data.ftCreationTime.dwHighDateTime << 32u;
                        entry.last_access_time =
                            (uint64_t)data.ftLastAccessTime.dwLowDateTime |
                            (uint64_t)data.ftLastAccessTime.dwHighDateTime << 32u;
                        entry.last_write_time =
                            (uint64_t)data.ftLastWriteTime.dwLowDateTime |
                            (uint64_t)data.ftLastWriteTime.dwHighDateTime << 32u;
                        entry.change_time = entry.last_write_time;
                        size = (uint64_t)data.nFileSizeLow |
                            (uint64_t)data.nFileSizeHigh << 32u;
                        entry.size = size;
                        entry.allocation_size = (size + 4095u) & ~4095ull;
                        entry.attributes = data.dwFileAttributes;
                        if (!recomp_directory_add(
                                &file_handles[i].directory, &entry)) {
                            break;
                        }
                    } while (FindNextFileA(find, &data));
                    FindClose(find);
                }
            }
            next_guest_handle += 4u;
            *status = RECOMP_STATUS_SUCCESS;
            return file_handles[i].guest_handle;
        }
    }
    *status = RECOMP_STATUS_NO_MEMORY;
    return 0u;
}

uint32_t recomp_kernel_open_readonly(const wchar_t *path, RecompReadFilter filter)
{
    uint32_t status;
    uint32_t handle;
    HANDLE file;

    if (path == NULL) {
        return 0u;
    }
    file = CreateFileW(path, GENERIC_READ,
        FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return 0u;
    }
    handle = register_file_handle(file, FILE_HANDLE_HOST_FILE, NULL,
        GENERIC_READ, FILE_SHARE_READ, 0, &status);
    if (handle == 0u) {
        CloseHandle(file);
        return 0u;
    }
    find_file_handle(handle)->read_filter = filter;
    return handle;
}

static int read_guest_ansi_string(uint32_t ansi_string_address, char *out, size_t out_size)
{
    if (ansi_string_address == 0u || out_size == 0u) {
        return 0;
    }
    uint16_t length = *(uint16_t *)recomp_memory_u32(ansi_string_address);
    uint32_t buffer = *recomp_memory_u32(ansi_string_address + 4u);
    if (buffer == 0u || length == 0u || length >= out_size) {
        return 0;
    }
    memcpy(out, (const void *)recomp_memory_u32(buffer), length);
    out[length] = '\0';
    return 1;
}

static int read_guest_object_name(uint32_t object_attributes, char *out,
    size_t out_size, uint32_t *root_directory)
{
    if (object_attributes == 0u) {
        return 0;
    }
    uint32_t compact_name = *recomp_memory_u32(object_attributes + 4u);
    if (compact_name != 0u && read_guest_ansi_string(compact_name, out, out_size)) {
        if (root_directory != NULL) {
            *root_directory = *recomp_memory_u32(object_attributes);
        }
        return 1;
    }
    uint32_t nt_name = *recomp_memory_u32(object_attributes + 8u);
    if (nt_name != 0u && read_guest_ansi_string(nt_name, out, out_size)) {
        if (root_directory != NULL) {
            *root_directory = *recomp_memory_u32(object_attributes + 4u);
        }
        return 1;
    }
    return 0;
}

static int append_segment(char *path, size_t path_size, const char *segment)
{
    size_t seg_len = strlen(segment);
    /* Win32 trims trailing dots/spaces, which can turn a name into "..". */
    if (seg_len == 0u || strchr(segment, ':') != NULL ||
        segment[seg_len - 1u] == '.' || segment[seg_len - 1u] == ' ') {
        return 0;
    }
    size_t len = strlen(path);
    if (len > 0 && path[len - 1] != '\\' && path[len - 1] != '/') {
        if (len + 1 >= path_size) {
            return 0;
        }
#ifdef _WIN32
        path[len++] = '\\';
#else
        path[len++] = '/';
#endif
        path[len] = '\0';
    }
    if (len + seg_len + 1 > path_size) {
        return 0;
    }
    memcpy(path + len, segment, seg_len + 1);
    return 1;
}

static int normalize_guest_path(const char *guest_path, char *body, size_t body_size)
{
    size_t i = 0;
    while (i + 1 < body_size && guest_path[i] != '\0') {
        char c = guest_path[i];
        body[i] = (c == '/') ? '\\' : (char)tolower((unsigned char)c);
        ++i;
    }
    body[i] = '\0';

    if (strncmp(body, "\\??\\", 4) == 0) {
        memmove(body, body + 4, strlen(body + 4) + 1);
    }
    if (body[0] == '\0' || body[1] != ':') {
        return 0;
    }
    char drive = (char)tolower((unsigned char)body[0]);
    if (drive != 'd' && drive != 'z') {
        return 0;
    }
    char *p = body + 2;
    while (*p == '\\') {
        ++p;
    }
    if (p != body + 2) {
        memmove(body + 2, p, strlen(p) + 1);
    }
    return 1;
}

static void copy_root(char *host_path, size_t host_path_size)
{
    strncpy(host_path, recomp_disc_root_path, host_path_size - 1u);
    host_path[host_path_size - 1u] = '\0';
    size_t len = strlen(host_path);
    if (len > 1 && (host_path[len - 1] == '\\' || host_path[len - 1] == '/')) {
        host_path[len - 1] = '\0';
    }
}

static int append_relative_path(const char *relative, char *host_path, size_t host_path_size)
{
    char segment[MAX_SEGMENT_LEN];
    size_t seg_i = 0;
    for (const char *p = relative; *p != '\0'; ++p) {
        if (*p == '\\' || *p == '/') {
            if (seg_i > 0) {
                segment[seg_i] = '\0';
                if (!append_segment(host_path, host_path_size, segment)) {
                    return 0;
                }
                seg_i = 0;
            }
        } else {
            if (seg_i >= sizeof(segment) - 1u) {
                return 0;
            }
            segment[seg_i++] = *p;
        }
    }
    if (seg_i > 0) {
        segment[seg_i] = '\0';
        if (!append_segment(host_path, host_path_size, segment)) {
            return 0;
        }
    }
    return strlen(host_path) > 0;
}

static int build_host_path(const char *relative, char *host_path, size_t host_path_size)
{
    copy_root(host_path, host_path_size);
    return append_relative_path(relative, host_path, host_path_size);
}

static bool is_profile_path(const char *path)
{
    char root[MAX_PATH_LEN];
    if (recomp_disc_root_path == NULL) {
        return false;
    }
    copy_root(root, sizeof root);
    if (!append_segment(root, sizeof root, ".recomp-storage") ||
        !append_segment(root, sizeof root, "partition1") ||
        !append_segment(root, sizeof root, "UDATA")) {
        return false;
    }
    size_t length = strlen(root);
    return _strnicmp(path, root, length) == 0 &&
        (path[length] == '\0' || path[length] == '\\' || path[length] == '/');
}

/* 0 is another device; -1 is a recognized save path with invalid components. */
static int build_save_path(
    const char *guest_path,
    char *host_path,
    size_t host_path_size)
{
    static const char prefix[] = "\\Device\\Harddisk0\\partition1";
    const size_t prefix_len = sizeof prefix - 1u;

    if (_strnicmp(guest_path, prefix, prefix_len) != 0 ||
        (guest_path[prefix_len] != '\0' &&
         guest_path[prefix_len] != '\\' &&
         guest_path[prefix_len] != '/')) {
        return 0;
    }

    copy_root(host_path, host_path_size);
    if (!append_segment(host_path, host_path_size, ".recomp-storage") ||
        !append_segment(host_path, host_path_size, "partition1")) {
        return -1;
    }

    const char *relative = guest_path + prefix_len;
    while (*relative == '\\' || *relative == '/') {
        ++relative;
    }

    return append_relative_path(relative, host_path, host_path_size) ? 1 : -1;
}

static int is_save_root_path(const char *guest_path)
{
    static const char prefix[] = "\\Device\\Harddisk0\\partition1";
    const size_t prefix_len = sizeof prefix - 1u;

    if (_strnicmp(guest_path, prefix, prefix_len) != 0) {
        return 0;
    }
    const char *relative = guest_path + prefix_len;
    while (*relative == '\\' || *relative == '/') {
        ++relative;
    }
    return *relative == '\0';
}

static int build_raw_partition_path(
    const char *guest_path,
    char *host_path,
    size_t host_path_size)
{
    static const char partition0[] = "\\Device\\Harddisk0\\partition0";

    if (_stricmp(guest_path, partition0) != 0) {
        return 0;
    }
    copy_root(host_path, host_path_size);
    return append_segment(host_path, host_path_size, ".recomp-storage") &&
        append_segment(host_path, host_path_size, "partition0");
}

static int create_directory_tree(const char *path)
{
    char current[MAX_PATH_LEN];
    size_t length = strlen(path);
    if (length == 0u || length >= sizeof current) {
        return 0;
    }
    memcpy(current, path, length + 1u);
    DWORD existing = GetFileAttributesA(current);
    if (existing != INVALID_FILE_ATTRIBUTES &&
        (existing & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return 1;
    }

    char *p = current;
    while (*p == '/' || *p == '\\') ++p;
#ifdef _WIN32
    if (p == current && current[0] != '\0' && current[1] == ':') {
        p = current + 2;
        while (*p == '/' || *p == '\\') ++p;
    }
#endif
    for (; *p != '\0'; ++p) {
        if (*p != '\\' && *p != '/') {
            continue;
        }
        char separator = *p;
        *p = '\0';
        if (strlen(current) > 0 && !CreateDirectoryA(current, NULL) &&
            GetLastError() != ERROR_ALREADY_EXISTS) {
            return 0;
        }
        *p = separator;
    }
    if (!CreateDirectoryA(current, NULL) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        return 0;
    }
    DWORD attributes = GetFileAttributesA(current);
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static int create_parent_directories(const char *path)
{
    char parent[MAX_PATH_LEN];
    size_t length = strlen(path);
    if (length == 0u || length >= sizeof parent) {
        return 0;
    }
    memcpy(parent, path, length + 1u);
    char *last_separator = strrchr(parent, '\\');
    char *last_slash = strrchr(parent, '/');
    if (last_slash && (!last_separator || last_slash > last_separator)) {
        last_separator = last_slash;
    }
    if (last_separator == NULL) {
        return 0;
    }
    *last_separator = '\0';
    return create_directory_tree(parent);
}

/* Report why a host open or create failed; only a missing path is "not found". */
static uint32_t host_error_status(DWORD error)
{
    switch (error) {
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS: return 0xc0000035u; /* STATUS_OBJECT_NAME_COLLISION */
    case ERROR_SHARING_VIOLATION: return 0xc0000043u;
    case ERROR_ACCESS_DENIED: return 0xc0000022u;
    default: return RECOMP_STATUS_OBJECT_NAME_NOT_FOUND;
    }
}

static HANDLE create_host_file(
    const char *path,
    uint32_t desired_access,
    uint32_t share_access,
    uint32_t create_disposition)
{
    DWORD host_access = 0u;
    DWORD host_share = 0u;
    DWORD host_disposition;

    if ((desired_access & 0x80000000u) != 0u) {
        host_access |= GENERIC_READ;
    }
    if ((desired_access & 0x40000000u) != 0u) {
        host_access |= GENERIC_WRITE;
    }
    if ((desired_access & 0x00010000u) != 0u && is_profile_path(path)) {
        host_access |= DELETE;
    }
    if ((desired_access & 0x00000100u) != 0u && is_profile_path(path)) {
        host_access |= FILE_WRITE_ATTRIBUTES;
    }
    if (host_access == 0u) {
        host_access = GENERIC_READ;
    }
    if ((share_access & 1u) != 0u || is_profile_path(path)) {
        host_share |= FILE_SHARE_READ;
    }
    if ((share_access & 2u) != 0u) {
        host_share |= FILE_SHARE_WRITE;
    }
    if ((share_access & 4u) != 0u && is_profile_path(path)) {
        host_share |= FILE_SHARE_DELETE;
    }

    switch (create_disposition) {
    case 0u: host_disposition = CREATE_ALWAYS; break;
    case 1u: host_disposition = OPEN_EXISTING; break;
    case 2u: host_disposition = CREATE_NEW; break;
    case 3u: host_disposition = OPEN_ALWAYS; break;
    case 4u: host_disposition = TRUNCATE_EXISTING; break;
    case 5u: host_disposition = CREATE_ALWAYS; break;
    default: return INVALID_HANDLE_VALUE;
    }

    return CreateFileA(
        path,
        host_access,
        host_share,
        NULL,
        host_disposition,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
}

static int open_raw_partition(const char *host_path, HANDLE *out_handle)
{
    const LONGLONG MINIMUM_RAW_PARTITION_SIZE = 0xa00;

    if (!create_parent_directories(host_path)) {
        return 0;
    }
    HANDLE handle = CreateFileA(
        host_path,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ,
        NULL,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        return 0;
    }

    LARGE_INTEGER size;
    if (!GetFileSizeEx(handle, &size)) {
        CloseHandle(handle);
        return 0;
    }
    if (size.QuadPart < MINIMUM_RAW_PARTITION_SIZE) {
        LARGE_INTEGER end;
        end.QuadPart = MINIMUM_RAW_PARTITION_SIZE;
        if (!SetFilePointerEx(handle, end, NULL, FILE_BEGIN) ||
            !SetEndOfFile(handle)) {
            CloseHandle(handle);
            return 0;
        }
    }
    *out_handle = handle;
    return 1;
}

static int find_only_child_directory(const char *root, char *child, size_t child_size)
{
    char pattern[MAX_PATH_LEN];
    snprintf(pattern, sizeof(pattern), "%s\\*", root);
    WIN32_FIND_DATAA data;
    HANDLE find = FindFirstFileA(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) {
        return 0;
    }
    int found = 0;
    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
            strcmp(data.cFileName, ".") != 0 &&
            strcmp(data.cFileName, "..") != 0 &&
            strcmp(data.cFileName, ".recomp-storage") != 0) {
            if (found) {
                FindClose(find);
                return 0;
            }
            strncpy(child, data.cFileName, child_size - 1u);
            child[child_size - 1u] = '\0';
            found = 1;
        }
    } while (FindNextFileA(find, &data));
    FindClose(find);
    return found;
}

static int try_open_host_file(
    char *host_path,
    size_t host_path_size,
    HANDLE *out_handle,
    uint32_t desired_access,
    uint32_t share_access,
    DWORD *open_error)
{
    const bool profile_path = is_profile_path(host_path);
    const bool metadata_open = profile_path &&
        (desired_access & (DELETE | FILE_WRITE_ATTRIBUTES)) != 0u;
    /* Preserve requested read rights without granting untracked data writes. */
    DWORD host_access = metadata_open
        ? desired_access & (GENERIC_READ | FILE_GENERIC_READ) : GENERIC_READ;
    DWORD host_share = profile_path ? (share_access & 7u) | FILE_SHARE_READ : FILE_SHARE_READ;

    if ((desired_access & DELETE) != 0u && profile_path) {
        host_access |= DELETE;
    }
    if ((desired_access & FILE_WRITE_ATTRIBUTES) != 0u && profile_path) {
        host_access |= FILE_WRITE_ATTRIBUTES;
    }

    *open_error = ERROR_SUCCESS;
    DWORD attributes = GetFileAttributesA(host_path);
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        HANDLE h = CreateFileA(
            host_path,
            host_access,
            host_share,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL);
        if (h != INVALID_HANDLE_VALUE) {
            *out_handle = h;
            return 1;
        }
        *open_error = GetLastError();
    }

    char child[MAX_SEGMENT_LEN];
    char root[MAX_PATH_LEN];
    copy_root(root, sizeof(root));
    if (!find_only_child_directory(root, child, sizeof(child))) {
        return 0;
    }

    char nested[MAX_PATH_LEN];
#ifdef _WIN32
    snprintf(nested, sizeof(nested), "%s\\%s", root, child);
#else
    snprintf(nested, sizeof(nested), "%s/%s", root, child);
#endif
    size_t nested_len = strlen(nested);
    size_t rel_offset = strlen(root);
    if (host_path[rel_offset] == '\\' || host_path[rel_offset] == '/') {
        ++rel_offset;
    }
    if (nested_len + 1 + strlen(host_path + rel_offset) >= sizeof(nested)) {
        return 0;
    }
#ifdef _WIN32
    nested[nested_len++] = '\\';
#else
    nested[nested_len++] = '/';
#endif
    strcpy(nested + nested_len, host_path + rel_offset);

    attributes = GetFileAttributesA(nested);
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        HANDLE h = CreateFileA(
            nested,
            host_access,
            host_share,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL);
        if (h != INVALID_HANDLE_VALUE) {
            strncpy(host_path, nested, host_path_size - 1u);
            host_path[host_path_size - 1u] = '\0';
            *out_handle = h;
            return 1;
        }
    }
    return 0;
}

static int try_open_host_directory(char *host_path, size_t host_path_size)
{
    DWORD attributes = GetFileAttributesA(host_path);
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return 1;
    }

    char child[MAX_SEGMENT_LEN];
    char root[MAX_PATH_LEN];
    copy_root(root, sizeof(root));
    if (!find_only_child_directory(root, child, sizeof(child))) {
        return 0;
    }

    char nested[MAX_PATH_LEN];
#ifdef _WIN32
    snprintf(nested, sizeof(nested), "%s\\%s", root, child);
#else
    snprintf(nested, sizeof(nested), "%s/%s", root, child);
#endif
    size_t nested_len = strlen(nested);
    size_t rel_offset = strlen(root);
    if (host_path[rel_offset] == '\\' || host_path[rel_offset] == '/') {
        ++rel_offset;
    }
    if (nested_len + 1 + strlen(host_path + rel_offset) >= sizeof(nested)) {
        return 0;
    }
#ifdef _WIN32
    nested[nested_len++] = '\\';
#else
    nested[nested_len++] = '/';
#endif
    strcpy(nested + nested_len, host_path + rel_offset);

    attributes = GetFileAttributesA(nested);
    if (attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        strncpy(host_path, nested, host_path_size - 1u);
        host_path[host_path_size - 1u] = '\0';
        return 1;
    }
    return 0;
}

static int host_directory_is_empty(const char *path)
{
    char pattern[MAX_PATH_LEN];
    WIN32_FIND_DATAA data;
    snprintf(pattern, sizeof pattern, "%s\\*", path);
    HANDLE find = FindFirstFileA(pattern, &data);
    if (find == INVALID_HANDLE_VALUE) return -1;
    int empty = 1;
    do {
        if (strcmp(data.cFileName, ".") != 0 &&
            strcmp(data.cFileName, "..") != 0) {
            empty = 0;
            break;
        }
    } while (FindNextFileA(find, &data));
    FindClose(find);
    return empty;
}

static bool path_is_delete_pending(const char *path)
{
    for (size_t i = 0; i < MAX_FILE_HANDLES; ++i) {
        const FileHandleEntry *entry = &file_handles[i];
        if (!entry->active || !entry->delete_on_close) continue;
        size_t length = strlen(entry->host_path);
        if (_strnicmp(path, entry->host_path, length) == 0 &&
            (path[length] == '\0' ||
             (entry->kind == FILE_HANDLE_DIRECTORY &&
              (path[length] == '\\' || path[length] == '/')))) {
            return true;
        }
    }
    return false;
}

static uint32_t build_directory_relative_path(
    uint32_t root_directory, const char *relative, char *host_path,
    int *out_is_writable)
{
    for (size_t i = 0; i < MAX_FILE_HANDLES; ++i) {
        const FileHandleEntry *entry = &file_handles[i];
        if (!entry->active || entry->guest_handle != root_directory) {
            continue;
        }
        if (entry->kind != FILE_HANDLE_DIRECTORY) {
            break;
        }
        strcpy(host_path, entry->host_path);
        if (!append_relative_path(relative, host_path, MAX_PATH_LEN)) {
            /* Keep the validated prefix for profile failure accounting. */
            return RECOMP_STATUS_INVALID_PARAMETER;
        }
        *out_is_writable = entry->is_writable;
        return RECOMP_STATUS_SUCCESS;
    }
    return RECOMP_STATUS_INVALID_HANDLE;
}

/* GENERIC_ALL implies every right tracked for profile paths. Disc paths keep
   their read-only meaning so they never become write sinks. */
static uint32_t profile_access(const char *host_path, uint32_t access)
{
    if ((access & 0x10000000u) != 0u && is_profile_path(host_path)) {
        access |= GENERIC_READ | GENERIC_WRITE | DELETE;
    }
    return access;
}

/* Mark a profile handle for deletion at close, as a disposition request would. */
static void request_profile_delete(
    FileHandleEntry *entry, uint32_t *status, const char **policy)
{
    const uint32_t STATUS_ACCESS_DENIED = 0xc0000022u;
    const uint32_t STATUS_CANNOT_DELETE = 0xc0000121u;
    const uint32_t STATUS_DIRECTORY_NOT_EMPTY = 0xc0000101u;
    DWORD attributes = GetFileAttributesA(entry->host_path);
    int empty = 1;

    if (attributes == INVALID_FILE_ATTRIBUTES) {
        *status = RECOMP_STATUS_OBJECT_NAME_NOT_FOUND;
        *policy = "profile-delete-path-missing";
        return;
    }
    if ((attributes & FILE_ATTRIBUTE_READONLY) != 0u) {
        *status = STATUS_CANNOT_DELETE;
        *policy = "profile-path-read-only";
        return;
    }
    if (entry->kind == FILE_HANDLE_DIRECTORY) empty = host_directory_is_empty(entry->host_path);
    if (empty == 0) {
        *status = STATUS_DIRECTORY_NOT_EMPTY;
        *policy = "profile-directory-not-empty";
    } else if (empty < 0) {
        *status = STATUS_ACCESS_DENIED;
        *policy = "profile-directory-enumeration-failed";
    } else {
        entry->delete_on_close = 1;
        entry->delete_owner = current_save_owner();
        *status = RECOMP_STATUS_SUCCESS;
        *policy = "profile-delete-pending";
    }
}

/* FILE_DELETE_ON_CLOSE marks a newly opened profile handle for deletion. A
   handle that cannot be marked is closed and the open fails. */
static void apply_delete_on_close_option(
    uint32_t *guest_handle, uint32_t options, uint32_t *status, const char **policy)
{
    if ((options & 0x00001000u) == 0u || *guest_handle == 0u ||
        *status != RECOMP_STATUS_SUCCESS) {
        return;
    }
    for (size_t i = 0; i < MAX_FILE_HANDLES; ++i) {
        FileHandleEntry *entry = &file_handles[i];
        if (!entry->active || entry->guest_handle != *guest_handle) continue;
        if (!is_profile_path(entry->host_path)) return;
        if (!entry->delete_access) {
            *status = RECOMP_STATUS_INVALID_PARAMETER;
            *policy = "delete-on-close-without-delete-access";
        } else {
            request_profile_delete(entry, status, policy);
        }
        if (*status != RECOMP_STATUS_SUCCESS) {
            if (entry->host_handle != INVALID_HANDLE_VALUE) CloseHandle(entry->host_handle);
            entry->active = 0;
            *guest_handle = 0u;
        }
        return;
    }
}

/* Resolve a guest object to a host file or directory. The caller registers
   the returned host handle or directory path as a guest handle. */
static const char *resolve_and_open(
    uint32_t object_attributes,
    char *guest_path,
    char *host_path,
    uint32_t desired_access,
    uint32_t share_access,
    HANDLE *out_host_handle,
    int *out_is_directory,
    int *out_is_writable,
    uint32_t *out_status)
{
    char normalized[MAX_PATH_LEN] = {0};
    char resolved_path[MAX_PATH_LEN] = {0};
    const char *path = guest_path;
    uint32_t root_directory = 0u;

    *out_host_handle = INVALID_HANDLE_VALUE;
    *out_is_directory = 0;
    *out_is_writable = 0;
    *out_status = RECOMP_STATUS_SUCCESS;
    host_path[0] = '\0';

    if (recomp_disc_root_path == NULL ||
        !read_guest_object_name(object_attributes, guest_path, MAX_PATH_LEN,
            &root_directory)) {
        return "pseudo-handle-open";
    }

    ensure_symbolic_links_initialized();
    if (recomp_symbolic_link_resolve_path(
            &symbolic_links, guest_path, resolved_path, sizeof resolved_path)) {
        static const char save_prefix[] =
            "\\Device\\Harddisk0\\partition1";
        const size_t save_prefix_length = sizeof save_prefix - 1u;

        if (_strnicmp(
                resolved_path, save_prefix, save_prefix_length) == 0 &&
            (resolved_path[save_prefix_length] == '\0' ||
             resolved_path[save_prefix_length] == '\\' ||
             resolved_path[save_prefix_length] == '/')) {
            path = resolved_path;
        }
    }

    if (build_raw_partition_path(path, host_path, MAX_PATH_LEN)) {
        *out_is_writable = 1;
        if (open_raw_partition(host_path, out_host_handle)) {
            return "host-raw-partition-open";
        }
        *out_status = RECOMP_STATUS_OBJECT_NAME_NOT_FOUND;
        return "host-raw-partition-open-failed";
    }

    const int save_path = build_save_path(path, host_path, MAX_PATH_LEN);
    /* Drive-qualified names can carry the special DOS-devices root handle. */
    if (root_directory != 0u && guest_path[0] != '\\' &&
        guest_path[0] != '/' && strchr(guest_path, ':') == NULL) {
        *out_status = build_directory_relative_path(
            root_directory, guest_path, host_path, out_is_writable);
        if (*out_status != RECOMP_STATUS_SUCCESS) {
            return "directory-relative-path-rejected";
        }
    } else if (save_path != 0) {
        if (save_path < 0) {
            *out_status = RECOMP_STATUS_INVALID_PARAMETER;
            return "invalid-save-path";
        }
        *out_is_writable = 1;
        if (is_save_root_path(path)) {
            (void)create_directory_tree(host_path);
        }
    } else {
        if (!normalize_guest_path(path, normalized, sizeof(normalized))) {
            return "pseudo-handle-open";
        }
        if (!build_host_path(normalized + 2, host_path, MAX_PATH_LEN)) {
            *out_status = RECOMP_STATUS_INVALID_PARAMETER;
            return "invalid-disc-path";
        }
    }

    if (path_is_delete_pending(host_path)) {
        /* Create/open must not retry or synthesize a handle for this path. */
        *out_is_writable = 0;
        *out_status = RECOMP_STATUS_DELETE_PENDING;
        return "profile-path-delete-pending";
    }
    DWORD open_error;
    if (try_open_host_file(host_path, MAX_PATH_LEN, out_host_handle,
            profile_access(host_path, desired_access), share_access, &open_error)) {
        return *out_is_writable ? "host-save-file-open" : "host-disc-file-open";
    }
    /* An existing profile file that refused this open is not missing. */
    if (is_profile_path(host_path) &&
        (open_error == ERROR_SHARING_VIOLATION || open_error == ERROR_ACCESS_DENIED)) {
        *out_status = host_error_status(open_error);
        return "host-save-file-open-rejected";
    }
    if (try_open_host_directory(host_path, MAX_PATH_LEN)) {
        *out_is_directory = 1;
        return *out_is_writable ? "host-save-directory-open" : "host-disc-directory-open";
    }
    *out_status = RECOMP_STATUS_OBJECT_NAME_NOT_FOUND;
    return *out_is_writable ? "host-save-path-open-failed" : "host-disc-file-open-failed";
}

static void bridge_nt_open_file(void)
{
    uint32_t file_handle_ptr = kernel_arg(1u);
    uint32_t desired_access = kernel_arg(2u);
    uint32_t share_access = kernel_arg(5u);
    uint32_t object_attributes = kernel_arg(3u);
    uint32_t io_status_block = kernel_arg(4u);
    uint32_t open_options = kernel_arg(6u);

    uint32_t status;
    uint32_t guest_handle = 1u;
    const char *policy;
    int is_directory;
    int is_writable;
    HANDLE host_handle;

    char guest_path[MAX_PATH_LEN] = {0};
    char host_path[MAX_PATH_LEN] = {0};

    policy = resolve_and_open(
        object_attributes, guest_path, host_path, desired_access, share_access,
        &host_handle, &is_directory, &is_writable, &status);
    uint32_t save_owner = current_save_owner();
    desired_access = profile_access(host_path, desired_access);
    bool requested_write = (desired_access & 0x40000000u) != 0u;
    bool profile_path = is_profile_path(host_path);
    bool required_save_io = requested_write ||
        (profile_path && (desired_access & (DELETE | FILE_WRITE_ATTRIBUTES)) != 0u);
    if (required_save_io && recomp_save_pending() &&
        ((profile_path && !recomp_save_active(save_owner)) ||
         (recomp_save_active(save_owner) && !profile_path))) {
        if (host_handle != INVALID_HANDLE_VALUE) CloseHandle(host_handle);
        recomp_stop(2, "save:unexpected-open-path-or-owner");
    }
    if (requested_write && is_writable && !is_directory) {
        if (host_handle != INVALID_HANDLE_VALUE) CloseHandle(host_handle);
        host_handle = create_host_file(host_path, desired_access, share_access, 1u);
        status = host_handle != INVALID_HANDLE_VALUE
            ? RECOMP_STATUS_SUCCESS : host_error_status(GetLastError());
        policy = host_handle != INVALID_HANDLE_VALUE
            ? "host-save-file-open" : "host-save-file-open-failed";
    }

    if (host_handle != INVALID_HANDLE_VALUE) {
        guest_handle = register_file_handle(
            host_handle, FILE_HANDLE_HOST_FILE, host_path,
            desired_access, share_access, is_writable, &status);
        if (guest_handle == 0u) {
            CloseHandle(host_handle);
        }
    } else if (is_directory) {
        guest_handle = register_file_handle(
            INVALID_HANDLE_VALUE, FILE_HANDLE_DIRECTORY, host_path,
            desired_access, share_access, is_writable, &status);
    } else if (status != RECOMP_STATUS_SUCCESS) {
        guest_handle = 0u;
    } else {
        guest_handle = register_file_handle(
            INVALID_HANDLE_VALUE, FILE_HANDLE_PSEUDO, NULL, desired_access, share_access, 0, &status);
    }
    apply_delete_on_close_option(&guest_handle, open_options, &status, &policy);

    if (required_save_io && status != RECOMP_STATUS_SUCCESS) {
        recomp_save_note_failure(save_owner);
    }

    fprintf(
        stderr,
        "recomp kernel: NtOpenFile path='%s' host='%s' policy='%s' handle=%u status=0x%08x\n",
        guest_path,
        host_path,
        policy,
        (unsigned)guest_handle,
        (unsigned)status);

    if (file_handle_ptr != 0u) {
        *recomp_memory_u32(file_handle_ptr) = guest_handle;
    }
    if (io_status_block != 0u) {
        *recomp_memory_u32(io_status_block) = status;
        *recomp_memory_u32(io_status_block + 4u) = 0u;
    }

    kernel_return(6u, status);
}

/* NtCreateFile is NtOpenFile with a create disposition and a write path.
   Partition1 paths use persistent host storage next to the disc root; disc
   paths remain read-only and use the synthetic write sink. */
static void bridge_nt_create_file(void)
{
    uint32_t file_handle_ptr = kernel_arg(1u);
    uint32_t desired_access = kernel_arg(2u);
    uint32_t object_attributes = kernel_arg(3u);
    uint32_t io_status_block = kernel_arg(4u);
    uint32_t share_access = kernel_arg(7u);
    uint32_t create_disposition = kernel_arg(8u);
    uint32_t create_options = kernel_arg(9u);

    const uint32_t FILE_OPEN_DISPOSITION = 1u;
    const uint32_t FILE_OPEN_IF_DISPOSITION = 3u;
    const uint32_t GENERIC_WRITE_ACCESS = 0x40000000u;

    uint32_t status;
    uint32_t guest_handle = 1u;
    const char *policy;
    int is_directory;
    int is_writable;
    HANDLE host_handle;

    char guest_path[MAX_PATH_LEN] = {0};
    char host_path[MAX_PATH_LEN] = {0};

    policy = resolve_and_open(
        object_attributes, guest_path, host_path, desired_access, share_access,
        &host_handle, &is_directory, &is_writable, &status);

    uint32_t save_owner = current_save_owner();
    bool profile_path = is_profile_path(host_path);
    desired_access = profile_access(host_path, desired_access);
    bool mutation = (desired_access & GENERIC_WRITE_ACCESS) != 0u ||
        (profile_path && (desired_access & (DELETE | FILE_WRITE_ATTRIBUTES)) != 0u) ||
        create_disposition != FILE_OPEN_DISPOSITION;
    if (mutation && recomp_save_pending() &&
        ((profile_path && !recomp_save_active(save_owner)) ||
         (recomp_save_active(save_owner) && !profile_path))) {
        if (host_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(host_handle);
        }
        recomp_stop(2, "save:unexpected-write-path-or-owner");
    }

    if (profile_path && (create_options & 0x00001000u) != 0u &&
        (desired_access & DELETE) == 0u) {
        /* Reject before any create or truncate can touch the file. */
        if (host_handle != INVALID_HANDLE_VALUE) CloseHandle(host_handle);
        status = RECOMP_STATUS_INVALID_PARAMETER;
        guest_handle = 0u;
        policy = "delete-on-close-without-delete-access";
    } else if (is_writable && (create_options & 1u) != 0u) {
        if (host_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(host_handle);
        }
        DWORD attributes = GetFileAttributesA(host_path);
        if (create_disposition != FILE_OPEN_DISPOSITION &&
            attributes == INVALID_FILE_ATTRIBUTES &&
            create_directory_tree(host_path)) {
            attributes = GetFileAttributesA(host_path);
        }
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            status = RECOMP_STATUS_SUCCESS;
            guest_handle = register_file_handle(
                INVALID_HANDLE_VALUE, FILE_HANDLE_DIRECTORY, host_path,
                desired_access, share_access, is_writable, &status);
            policy = "host-save-directory-open";
        } else if (attributes != INVALID_FILE_ATTRIBUTES) {
            status = 0xc0000103u; /* STATUS_NOT_A_DIRECTORY */
            guest_handle = 0u;
            policy = "host-save-path-not-directory";
        } else {
            status = RECOMP_STATUS_OBJECT_NAME_NOT_FOUND;
            guest_handle = 0u;
            policy = "host-save-directory-open-failed";
        }
    } else if (is_writable && ((desired_access & GENERIC_WRITE_ACCESS) != 0u ||
               (create_disposition != FILE_OPEN_DISPOSITION &&
                (status == RECOMP_STATUS_OBJECT_NAME_NOT_FOUND ||
                 (host_handle != INVALID_HANDLE_VALUE &&
                  create_disposition != FILE_OPEN_IF_DISPOSITION))))) {
        /* Writers, creates of a missing save path, and create, supersede,
           or overwrite of an existing one all need the host disposition. */
        if (host_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(host_handle);
        }
        host_handle = INVALID_HANDLE_VALUE;
        DWORD create_error = ERROR_PATH_NOT_FOUND;
        /* Creating or replacing a file is a mutation even for a read handle. */
        const bool replaces = GetFileAttributesA(host_path) == INVALID_FILE_ATTRIBUTES ||
            create_disposition == 0u || create_disposition == 4u || create_disposition == 5u;
        /* Check guest sharing before a create can truncate or replace the file. */
        if (profile_path && profile_sharing_conflict(host_path, desired_access, share_access)) {
            create_error = ERROR_SHARING_VIOLATION;
        } else if (create_parent_directories(host_path)) {
            host_handle = create_host_file(
                host_path, desired_access, share_access, create_disposition);
            if (host_handle == INVALID_HANDLE_VALUE) create_error = GetLastError();
        }
        if (host_handle != INVALID_HANDLE_VALUE) {
            status = RECOMP_STATUS_SUCCESS;
            guest_handle = register_file_handle(
                host_handle, FILE_HANDLE_HOST_FILE, host_path,
                desired_access, share_access, is_writable, &status);
            policy = "host-save-file-open";
            if (guest_handle == 0u) {
                CloseHandle(host_handle);
            } else if (profile_path && replaces) {
                FileHandleEntry *entry = find_file_handle(guest_handle);
                entry->save_owned = 1;
                entry->save_owner = save_owner;
            }
        } else {
            status = host_error_status(create_error);
            guest_handle = 0u;
            policy = "host-save-file-open-failed";
        }
    } else if (host_handle == INVALID_HANDLE_VALUE && !is_directory &&
        status != RECOMP_STATUS_SUCCESS) {
        /* Path did not resolve. The disposition decides whether that is an
           error (FILE_OPEN) or a pseudo-handle create (the rest). */
        if (status != RECOMP_STATUS_OBJECT_NAME_NOT_FOUND ||
            create_disposition == FILE_OPEN_DISPOSITION) {
            guest_handle = 0u;
            policy = "pseudo-missing-file-open";
        } else {
            status = RECOMP_STATUS_SUCCESS;
            guest_handle = register_file_handle(
                INVALID_HANDLE_VALUE, FILE_HANDLE_PSEUDO, NULL, desired_access, share_access, 0, &status);
            policy = "pseudo-handle-create";
        }
    } else if ((desired_access & GENERIC_WRITE_ACCESS) != 0u) {
        /* A resolved path opened for write becomes a synthetic sink: reads
           return nothing, writes are dropped. */
        if (host_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(host_handle);
        }
        guest_handle = register_file_handle(
            INVALID_HANDLE_VALUE, FILE_HANDLE_PSEUDO, NULL, desired_access, share_access, 0, &status);
        if (guest_handle != 0u) policy = "synthetic-disc-write-sink";
    } else if (host_handle != INVALID_HANDLE_VALUE) {
        guest_handle = register_file_handle(
            host_handle, FILE_HANDLE_HOST_FILE, host_path,
            desired_access, share_access, is_writable, &status);
        if (guest_handle == 0u) {
            CloseHandle(host_handle);
        }
    } else if (is_directory) {
        guest_handle = register_file_handle(
            INVALID_HANDLE_VALUE, FILE_HANDLE_DIRECTORY, host_path,
            desired_access, share_access, is_writable, &status);
    }
    apply_delete_on_close_option(&guest_handle, create_options, &status, &policy);

    if (mutation && recomp_save_active(save_owner) &&
        (status != RECOMP_STATUS_SUCCESS || guest_handle == 0u)) {
        recomp_save_note_failure(save_owner);
    }

    fprintf(
        stderr,
        "recomp kernel: NtCreateFile path='%s' host='%s' policy='%s' handle=%u status=0x%08x\n",
        guest_path,
        host_path,
        policy,
        (unsigned)guest_handle,
        (unsigned)status);

    if (file_handle_ptr != 0u) {
        *recomp_memory_u32(file_handle_ptr) = guest_handle;
    }
    if (io_status_block != 0u) {
        *recomp_memory_u32(io_status_block) = status;
        *recomp_memory_u32(io_status_block + 4u) = 0u;
    }

    kernel_return(9u, status);
}

static void bridge_nt_query_information_file(void)
{
    uint32_t guest_handle = kernel_arg(1u);
    uint32_t io_status_block = kernel_arg(2u);
    uint32_t file_information = kernel_arg(3u);
    uint32_t length = kernel_arg(4u);
    uint32_t file_information_class = kernel_arg(5u);

    const uint32_t FILE_STANDARD_INFORMATION = 5u;
    const uint32_t FILE_NETWORK_OPEN_INFORMATION = 0x22u;
    const uint32_t FILE_BASIC_INFORMATION = 4u;
    const uint32_t FILE_POSITION_INFORMATION = 14u;
    uint32_t status = RECOMP_STATUS_SUCCESS;
    uint64_t file_size = 0u;
    const char *policy = "zero-filled-pseudo-file-information";

    if (file_information != 0u && length != 0u) {
        memset((void *)recomp_memory_i8(file_information), 0, length);
    }

    for (size_t i = 0; i < MAX_FILE_HANDLES; ++i) {
        if (!file_handles[i].active ||
            file_handles[i].guest_handle != guest_handle) {
            continue;
        }

        if (file_information_class == FILE_POSITION_INFORMATION) {
            if (file_information == 0u || length < 8u) {
                status = 0xc0000004u; /* STATUS_INFO_LENGTH_MISMATCH */
                policy = "position-length-mismatch";
            } else {
                *recomp_memory_u32(file_information) = (uint32_t)file_handles[i].cursor;
                *recomp_memory_u32(file_information + 4u) =
                    (uint32_t)(file_handles[i].cursor >> 32u);
                policy = "file-position-information";
            }
            break;
        }

        if (file_handles[i].kind == FILE_HANDLE_PSEUDO) {
            break;
        }

        const FileHandleEntry *entry = &file_handles[i];
        const bool directory = entry->kind == FILE_HANDLE_DIRECTORY;
        /* Profile metadata can change through NtSetInformationFile, so report
           the host's, read through an attribute-only handle that sharing
           modes never block. Disc files keep the frozen host's answers. */
        FILE_BASIC_INFO host = {0};
        bool profile_metadata = false;
        if (is_profile_path(entry->host_path)) {
            HANDLE query = CreateFileA(entry->host_path, FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
            if (query != INVALID_HANDLE_VALUE) {
                profile_metadata = GetFileInformationByHandleEx(
                    query, FileBasicInfo, &host, sizeof host) != 0;
                CloseHandle(query);
            }
        }
        LARGE_INTEGER size = {0};
        if ((directory && !profile_metadata) ||
            (!directory && (entry->host_handle == INVALID_HANDLE_VALUE ||
                            !GetFileSizeEx(entry->host_handle, &size)))) {
            status = RECOMP_STATUS_INVALID_HANDLE;
            policy = "host-file-information-failed";
            break;
        }
        if (profile_metadata && file_information_class == FILE_BASIC_INFORMATION &&
            length < 0x28u) {
            status = 0xc0000004u; /* STATUS_INFO_LENGTH_MISMATCH */
            policy = "basic-information-length-mismatch";
            break;
        }

        file_size = (uint64_t)size.QuadPart;
        policy = profile_metadata ? "host-profile-file-information" : "host-file-information";
        const uint32_t attributes = profile_metadata
            ? host.FileAttributes : FILE_ATTRIBUTE_NORMAL;
        if (profile_metadata && file_information != 0u &&
            ((file_information_class == FILE_BASIC_INFORMATION && length >= 0x28u) ||
             (file_information_class == FILE_NETWORK_OPEN_INFORMATION && length >= 0x38u))) {
            const LARGE_INTEGER times[4] = {host.CreationTime, host.LastAccessTime,
                host.LastWriteTime, host.ChangeTime};
            for (uint32_t t = 0u; t < 4u; ++t) {
                *recomp_memory_u32(file_information + t * 8u) = times[t].LowPart;
                *recomp_memory_u32(file_information + t * 8u + 4u) = (uint32_t)times[t].HighPart;
            }
        }
        if (file_information_class == FILE_NETWORK_OPEN_INFORMATION &&
            file_information != 0u && length >= 0x38u) {
            *recomp_memory_u32(file_information + 0x20u) =
                (uint32_t)file_size;
            *recomp_memory_u32(file_information + 0x24u) =
                (uint32_t)(file_size >> 32u);
            *recomp_memory_u32(file_information + 0x28u) =
                (uint32_t)file_size;
            *recomp_memory_u32(file_information + 0x2cu) =
                (uint32_t)(file_size >> 32u);
            *recomp_memory_u32(file_information + 0x30u) =
                attributes;
        } else if (file_information_class == FILE_STANDARD_INFORMATION &&
                   file_information != 0u && length >= 0x16u) {
            *recomp_memory_u32(file_information + 0x00u) =
                (uint32_t)file_size;
            *recomp_memory_u32(file_information + 0x04u) =
                (uint32_t)(file_size >> 32u);
            *recomp_memory_u32(file_information + 0x08u) =
                (uint32_t)file_size;
            *recomp_memory_u32(file_information + 0x0cu) =
                (uint32_t)(file_size >> 32u);
            *recomp_memory_u32(file_information + 0x10u) = 1u;
            *recomp_memory_i8(file_information + 0x14u) = (int8_t)(entry->delete_on_close != 0);
            *recomp_memory_i8(file_information + 0x15u) = (int8_t)directory;
        } else if (file_information_class == FILE_BASIC_INFORMATION &&
                   file_information != 0u && length >= 0x28u && profile_metadata) {
            *recomp_memory_u32(file_information + 0x20u) = attributes;
        }
        break;
    }

    if (io_status_block != 0u) {
        *recomp_memory_u32(io_status_block) = status;
        *recomp_memory_u32(io_status_block + 4u) =
            status == RECOMP_STATUS_SUCCESS ? length : 0u;
    }

    fprintf(
        stderr,
        "recomp kernel: NtQueryInformationFile handle=%u class=%u len=%u"
        " size=%llu policy='%s' status=0x%08x\n",
        (unsigned)guest_handle,
        (unsigned)file_information_class,
        (unsigned)length,
        (unsigned long long)file_size,
        policy,
        (unsigned)status);

    kernel_return(5u, status);
}

static bool handle_profile_mutation_information(
    FileHandleEntry *entry,
    uint32_t file_information_class,
    uint32_t file_information,
    uint32_t length,
    bool *required_save_io,
    uint32_t *status,
    const char **policy)
{
    const uint32_t FILE_BASIC_INFORMATION = 4u;
    const uint32_t FILE_DISPOSITION_INFORMATION = 13u;
    const uint32_t STATUS_UNSUCCESSFUL = 0xc0000001u;
    const uint32_t STATUS_INFO_LENGTH_MISMATCH = 0xc0000004u;
    const uint32_t STATUS_ACCESS_DENIED = 0xc0000022u;

    /* Renaming or linking save files is not implemented; failing keeps a
       save from committing without the requested change. */
    if ((file_information_class == 10u || file_information_class == 11u) &&
        is_profile_path(entry->host_path)) {
        *required_save_io = true;
        *status = 0xc00000bbu; /* STATUS_NOT_SUPPORTED */
        *policy = "profile-rename-unsupported";
        return true;
    }
    if (file_information_class != FILE_BASIC_INFORMATION &&
        file_information_class != FILE_DISPOSITION_INFORMATION) {
        return false;
    }

    const bool profile_path = is_profile_path(entry->host_path);
    *required_save_io = entry->save_write || profile_path;
    if (profile_path && recomp_save_pending() &&
        (!recomp_save_active(current_save_owner()) ||
         (entry->save_owned && entry->save_owner != current_save_owner()))) {
        recomp_save_note_pending_failure();
        *status = STATUS_UNSUCCESSFUL;
        *policy = "save-profile-mutation-owner-rejected";
        return true;
    }
    if (*required_save_io && !profile_path &&
        !save_write_allowed(entry, current_save_owner())) {
        *status = STATUS_UNSUCCESSFUL;
        *policy = "save-write-owner-rejected";
        return true;
    }

    if (file_information_class == FILE_BASIC_INFORMATION) {
        if (length < 0x28u || file_information == 0u) {
            *status = STATUS_INFO_LENGTH_MISMATCH;
            *policy = "basic-information-length-mismatch";
        } else if (!profile_path) {
            *status = RECOMP_STATUS_SUCCESS;
            *policy = "basic-information-accepted-without-action";
        } else {
            FILE_BASIC_INFO info = {0};
            LARGE_INTEGER *times[4] = {&info.CreationTime, &info.LastAccessTime,
                &info.LastWriteTime, &info.ChangeTime};
            bool unchanged = true;
            for (uint32_t t = 0u; t < 4u; ++t) {
                times[t]->LowPart = *recomp_memory_u32(file_information + t * 8u);
                times[t]->HighPart = (LONG)*recomp_memory_u32(file_information + t * 8u + 4u);
                unchanged = unchanged && times[t]->QuadPart == 0;
            }
            info.FileAttributes = *recomp_memory_u32(file_information + 0x20u);
            unchanged = unchanged && info.FileAttributes == 0u;
            if (!entry->write_attributes) {
                *status = STATUS_ACCESS_DENIED;
                *policy = "profile-attributes-access-denied";
            } else if (unchanged) {
                *status = RECOMP_STATUS_SUCCESS;
                *policy = "basic-information-attributes-unchanged";
            } else {
                /* Attribute-only access never conflicts with sharing modes. */
                HANDLE host = CreateFileA(entry->host_path, FILE_WRITE_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
                const bool set = host != INVALID_HANDLE_VALUE &&
                    SetFileInformationByHandle(host, FileBasicInfo, &info, sizeof info);
                if (host != INVALID_HANDLE_VALUE) CloseHandle(host);
                *status = set ? RECOMP_STATUS_SUCCESS : STATUS_ACCESS_DENIED;
                *policy = set ? "profile-basic-information-set" : "profile-basic-information-set-failed";
            }
        }
        return true;
    }

    if (!profile_path) {
        *status = RECOMP_STATUS_SUCCESS;
        *policy = "class-accepted-without-action";
    } else if (file_information == 0u || length < 1u) {
        *status = STATUS_INFO_LENGTH_MISMATCH;
        *policy = "disposition-length-mismatch";
    } else if (!entry->delete_access) {
        *status = STATUS_ACCESS_DENIED;
        *policy = "profile-delete-access-denied";
    } else if (*recomp_memory_i8(file_information) == 0) {
        entry->delete_on_close = 0;
        entry->delete_owner = 0u;
        *status = RECOMP_STATUS_SUCCESS;
        *policy = "profile-delete-cancelled";
    } else {
        request_profile_delete(entry, status, policy);
    }
    return true;
}

/* NtSetInformationFile(FileHandle, IoStatusBlock, FileInformation, Length,
   FileInformationClass) is stdcall with five arguments. Position and EOF
   updates affect subsequent reads and writes. Other unhandled classes preserve
   the frozen host's successful no-op behavior. */
static void bridge_nt_set_information_file(void)
{
    const uint32_t FILE_POSITION_INFORMATION = 14u;
    const uint32_t FILE_END_OF_FILE_INFORMATION = 20u;
    const uint32_t FILE_ALLOCATION_INFORMATION = 19u;
    uint32_t guest_handle = kernel_arg(1u);
    uint32_t io_status_block = kernel_arg(2u);
    uint32_t file_information = kernel_arg(3u);
    uint32_t length = kernel_arg(4u);
    uint32_t file_information_class = kernel_arg(5u);

    uint32_t status = RECOMP_STATUS_INVALID_HANDLE;
    uint64_t value = 0u;
    /* A mutation on a bad handle still fails the save it belongs to. */
    bool required_save_io = file_information_class == 4u ||
        file_information_class == 10u || file_information_class == 11u ||
        file_information_class == 13u ||
        file_information_class == FILE_END_OF_FILE_INFORMATION;
    const char *policy = "invalid-handle";

    if (file_information != 0u && length >= 8u) {
        value = (uint64_t)*recomp_memory_u32(file_information) |
                ((uint64_t)*recomp_memory_u32(file_information + 4u) << 32u);
    }

    for (size_t i = 0; i < MAX_FILE_HANDLES; ++i) {
        if (!file_handles[i].active ||
            file_handles[i].guest_handle != guest_handle) {
            continue;
        }

        if (handle_profile_mutation_information(
                &file_handles[i], file_information_class, file_information,
                length, &required_save_io, &status, &policy)) {
            break;
        }

        required_save_io = file_handles[i].save_write ||
            file_information_class == FILE_END_OF_FILE_INFORMATION;
        if (required_save_io &&
            !save_write_allowed(&file_handles[i], current_save_owner())) {
            status = 0xc0000001u;
            policy = "save-write-owner-rejected";
            break;
        }

        if (file_handles[i].kind == FILE_HANDLE_PSEUDO) {
            status = RECOMP_STATUS_SUCCESS;
            policy = "pseudo-handle-accepted";
            break;
        }

        if (file_information_class == FILE_POSITION_INFORMATION) {
            if (length < 8u || file_information == 0u) {
                status = 0xc0000004u; /* STATUS_INFO_LENGTH_MISMATCH */
                policy = "position-length-mismatch";
                break;
            }
            /* The cursor this runtime keeps is the one NtReadFile and
               NtWriteFile use when the guest passes no explicit offset, so
               moving it here is the whole point of the call. */
            file_handles[i].cursor = value;
            status = RECOMP_STATUS_SUCCESS;
            policy = "host-file-position-set";
            break;
        }

        if (file_information_class == FILE_END_OF_FILE_INFORMATION) {
            LARGE_INTEGER move;
            HANDLE host = file_handles[i].host_handle;

            if (length < 8u || file_information == 0u) {
                status = 0xc0000004u;
                policy = "end-of-file-length-mismatch";
                break;
            }
            if (host == INVALID_HANDLE_VALUE) {
                status = RECOMP_STATUS_INVALID_HANDLE;
                policy = "end-of-file-no-host-handle";
                break;
            }
            move.QuadPart = (LONGLONG)value;
            if (!SetFilePointerEx(host, move, NULL, FILE_BEGIN) ||
                !SetEndOfFile(host)) {
                status = 0xc0000022u; /* STATUS_ACCESS_DENIED */
                policy = "host-file-truncate-failed";
                break;
            }
            status = RECOMP_STATUS_SUCCESS;
            policy = "host-file-end-of-file-set";
            break;
        }

        /* Allocation size is a hint on a host filesystem that grows files on
           demand, so accepting it without acting is faithful, not a stub. */
        status = RECOMP_STATUS_SUCCESS;
        policy = file_information_class == FILE_ALLOCATION_INFORMATION
            ? "allocation-hint-accepted"
            : "class-accepted-without-action";
        break;
    }

    if (required_save_io && status != RECOMP_STATUS_SUCCESS) {
        recomp_save_note_failure(current_save_owner());
    }

    if (io_status_block != 0u) {
        *recomp_memory_u32(io_status_block) = status;
        *recomp_memory_u32(io_status_block + 4u) =
            status == RECOMP_STATUS_SUCCESS ? length : 0u;
    }

    fprintf(
        stderr,
        "recomp kernel: NtSetInformationFile handle=%u class=%u len=%u"
        " value=%llu policy='%s' status=0x%08x\n",
        (unsigned)guest_handle,
        (unsigned)file_information_class,
        (unsigned)length,
        (unsigned long long)value,
        policy,
        (unsigned)status);

    kernel_return(5u, status);
}

static void bridge_nt_query_directory_file(void)

{
    const uint32_t RECOMP_STATUS_NO_MORE_FILES = 0x80000006u;
    const uint32_t FILE_DIRECTORY_INFORMATION = 1u;
    const uint32_t DIRECTORY_HEADER_SIZE = 0x40u;
    uint32_t guest_handle = kernel_arg(1u);
    uint32_t io_status_block = kernel_arg(5u);
    uint32_t file_information = kernel_arg(6u);
    uint32_t length = kernel_arg(7u);
    uint32_t file_information_class = kernel_arg(8u);
    uint32_t file_name = kernel_arg(9u);
    /* Xbox BOOLEAN occupies only the low byte of its stack argument. */
    uint32_t restart_scan = (uint8_t)kernel_arg(10u);
    uint32_t status = RECOMP_STATUS_INVALID_HANDLE;
    uint32_t bytes_written = 0u;
    size_t cursor = 0u;
    const char *policy = "invalid-directory-handle";
    char pattern[RECOMP_DIRECTORY_NAME_SIZE] = {0};

    if (file_information != 0u && length != 0u) {
        memset(recomp_memory_i8(file_information), 0, length);
    }
    if (file_name != 0u) {
        uint16_t pattern_length = *recomp_memory_u16(file_name);
        uint32_t pattern_buffer = *recomp_memory_u32(file_name + 4u);

        if (pattern_length >= sizeof pattern ||
            (pattern_length != 0u && pattern_buffer == 0u)) {
            status = RECOMP_STATUS_INVALID_PARAMETER;
            policy = "invalid-directory-pattern";
            goto finish;
        }
        if (pattern_length != 0u) {
            memcpy(pattern, recomp_memory_i8(pattern_buffer), pattern_length);
            pattern[pattern_length] = '\0';
        }
    }
    if (file_information == 0u || length < DIRECTORY_HEADER_SIZE + 1u ||
        file_information_class != FILE_DIRECTORY_INFORMATION) {
        status = RECOMP_STATUS_INVALID_PARAMETER;
        policy = "unsupported-directory-query";
        goto finish;
    }

    for (size_t i = 0u; i < MAX_FILE_HANDLES; ++i) {
        RecompDirectoryEntry entry;
        size_t name_length;

        if (!file_handles[i].active ||
            file_handles[i].guest_handle != guest_handle) {
            continue;
        }
        if (file_handles[i].kind != FILE_HANDLE_DIRECTORY) {
            break;
        }
        if (restart_scan != 0u) {
            recomp_directory_restart(&file_handles[i].directory);
        }
        if (!recomp_directory_next(
                &file_handles[i].directory, pattern, &entry)) {
            status = RECOMP_STATUS_NO_MORE_FILES;
            policy = "host-directory-exhausted";
            cursor = file_handles[i].directory.cursor;
            break;
        }
        name_length = strlen(entry.name);
        if (name_length + DIRECTORY_HEADER_SIZE + 1u > length) {
            status = RECOMP_STATUS_INVALID_PARAMETER;
            policy = "directory-entry-buffer-too-small";
            cursor = file_handles[i].directory.cursor;
            break;
        }

        {
            size_t serialized_size = 0u;

            if (!recomp_directory_serialize(
                    &entry,
                    recomp_memory_i8(file_information),
                    length,
                    &serialized_size)) {
                status = RECOMP_STATUS_INVALID_PARAMETER;
                policy = "directory-entry-serialize-failed";
                cursor = file_handles[i].directory.cursor;
                break;
            }
            bytes_written = (uint32_t)serialized_size;
        }
        status = RECOMP_STATUS_SUCCESS;
        policy = "host-directory-entry";
        cursor = file_handles[i].directory.cursor;
        break;
    }

finish:
    if (io_status_block != 0u) {
        *recomp_memory_u32(io_status_block) = status;
        *recomp_memory_u32(io_status_block + 4u) = bytes_written;
    }
    fprintf(
        stderr,
        "recomp kernel: NtQueryDirectoryFile handle=%u class=%u len=%u"
        " restart=%u cursor=%zu policy='%s' status=0x%08x\n",
        (unsigned)guest_handle,
        (unsigned)file_information_class,
        (unsigned)length,
        (unsigned)restart_scan,
        cursor,
        policy,
        (unsigned)status);
    kernel_return(10u, status);
}

static void bridge_nt_write_file(void)
{
    uint32_t guest_handle = kernel_arg(1u);
    uint32_t io_status_block = kernel_arg(5u);
    uint32_t buffer = kernel_arg(6u);
    uint32_t length = kernel_arg(7u);
    uint32_t byte_offset = kernel_arg(8u);

    const uint32_t RECOMP_STATUS_UNSUCCESSFUL = 0xc0000001u;
    uint32_t status = RECOMP_STATUS_INVALID_HANDLE;
    uint32_t bytes_written = 0u;
    uint64_t write_offset = 0u;
    const char *policy = "invalid-file-handle";
    int tracked_handle_seen = 0;
    bool interrupt_after_write = false;

    for (size_t i = 0; i < MAX_FILE_HANDLES; ++i) {
        if (!file_handles[i].active ||
            file_handles[i].guest_handle != guest_handle) {
            continue;
        }

        tracked_handle_seen = 1;
        if (!save_write_allowed(&file_handles[i], current_save_owner())) {
            status = RECOMP_STATUS_UNSUCCESSFUL;
            policy = "save-write-owner-rejected";
            break;
        }
        write_offset = file_handles[i].cursor;
        if (byte_offset != 0u) {
            write_offset = *recomp_memory_u32(byte_offset);
            write_offset |=
                (uint64_t)*recomp_memory_u32(byte_offset + 4u) << 32u;
        }

        if (file_handles[i].kind == FILE_HANDLE_PSEUDO) {
            bytes_written = length;
            file_handles[i].cursor = write_offset + bytes_written;
            status = RECOMP_STATUS_SUCCESS;
            policy = "pseudo-file-write-sink";
            break;
        }
        if (file_handles[i].kind == FILE_HANDLE_DIRECTORY ||
            file_handles[i].host_handle == INVALID_HANDLE_VALUE ||
            (buffer == 0u && length != 0u)) {
            break;
        }

        LARGE_INTEGER distance;
        distance.QuadPart = (LONGLONG)write_offset;
        if (!SetFilePointerEx(
                file_handles[i].host_handle, distance, NULL, FILE_BEGIN)) {
            status = RECOMP_STATUS_UNSUCCESSFUL;
            policy = "host-file-seek-failed";
            break;
        }

        const void *host_buffer = length == 0u
            ? NULL
            : (const void *)recomp_memory_i8(buffer);
        bool owned_write = length != 0u && file_handles[i].save_write &&
            recomp_save_active(current_save_owner());
        DWORD host_length = length;
        if (owned_write) {
            ++save_write_number;
            if (save_fault_write("RECOMP_SAVE_SHORT_WRITE_AT") == save_write_number) {
                host_length /= 2u;
            }
        }
        DWORD host_bytes_written = 0u;
        if (!WriteFile(
                file_handles[i].host_handle,
                host_buffer,
                host_length,
                &host_bytes_written,
                NULL)) {
            status = RECOMP_STATUS_UNSUCCESSFUL;
            policy = "host-file-write-failed";
            break;
        }

        bytes_written = host_bytes_written;
        file_handles[i].cursor = write_offset + bytes_written;
        status = bytes_written == length
            ? RECOMP_STATUS_SUCCESS : RECOMP_STATUS_UNSUCCESSFUL;
        policy = bytes_written == length ? "host-file-write" : "host-file-short-write";
        interrupt_after_write = owned_write && bytes_written == length &&
            save_fault_write("RECOMP_SAVE_INTERRUPT_AFTER_WRITE") == save_write_number;
        break;
    }

    if (!tracked_handle_seen) {
        policy = "untracked-file-handle";
    }

    if (status != RECOMP_STATUS_SUCCESS || bytes_written != length) {
        recomp_save_note_failure(current_save_owner());
    }

    if (io_status_block != 0u) {
        *recomp_memory_u32(io_status_block) = status;
        *recomp_memory_u32(io_status_block + 4u) = bytes_written;
    }

    fprintf(
        stderr,
        "recomp kernel: NtWriteFile handle=%u buffer=0x%08x len=%u"
        " offset=%llu written=%u policy='%s' status=0x%08x\n",
        (unsigned)guest_handle,
        (unsigned)buffer,
        (unsigned)length,
        (unsigned long long)write_offset,
        (unsigned)bytes_written,
        policy,
        (unsigned)status);

    if (interrupt_after_write) {
        fprintf(stderr, "recomp save fault: interrupt after write=%u\n", save_write_number);
        fflush(stderr);
        TerminateProcess(GetCurrentProcess(), 92u);
        abort();
    }

    kernel_return(8u, status);
}

static void bridge_nt_read_file(void)
{
    uint32_t guest_handle = kernel_arg(1u);
    uint32_t io_status_block = kernel_arg(5u);
    uint32_t buffer = kernel_arg(6u);
    uint32_t length = kernel_arg(7u);
    uint32_t byte_offset = kernel_arg(8u);

    const uint32_t RECOMP_STATUS_UNSUCCESSFUL = 0xc0000001u;
    const uint32_t RECOMP_STATUS_END_OF_FILE = 0xc0000011u;
    uint32_t status = RECOMP_STATUS_INVALID_HANDLE;
    uint32_t bytes_read = 0u;
    uint64_t read_offset = 0u;
    const char *policy = "invalid-file-handle";
    int tracked_handle_seen = 0;

    for (size_t i = 0; i < MAX_FILE_HANDLES; ++i) {
        if (!file_handles[i].active ||
            file_handles[i].guest_handle != guest_handle) {
            continue;
        }

        tracked_handle_seen = 1;
        read_offset = file_handles[i].cursor;
        if (byte_offset != 0u) {
            read_offset = *recomp_memory_u32(byte_offset);
            read_offset |=
                (uint64_t)*recomp_memory_u32(byte_offset + 4u) << 32u;
        }

        if (file_handles[i].kind == FILE_HANDLE_PSEUDO) {
            status = RECOMP_STATUS_END_OF_FILE;
            policy = "pseudo-empty-file-eof";
            break;
        }
        if (file_handles[i].kind == FILE_HANDLE_DIRECTORY ||
            file_handles[i].host_handle == INVALID_HANDLE_VALUE ||
            (buffer == 0u && length != 0u)) {
            break;
        }

        LARGE_INTEGER distance;
        distance.QuadPart = (LONGLONG)read_offset;
        if (!SetFilePointerEx(
                file_handles[i].host_handle, distance, NULL, FILE_BEGIN)) {
            status = RECOMP_STATUS_UNSUCCESSFUL;
            policy = "host-file-seek-failed";
            break;
        }

        void *host_buffer = length == 0u
            ? NULL
            : (void *)recomp_memory_i8(buffer);
        DWORD host_bytes_read = 0u;
        if (!ReadFile(
                file_handles[i].host_handle,
                host_buffer,
                length,
                &host_bytes_read,
                NULL)) {
            status = RECOMP_STATUS_UNSUCCESSFUL;
            policy = "host-file-read-failed";
            break;
        }

        bytes_read = host_bytes_read;
        if (file_handles[i].read_filter != NULL && bytes_read != 0u) {
            file_handles[i].read_filter(read_offset, host_buffer, bytes_read);
        }
        file_handles[i].cursor = read_offset + bytes_read;
        status = bytes_read == 0u && length != 0u
            ? RECOMP_STATUS_END_OF_FILE
            : RECOMP_STATUS_SUCCESS;
        policy = "host-file-read";
        break;
    }

    if (!tracked_handle_seen) {
        policy = "untracked-file-handle";
    }

    if (io_status_block != 0u) {
        *recomp_memory_u32(io_status_block) = status;
        *recomp_memory_u32(io_status_block + 4u) = bytes_read;
    }

    fprintf(
        stderr,
        "recomp kernel: NtReadFile handle=%u buffer=0x%08x len=%u"
        " offset=%llu read=%u policy='%s' status=0x%08x\n",
        (unsigned)guest_handle,
        (unsigned)buffer,
        (unsigned)length,
        (unsigned long long)read_offset,
        (unsigned)bytes_read,
        policy,
        (unsigned)status);

    kernel_return(8u, status);
}

/* A synchronous device query succeeds with an empty status block. The one
   IOCTL this XBE issues on the CD-ROM device is a query whose input the
   native host confirms by setting three bytes past the caller's buffer; that
   confirmation is what the guest reads back, so we reproduce it even though
   it lands beyond the declared input length. */
static void bridge_nt_fs_control_file(void)
{
    uint32_t handle = kernel_arg(1u);
    uint32_t iosb = kernel_arg(5u);
    uint32_t code = kernel_arg(6u);
    uint32_t status = RECOMP_STATUS_INVALID_HANDLE;
    for (unsigned i = 0u; i < MAX_FILE_HANDLES; ++i) {
        if (!file_handles[i].active || file_handles[i].guest_handle != handle) continue;
        status = 0xc0000010u;
        /* No block filesystem is mounted on the virtual cache device. */
        if (code == 0x00090020u && file_handles[i].kind == FILE_HANDLE_PSEUDO)
            status = RECOMP_STATUS_SUCCESS;
        break;
    }
    if (iosb != 0u) {
        *recomp_memory_u32(iosb) = status;
        *recomp_memory_u32(iosb + 4u) = 0u;
    }
    fprintf(stderr, "recomp kernel: NtFsControlFile handle=%u code=0x%08x status=0x%08x\n",
        (unsigned)handle, (unsigned)code, (unsigned)status);
    kernel_return(10u, status);
}

static void bridge_nt_device_io_control_file(void)
{
    uint32_t io_status_block = kernel_arg(5u);
    uint32_t io_control_code = kernel_arg(6u);
    uint32_t input_buffer = kernel_arg(7u);
    uint32_t input_buffer_length = kernel_arg(8u);

    uint32_t status = RECOMP_STATUS_SUCCESS;
    uint32_t written = 0u;
    if (io_control_code == 0x00070000u || io_control_code == 0x00074004u) {
        uint32_t output = kernel_arg(9u);
        uint32_t length = kernel_arg(10u);
        uint32_t required = io_control_code == 0x00070000u ? 24u : 32u;
        status = recomp_device_disk_query(io_control_code,
            output == 0u || length < required ? NULL : recomp_memory(output, required),
            length, &written);
    }

    if (io_status_block != 0u) {
        *recomp_memory_u32(io_status_block) = status;
        *recomp_memory_u32(io_status_block + 4u) = written;
    }
    /* The 0x4d014 query is the CRT's real-time-clock read. The guest reads
       three response bytes back from the query struct at offsets 0x36..0x38
       and treats all-zero as an invalid clock, which sends it to the
       dashboard. The struct is declared 0x2c bytes but the response occupies
       those higher offsets, so set them for this code regardless of the
       input-length argument. The native host's >=0x39 gate was wrong. */
    if (io_control_code == 0x0004d014u && input_buffer != 0u) {
        *recomp_memory_i8(input_buffer + 0x36u) = 1;
        *recomp_memory_i8(input_buffer + 0x37u) = 1;
        *recomp_memory_i8(input_buffer + 0x38u) = 1;
    }

    fprintf(
        stderr,
        "recomp kernel: NtDeviceIoControlFile code=0x%08x input=0x%08x len=%u status=0x%08x\n",
        (unsigned)io_control_code,
        (unsigned)input_buffer,
        (unsigned)input_buffer_length,
        (unsigned)status);

    kernel_return(10u, status);
}

/* Volume geometry for the mounted partition. The native host reports a
   plausible fixed size so a caller sizing a cache or free-space check gets a
   consistent answer; the values are not tied to the real host filesystem. */
static void bridge_nt_query_volume_information_file(void)
{
    uint32_t io_status_block = kernel_arg(2u);
    uint32_t fs_information = kernel_arg(3u);
    uint32_t length = kernel_arg(4u);
    uint32_t fs_information_class = kernel_arg(5u);

    const uint32_t FILE_FS_SIZE_INFORMATION = 3u;
    const uint32_t FS_SIZE_INFORMATION_LENGTH = 0x18u;
    uint32_t status = RECOMP_STATUS_SUCCESS;

    if (io_status_block != 0u) {
        *recomp_memory_u32(io_status_block) = status;
        *recomp_memory_u32(io_status_block + 4u) =
            fs_information_class == FILE_FS_SIZE_INFORMATION
                ? (length < FS_SIZE_INFORMATION_LENGTH
                       ? length
                       : FS_SIZE_INFORMATION_LENGTH)
                : 0u;
    }
    if (fs_information_class == FILE_FS_SIZE_INFORMATION &&
        fs_information != 0u && length >= FS_SIZE_INFORMATION_LENGTH) {
        *recomp_memory_u32(fs_information + 0u) = 0x00100000u;
        *recomp_memory_u32(fs_information + 4u) = 0u;
        *recomp_memory_u32(fs_information + 8u) = 0x00080000u;
        *recomp_memory_u32(fs_information + 12u) = 0u;
        *recomp_memory_u32(fs_information + 16u) = 0x20u;
        *recomp_memory_u32(fs_information + 20u) = 0x200u;
    }

    fprintf(
        stderr,
        "recomp kernel: NtQueryVolumeInformationFile class=%u len=%u status=0x%08x\n",
        (unsigned)fs_information_class,
        (unsigned)length,
        (unsigned)status);

    kernel_return(5u, status);
}

/* Mounting a drive letter as a symbolic link succeeds by registering the
   mapping; there is no real device underneath in the bring-up model. The
   names are logged so the mount sequence is visible. */
static void copy_guest_object_string(uint32_t address, char *out, size_t size)
{
    if (address == 0u || read_guest_ansi_string(address, out, size) == 0) {
        snprintf(out, size, "(null)");
    }
}

static void bridge_io_create_symbolic_link(void)
{
    uint32_t link_name = kernel_arg(1u);
    uint32_t device_name = kernel_arg(2u);
    char link[256];
    char device[256];

    copy_guest_object_string(link_name, link, sizeof link);
    copy_guest_object_string(device_name, device, sizeof device);
    ensure_symbolic_links_initialized();
    fprintf(
        stderr,
        "recomp kernel: IoCreateSymbolicLink '%s' -> '%s'\n",
        link,
        device);
    kernel_return(
        2u,
        recomp_symbolic_link_create(&symbolic_links, link, device)
            ? RECOMP_STATUS_SUCCESS
            : RECOMP_STATUS_NO_MEMORY);
}

static void bridge_io_delete_symbolic_link(void)
{
    uint32_t link_name = kernel_arg(1u);
    char link[256];

    copy_guest_object_string(link_name, link, sizeof link);
    ensure_symbolic_links_initialized();
    kernel_return(
        1u,
        recomp_symbolic_link_delete(&symbolic_links, link)
            ? RECOMP_STATUS_SUCCESS
            : RECOMP_STATUS_OBJECT_NAME_NOT_FOUND);
}

static void bridge_nt_open_symbolic_link_object(void)
{
    uint32_t link_handle = kernel_arg(1u);
    uint32_t object_attributes = kernel_arg(2u);
    uint32_t guest_handle = 0u;
    uint32_t status = RECOMP_STATUS_OBJECT_NAME_NOT_FOUND;
    char link[256] = {0};

    ensure_symbolic_links_initialized();
    if (link_handle == 0u ||
        !read_guest_object_name(object_attributes, link, sizeof link, NULL)) {
        status = RECOMP_STATUS_INVALID_PARAMETER;
    } else if (recomp_symbolic_link_open(
                   &symbolic_links, link, &guest_handle)) {
        *recomp_memory_u32(link_handle) = guest_handle;
        status = RECOMP_STATUS_SUCCESS;
    }
    fprintf(
        stderr,
        "recomp kernel: NtOpenSymbolicLinkObject path='%s' handle=%u"
        " status=0x%08x\n",
        link,
        (unsigned)guest_handle,
        (unsigned)status);
    kernel_return(2u, status);
}

static void bridge_nt_query_symbolic_link_object(void)
{
    uint32_t guest_handle = kernel_arg(1u);
    uint32_t target_string = kernel_arg(2u);
    uint32_t returned_length = kernel_arg(3u);
    uint32_t status = RECOMP_STATUS_INVALID_HANDLE;
    uint16_t maximum_length = 0u;
    uint32_t buffer = 0u;
    char target[RECOMP_SYMBOLIC_LINK_NAME_SIZE] = {0};
    size_t length = 0u;

    ensure_symbolic_links_initialized();
    if (recomp_symbolic_link_query(
            &symbolic_links, guest_handle, target, sizeof target)) {
        length = strlen(target);
        if (target_string == 0u) {
            status = RECOMP_STATUS_INVALID_PARAMETER;
        } else {
            maximum_length = *recomp_memory_u16(target_string + 2u);
            buffer = *recomp_memory_u32(target_string + 4u);
            if (buffer == 0u || maximum_length == 0u ||
                length >= maximum_length) {
                status = RECOMP_STATUS_INVALID_PARAMETER;
            } else {
                memcpy(recomp_memory_i8(buffer), target, length + 1u);
                *recomp_memory_u16(target_string) = (uint16_t)length;
                if (returned_length != 0u) {
                    *recomp_memory_u32(returned_length) = (uint32_t)length;
                }
                status = RECOMP_STATUS_SUCCESS;
            }
        }
    }
    fprintf(
        stderr,
        "recomp kernel: NtQuerySymbolicLinkObject handle=%u target='%s'"
        " length=%u status=0x%08x\n",
        (unsigned)guest_handle,
        target,
        (unsigned)length,
        (unsigned)status);
    kernel_return(3u, status);
}

uint32_t recomp_kernel_close_file(uint32_t guest_handle, uint32_t owner)
{
    uint32_t status = RECOMP_STATUS_SUCCESS;
    const uint32_t RECOMP_STATUS_UNSUCCESSFUL = 0xc0000001u;
    const uint32_t RECOMP_STATUS_ACCESS_DENIED = 0xc0000022u;

    ensure_symbolic_links_initialized();
    (void)recomp_symbolic_link_close(&symbolic_links, guest_handle);
    for (size_t i = 0; i < MAX_FILE_HANDLES; ++i) {
        FileHandleEntry *entry = &file_handles[i];
        if (!entry->active || entry->guest_handle != guest_handle) {
            continue;
        }
        bool delete_on_close = entry->delete_on_close != 0;
        if (delete_on_close && recomp_save_pending() &&
            (!recomp_save_active(owner) ||
             entry->delete_owner != owner ||
             (entry->save_write &&
              entry->save_owner != owner))) {
            status = RECOMP_STATUS_UNSUCCESSFUL;
            delete_on_close = false;
        }
        if (entry->host_handle != INVALID_HANDLE_VALUE) {
            if (entry->save_write && !save_write_allowed(entry, owner)) {
                status = RECOMP_STATUS_UNSUCCESSFUL;
            }
            if (entry->save_write && !FlushFileBuffers(entry->host_handle)) {
                status = RECOMP_STATUS_UNSUCCESSFUL;
            }
            if (!CloseHandle(entry->host_handle)) {
                status = RECOMP_STATUS_UNSUCCESSFUL;
            } else {
                entry->active = 0;
            }
        } else {
            entry->active = 0;
        }
        if (delete_on_close && !entry->active) {
            BOOL removed = entry->kind == FILE_HANDLE_DIRECTORY
                ? RemoveDirectoryA(entry->host_path)
                : DeleteFileA(entry->host_path);
            if (!removed) {
                DWORD error = GetLastError();
                if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
                    status = RECOMP_STATUS_ACCESS_DENIED;
                }
            }
        }
        if ((entry->save_owned || entry->delete_on_close) &&
            status != RECOMP_STATUS_SUCCESS) {
            recomp_save_note_failure(entry->save_owner);
            if (entry->delete_on_close) recomp_save_note_failure(entry->delete_owner);
        }
        break;
    }
    return status;
}

static void bridge_nt_close(void)
{
    kernel_return(1u, recomp_kernel_close_file(kernel_arg(1u), current_save_owner()));
}

RecompFunction recomp_kernel_file(uint32_t ordinal)
{
    switch (ordinal) {
    case 67u: return bridge_io_create_symbolic_link;
    case 68u: return bridge_io_delete_symbolic_link;
    case 187u: return bridge_nt_close;
    case 190u: return bridge_nt_create_file;
    case 196u: return bridge_nt_device_io_control_file;
    case 200u: return bridge_nt_fs_control_file;
    case 202u: return bridge_nt_open_file;
    case 203u: return bridge_nt_open_symbolic_link_object;
    case 207u: return bridge_nt_query_directory_file;
    case 211u: return bridge_nt_query_information_file;
    case 215u: return bridge_nt_query_symbolic_link_object;
    case 218u: return bridge_nt_query_volume_information_file;
    case 219u: return bridge_nt_read_file;
    case 226u: return bridge_nt_set_information_file;
    case 236u: return bridge_nt_write_file;
    default: return NULL;
    }
}
