#include "screenshot.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>
#else
#include <stdatomic.h>
#include <sys/stat.h>
#define make_folder(path) mkdir(path, 0755)
#define open_file(path, mode) fopen(path, mode)
#define remove_file(path) (remove(path) == 0)
#endif

#ifdef _WIN32
/* Paths are UTF-8 here; Windows gets them as UTF-16 so any user name works. */
static bool widen(const char *text, wchar_t *wide, int size)
{
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, wide, size) > 0;
}

static bool narrow(const wchar_t *wide, char *text, size_t size)
{
    return WideCharToMultiByte(CP_UTF8, 0, wide, -1, text, (int)size, NULL, NULL) > 0;
}

static int make_folder(const char *path)
{
    wchar_t wide[1200];
    if (!widen(path, wide, 1200)) {
        errno = EINVAL;
        return -1;
    }
    return _wmkdir(wide);
}

static FILE *open_file(const char *path, const char *mode)
{
    wchar_t wide[1200], wide_mode[8];
    return widen(path, wide, 1200) && widen(mode, wide_mode, 8) ? _wfopen(wide, wide_mode) : NULL;
}

static bool remove_file(const char *path)
{
    wchar_t wide[1200];
    return widen(path, wide, 1200) && _wremove(wide) == 0;
}
#endif

#ifdef _WIN32
static volatile LONG requested;
#else
static atomic_bool requested;
#endif

void recomp_screenshot_request(void)
{
#ifdef _WIN32
    InterlockedExchange(&requested, 1);
#else
    atomic_store(&requested, true);
#endif
}

bool recomp_screenshot_take_request(void)
{
#ifdef _WIN32
    return InterlockedExchange(&requested, 0) != 0;
#else
    return atomic_exchange(&requested, false);
#endif
}

void recomp_screenshot_poll_trigger(void)
{
    static int enabled = -1;
    static char trigger[1024];
    if (enabled < 0) {
#ifdef _WIN32
        /* getenv would give the ANSI code page; the path is kept as UTF-8. */
        const wchar_t *chosen = _wgetenv(L"RECOMP_SCREENSHOT_TRIGGER");
        enabled = chosen != NULL && chosen[0] != L'\0' && narrow(chosen, trigger, sizeof trigger);
#else
        const char *chosen = getenv("RECOMP_SCREENSHOT_TRIGGER");
        enabled = chosen != NULL && chosen[0] != '\0' &&
            (size_t)snprintf(trigger, sizeof trigger, "%s", chosen) < sizeof trigger;
#endif
    }
    if (!enabled) return;
    FILE *file = open_file(trigger, "rb");
    if (file == NULL) return;
    fclose(file);
    /* A file that stays would request a screenshot every frame. */
    if (!remove_file(trigger)) {
        enabled = 0;
        fprintf(stderr, "recomp screenshot: cannot remove the trigger %s; triggers are off\n", trigger);
        return;
    }
    recomp_screenshot_request();
}

static uint32_t crc_table[256];

static uint32_t crc_update(uint32_t crc, const uint8_t *bytes, size_t length)
{
    if (crc_table[1] == 0u) {
        for (uint32_t n = 0u; n < 256u; ++n) {
            uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[n] = c;
        }
    }
    for (size_t i = 0u; i < length; ++i) crc = crc_table[(crc ^ bytes[i]) & 0xFFu] ^ (crc >> 8);
    return crc;
}

static void put32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static bool write_chunk(FILE *file, const char type[4], const uint8_t *data, uint32_t length)
{
    uint8_t header[8];
    uint8_t trailer[4];
    put32(header, length);
    memcpy(header + 4, type, 4);
    uint32_t crc = crc_update(0xFFFFFFFFu, (const uint8_t *)type, 4);
    crc = crc_update(crc, data, length) ^ 0xFFFFFFFFu;
    put32(trailer, crc);
    return fwrite(header, 1, 8, file) == 8 &&
        (length == 0u || fwrite(data, 1, length, file) == length) &&
        fwrite(trailer, 1, 4, file) == 4;
}

bool recomp_screenshot_write_png(const char *path, const uint8_t *bgra,
    uint32_t width, uint32_t height, uint32_t pitch)
{
    if (path == NULL || bgra == NULL || width == 0u || height == 0u ||
        width > 16384u || height > 16384u || pitch < width * 4u) {
        return false;
    }
    /* Each row: a filter byte (0) and RGB. The zlib stream stores it in
       blocks of at most 65535 bytes. */
    const size_t row = 1u + (size_t)width * 3u;
    const size_t raw_size = row * height;
    const size_t blocks = (raw_size + 65534u) / 65535u;
    const size_t zlib_size = 2u + raw_size + blocks * 5u + 4u;
    uint8_t *raw = malloc(raw_size);
    uint8_t *zlib = malloc(zlib_size);
    bool written = false;
    if (raw == NULL || zlib == NULL) goto done;

    for (uint32_t y = 0u; y < height; ++y) {
        uint8_t *out = raw + y * row;
        const uint8_t *in = bgra + (size_t)y * pitch;
        out[0] = 0u;
        for (uint32_t x = 0u; x < width; ++x) {
            out[1u + x * 3u] = in[x * 4u + 2u];
            out[2u + x * 3u] = in[x * 4u + 1u];
            out[3u + x * 3u] = in[x * 4u];
        }
    }

    size_t at = 0u;
    zlib[at++] = 0x78u;
    zlib[at++] = 0x01u;
    uint32_t a = 1u, b = 0u;
    for (size_t done_bytes = 0u; done_bytes < raw_size;) {
        size_t take = raw_size - done_bytes > 65535u ? 65535u : raw_size - done_bytes;
        zlib[at++] = done_bytes + take == raw_size ? 1u : 0u;
        zlib[at++] = (uint8_t)take;
        zlib[at++] = (uint8_t)(take >> 8);
        zlib[at++] = (uint8_t)~take;
        zlib[at++] = (uint8_t)(~take >> 8);
        memcpy(zlib + at, raw + done_bytes, take);
        for (size_t i = 0u; i < take; ++i) {
            a = (a + raw[done_bytes + i]) % 65521u;
            b = (b + a) % 65521u;
        }
        at += take;
        done_bytes += take;
    }
    put32(zlib + at, (b << 16) | a);
    at += 4u;

    FILE *file = open_file(path, "wb");
    if (file == NULL) goto done;
    static const uint8_t signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    uint8_t header[13];
    put32(header, width);
    put32(header + 4, height);
    header[8] = 8u;   /* bits per channel */
    header[9] = 2u;   /* RGB */
    header[10] = header[11] = header[12] = 0u;
    written = fwrite(signature, 1, 8, file) == 8 &&
        write_chunk(file, "IHDR", header, sizeof header) &&
        write_chunk(file, "IDAT", zlib, (uint32_t)at) &&
        write_chunk(file, "IEND", NULL, 0u);
    if (fclose(file) != 0) written = false;
    if (!written) (void)remove_file(path);
done:
    free(raw);
    free(zlib);
    return written;
}

/* RECOMP_SCREENSHOT_DIR, or DOAXBV in the user's Pictures folder. */
static bool screenshot_folder(char *folder, size_t size)
{
    char pictures[1024];
#ifdef _WIN32
    const wchar_t *chosen = _wgetenv(L"RECOMP_SCREENSHOT_DIR");
    if (chosen != NULL && chosen[0] != L'\0') return narrow(chosen, folder, size);
    /* The known folder follows a relocated or OneDrive-backed Pictures. */
    PWSTR known = NULL;
    bool found = SUCCEEDED(SHGetKnownFolderPath(&FOLDERID_Pictures, KF_FLAG_CREATE, NULL, &known)) &&
        narrow(known, pictures, sizeof pictures);
    CoTaskMemFree(known);
    if (!found) return false;
#else
    const char *chosen = getenv("RECOMP_SCREENSHOT_DIR");
    if (chosen != NULL && chosen[0] != '\0') {
        snprintf(folder, size, "%s", chosen);
        return true;
    }
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') return false;
    snprintf(pictures, sizeof pictures, "%s/Pictures", home);
    if (make_folder(pictures) != 0 && errno != EEXIST) return false;
#endif
    snprintf(folder, size, "%s/DOAXBV", pictures);
    return true;
}

bool recomp_screenshot_path(char *path, size_t size)
{
    char folder[1024];
    if (!screenshot_folder(folder, sizeof folder)) return false;
    if (make_folder(folder) != 0 && errno != EEXIST) return false;

    time_t now = time(NULL);
    struct tm local;
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &local);
    for (unsigned n = 0u; n < 100u; ++n) {
        if (n == 0u) snprintf(path, size, "%s/DOAXBV-%s.png", folder, stamp);
        else snprintf(path, size, "%s/DOAXBV-%s-%u.png", folder, stamp, n);
        FILE *existing = open_file(path, "rb");
        if (existing == NULL) return true;
        fclose(existing);
    }
    return false;
}

void recomp_screenshot_save(const uint8_t *bgra, uint32_t width,
    uint32_t height, uint32_t pitch)
{
    char path[1200];
    if (!recomp_screenshot_path(path, sizeof path)) {
        fprintf(stderr, "recomp screenshot: no folder to save in\n");
        return;
    }
    if (recomp_screenshot_write_png(path, bgra, width, height, pitch)) {
        fprintf(stderr, "recomp screenshot: saved %s (%ux%u)\n", path, width, height);
    } else {
        fprintf(stderr, "recomp screenshot: could not write %s\n", path);
    }
}
