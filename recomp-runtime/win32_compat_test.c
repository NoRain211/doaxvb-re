#include "win32_compat.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); return 1; } } while (0)

/* The disc's names differ in case from the game's, so lookups must ignore case. */
int main(void)
{
    char root[] = "/tmp/recomp-case-XXXXXX";
    char path[PATH_MAX];
    CHECK(mkdtemp(root) != NULL);
    snprintf(path, sizeof path, "%s/Dir", root);
    CHECK(mkdir(path, 0755) == 0);
    snprintf(path, sizeof path, "%s/Dir/File.TXT", root);
    FILE *file = fopen(path, "w");
    CHECK(file != NULL);
    fclose(file);

    snprintf(path, sizeof path, "%s/dir/file.txt", root);
    CHECK(GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES);
    CHECK(chdir(root) == 0);
    CHECK(GetFileAttributesA("DIR/FILE.txt") != INVALID_FILE_ATTRIBUTES);

    /* A new leaf keeps its own case inside the matched directory. */
    HANDLE handle = CreateFileA("dir/NewLeaf.bin", GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    CHECK(handle != INVALID_HANDLE_VALUE);
    CloseHandle(handle);
    struct stat info;
    CHECK(stat("Dir/NewLeaf.bin", &info) == 0);

    WIN32_FIND_DATAA found;
    HANDLE find = FindFirstFileA("dir/*.TXT", &found);
    CHECK(find != INVALID_HANDLE_VALUE);
    CHECK(strcmp(found.cFileName, "File.TXT") == 0);
    FindClose(find);

    unlink("Dir/NewLeaf.bin");
    unlink("Dir/File.TXT");
    rmdir("Dir");
    CHECK(chdir("/") == 0);
    rmdir(root);
    return 0;
}
