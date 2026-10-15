#include "screenshot.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#define make_folder(path) _mkdir(path)
#else
#include <sys/stat.h>
#define make_folder(path) mkdir(path, 0755)
#endif

static atomic_bool requested;

void recomp_screenshot_request(void)
{
    atomic_store(&requested, true);
}

bool recomp_screenshot_take_request(void)
{
    return atomic_exchange(&requested, false);
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

    FILE *file = fopen(path, "wb");
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
    if (!written) remove(path);
done:
    free(raw);
    free(zlib);
    return written;
}

bool recomp_screenshot_path(char *path, size_t size)
{
    char folder[1024];
    const char *chosen = getenv("RECOMP_SCREENSHOT_DIR");
    if (chosen != NULL && chosen[0] != '\0') {
        snprintf(folder, sizeof folder, "%s", chosen);
    } else {
#ifdef _WIN32
        const char *home = getenv("USERPROFILE");
#else
        const char *home = getenv("HOME");
#endif
        if (home == NULL || home[0] == '\0') return false;
        char pictures[1024];
        snprintf(pictures, sizeof pictures, "%s/Pictures", home);
        if (make_folder(pictures) != 0 && errno != EEXIST) return false;
        snprintf(folder, sizeof folder, "%s/DOAXBV", pictures);
    }
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
        FILE *existing = fopen(path, "rb");
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
