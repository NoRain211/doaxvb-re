#include "screenshot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <wchar.h>
#include <windows.h>
#endif

static int failures;

static void check(bool condition, const char *what)
{
    if (!condition) {
        fprintf(stderr, "screenshot test failed: %s\n", what);
        ++failures;
    }
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "screenshot_test.png";
    enum { W = 300, H = 250, PITCH = W * 4 + 8 };
    static uint8_t bgra[H * PITCH];
    for (unsigned y = 0; y < H; ++y) {
        for (unsigned x = 0; x < W; ++x) {
            uint8_t *p = bgra + y * PITCH + x * 4;
            p[0] = (uint8_t)x; p[1] = (uint8_t)y; p[2] = 200; p[3] = 255;
        }
    }

    check(!recomp_screenshot_take_request(), "no request at first");
    recomp_screenshot_request();
    check(recomp_screenshot_take_request(), "a request is taken");
    check(!recomp_screenshot_take_request(), "a request is taken once");

    check(!recomp_screenshot_write_png(path, bgra, W, H, W * 3), "a pitch shorter than a row is refused");
    check(recomp_screenshot_write_png(path, bgra, W, H, PITCH), "a PNG is written");

    FILE *file = fopen(path, "rb");
    unsigned char head[33] = {0};
    long size = 0;
    if (file != NULL) {
        size = (long)fread(head, 1, sizeof head, file);
        fseek(file, 0, SEEK_END);
        size = ftell(file);
        fclose(file);
    }
    static const unsigned char signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    check(memcmp(head, signature, 8) == 0, "the file starts with the PNG signature");
    check(memcmp(head + 12, "IHDR", 4) == 0, "the header chunk comes first");
    check(head[16] == 0 && head[17] == 0 && head[18] == 1 && head[19] == 44, "the width is stored");
    check(head[24] == 8 && head[25] == 2, "8-bit RGB");
    check(size > (long)(W * 3 + 1) * H, "the image data is all there");
    if (argc <= 1) remove(path);

#ifdef _WIN32
    /* A folder outside the ANSI code page, as a non-English user name gives. */
    static const wchar_t folder[] = L"screenshot_test_\u00e9\u4e16";
    _wputenv_s(L"RECOMP_SCREENSHOT_DIR", folder);
    char shot[1200] = {0};
    wchar_t wide[1200] = {0};
    check(recomp_screenshot_path(shot, sizeof shot), "a path under a Unicode folder");
    check(recomp_screenshot_write_png(shot, bgra, W, H, PITCH), "a PNG under a Unicode folder");
    MultiByteToWideChar(CP_UTF8, 0, shot, -1, wide, 1200);
    check(wcsstr(wide, folder) == wide, "the path is the folder's, in UTF-8");
    FILE *unicode = _wfopen(wide, L"rb");
    check(unicode != NULL, "the PNG is under the Unicode folder");
    if (unicode != NULL) fclose(unicode);
    _wremove(wide);
    _wrmdir(folder);
    _wputenv_s(L"RECOMP_SCREENSHOT_DIR", L"");
#endif

    if (failures == 0) printf("screenshot test passed\n");
    return failures == 0 ? 0 : 1;
}
