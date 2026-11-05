#include "replay.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(const char *name, bool passed)
{
    if (!passed) {
        fprintf(stderr, "FAIL %s\n", name);
        ++failures;
    }
}

static RecompInputGamepad pad(uint16_t buttons)
{
    RecompInputGamepad value;
    memset(&value, 0, sizeof value);
    value.buttons = buttons;
    // Every field differs, and stays 0 on idle frames.
    for (unsigned i = 0u; i < RECOMP_INPUT_ANALOG_BUTTON_COUNT; ++i) value.analog_buttons[i] = (uint8_t)(buttons * (i + 1u));
    value.thumb_lx = (int16_t)-buttons;
    value.thumb_ly = (int16_t)(buttons * 3);
    value.thumb_rx = (int16_t)(buttons * 5);
    value.thumb_ry = (int16_t)-(buttons * 7);
    return value;
}

/* Frame i's pads: idle for long stretches, then a new pad each frame. */
static void frame_pads(unsigned i, RecompInputGamepad pads[2])
{
    pads[0] = pad(i < 600u ? 0u : (uint16_t)i);
    pads[1] = pad(i < 300u ? 0u : 7u);
}

int main(void)
{
    static uint8_t file[1u << 16];
    static const uint8_t host[] = {1, 2, 3}, joiner[] = {9};
    RecompReplayInfo info;
    memset(&info, 0, sizeof info);
    memset(info.digest, 'a', sizeof info.digest);
    info.player = 1u;
    info.delay = 2u;
    info.flags = 5u;
    info.start_frame = 77u;
    info.blob_size[0] = sizeof host;
    info.blob_size[1] = sizeof joiner;

    RecompReplayWriter writer;
    size_t size = recomp_replay_header(&writer, &info, host, joiner, file);
    check("header size", size == RECOMP_REPLAY_HEADER_BYTES + sizeof host + sizeof joiner);
    enum { FRAMES = 700u };
    for (unsigned i = 0u; i < FRAMES; ++i) {
        RecompInputGamepad pads[2];
        frame_pads(i, pads);
        const size_t added = recomp_replay_frame(&writer, pads, file + size);
        check("frame bound", added <= RECOMP_REPLAY_FRAME_BYTES);
        size += added;
        if (i == 400u) size += recomp_replay_flush(&writer, file + size); // a periodic flush splits a run
    }
    size += recomp_replay_flush(&writer, file + size);
    check("idle frames are small", size < 200u + 100u * (1u + 2u * RECOMP_NETPLAY_PAD_BYTES));

    RecompReplayReader reader;
    RecompReplayInfo read;
    const uint8_t *host_blob = NULL, *joiner_blob = NULL;
    check("opens", recomp_replay_open(&reader, file, size, &read, &host_blob, &joiner_blob));
    check("header round trip", memcmp(read.digest, info.digest, sizeof info.digest) == 0 && read.player == 1u &&
        read.delay == 2u && read.flags == 5u && read.start_frame == 77u);
    check("blobs", read.blob_size[0] == sizeof host && memcmp(host_blob, host, sizeof host) == 0 &&
        read.blob_size[1] == sizeof joiner && memcmp(joiner_blob, joiner, sizeof joiner) == 0);
    unsigned frames = 0u;
    RecompInputGamepad pads[2], want[2];
    bool equal = true;
    while (recomp_replay_next(&reader, pads)) {
        frame_pads(frames++, want);
        equal = equal && memcmp(pads, want, sizeof pads) == 0;
    }
    check("frames round trip", equal && frames == FRAMES);

    // A crash can cut the last frame short; the frames before it still play.
    check("opens cut file", recomp_replay_open(&reader, file, size - 5u, &read, &host_blob, &joiner_blob));
    frames = 0u;
    while (recomp_replay_next(&reader, pads)) ++frames;
    check("cut frame dropped", frames == FRAMES - 1u);

    check("cut header refused", !recomp_replay_open(&reader, file, RECOMP_REPLAY_HEADER_BYTES, &read, &host_blob, &joiner_blob));
    file[4] = 2u;
    check("other version refused", !recomp_replay_open(&reader, file, size, &read, &host_blob, &joiner_blob));

    if (failures != 0) return 1;
    puts("PASS replay files round trip header, blobs and run-length frames");
    return 0;
}
