#define _GNU_SOURCE
#include "host_file_posix.h"
#ifndef _WIN32
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

struct RecompPosixFile { int fd; DIR *directory; };
static const char metadata_key[] = "user.recomp.file-v1";
typedef struct GuestMetadata { uint64_t creation, change; uint32_t attributes; } GuestMetadata;

static int ascii_equal(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b) {
        unsigned char x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return 0;
    }
    return *a == *b;
}

int recomp_posix_resolve_path(const char *path, char *out, size_t capacity)
{
    char input[4096];
    if (!path || !*path || strlen(path) >= sizeof input || capacity < 2) {
        errno = ENAMETOOLONG;
        return 0;
    }
    strcpy(input, path);
    for (char *p = input; *p; ++p) if (*p == '\\') *p = '/';
    strcpy(out, input[0] == '/' ? "/" : ".");
    char *cursor = NULL;
    int missing = 0;
    for (char *part = strtok_r(input, "/", &cursor); part;
         part = strtok_r(NULL, "/", &cursor)) {
        if (!strcmp(part, ".")) continue;
        if (!strcmp(part, "..")) { errno = EACCES; return 0; }
        char found[256];
        const char *name = part;
        if (!missing) {
            DIR *dir = opendir(out);
            if (!dir) return 0;
            int matches = 0, scan_error;
            struct dirent *entry;
            errno = 0;
            while ((entry = readdir(dir)) != NULL) {
                if (ascii_equal(entry->d_name, part)) {
                    if (++matches > 1) break;
                    strcpy(found, entry->d_name);
                }
            }
            scan_error = errno;
            closedir(dir);
            if (matches > 1 || scan_error) {
                errno = matches > 1 ? EEXIST : scan_error;
                return 0;
            }
            if (matches) name = found;
            else missing = 1;
        }
        size_t length = strlen(out), count = strlen(name);
        int separator = out[length - 1] != '/';
        if (length + separator + count >= capacity) { errno = ENAMETOOLONG; return 0; }
        if (separator) out[length++] = '/';
        memcpy(out + length, name, count + 1);
        if (!missing) {
            struct stat info;
            if (lstat(out, &info) != 0) return 0;
            if (S_ISLNK(info.st_mode)) { errno = ELOOP; return 0; }
        }
    }
    return 1;
}

DWORD GetLastError(void)
{
    switch (errno) {
    case 0: return ERROR_SUCCESS;
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EEXIST: return ERROR_ALREADY_EXISTS;
    default: return ERROR_ACCESS_DENIED;
    }
}

HANDLE CreateFileA(const char *path, DWORD access, DWORD share, void *security,
    DWORD disposition, DWORD attributes, HANDLE template_file)
{
    (void)share; (void)security; (void)attributes; (void)template_file;
    char resolved[4096];
    if (!recomp_posix_resolve_path(path, resolved, sizeof resolved)) return INVALID_HANDLE_VALUE;
    int flags = O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK;
    flags |= (access & GENERIC_WRITE) ? ((access & GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disposition) {
    case CREATE_NEW: flags |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: flags |= O_CREAT | O_TRUNC; break;
    case OPEN_EXISTING: break;
    case OPEN_ALWAYS: flags |= O_CREAT; break;
    case TRUNCATE_EXISTING: flags |= O_TRUNC; break;
    default: errno = EINVAL; return INVALID_HANDLE_VALUE;
    }
    int fd = open(resolved, flags, 0666);
    if (fd < 0) return INVALID_HANDLE_VALUE;
    struct stat info;
    if (fstat(fd, &info) != 0 || (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode))) {
        close(fd); errno = EACCES; return INVALID_HANDLE_VALUE;
    }
    HANDLE handle = calloc(1, sizeof *handle);
    if (!handle) { close(fd); errno = ENOMEM; return INVALID_HANDLE_VALUE; }
    handle->fd = fd;
    return handle;
}

HANDLE CreateFileW(const wchar_t *path, DWORD access, DWORD share, void *security,
    DWORD disposition, DWORD attributes, HANDLE template_file)
{
    char bytes[4096];
    size_t at = 0;
    /* wchar_t is UTF-32 on Linux; avoid the process locale for UTF-8 paths. */
    for (; *path; ++path) {
        uint32_t c = (uint32_t)*path;
        unsigned count = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
        if (c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff) || at + count >= sizeof bytes) {
            errno = EINVAL; return INVALID_HANDLE_VALUE;
        }
        if (count == 1) bytes[at++] = (char)c;
        else {
            bytes[at++] = (char)((count == 2 ? 0xc0 : count == 3 ? 0xe0 : 0xf0) |
                (c >> (6 * (count - 1))));
            for (unsigned i = count - 1; i; --i) bytes[at++] = (char)(0x80 | ((c >> (6 * (i - 1))) & 63));
        }
    }
    bytes[at] = 0;
    return CreateFileA(bytes, access, share, security, disposition, attributes, template_file);
}

BOOL CloseHandle(HANDLE handle)
{
    if (!handle || handle == INVALID_HANDLE_VALUE) { errno = EBADF; return 0; }
    int result = handle->directory ? closedir(handle->directory) : close(handle->fd);
    free(handle);
    return result == 0;
}
BOOL ReadFile(HANDLE h, void *bytes, DWORD count, DWORD *done, void *overlapped)
{
    (void)overlapped;
    ssize_t result;
    do { result = read(h->fd, bytes, count); } while (result < 0 && errno == EINTR);
    *done = result < 0 ? 0 : (DWORD)result;
    return result >= 0;
}
BOOL WriteFile(HANDLE h, const void *bytes, DWORD count, DWORD *done, void *overlapped)
{
    (void)overlapped;
    ssize_t result;
    do { result = write(h->fd, bytes, count); } while (result < 0 && errno == EINTR);
    *done = result < 0 ? 0 : (DWORD)result;
    return result >= 0;
}
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER distance, LARGE_INTEGER *position, DWORD method)
{
    off_t result = lseek(h->fd, distance.QuadPart, (int)method);
    if (position) position->QuadPart = result;
    return result != (off_t)-1;
}
BOOL SetEndOfFile(HANDLE h)
{
    off_t position = lseek(h->fd, 0, SEEK_CUR);
    return position >= 0 && ftruncate(h->fd, position) == 0;
}
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER *size)
{
    struct stat info;
    if (fstat(h->fd, &info)) return 0;
    size->QuadPart = info.st_size;
    return 1;
}
BOOL FlushFileBuffers(HANDLE h) { return fsync(h->fd) == 0; }

static uint64_t ticks(struct timespec time)
{
    return ((uint64_t)time.tv_sec + 11644473600ull) * 10000000ull + time.tv_nsec / 100;
}
static struct timespec unix_time(int64_t time)
{
    struct timespec out = {time / 10000000 - 11644473600ll, (time % 10000000) * 100};
    return out;
}
static int metadata(int fd, const struct stat *st, GuestMetadata *out)
{
    memset(out, 0, sizeof *out);
    ssize_t count = fgetxattr(fd, metadata_key, out, sizeof *out);
    if (count < 0 && errno != ENODATA && errno != ENOTSUP) return 0;
    if (count >= 0 && count != sizeof *out) { errno = EIO; return 0; }
    if (!out->creation) out->creation = ticks(st->st_ctim);
    if (!out->change) out->change = ticks(st->st_ctim);
    out->attributes &= ~FILE_ATTRIBUTE_DIRECTORY;
    if (S_ISDIR(st->st_mode)) out->attributes |= FILE_ATTRIBUTE_DIRECTORY;
    if (!(st->st_mode & S_IWUSR)) out->attributes |= FILE_ATTRIBUTE_READONLY;
    if (!out->attributes) out->attributes = FILE_ATTRIBUTE_NORMAL;
    return 1;
}
BOOL GetFileInformationByHandleEx(HANDLE h, unsigned kind, FILE_BASIC_INFO *out, DWORD size)
{
    struct stat st;
    GuestMetadata saved;
    if (kind != FileBasicInfo || size < sizeof *out || fstat(h->fd, &st) ||
        !metadata(h->fd, &st, &saved)) return 0;
    out->CreationTime.QuadPart = saved.creation;
    out->LastAccessTime.QuadPart = ticks(st.st_atim);
    out->LastWriteTime.QuadPart = ticks(st.st_mtim);
    out->ChangeTime.QuadPart = saved.change;
    out->FileAttributes = saved.attributes;
    return 1;
}
BOOL SetFileInformationByHandle(HANDLE h, unsigned kind, const FILE_BASIC_INFO *in, DWORD size)
{
    struct stat st;
    GuestMetadata saved;
    if (kind != FileBasicInfo || size < sizeof *in || fstat(h->fd, &st) ||
        !metadata(h->fd, &st, &saved)) return 0;
    if (in->CreationTime.QuadPart > 0) saved.creation = in->CreationTime.QuadPart;
    if (in->ChangeTime.QuadPart > 0) saved.change = in->ChangeTime.QuadPart;
    else saved.change = 0;
    if (in->FileAttributes) {
        /* Only attributes the journal can round-trip are accepted. */
        if (in->FileAttributes & ~0xb7u) { errno = ENOTSUP; return 0; }
        saved.attributes = in->FileAttributes & ~FILE_ATTRIBUTE_DIRECTORY;
    }
    if (fsetxattr(h->fd, metadata_key, &saved, sizeof saved, 0)) return 0;
    if (in->FileAttributes) {
        mode_t mode = st.st_mode;
        if (saved.attributes & FILE_ATTRIBUTE_READONLY) mode &= ~(S_IWUSR | S_IWGRP | S_IWOTH);
        else mode |= S_IWUSR;
        if (fchmod(h->fd, mode)) return 0;
    }
    struct timespec times[2] = {st.st_atim, st.st_mtim};
    if (in->LastAccessTime.QuadPart > 0) times[0] = unix_time(in->LastAccessTime.QuadPart);
    if (in->LastWriteTime.QuadPart > 0) times[1] = unix_time(in->LastWriteTime.QuadPart);
    return futimens(h->fd, times) == 0;
}
static FILETIME filetime(uint64_t value)
{
    FILETIME result = {(DWORD)value, (DWORD)(value >> 32)};
    return result;
}
BOOL GetFileAttributesExA(const char *path, unsigned kind, WIN32_FILE_ATTRIBUTE_DATA *out)
{
    (void)kind;
    HANDLE h = CreateFileA(path, FILE_READ_ATTRIBUTES, 7, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FILE_BASIC_INFO info;
    LARGE_INTEGER size;
    int ok = GetFileInformationByHandleEx(h, FileBasicInfo, &info, sizeof info) && GetFileSizeEx(h, &size);
    int error = errno;
    CloseHandle(h);
    if (!ok) { errno = error; return 0; }
    out->dwFileAttributes = info.FileAttributes;
    out->ftCreationTime = filetime(info.CreationTime.QuadPart);
    out->ftLastAccessTime = filetime(info.LastAccessTime.QuadPart);
    out->ftLastWriteTime = filetime(info.LastWriteTime.QuadPart);
    out->nFileSizeLow = (DWORD)size.QuadPart;
    out->nFileSizeHigh = (DWORD)((uint64_t)size.QuadPart >> 32);
    return 1;
}
DWORD GetFileAttributesA(const char *path)
{
    WIN32_FILE_ATTRIBUTE_DATA info;
    return GetFileAttributesExA(path, 0, &info) ? info.dwFileAttributes : INVALID_FILE_ATTRIBUTES;
}
BOOL SetFileAttributesA(const char *path, DWORD attributes)
{
    HANDLE h = CreateFileA(path, FILE_WRITE_ATTRIBUTES, 7, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FILE_BASIC_INFO info = {0};
    info.FileAttributes = attributes;
    int ok = SetFileInformationByHandle(h, FileBasicInfo, &info, sizeof info);
    int error = errno;
    CloseHandle(h);
    errno = error;
    return ok;
}
BOOL CreateDirectoryA(const char *path, void *security)
{
    (void)security;
    char resolved[4096];
    return recomp_posix_resolve_path(path, resolved, sizeof resolved) && mkdir(resolved, 0777) == 0;
}
BOOL RemoveDirectoryA(const char *path)
{
    char resolved[4096];
    return recomp_posix_resolve_path(path, resolved, sizeof resolved) && rmdir(resolved) == 0;
}
BOOL DeleteFileA(const char *path)
{
    char resolved[4096];
    DWORD attributes = GetFileAttributesA(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) return 0;
    /* POSIX unlink ignores the file's mode, but the guest delete contract
       must still fail if an attribute change made the pending file readonly. */
    if (attributes & FILE_ATTRIBUTE_READONLY) { errno = EACCES; return 0; }
    return recomp_posix_resolve_path(path, resolved, sizeof resolved) && unlink(resolved) == 0;
}
BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *out)
{
    struct dirent *entry;
    while ((entry = readdir(h->directory)) != NULL) {
        int fd = openat(dirfd(h->directory), entry->d_name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd < 0) return 0;
        struct stat st;
        GuestMetadata saved;
        int ok = fstat(fd, &st) == 0 && metadata(fd, &st, &saved);
        close(fd);
        if (!ok) return 0;
        memset(out, 0, sizeof *out);
        strcpy(out->cFileName, entry->d_name);
        out->dwFileAttributes = saved.attributes;
        out->ftCreationTime = filetime(saved.creation);
        out->ftLastAccessTime = filetime(ticks(st.st_atim));
        out->ftLastWriteTime = filetime(ticks(st.st_mtim));
        out->nFileSizeLow = (DWORD)st.st_size;
        out->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
        return 1;
    }
    return 0;
}
HANDLE FindFirstFileA(const char *pattern, WIN32_FIND_DATAA *out)
{
    char path[4096], resolved[4096];
    size_t length = strlen(pattern);
    /* Kernel enumeration always supplies a trailing *; guest matching is in
       directory_model.c. Do not silently treat other patterns as matches. */
    if (length < 2 || length >= sizeof path || pattern[length - 1] != '*' ||
        (pattern[length - 2] != '/' && pattern[length - 2] != '\\')) {
        errno = EINVAL; return INVALID_HANDLE_VALUE;
    }
    memcpy(path, pattern, length - 1); path[length - 1] = 0;
    if (!recomp_posix_resolve_path(path, resolved, sizeof resolved)) return INVALID_HANDLE_VALUE;
    DIR *dir = opendir(resolved);
    if (!dir) return INVALID_HANDLE_VALUE;
    HANDLE h = calloc(1, sizeof *h);
    if (!h) { closedir(dir); return INVALID_HANDLE_VALUE; }
    h->directory = dir;
    if (!FindNextFileA(h, out)) { CloseHandle(h); return INVALID_HANDLE_VALUE; }
    return h;
}
BOOL FindClose(HANDLE h) { return CloseHandle(h); }

int recomp_posix_sync_directory(const char *path)
{
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    int ok = fsync(fd) == 0;
    int closed = close(fd) == 0;
    return ok && closed;
}
int recomp_posix_replace(const char *temporary, const char *path)
{
    char parent[4096];
    if (strlen(path) >= sizeof parent) { errno = ENAMETOOLONG; return 0; }
    strcpy(parent, path);
    char *slash = strrchr(parent, '/');
    if (slash) { if (slash == parent) slash[1] = 0; else *slash = 0; }
    else strcpy(parent, ".");
    int fd = open(temporary, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    int ok = fsync(fd) == 0;
    int closed = close(fd) == 0;
    return ok && closed && rename(temporary, path) == 0 && recomp_posix_sync_directory(parent);
}
#endif
