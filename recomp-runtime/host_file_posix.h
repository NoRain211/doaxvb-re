#ifndef RECOMP_HOST_FILE_POSIX_H
#define RECOMP_HOST_FILE_POSIX_H

/* Private file-API seam for kernel_file.c and its tests, not a Win32 runtime.
   Paths accept both separators and resolve ASCII case per component. There is
   no cache, Unicode case folding, or cross-process Windows share-mode support.
   Ambiguous case aliases and symlinks are rejected. Limits remain the guest
   layer's 512-byte path and 127-byte component; this helper allows 4096 bytes.
   Guest attributes and creation/change times require user xattr support.
   Native ctime/birth time cannot be restored; guest queries use saved values. */
#ifndef _WIN32
#include <stdint.h>
#include <stddef.h>
#include <wchar.h>
#include <strings.h>

typedef uint32_t DWORD;
typedef int BOOL;
typedef int32_t LONG;
typedef int64_t LONGLONG;
typedef union LARGE_INTEGER {
    struct { uint32_t LowPart; int32_t HighPart; };
    int64_t QuadPart;
} LARGE_INTEGER;
typedef struct FILETIME { uint32_t dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct FILE_BASIC_INFO {
    LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime;
    DWORD FileAttributes;
} FILE_BASIC_INFO;
typedef struct WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA;
typedef struct WIN32_FIND_DATAA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD nFileSizeHigh, nFileSizeLow;
    char cFileName[256];
} WIN32_FIND_DATAA;
typedef struct RecompPosixFile *HANDLE;
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES UINT32_MAX
#define MAX_PATH 512
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define GENERIC_EXECUTE 0x20000000u
#define GENERIC_ALL 0x10000000u
#define DELETE 0x10000u
#define READ_CONTROL 0x20000u
#define SYNCHRONIZE 0x100000u
#define FILE_GENERIC_READ 0x120089u
#define FILE_READ_ATTRIBUTES 0x80u
#define FILE_WRITE_ATTRIBUTES 0x100u
#define FILE_WRITE_DATA 2u
#define FILE_SHARE_READ 1u
#define FILE_SHARE_WRITE 2u
#define FILE_SHARE_DELETE 4u
#define FILE_ATTRIBUTE_READONLY 1u
#define FILE_ATTRIBUTE_HIDDEN 2u
#define FILE_ATTRIBUTE_SYSTEM 4u
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define FILE_ATTRIBUTE_ARCHIVE 0x20u
#define FILE_ATTRIBUTE_NORMAL 0x80u
#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000u
#define CREATE_NEW 1u
#define CREATE_ALWAYS 2u
#define OPEN_EXISTING 3u
#define OPEN_ALWAYS 4u
#define TRUNCATE_EXISTING 5u
#define FILE_BEGIN 0u
#define FileBasicInfo 0u
#define GetFileExInfoStandard 0u
#define ERROR_SUCCESS 0u
#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_PATH_NOT_FOUND 3u
#define ERROR_ACCESS_DENIED 5u
#define ERROR_SHARING_VIOLATION 32u
#define ERROR_FILE_EXISTS 80u
#define ERROR_ALREADY_EXISTS 183u
#define _stricmp strcasecmp
#define _strnicmp strncasecmp

#ifdef __cplusplus
extern "C" {
#endif
int recomp_posix_resolve_path(const char *path, char *out, size_t capacity);
int recomp_posix_sync_directory(const char *path);
int recomp_posix_replace(const char *temporary, const char *path);
DWORD GetLastError(void);
HANDLE CreateFileA(const char *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
HANDLE CreateFileW(const wchar_t *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
BOOL CloseHandle(HANDLE);
BOOL ReadFile(HANDLE, void *, DWORD, DWORD *, void *);
BOOL WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
BOOL SetFilePointerEx(HANDLE, LARGE_INTEGER, LARGE_INTEGER *, DWORD);
BOOL SetEndOfFile(HANDLE);
BOOL GetFileSizeEx(HANDLE, LARGE_INTEGER *);
BOOL FlushFileBuffers(HANDLE);
BOOL GetFileInformationByHandleEx(HANDLE, unsigned, FILE_BASIC_INFO *, DWORD);
BOOL SetFileInformationByHandle(HANDLE, unsigned, const FILE_BASIC_INFO *, DWORD);
DWORD GetFileAttributesA(const char *);
BOOL GetFileAttributesExA(const char *, unsigned, WIN32_FILE_ATTRIBUTE_DATA *);
BOOL SetFileAttributesA(const char *, DWORD);
BOOL CreateDirectoryA(const char *, void *);
BOOL RemoveDirectoryA(const char *);
BOOL DeleteFileA(const char *);
HANDLE FindFirstFileA(const char *, WIN32_FIND_DATAA *);
BOOL FindNextFileA(HANDLE, WIN32_FIND_DATAA *);
BOOL FindClose(HANDLE);
#ifdef __cplusplus
}
#endif
#endif
#endif
