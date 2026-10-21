#ifndef _WIN32
#include "win32_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <fnmatch.h>
#include <sys/file.h>
#include <sys/time.h>
#ifdef __SWITCH__
/* newlib on the Switch declares timespec_get but does not implement it. */
int timespec_get(struct timespec *ts, int base)
{
    return base == TIME_UTC && clock_gettime(CLOCK_REALTIME, ts) == 0 ? base : 0;
}

/* Horizon stores no Win32 attributes and cannot set file times, so each
   directory keeps them for its entries in an index file hidden from listings.
   A save rollback restores the index with the rest of the directory. */
#define META_INDEX ".recomp-attrs"

typedef struct FileMeta {
    uint32_t attributes;
    uint64_t creation, access, write, change; /* FILETIME units; 0 uses stat */
} FileMeta;

static void meta_index_path(const char *path, char *index, size_t size, const char **name)
{
    const char *slash = strrchr(path, '/');
    *name = slash ? slash + 1 : path;
    if (slash) snprintf(index, size, "%.*s/" META_INDEX, (int)(slash - path), path);
    else snprintf(index, size, "./" META_INDEX);
}

/* The entry name of an index line, or NULL for a malformed line. */
static const char *meta_line_name(char *line, FileMeta *meta)
{
    unsigned attributes;
    unsigned long long creation, access, write, change;
    int offset = 0;

    line[strcspn(line, "\n")] = '\0';
    if (sscanf(line, "%x %llu %llu %llu %llu %n", &attributes, &creation, &access,
            &write, &change, &offset) != 5 || offset == 0) {
        return NULL;
    }
    *meta = (FileMeta){attributes, creation, access, write, change};
    return line + offset;
}

static bool meta_get(const char *path, FileMeta *meta)
{
    char index[PATH_MAX], line[PATH_MAX + 128];
    const char *name, *entry;
    bool found = false;
    FILE *file;

    memset(meta, 0, sizeof *meta);
    meta_index_path(path, index, sizeof index, &name);
    if ((file = fopen(index, "r")) == NULL) return false;
    while (!found && fgets(line, sizeof line, file) != NULL) {
        FileMeta parsed;
        entry = meta_line_name(line, &parsed);
        if (entry != NULL && strcmp(entry, name) == 0) {
            *meta = parsed;
            found = true;
        }
    }
    fclose(file);
    return found;
}

/* Replaces path's entry, or removes it when meta is NULL. An emptied index is
   deleted so its directory can be removed.
   shortcut: the index is rewritten in place, so a crash mid-write can lose
   attributes; write and rename a copy if that is ever seen. */
static bool meta_set(const char *path, const FileMeta *meta)
{
    char index[PATH_MAX], line[PATH_MAX + 128];
    const char *name;
    char *kept = NULL;
    size_t kept_size = 0;
    bool found = false, ok;
    FILE *file;

    meta_index_path(path, index, sizeof index, &name);
    if ((file = fopen(index, "r")) != NULL) {
        while (fgets(line, sizeof line, file) != NULL) {
            FileMeta parsed;
            const char *entry = meta_line_name(line, &parsed);
            size_t length = strlen(line);
            char *grown;
            if (entry != NULL && strcmp(entry, name) == 0) {
                found = true;
                continue;
            }
            if ((grown = (char *)realloc(kept, kept_size + length + 1)) == NULL) {
                free(kept);
                fclose(file);
                return false;
            }
            kept = grown;
            memcpy(kept + kept_size, line, length);
            kept_size += length;
            kept[kept_size++] = '\n';
        }
        fclose(file);
    }
    if (meta == NULL && (!found || kept_size == 0)) {
        free(kept);
        return !found || unlink(index) == 0;
    }
    if ((file = fopen(index, "w")) == NULL) {
        free(kept);
        return false;
    }
    ok = kept_size == 0 || fwrite(kept, 1, kept_size, file) == kept_size;
    if (meta != NULL) {
        ok = fprintf(file, "%08x %llu %llu %llu %llu %s\n", (unsigned)meta->attributes,
            (unsigned long long)meta->creation, (unsigned long long)meta->access,
            (unsigned long long)meta->write, (unsigned long long)meta->change, name) > 0 && ok;
    }
    ok = fclose(file) == 0 && ok;
    free(kept);
    return ok;
}

static void meta_filetime(uint64_t value, FILETIME *time)
{
    if (value == 0u) return;
    time->dwLowDateTime = (DWORD)value;
    time->dwHighDateTime = (DWORD)(value >> 32u);
}
#else
#include <sys/xattr.h>
#endif

typedef struct RecompWin32FileLock {
    uint64_t offset;
    uint64_t length;
    struct RecompWin32FileLock *next;
} RecompWin32FileLock;

typedef struct RecompWin32FileHandle {
    int fd;
    char path[512];
    uint32_t flags;
    bool is_dir;
    dev_t dev;
    ino_t ino;
    DWORD access;
    DWORD share;
    bool written;
    RecompWin32FileLock *locks;
    struct RecompWin32FileHandle *next;
} RecompWin32FileHandle;

static RecompWin32FileHandle *open_file_handles = NULL;

typedef struct RecompFindHandle {
    DIR *dir;
    char dir_path[512];
    char pattern[260];
} RecompFindHandle;

static _Thread_local DWORD last_error = 0;

DWORD GetLastError(void)
{
    return last_error;
}

void SetLastError(DWORD err)
{
    last_error = err;
}

static void resolve_case_insensitive(char *path, size_t path_size)
{
    if (!path || path[0] == '\0') return;
    if (access(path, F_OK) == 0) return;

    char temp[PATH_MAX];
    if (strlen(path) >= sizeof(temp)) return;
    strcpy(temp, path);

    char resolved[PATH_MAX];
    size_t res_len = 0;
    resolved[0] = '\0';

    char *saveptr = NULL;
    bool is_absolute = (temp[0] == '/');
    if (is_absolute) {
        resolved[0] = '/';
        resolved[1] = '\0';
        res_len = 1;
    }

    char *token = strtok_r(temp, "/", &saveptr);
    bool missing_parent = false;

    while (token != NULL) {
        if (missing_parent || strcmp(token, ".") == 0 || strcmp(token, "..") == 0 ||
            strchr(token, '*') != NULL || strchr(token, '?') != NULL) {
            if (res_len > 0 && resolved[res_len - 1] != '/') {
                if (res_len + 1 < sizeof(resolved)) resolved[res_len++] = '/';
            }
            size_t tlen = strlen(token);
            if (res_len + tlen < sizeof(resolved)) {
                memcpy(&resolved[res_len], token, tlen);
                res_len += tlen;
                resolved[res_len] = '\0';
            }
            token = strtok_r(NULL, "/", &saveptr);
            continue;
        }

        char test_path[PATH_MAX];
        if (res_len == 0) {
            snprintf(test_path, sizeof(test_path), "%s", token);
        } else if (res_len == 1 && resolved[0] == '/') {
            snprintf(test_path, sizeof(test_path), "/%s", token);
        } else {
            snprintf(test_path, sizeof(test_path), "%s/%s", resolved, token);
        }

        if (access(test_path, F_OK) == 0) {
            if (res_len > 0 && resolved[res_len - 1] != '/') {
                if (res_len + 1 < sizeof(resolved)) resolved[res_len++] = '/';
            }
            size_t tlen = strlen(token);
            if (res_len + tlen < sizeof(resolved)) {
                memcpy(&resolved[res_len], token, tlen);
                res_len += tlen;
                resolved[res_len] = '\0';
            }
        } else {
            const char *dir_to_open = (res_len == 0) ? "." : resolved;
            DIR *d = opendir(dir_to_open);
            const char *matched_name = NULL;
            char match_buf[256];
            if (d) {
                struct dirent *ent;
                while ((ent = readdir(d)) != NULL) {
                    if (strcasecmp(ent->d_name, token) == 0) {
                        strncpy(match_buf, ent->d_name, sizeof(match_buf) - 1);
                        match_buf[sizeof(match_buf) - 1] = '\0';
                        matched_name = match_buf;
                        break;
                    }
                }
                closedir(d);
            }

            const char *name_to_append = matched_name ? matched_name : token;
            if (!matched_name) {
                missing_parent = true;
            }

            if (res_len > 0 && resolved[res_len - 1] != '/') {
                if (res_len + 1 < sizeof(resolved)) resolved[res_len++] = '/';
            }
            size_t tlen = strlen(name_to_append);
            if (res_len + tlen < sizeof(resolved)) {
                memcpy(&resolved[res_len], name_to_append, tlen);
                res_len += tlen;
                resolved[res_len] = '\0';
            }
        }

        token = strtok_r(NULL, "/", &saveptr);
    }

    if (res_len < path_size) {
        strcpy(path, resolved);
    }
}

static void normalize_path(const char *in, char *out, size_t out_size)
{
    if (!in || !out || out_size == 0) return;
    size_t j = 0;
    for (size_t i = 0; in[i] != '\0' && j + 1 < out_size; ++i) {
        char c = in[i];
        if (c == '\\') c = '/';
        if (c == '/' && j > 0 && out[j - 1] == '/') {
            continue;
        }
        out[j++] = c;
    }
    if (j > 1 && out[j - 1] == '/') {
        j--;
    }
    out[j] = '\0';

    resolve_case_insensitive(out, out_size);
}

static void set_last_error_from_errno(void)
{
    switch (errno) {
    case ENOENT: last_error = ERROR_FILE_NOT_FOUND; break;
    case EACCES:
    case EPERM:  last_error = ERROR_ACCESS_DENIED; break;
    case EEXIST: last_error = ERROR_ALREADY_EXISTS; break;
    case EBADF:  last_error = ERROR_INVALID_HANDLE; break;
    case ENOTEMPTY: last_error = 145; break; // ERROR_DIR_NOT_EMPTY
    default:     last_error = ERROR_ACCESS_DENIED; break;
    }
}

#if defined(__APPLE__)
#define STAT_ATIME(st) ((st).st_atimespec)
#define STAT_MTIME(st) ((st).st_mtimespec)
#define STAT_CTIME(st) ((st).st_ctimespec)
#define STAT_BTIME(st) ((st).st_birthtimespec)
#else
#define STAT_ATIME(st) ((st).st_atim)
#define STAT_MTIME(st) ((st).st_mtim)
#define STAT_CTIME(st) ((st).st_ctim)
#define STAT_BTIME(st) ((st).st_ctim)
#endif

static void timespec_to_filetime(const struct timespec *ts, FILETIME *ft)
{
    // Windows file time: 100-nanosecond intervals since Jan 1, 1601 UTC
    // Unix epoch is 11644473600 seconds after Windows epoch
    uint64_t intervals = ((uint64_t)ts->tv_sec + 11644473600ULL) * 10000000ULL +
                         (uint64_t)ts->tv_nsec / 100ULL;
    ft->dwLowDateTime = (DWORD)(intervals & 0xffffffffu);
    ft->dwHighDateTime = (DWORD)(intervals >> 32u);
}

HANDLE CreateFileA(
    LPCSTR lpFileName,
    DWORD dwDesiredAccess,
    DWORD dwShareMode,
    void *lpSecurityAttributes,
    DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes,
    HANDLE hTemplateFile)
{
    (void)lpSecurityAttributes;
    (void)hTemplateFile;

    char norm[PATH_MAX];
    normalize_path(lpFileName, norm, sizeof(norm));

    struct stat existing_st = {0};
    const bool existed = stat(norm, &existing_st) == 0;
    if (existed) {
        bool wants_read = (dwDesiredAccess & (GENERIC_READ | 0x1u /* FILE_READ_DATA */)) != 0;
        bool wants_write = (dwDesiredAccess & (GENERIC_WRITE | 0x2u /* FILE_WRITE_DATA */ | 0x4u /* FILE_APPEND_DATA */)) != 0;
        bool wants_delete = (dwDesiredAccess & DELETE) != 0;

        for (RecompWin32FileHandle *cur = open_file_handles; cur != NULL; cur = cur->next) {
            if (cur->dev == existing_st.st_dev && cur->ino == existing_st.st_ino) {
                if (wants_read && !(cur->share & FILE_SHARE_READ)) {
                    last_error = 32; // ERROR_SHARING_VIOLATION
                    return INVALID_HANDLE_VALUE;
                }
                if (wants_write && !(cur->share & FILE_SHARE_WRITE)) {
                    last_error = 32;
                    return INVALID_HANDLE_VALUE;
                }
                if (wants_delete && !(cur->share & FILE_SHARE_DELETE)) {
                    last_error = 32;
                    return INVALID_HANDLE_VALUE;
                }
                bool cur_has_read = (cur->access & (GENERIC_READ | 0x1u)) != 0;
                bool cur_has_write = (cur->access & (GENERIC_WRITE | 0x2u | 0x4u)) != 0;
                bool cur_has_delete = (cur->access & DELETE) != 0;

                if (cur_has_read && !(dwShareMode & FILE_SHARE_READ)) {
                    last_error = 32;
                    return INVALID_HANDLE_VALUE;
                }
                if (cur_has_write && !(dwShareMode & FILE_SHARE_WRITE)) {
                    last_error = 32;
                    return INVALID_HANDLE_VALUE;
                }
                if (cur_has_delete && !(dwShareMode & FILE_SHARE_DELETE)) {
                    last_error = 32;
                    return INVALID_HANDLE_VALUE;
                }
            }
        }
    }

    int flags = 0;
    const bool read = (dwDesiredAccess & (GENERIC_READ | 0x1u | 0x20u)) != 0;
    const bool write = (dwDesiredAccess & (GENERIC_WRITE | 0x2u /* FILE_WRITE_DATA */ | 0x4u /* FILE_APPEND_DATA */)) != 0;

    if (read && write) {
        flags = O_RDWR;
    } else if (write) {
        flags = O_WRONLY;
    } else {
        flags = O_RDONLY;
    }

    switch (dwCreationDisposition) {
    case CREATE_NEW:
        flags |= O_CREAT | O_EXCL;
        break;
    case CREATE_ALWAYS:
        flags |= O_CREAT | O_TRUNC;
        break;
    case OPEN_EXISTING:
        break;
    case OPEN_ALWAYS:
        flags |= O_CREAT;
        break;
    case TRUNCATE_EXISTING:
        flags |= O_TRUNC;
        break;
    default:
        break;
    }
#ifdef __SWITCH__
    /* Horizon cannot mark a file read-only, so refuse what the mode bit would. */
    if (existed && !S_ISDIR(existing_st.st_mode) &&
        (write || (flags & O_TRUNC) != 0)) {
        FileMeta meta;
        if (meta_get(norm, &meta) && (meta.attributes & FILE_ATTRIBUTE_READONLY) != 0) {
            last_error = ERROR_ACCESS_DENIED;
            return INVALID_HANDLE_VALUE;
        }
    }
#endif

    int fd = open(norm, flags, 0666);
    if (fd < 0) {
        // If it's a directory and backup semantics requested, try opening read-only
        if ((dwFlagsAndAttributes & FILE_FLAG_BACKUP_SEMANTICS) != 0) {
            fd = open(norm, O_RDONLY);
        }
    }
    bool no_descriptor = false;
#ifdef __SWITCH__
    /* Horizon cannot open a directory as a file; its handle works by path. */
    no_descriptor = fd < 0 && existed && S_ISDIR(existing_st.st_mode) &&
        (dwFlagsAndAttributes & FILE_FLAG_BACKUP_SEMANTICS) != 0;
#endif
    if (fd < 0 && !no_descriptor) {
        set_last_error_from_errno();
        return INVALID_HANDLE_VALUE;
    }
#ifdef __SWITCH__
    if (!existed) meta_set(norm, NULL); /* A deleted file's entry must not carry over. */
#endif

    RecompWin32FileHandle *h = (RecompWin32FileHandle *)malloc(sizeof(RecompWin32FileHandle));
    if (!h) {
        close(fd);
        last_error = ERROR_ACCESS_DENIED;
        return INVALID_HANDLE_VALUE;
    }
    h->fd = fd;
    strncpy(h->path, norm, sizeof(h->path) - 1);
    h->path[sizeof(h->path) - 1] = '\0';
    h->flags = dwFlagsAndAttributes;
    
    struct stat st = existing_st;
    h->is_dir = ((no_descriptor || fstat(fd, &st) == 0) && S_ISDIR(st.st_mode));
    h->dev = st.st_dev;
    h->ino = st.st_ino;
    h->access = dwDesiredAccess;
    h->share = dwShareMode;
    h->written = false;
    h->locks = NULL;
    h->next = open_file_handles;
    open_file_handles = h;

    return (HANDLE)h;
}

HANDLE CreateFileW(
    LPCWSTR lpFileName,
    DWORD dwDesiredAccess,
    DWORD dwShareMode,
    void *lpSecurityAttributes,
    DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes,
    HANDLE hTemplateFile)
{
    char utf8[512];
    size_t len = wcstombs(utf8, lpFileName, sizeof(utf8) - 1);
    if (len == (size_t)-1) {
        last_error = ERROR_FILE_NOT_FOUND;
        return INVALID_HANDLE_VALUE;
    }
    utf8[len] = '\0';
    return CreateFileA(utf8, dwDesiredAccess, dwShareMode, lpSecurityAttributes,
                       dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile);
}

BOOL ReadFile(
    HANDLE hFile,
    LPVOID lpBuffer,
    DWORD nNumberOfBytesToRead,
    DWORD *lpNumberOfBytesRead,
    void *lpOverlapped)
{
    (void)lpOverlapped;
    if (hFile == INVALID_HANDLE_VALUE || hFile == NULL) {
        last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    RecompWin32FileHandle *h = (RecompWin32FileHandle *)hFile;
    if ((h->access & (GENERIC_READ | 0x1u /* FILE_READ_DATA */)) == 0) {
        last_error = ERROR_ACCESS_DENIED;
        return FALSE;
    }
    off_t current_offset = lseek(h->fd, 0, SEEK_CUR);
    if (current_offset != (off_t)-1 && nNumberOfBytesToRead > 0) {
        uint64_t start = (uint64_t)current_offset;
        uint64_t end = start + nNumberOfBytesToRead;
        for (RecompWin32FileHandle *cur = open_file_handles; cur != NULL; cur = cur->next) {
            if (cur != h && cur->dev == h->dev && cur->ino == h->ino) {
                for (RecompWin32FileLock *lk = cur->locks; lk != NULL; lk = lk->next) {
                    uint64_t lk_start = lk->offset;
                    uint64_t lk_end = lk->offset + lk->length;
                    if (start < lk_end && lk_start < end) {
                        last_error = 33; // ERROR_LOCK_VIOLATION
                        return FALSE;
                    }
                }
            }
        }
    }
    ssize_t ret = read(h->fd, lpBuffer, nNumberOfBytesToRead);
    if (ret < 0) {
        set_last_error_from_errno();
        return FALSE;
    }
    if (lpNumberOfBytesRead) {
        *lpNumberOfBytesRead = (DWORD)ret;
    }
    return TRUE;
}

BOOL WriteFile(
    HANDLE hFile,
    LPCVOID lpBuffer,
    DWORD nNumberOfBytesToWrite,
    DWORD *lpNumberOfBytesWritten,
    void *lpOverlapped)
{
    (void)lpOverlapped;
    if (hFile == INVALID_HANDLE_VALUE || hFile == NULL) {
        last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    RecompWin32FileHandle *h = (RecompWin32FileHandle *)hFile;
    if ((h->access & (GENERIC_WRITE | 0x2u /* FILE_WRITE_DATA */ | 0x4u /* FILE_APPEND_DATA */)) == 0) {
        last_error = ERROR_ACCESS_DENIED;
        return FALSE;
    }
    off_t current_offset = lseek(h->fd, 0, SEEK_CUR);
    if (current_offset != (off_t)-1 && nNumberOfBytesToWrite > 0) {
        uint64_t start = (uint64_t)current_offset;
        uint64_t end = start + nNumberOfBytesToWrite;
        for (RecompWin32FileHandle *cur = open_file_handles; cur != NULL; cur = cur->next) {
            if (cur != h && cur->dev == h->dev && cur->ino == h->ino) {
                for (RecompWin32FileLock *lk = cur->locks; lk != NULL; lk = lk->next) {
                    uint64_t lk_start = lk->offset;
                    uint64_t lk_end = lk->offset + lk->length;
                    if (start < lk_end && lk_start < end) {
                        last_error = 33; // ERROR_LOCK_VIOLATION
                        return FALSE;
                    }
                }
            }
        }
    }
    ssize_t ret = write(h->fd, lpBuffer, nNumberOfBytesToWrite);
    if (ret < 0) {
        set_last_error_from_errno();
        return FALSE;
    }
    if (lpNumberOfBytesWritten) {
        *lpNumberOfBytesWritten = (DWORD)ret;
    }
    h->written = h->written || ret > 0;
    return TRUE;
}

BOOL CloseHandle(HANDLE hObject)
{
    if (hObject == INVALID_HANDLE_VALUE || hObject == NULL) {
        last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    RecompWin32FileHandle *h = (RecompWin32FileHandle *)hObject;
    RecompWin32FileHandle **curr = &open_file_handles;
    while (*curr) {
        if (*curr == h) {
            *curr = h->next;
            break;
        }
        curr = &(*curr)->next;
    }
    /* Close first: Horizon cannot delete a file that is still open. */
    if (h->fd >= 0) close(h->fd);
    if ((h->flags & FILE_FLAG_DELETE_ON_CLOSE) != 0) {
        unlink(h->path);
    }
#ifdef __SWITCH__
    if ((h->flags & FILE_FLAG_DELETE_ON_CLOSE) != 0) {
        meta_set(h->path, NULL);
    } else if (h->written) {
        /* A write after the times were set moves the write time on, as on Windows. */
        FileMeta meta;
        if (meta_get(h->path, &meta) && (meta.write != 0u || meta.change != 0u)) {
            meta.write = meta.change = 0u;
            meta_set(h->path, &meta);
        }
    }
#endif
    RecompWin32FileLock *lk = h->locks;
    while (lk) {
        RecompWin32FileLock *next = lk->next;
        free(lk);
        lk = next;
    }
    free(h);
    return TRUE;
}

BOOL SetFilePointerEx(
    HANDLE hFile,
    LARGE_INTEGER liDistanceToMove,
    LARGE_INTEGER *lpNewFilePointer,
    DWORD dwMoveMethod)
{
    if (hFile == INVALID_HANDLE_VALUE || hFile == NULL) {
        last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    RecompWin32FileHandle *h = (RecompWin32FileHandle *)hFile;
    int whence = SEEK_SET;
    if (dwMoveMethod == FILE_CURRENT) whence = SEEK_CUR;
    else if (dwMoveMethod == FILE_END) whence = SEEK_END;

    off_t pos = lseek(h->fd, (off_t)liDistanceToMove.QuadPart, whence);
    if (pos == (off_t)-1) {
        set_last_error_from_errno();
        return FALSE;
    }
    if (lpNewFilePointer) {
        lpNewFilePointer->QuadPart = (int64_t)pos;
    }
    return TRUE;
}

BOOL GetFileSizeEx(
    HANDLE hFile,
    LARGE_INTEGER *lpFileSize)
{
    if (hFile == INVALID_HANDLE_VALUE || hFile == NULL || lpFileSize == NULL) {
        last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    RecompWin32FileHandle *h = (RecompWin32FileHandle *)hFile;
    struct stat st;
    if (fstat(h->fd, &st) != 0) {
        set_last_error_from_errno();
        return FALSE;
    }
    lpFileSize->QuadPart = (int64_t)st.st_size;
    return TRUE;
}

BOOL SetEndOfFile(HANDLE hFile)
{
    if (hFile == INVALID_HANDLE_VALUE || hFile == NULL) {
        last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    RecompWin32FileHandle *h = (RecompWin32FileHandle *)hFile;
    off_t cur = lseek(h->fd, 0, SEEK_CUR);
    if (cur == (off_t)-1) {
        set_last_error_from_errno();
        return FALSE;
    }
    if (ftruncate(h->fd, cur) != 0) {
        set_last_error_from_errno();
        return FALSE;
    }
    h->written = true;
    return TRUE;
}

BOOL FlushFileBuffers(HANDLE hFile)
{
    if (hFile == INVALID_HANDLE_VALUE || hFile == NULL) {
        last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    RecompWin32FileHandle *h = (RecompWin32FileHandle *)hFile;
    return fsync(h->fd) == 0;
}

DWORD GetFileAttributesA(LPCSTR lpFileName)
{
    char norm[PATH_MAX];
    normalize_path(lpFileName, norm, sizeof(norm));
    struct stat st;
    if (stat(norm, &st) != 0) {
        set_last_error_from_errno();
        return INVALID_FILE_ATTRIBUTES;
    }
    DWORD attrs = 0;
    if (S_ISDIR(st.st_mode)) {
        attrs |= FILE_ATTRIBUTE_DIRECTORY;
    } else {
        attrs |= FILE_ATTRIBUTE_NORMAL;
    }
    if ((st.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)) == 0) {
        attrs |= FILE_ATTRIBUTE_READONLY;
    }
#if defined(__APPLE__)
    if ((st.st_flags & 0x8000) != 0) {
        attrs |= FILE_ATTRIBUTE_HIDDEN;
    }
    uint32_t xattr_val = 0;
    if (getxattr(norm, "user.win32_attrs", &xattr_val, sizeof(xattr_val), 0, 0) == sizeof(xattr_val)) {
        attrs |= xattr_val;
    }
#elif defined(__SWITCH__)
    FileMeta meta;
    if (meta_get(norm, &meta)) {
        attrs |= meta.attributes;
    }
#else
    uint32_t xattr_val = 0;
    if (getxattr(norm, "user.win32_attrs", &xattr_val, sizeof(xattr_val)) == sizeof(xattr_val)) {
        attrs |= xattr_val;
    }
#endif
    return attrs;
}

BOOL GetFileAttributesExA(
    LPCSTR lpFileName,
    GET_FILEEX_INFO_LEVELS fInfoLevelId,
    LPVOID lpFileInformation)
{
    (void)fInfoLevelId;
    char norm[PATH_MAX];
    normalize_path(lpFileName, norm, sizeof(norm));
    struct stat st;
    if (stat(norm, &st) != 0) {
        set_last_error_from_errno();
        return FALSE;
    }
    WIN32_FILE_ATTRIBUTE_DATA *data = (WIN32_FILE_ATTRIBUTE_DATA *)lpFileInformation;
    data->dwFileAttributes = GetFileAttributesA(norm);
    timespec_to_filetime(&STAT_CTIME(st), &data->ftCreationTime);
    timespec_to_filetime(&STAT_ATIME(st), &data->ftLastAccessTime);
    timespec_to_filetime(&STAT_MTIME(st), &data->ftLastWriteTime);
    data->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32u);
    data->nFileSizeLow = (DWORD)((uint64_t)st.st_size & 0xffffffffu);
#ifdef __SWITCH__
    FileMeta meta;
    if (meta_get(norm, &meta)) {
        meta_filetime(meta.creation, &data->ftCreationTime);
        meta_filetime(meta.access, &data->ftLastAccessTime);
        meta_filetime(meta.write, &data->ftLastWriteTime);
    }
#endif
    return TRUE;
}

BOOL SetFileAttributesA(
    LPCSTR lpFileName,
    DWORD dwFileAttributes)
{
    char norm[PATH_MAX];
    normalize_path(lpFileName, norm, sizeof(norm));
    struct stat st;
    if (stat(norm, &st) != 0) {
        set_last_error_from_errno();
        return FALSE;
    }
#ifdef __SWITCH__
    FileMeta meta;
    meta_get(norm, &meta);
    meta.attributes = dwFileAttributes;
    if (!meta_set(norm, &meta)) {
        set_last_error_from_errno();
        return FALSE;
    }
    return TRUE;
#else
    mode_t mode = st.st_mode;
    if ((dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
        mode &= ~(S_IWUSR | S_IWGRP | S_IWOTH);
    } else {
        mode |= S_IWUSR;
    }
    /* Linux needs write permission to set a user xattr, so hold owner write while
       it is stored. A chmod we may not make fails here, before anything changes. */
    if (chmod(norm, mode | S_IWUSR) != 0) {
        set_last_error_from_errno();
        return FALSE;
    }
#if defined(__APPLE__)
    u_int flags = (dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) ? 0x8000 : 0;
    chflags(norm, flags);
    uint32_t val = (uint32_t)dwFileAttributes;
    setxattr(norm, "user.win32_attrs", &val, sizeof(val), 0, 0);
#else
    uint32_t val = (uint32_t)dwFileAttributes;
    setxattr(norm, "user.win32_attrs", &val, sizeof(val), 0);
#endif
    if ((mode & S_IWUSR) == 0 && chmod(norm, mode) != 0) {
        set_last_error_from_errno();
        return FALSE;
    }
    return TRUE;
#endif
}

BOOL CreateDirectoryA(
    LPCSTR lpPathName,
    void *lpSecurityAttributes)
{
    (void)lpSecurityAttributes;
    char norm[PATH_MAX];
    normalize_path(lpPathName, norm, sizeof(norm));
    if (mkdir(norm, 0777) != 0) {
        set_last_error_from_errno();
        return FALSE;
    }
#ifdef __SWITCH__
    meta_set(norm, NULL);
#endif
    return TRUE;
}

BOOL RemoveDirectoryA(LPCSTR lpPathName)
{
    char norm[PATH_MAX];
    normalize_path(lpPathName, norm, sizeof(norm));
    if (rmdir(norm) != 0) {
#ifdef __SWITCH__
        /* Its own index keeps an otherwise empty directory from being removed. */
        char path[PATH_MAX];
        WIN32_FIND_DATAA data;
        snprintf(path, sizeof path, "%s/*", norm);
        HANDLE find = FindFirstFileA(path, &data);
        if (find != INVALID_HANDLE_VALUE) {
            FindClose(find);
            last_error = 145; // ERROR_DIR_NOT_EMPTY
            return FALSE;
        }
        if (GetLastError() != ERROR_NO_MORE_FILES) return FALSE;
        snprintf(path, sizeof path, "%s/" META_INDEX, norm);
        if (unlink(path) != 0 || rmdir(norm) != 0) {
            set_last_error_from_errno();
            return FALSE;
        }
#else
        set_last_error_from_errno();
        return FALSE;
#endif
    }
#ifdef __SWITCH__
    meta_set(norm, NULL);
#endif
    return TRUE;
}

BOOL DeleteFileA(LPCSTR lpFileName)
{
    char norm[PATH_MAX];
    normalize_path(lpFileName, norm, sizeof(norm));
    struct stat st;
    if (stat(norm, &st) == 0) {
        // Read-only files cannot be deleted on Win32
        if ((st.st_mode & (S_IWUSR | S_IWGRP | S_IWOTH)) == 0) {
            last_error = ERROR_ACCESS_DENIED;
            return FALSE;
        }
    }
#ifdef __SWITCH__
    FileMeta meta;
    if (meta_get(norm, &meta) && (meta.attributes & FILE_ATTRIBUTE_READONLY) != 0) {
        last_error = ERROR_ACCESS_DENIED;
        return FALSE;
    }
#endif
    if (unlink(norm) != 0) {
#ifdef __SWITCH__
        /* Horizon refuses to delete an open file; delete it at its last
           close, as Windows does. */
        bool deferred = false;
        for (RecompWin32FileHandle *cur = open_file_handles; cur != NULL; cur = cur->next) {
            if (strcmp(cur->path, norm) == 0) {
                cur->flags |= FILE_FLAG_DELETE_ON_CLOSE;
                deferred = true;
            }
        }
        if (deferred) return TRUE;
#endif
        set_last_error_from_errno();
        return FALSE;
    }
#ifdef __SWITCH__
    meta_set(norm, NULL);
#endif
    return TRUE;
}

HANDLE FindFirstFileA(
    LPCSTR lpFileName,
    WIN32_FIND_DATAA *lpFindFileData)
{
    char dir_path[512];
    normalize_path(lpFileName, dir_path, sizeof(dir_path));

    char *sep = strrchr(dir_path, '/');
    char pattern[260] = "*";
    if (sep) {
        strncpy(pattern, sep + 1, sizeof(pattern) - 1);
        pattern[sizeof(pattern) - 1] = '\0';
        *sep = '\0';
    } else {
        strcpy(dir_path, ".");
    }

    DIR *dir = opendir(dir_path);
    if (!dir) {
        set_last_error_from_errno();
        return INVALID_HANDLE_VALUE;
    }

    RecompFindHandle *h = (RecompFindHandle *)malloc(sizeof(RecompFindHandle));
    if (!h) {
        closedir(dir);
        last_error = ERROR_ACCESS_DENIED;
        return INVALID_HANDLE_VALUE;
    }
    h->dir = dir;
    strncpy(h->dir_path, dir_path, sizeof(h->dir_path) - 1);
    h->dir_path[sizeof(h->dir_path) - 1] = '\0';
    strncpy(h->pattern, pattern, sizeof(h->pattern) - 1);
    h->pattern[sizeof(h->pattern) - 1] = '\0';

    if (!FindNextFileA((HANDLE)h, lpFindFileData)) {
        FindClose((HANDLE)h);
        return INVALID_HANDLE_VALUE;
    }

    return (HANDLE)h;
}

BOOL FindNextFileA(
    HANDLE hFindFile,
    WIN32_FIND_DATAA *lpFindFileData)
{
    if (hFindFile == INVALID_HANDLE_VALUE || hFindFile == NULL) {
        last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    RecompFindHandle *h = (RecompFindHandle *)hFindFile;
    struct dirent *entry;
    while ((entry = readdir(h->dir)) != NULL) {
#ifdef __SWITCH__
        if (strcmp(entry->d_name, META_INDEX) == 0) continue;
#endif
        if (h->pattern[0] != '\0' && strcmp(h->pattern, "*") != 0) {
            if (fnmatch(h->pattern, entry->d_name, FNM_CASEFOLD) != 0) {
                continue;
            }
        }
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", h->dir_path, entry->d_name);
        struct stat st;
        if (stat(full, &st) != 0) {
            continue;
        }

        memset(lpFindFileData, 0, sizeof(WIN32_FIND_DATAA));
        strncpy(lpFindFileData->cFileName, entry->d_name, sizeof(lpFindFileData->cFileName) - 1);
        lpFindFileData->cFileName[sizeof(lpFindFileData->cFileName) - 1] = '\0';
        lpFindFileData->dwFileAttributes = GetFileAttributesA(full);
        timespec_to_filetime(&STAT_CTIME(st), &lpFindFileData->ftCreationTime);
        timespec_to_filetime(&STAT_ATIME(st), &lpFindFileData->ftLastAccessTime);
        timespec_to_filetime(&STAT_MTIME(st), &lpFindFileData->ftLastWriteTime);
        lpFindFileData->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32u);
        lpFindFileData->nFileSizeLow = (DWORD)((uint64_t)st.st_size & 0xffffffffu);
#ifdef __SWITCH__
        FileMeta meta;
        if (meta_get(full, &meta)) {
            meta_filetime(meta.creation, &lpFindFileData->ftCreationTime);
            meta_filetime(meta.access, &lpFindFileData->ftLastAccessTime);
            meta_filetime(meta.write, &lpFindFileData->ftLastWriteTime);
        }
#endif
        return TRUE;
    }
    last_error = ERROR_NO_MORE_FILES;
    return FALSE;
}

BOOL FindClose(HANDLE hFindFile)
{
    if (hFindFile == INVALID_HANDLE_VALUE || hFindFile == NULL) {
        last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    RecompFindHandle *h = (RecompFindHandle *)hFindFile;
    closedir(h->dir);
    free(h);
    return TRUE;
}

BOOL LockFile(
    HANDLE hFile,
    DWORD dwFileOffsetLow,
    DWORD dwFileOffsetHigh,
    DWORD nNumberOfBytesToLockLow,
    DWORD nNumberOfBytesToLockHigh)
{
    if (hFile == INVALID_HANDLE_VALUE || hFile == NULL) {
        last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    RecompWin32FileHandle *h = (RecompWin32FileHandle *)hFile;
    uint64_t offset = ((uint64_t)dwFileOffsetHigh << 32u) | dwFileOffsetLow;
    uint64_t length = ((uint64_t)nNumberOfBytesToLockHigh << 32u) | nNumberOfBytesToLockLow;
    uint64_t start = offset;
    uint64_t end = offset + length;

    for (RecompWin32FileHandle *cur = open_file_handles; cur != NULL; cur = cur->next) {
        if (cur->dev == h->dev && cur->ino == h->ino) {
            for (RecompWin32FileLock *lk = cur->locks; lk != NULL; lk = lk->next) {
                uint64_t lk_start = lk->offset;
                uint64_t lk_end = lk->offset + lk->length;
                if (start < lk_end && lk_start < end) {
                    last_error = 33; // ERROR_LOCK_VIOLATION
                    return FALSE;
                }
            }
        }
    }

    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = (off_t)offset;
    fl.l_len = (off_t)length;
    if (fcntl(h->fd, F_SETLK, &fl) == -1) {
        if (errno == EACCES || errno == EAGAIN) {
            last_error = 33; // ERROR_LOCK_VIOLATION
        } else {
            set_last_error_from_errno();
        }
        return FALSE;
    }

    RecompWin32FileLock *lk = (RecompWin32FileLock *)malloc(sizeof(RecompWin32FileLock));
    if (!lk) {
        fl.l_type = F_UNLCK;
        fcntl(h->fd, F_SETLK, &fl);
        last_error = ERROR_ACCESS_DENIED;
        return FALSE;
    }
    lk->offset = offset;
    lk->length = length;
    lk->next = h->locks;
    h->locks = lk;
    return TRUE;
}

BOOL UnlockFile(
    HANDLE hFile,
    DWORD dwFileOffsetLow,
    DWORD dwFileOffsetHigh,
    DWORD nNumberOfBytesToUnlockLow,
    DWORD nNumberOfBytesToUnlockHigh)
{
    if (hFile == INVALID_HANDLE_VALUE || hFile == NULL) {
        last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    RecompWin32FileHandle *h = (RecompWin32FileHandle *)hFile;
    uint64_t offset = ((uint64_t)dwFileOffsetHigh << 32u) | dwFileOffsetLow;
    uint64_t length = ((uint64_t)nNumberOfBytesToUnlockHigh << 32u) | nNumberOfBytesToUnlockLow;

    RecompWin32FileLock **curr = &h->locks;
    while (*curr) {
        if ((*curr)->offset == offset && (*curr)->length == length) {
            RecompWin32FileLock *to_free = *curr;
            *curr = to_free->next;
            free(to_free);
            struct flock fl;
            memset(&fl, 0, sizeof(fl));
            fl.l_type = F_UNLCK;
            fl.l_whence = SEEK_SET;
            fl.l_start = (off_t)offset;
            fl.l_len = (off_t)length;
            fcntl(h->fd, F_SETLK, &fl);
            return TRUE;
        }
        curr = &(*curr)->next;
    }
    last_error = 158; // ERROR_NOT_LOCKED
    return FALSE;
}

BOOL GetFileInformationByHandleEx(
    HANDLE hFile,
    FILE_INFO_BY_HANDLE_CLASS FileInformationClass,
    LPVOID lpFileInformation,
    DWORD dwBufferSize)
{
    if (hFile == INVALID_HANDLE_VALUE || hFile == NULL) {
        last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    RecompWin32FileHandle *h = (RecompWin32FileHandle *)hFile;
    if (FileInformationClass == FileBasicInfo) {
        if (dwBufferSize < sizeof(FILE_BASIC_INFO)) {
            last_error = ERROR_ACCESS_DENIED;
            return FALSE;
        }
        struct stat st;
        if ((h->fd >= 0 ? fstat(h->fd, &st) : stat(h->path, &st)) != 0) {
            set_last_error_from_errno();
            return FALSE;
        }
        FILE_BASIC_INFO *info = (FILE_BASIC_INFO *)lpFileInformation;
        FILETIME ft;
        timespec_to_filetime(&STAT_BTIME(st), &ft);
        info->CreationTime.QuadPart = (int64_t)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
        timespec_to_filetime(&STAT_ATIME(st), &ft);
        info->LastAccessTime.QuadPart = (int64_t)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
        timespec_to_filetime(&STAT_MTIME(st), &ft);
        info->LastWriteTime.QuadPart = (int64_t)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
        info->ChangeTime.QuadPart = info->LastWriteTime.QuadPart;
        info->FileAttributes = GetFileAttributesA(h->path);
#ifdef __SWITCH__
        FileMeta meta;
        if (meta_get(h->path, &meta)) {
            if (meta.creation != 0u) info->CreationTime.QuadPart = (int64_t)meta.creation;
            if (meta.access != 0u) info->LastAccessTime.QuadPart = (int64_t)meta.access;
            if (meta.write != 0u) info->LastWriteTime.QuadPart = info->ChangeTime.QuadPart = (int64_t)meta.write;
            if (meta.change != 0u) info->ChangeTime.QuadPart = (int64_t)meta.change;
        }
#endif
        return TRUE;
    }
    return FALSE;
}

BOOL SetFileInformationByHandle(
    HANDLE hFile,
    FILE_INFO_BY_HANDLE_CLASS FileInformationClass,
    LPVOID lpFileInformation,
    DWORD dwBufferSize)
{
    if (hFile == INVALID_HANDLE_VALUE || hFile == NULL) {
        last_error = ERROR_INVALID_HANDLE;
        return FALSE;
    }
    RecompWin32FileHandle *h = (RecompWin32FileHandle *)hFile;
    if (FileInformationClass == FileBasicInfo) {
        if (dwBufferSize < sizeof(FILE_BASIC_INFO)) {
            last_error = ERROR_ACCESS_DENIED;
            return FALSE;
        }
        FILE_BASIC_INFO *info = (FILE_BASIC_INFO *)lpFileInformation;
#ifdef __SWITCH__
        FileMeta meta;
        bool changed = false;
        meta_get(h->path, &meta);
        /* Zero leaves a field unchanged; negative values ask Windows to stop
           updating it, which nothing here needs. */
        if (info->FileAttributes != 0) meta.attributes = info->FileAttributes, changed = true;
        if (info->CreationTime.QuadPart > 0) meta.creation = (uint64_t)info->CreationTime.QuadPart, changed = true;
        if (info->LastAccessTime.QuadPart > 0) meta.access = (uint64_t)info->LastAccessTime.QuadPart, changed = true;
        if (info->LastWriteTime.QuadPart > 0) {
            meta.write = (uint64_t)info->LastWriteTime.QuadPart;
            h->written = false;
            changed = true;
        }
        if (info->ChangeTime.QuadPart > 0) meta.change = (uint64_t)info->ChangeTime.QuadPart, changed = true;
        if (changed && !meta_set(h->path, &meta)) {
            set_last_error_from_errno();
            return FALSE;
        }
        return TRUE;
#else
        struct stat st;
        if (fstat(h->fd, &st) != 0) {
            set_last_error_from_errno();
            return FALSE;
        }
        if (info->FileAttributes != 0) {
            mode_t mode = st.st_mode;
            if (info->FileAttributes & FILE_ATTRIBUTE_READONLY) {
                mode &= ~(S_IWUSR | S_IWGRP | S_IWOTH);
            } else {
                mode |= (S_IWUSR | S_IWGRP);
                if (!h->is_dir) {
                    mode |= S_IWOTH;
                }
            }
            /* Hold owner write while the xattr is stored; see SetFileAttributesA. */
            if (fchmod(h->fd, mode | S_IWUSR) == 0) {
#if defined(__APPLE__)
                u_int flags = (info->FileAttributes & FILE_ATTRIBUTE_HIDDEN) ? 0x8000 : 0;
                fchflags(h->fd, flags);
                uint32_t val = (uint32_t)info->FileAttributes;
                fsetxattr(h->fd, "user.win32_attrs", &val, sizeof(val), 0, 0);
#else
                uint32_t val = (uint32_t)info->FileAttributes;
                fsetxattr(h->fd, "user.win32_attrs", &val, sizeof(val), 0);
#endif
                if ((mode & S_IWUSR) == 0) fchmod(h->fd, mode);
            }
        }
        if (info->LastWriteTime.QuadPart != 0 || info->LastAccessTime.QuadPart != 0) {
            struct timeval tv[2];
            if (info->LastAccessTime.QuadPart != 0) {
                tv[0].tv_sec = (time_t)((info->LastAccessTime.QuadPart / 10000000ULL) - 11644473600ULL);
                tv[0].tv_usec = (suseconds_t)((info->LastAccessTime.QuadPart % 10000000ULL) / 10);
            } else {
                tv[0].tv_sec = STAT_ATIME(st).tv_sec;
                tv[0].tv_usec = (suseconds_t)(STAT_ATIME(st).tv_nsec / 1000);
            }
            if (info->LastWriteTime.QuadPart != 0) {
                tv[1].tv_sec = (time_t)((info->LastWriteTime.QuadPart / 10000000ULL) - 11644473600ULL);
                tv[1].tv_usec = (suseconds_t)((info->LastWriteTime.QuadPart % 10000000ULL) / 10);
            } else {
                tv[1].tv_sec = STAT_MTIME(st).tv_sec;
                tv[1].tv_usec = (suseconds_t)(STAT_MTIME(st).tv_nsec / 1000);
            }
            futimes(h->fd, tv);
        }
        return TRUE;
#endif
    }
    return FALSE;
}

DWORD GetTempPathA(DWORD nBufferLength, LPSTR lpBuffer)
{
    char resolved[PATH_MAX];
    const char *tmp = getenv("TMPDIR");
    if (!tmp) tmp = "/tmp";
    if (realpath(tmp, resolved)) {
        tmp = resolved;
    }
    size_t len = strlen(tmp);
    if (len + 2 > nBufferLength) return (DWORD)(len + 2);
    snprintf(lpBuffer, nBufferLength, "%s%s", tmp, (tmp[len - 1] == '/') ? "" : "/");
    return (DWORD)strlen(lpBuffer);
}

UINT GetTempFileNameA(LPCSTR lpPathName, LPCSTR lpPrefixString, UINT uUnique, LPSTR lpTempFileName)
{
    static unsigned counter = 0;
    char norm[PATH_MAX];
    normalize_path(lpPathName, norm, sizeof(norm));
    char prefix[4] = "tmp";
    if (lpPrefixString) {
        strncpy(prefix, lpPrefixString, 3);
        prefix[3] = '\0';
    }
    if (uUnique != 0) {
        snprintf(lpTempFileName, MAX_PATH, "%s/%s%04x.tmp", norm, prefix, uUnique & 0xffff);
        return uUnique & 0xffff;
    } else {
        for (int retry = 0; retry < 1000; ++retry) {
            unsigned val = (unsigned)(++counter & 0xffff);
            if (val == 0) val = (unsigned)(++counter & 0xffff);
            snprintf(lpTempFileName, MAX_PATH, "%s/%s%04x%04x.tmp", norm, prefix,
                     (unsigned)(getpid() & 0xffff), val);
            int fd = open(lpTempFileName, O_CREAT | O_EXCL | O_RDWR, 0666);
            if (fd >= 0) {
                close(fd);
                return (UINT)val;
            }
            if (errno != EEXIST) {
                set_last_error_from_errno();
                return 0;
            }
        }
        last_error = 80; // ERROR_FILE_EXISTS
        return 0;
    }
}

#endif // !_WIN32
