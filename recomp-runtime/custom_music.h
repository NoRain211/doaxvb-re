#ifndef DOAXBV_RECOMP_CUSTOM_MUSIC_H
#define DOAXBV_RECOMP_CUSTOM_MUSIC_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    RECOMP_MUSIC_RATE = 44100,
    RECOMP_MUSIC_FRAME_BYTES = 4,
    /* Largest single read, and the largest XMO packet the adapter accepts. */
    RECOMP_MUSIC_MAX_READ = 160000,
    /* One Xbox soundtrack holds at most 500 songs. */
    RECOMP_MUSIC_MAX_TRACKS = 500,
    RECOMP_MUSIC_NAME_UNITS = 32
};
typedef struct RecompMusicTrack {
    uint32_t id, duration_ms;
    uint16_t name[RECOMP_MUSIC_NAME_UNITS];
} RecompMusicTrack;
typedef struct RecompMusicDecoder RecompMusicDecoder;

/* Runtime thread only. Scan once at startup; folder is the private UserMusic
   folder, created when missing.
   Originals are opened read-only. No encoded or decoded cache is written.
   Windows uses Media Foundation; without it (Windows N editions), the catalog
   stays empty. Linux uses the pinned miniaudio WAV/MP3/FLAC decoders.
   Close every decoder before recomp_music_shutdown. */
void recomp_music_initialize(const char *folder);
void recomp_music_shutdown(void);
uint32_t recomp_music_count(void);
/* Sum of every track's duration in milliseconds, saturated. */
uint32_t recomp_music_duration(void);
const RecompMusicTrack *recomp_music_track(uint32_t index);
/* Native wide path (UTF-16 on Windows, UTF-32 on Linux).
   NULL when id is not in the catalog. */
const wchar_t *recomp_music_path(uint32_t id);
RecompMusicDecoder *recomp_music_open(uint32_t id);
void recomp_music_close(RecompMusicDecoder *decoder);
/* Fills bytes (a multiple of RECOMP_MUSIC_FRAME_BYTES, at most
   RECOMP_MUSIC_MAX_READ) with signed 16-bit stereo PCM at RECOMP_MUSIC_RATE.
   A short count means end of song; -1 means a decode error, after which the
   decoder fails until rewound. */
int recomp_music_read(RecompMusicDecoder *decoder, void *pcm, uint32_t bytes);
/* Returns to the start of the song. 1 on success, 0 on failure. */
int recomp_music_rewind(RecompMusicDecoder *decoder);

#ifdef __cplusplus
}
#endif
#endif
