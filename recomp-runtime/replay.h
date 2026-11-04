#ifndef DOAXBV_RECOMP_REPLAY_H
#define DOAXBV_RECOMP_REPLAY_H

#include "netplay.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A match replay (.dxbr): what a netplay session needs to run again on the
   same build. Little endian: "DXBR", a 2-byte version, the netplay build, the
   local player, input delay and lobby flags, the frame the session started
   at, then the host's and the joiner's profile blobs, each after a 4-byte
   size. Frames follow with both guest ports' pads: a 0 byte and two 18-byte
   pads for a frame with new pads, or a count of 1 to 255 frames that repeat
   the last pads. */
enum {
    RECOMP_REPLAY_VERSION = 1u,
    RECOMP_REPLAY_HEADER_BYTES = 4u + 2u + RECOMP_NETPLAY_DIGEST_BYTES + 3u + 4u + 4u + 4u,
    /* The most one frame, or a flush, adds. */
    RECOMP_REPLAY_FRAME_BYTES = 2u + 2u * RECOMP_NETPLAY_PAD_BYTES,
};

typedef struct RecompReplayInfo {
    char digest[RECOMP_NETPLAY_DIGEST_BYTES];
    uint8_t player, delay, flags;
    uint32_t start_frame;
    uint32_t blob_size[2]; /* the host's, then the joiner's; each at most RECOMP_NETPLAY_MAX_PROFILE */
} RecompReplayInfo;

typedef struct RecompReplayWriter {
    RecompInputGamepad last[2];
    uint32_t repeat; /* frames of the last pads not yet written */
} RecompReplayWriter;

typedef struct RecompReplayReader {
    const uint8_t *data;
    size_t size, at;
    RecompInputGamepad last[2];
    uint32_t repeat; /* frames of the last pads not yet returned */
} RecompReplayReader;

/* Writes RECOMP_REPLAY_HEADER_BYTES and both blobs to out and starts writer. */
size_t recomp_replay_header(RecompReplayWriter *writer, const RecompReplayInfo *info,
    const uint8_t *host_blob, const uint8_t *joiner_blob, uint8_t *out);
/* Each returns the bytes written to out, at most RECOMP_REPLAY_FRAME_BYTES. */
size_t recomp_replay_frame(RecompReplayWriter *writer, const RecompInputGamepad pads[2], uint8_t *out);
size_t recomp_replay_flush(RecompReplayWriter *writer, uint8_t *out);

/* False for another format, version or a damaged header. The blobs point into data. */
bool recomp_replay_open(RecompReplayReader *reader, const uint8_t *data, size_t size, RecompReplayInfo *info,
    const uint8_t **host_blob, const uint8_t **joiner_blob);
/* False at the end, including a frame cut short by a crash. */
bool recomp_replay_next(RecompReplayReader *reader, RecompInputGamepad pads[2]);

#ifdef __cplusplus
}
#endif

#endif
