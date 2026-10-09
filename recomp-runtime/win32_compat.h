#ifndef DOAXBV_RECOMP_WIN32_COMPAT_H
#define DOAXBV_RECOMP_WIN32_COMPAT_H

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <wchar.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

#include <stdlib.h>

#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define MAX_PATH 260

#define WINAPI
#define VOID void
typedef size_t SIZE_T;
typedef unsigned int UINT;

typedef void *HANDLE;
typedef uint32_t DWORD;
typedef int BOOL;
typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef void *LPVOID;
typedef const void *LPCVOID;
typedef char *LPSTR;
typedef const char *LPCSTR;
typedef wchar_t *LPWSTR;
typedef const wchar_t *LPCWSTR;
typedef int32_t LONG;
typedef int64_t LONGLONG;
typedef uint64_t ULONGLONG;

typedef union _LARGE_INTEGER {
    struct {
        DWORD LowPart;
        int32_t HighPart;
    };
    int64_t QuadPart;
} LARGE_INTEGER;

typedef union _ULARGE_INTEGER {
    struct {
        DWORD LowPart;
        DWORD HighPart;
    };
    uint64_t QuadPart;
} ULARGE_INTEGER;

typedef struct _FILETIME {
    DWORD dwLowDateTime;
    DWORD dwHighDateTime;
} FILETIME;

#define TRUE 1
#define FALSE 0
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#define INVALID_SET_FILE_POINTER ((DWORD)-1)

#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define GENERIC_EXECUTE 0x20000000u
#define GENERIC_ALL 0x10000000u

#define FILE_SHARE_READ 1u
#define FILE_SHARE_WRITE 2u
#define FILE_SHARE_DELETE 4u

#define CREATE_NEW 1u
#define CREATE_ALWAYS 2u
#define OPEN_EXISTING 3u
#define OPEN_ALWAYS 4u
#define TRUNCATE_EXISTING 5u

#define FILE_ATTRIBUTE_READONLY 0x00000001u
#define FILE_ATTRIBUTE_HIDDEN 0x00000002u
#define FILE_ATTRIBUTE_SYSTEM 0x00000004u
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010u
#define FILE_ATTRIBUTE_ARCHIVE 0x00000020u
#define FILE_ATTRIBUTE_NORMAL 0x00000080u
#define FILE_ATTRIBUTE_TEMPORARY 0x00000100u
#define FILE_ATTRIBUTE_REPARSE_POINT 0x00000400u

#define FILE_FLAG_BACKUP_SEMANTICS 0x02000000u
#define FILE_FLAG_DELETE_ON_CLOSE 0x04000000u
#define FILE_FLAG_OPEN_REPARSE_POINT 0x00200000u

#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2

#define DELETE 0x00010000u
#define READ_CONTROL 0x00020000u
#define WRITE_DAC 0x00040000u
#define WRITE_OWNER 0x00080000u
#define SYNCHRONIZE 0x00100000u

#define FILE_WRITE_DATA 0x00000002u
#define FILE_WRITE_ATTRIBUTES 0x00000100u
#define FILE_READ_ATTRIBUTES 0x00000080u
#define FILE_GENERIC_READ 0x00120089u

#define ERROR_SUCCESS 0
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_PATH_NOT_FOUND 3
#define ERROR_ACCESS_DENIED 5
#define ERROR_INVALID_HANDLE 6
#define ERROR_SHARING_VIOLATION 32
#define ERROR_FILE_EXISTS 80
#define ERROR_ALREADY_EXISTS 183

static inline HANDLE GetCurrentProcess(void) { return (HANDLE)(intptr_t)-1; }
static inline BOOL TerminateProcess(HANDLE hProcess, DWORD uExitCode) { (void)hProcess; exit((int)uExitCode); return TRUE; }

typedef struct _WIN32_FIND_DATAA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
    DWORD dwReserved0;
    DWORD dwReserved1;
    char cFileName[260];
    char cAlternateFileName[14];
} WIN32_FIND_DATAA;

typedef struct _WIN32_FILE_ATTRIBUTE_DATA {
    DWORD dwFileAttributes;
    FILETIME ftCreationTime;
    FILETIME ftLastAccessTime;
    FILETIME ftLastWriteTime;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
} WIN32_FILE_ATTRIBUTE_DATA;

typedef enum _GET_FILEEX_INFO_LEVELS {
    GetFileExInfoStandard
} GET_FILEEX_INFO_LEVELS;

typedef enum _FILE_INFO_BY_HANDLE_CLASS {
    FileBasicInfo = 0,
    FileStandardInfo = 1,
    FileNameInfo = 2,
    FileRenameInfo = 3,
    FileDispositionInfo = 4,
    FileAllocationInfo = 5,
    FileEndOfFileInfo = 6,
    FileStreamInfo = 7,
    FileCompressionInfo = 8,
    FileAttributeTagInfo = 9,
    FileIdBothDirectoryInfo = 10,
    FileIdBothDirectoryRestartInfo = 11,
    FileIoPriorityHintInfo = 12,
    FileRemoteProtocolInfo = 13,
    FileFullDirectoryInfo = 14,
    FileFullDirectoryRestartInfo = 15,
    FileStorageInfo = 16,
    FileAlignmentInfo = 17,
    FileIdInfo = 18,
    FileIdExtdDirectoryInfo = 19,
    FileIdExtdDirectoryRestartInfo = 20,
    MaximumFileInfoByHandleClass
} FILE_INFO_BY_HANDLE_CLASS;

typedef struct _FILE_BASIC_INFO {
    LARGE_INTEGER CreationTime;
    LARGE_INTEGER LastAccessTime;
    LARGE_INTEGER LastWriteTime;
    LARGE_INTEGER ChangeTime;
    DWORD FileAttributes;
} FILE_BASIC_INFO;

typedef struct _FILE_DISPOSITION_INFO {
    BOOL DeleteFile;
} FILE_DISPOSITION_INFO;

#ifdef __cplusplus
extern "C" {
#endif

DWORD GetLastError(void);
void SetLastError(DWORD err);

HANDLE CreateFileA(
    LPCSTR lpFileName,
    DWORD dwDesiredAccess,
    DWORD dwShareMode,
    void *lpSecurityAttributes,
    DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes,
    HANDLE hTemplateFile);

HANDLE CreateFileW(
    LPCWSTR lpFileName,
    DWORD dwDesiredAccess,
    DWORD dwShareMode,
    void *lpSecurityAttributes,
    DWORD dwCreationDisposition,
    DWORD dwFlagsAndAttributes,
    HANDLE hTemplateFile);

BOOL ReadFile(
    HANDLE hFile,
    LPVOID lpBuffer,
    DWORD nNumberOfBytesToRead,
    DWORD *lpNumberOfBytesRead,
    void *lpOverlapped);

BOOL WriteFile(
    HANDLE hFile,
    LPCVOID lpBuffer,
    DWORD nNumberOfBytesToWrite,
    DWORD *lpNumberOfBytesWritten,
    void *lpOverlapped);

BOOL CloseHandle(HANDLE hObject);

BOOL SetFilePointerEx(
    HANDLE hFile,
    LARGE_INTEGER liDistanceToMove,
    LARGE_INTEGER *lpNewFilePointer,
    DWORD dwMoveMethod);

BOOL GetFileSizeEx(
    HANDLE hFile,
    LARGE_INTEGER *lpFileSize);

BOOL SetEndOfFile(HANDLE hFile);

BOOL FlushFileBuffers(HANDLE hFile);

DWORD GetFileAttributesA(LPCSTR lpFileName);
BOOL GetFileAttributesExA(
    LPCSTR lpFileName,
    GET_FILEEX_INFO_LEVELS fInfoLevelId,
    LPVOID lpFileInformation);

BOOL SetFileAttributesA(
    LPCSTR lpFileName,
    DWORD dwFileAttributes);

BOOL CreateDirectoryA(
    LPCSTR lpPathName,
    void *lpSecurityAttributes);

BOOL RemoveDirectoryA(LPCSTR lpPathName);
BOOL DeleteFileA(LPCSTR lpFileName);

HANDLE FindFirstFileA(
    LPCSTR lpFileName,
    WIN32_FIND_DATAA *lpFindFileData);

BOOL FindNextFileA(
    HANDLE hFindFile,
    WIN32_FIND_DATAA *lpFindFileData);

BOOL FindClose(HANDLE hFindFile);

BOOL LockFile(
    HANDLE hFile,
    DWORD dwFileOffsetLow,
    DWORD dwFileOffsetHigh,
    DWORD nNumberOfBytesToLockLow,
    DWORD nNumberOfBytesToLockHigh);

BOOL UnlockFile(
    HANDLE hFile,
    DWORD dwFileOffsetLow,
    DWORD dwFileOffsetHigh,
    DWORD nNumberOfBytesToUnlockLow,
    DWORD nNumberOfBytesToUnlockHigh);

BOOL GetFileInformationByHandleEx(
    HANDLE hFile,
    FILE_INFO_BY_HANDLE_CLASS FileInformationClass,
    LPVOID lpFileInformation,
    DWORD dwBufferSize);

BOOL SetFileInformationByHandle(
    HANDLE hFile,
    FILE_INFO_BY_HANDLE_CLASS FileInformationClass,
    LPVOID lpFileInformation,
    DWORD dwBufferSize);

DWORD GetTempPathA(DWORD nBufferLength, LPSTR lpBuffer);
UINT GetTempFileNameA(LPCSTR lpPathName, LPCSTR lpPrefixString, UINT uUnique, LPSTR lpTempFileName);

#ifdef __cplusplus
}
#endif

#endif // !_WIN32
#endif // DOAXBV_RECOMP_WIN32_COMPAT_H
