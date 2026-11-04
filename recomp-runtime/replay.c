#include "replay.h"

#include <string.h>

static uint8_t *put(uint8_t *out, uint32_t value, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; ++i) *out++ = (uint8_t)(value >> (8u * i));
    return out;
}

static uint32_t get(const uint8_t *in, unsigned bytes)
{
    uint32_t value = 0u;
    for (unsigned i = 0; i < bytes; ++i) value |= (uint32_t)in[i] << (8u * i);
    return value;
}

/* The netplay packet's pad layout. */
static uint8_t *put_pad(uint8_t *out, const RecompInputGamepad *pad)
{
    out = put(out, pad->buttons, 2u);
    memcpy(out, pad->analog_buttons, RECOMP_INPUT_ANALOG_BUTTON_COUNT);
    out += RECOMP_INPUT_ANALOG_BUTTON_COUNT;
    out = put(out, (uint16_t)pad->thumb_lx, 2u);
    out = put(out, (uint16_t)pad->thumb_ly, 2u);
    out = put(out, (uint16_t)pad->thumb_rx, 2u);
    return put(out, (uint16_t)pad->thumb_ry, 2u);
}

static void get_pad(const uint8_t *in, RecompInputGamepad *pad)
{
    memset(pad, 0, sizeof *pad);
    pad->buttons = (uint16_t)get(in, 2u);
    memcpy(pad->analog_buttons, in + 2, RECOMP_INPUT_ANALOG_BUTTON_COUNT);
    pad->thumb_lx = (int16_t)get(in + 10, 2u);
    pad->thumb_ly = (int16_t)get(in + 12, 2u);
    pad->thumb_rx = (int16_t)get(in + 14, 2u);
    pad->thumb_ry = (int16_t)get(in + 16, 2u);
}

size_t recomp_replay_header(RecompReplayWriter *w, const RecompReplayInfo *info,
    const uint8_t *host_blob, const uint8_t *joiner_blob, uint8_t *out)
{
    uint8_t *at = out;
    memcpy(at, "DXBR", 4u);
    at = put(at + 4, RECOMP_REPLAY_VERSION, 2u);
    memcpy(at, info->digest, sizeof info->digest);
    at += sizeof info->digest;
    *at++ = info->player;
    *at++ = info->delay;
    *at++ = info->flags;
    at = put(at, info->start_frame, 4u);
    at = put(at, info->blob_size[0], 4u);
    memcpy(at, host_blob, info->blob_size[0]);
    at = put(at + info->blob_size[0], info->blob_size[1], 4u);
    memcpy(at, joiner_blob, info->blob_size[1]);
    memset(w, 0, sizeof *w);
    return (size_t)(at - out) + info->blob_size[1];
}

size_t recomp_replay_flush(RecompReplayWriter *w, uint8_t *out)
{
    if (w->repeat == 0u) return 0u;
    out[0] = (uint8_t)w->repeat;
    w->repeat = 0u;
    return 1u;
}

static bool same(const RecompInputGamepad *a, const RecompInputGamepad *b)
{
    uint8_t x[2u * RECOMP_NETPLAY_PAD_BYTES], y[2u * RECOMP_NETPLAY_PAD_BYTES];
    put_pad(put_pad(x, &a[0]), &a[1]);
    put_pad(put_pad(y, &b[0]), &b[1]);
    return memcmp(x, y, sizeof x) == 0;
}

size_t recomp_replay_frame(RecompReplayWriter *w, const RecompInputGamepad pads[2], uint8_t *out)
{
    if (same(pads, w->last)) {
        if (++w->repeat < 255u) return 0u;
        return recomp_replay_flush(w, out);
    }
    size_t size = recomp_replay_flush(w, out);
    out[size++] = 0u;
    put_pad(put_pad(out + size, &pads[0]), &pads[1]);
    w->last[0] = pads[0];
    w->last[1] = pads[1];
    return size + 2u * RECOMP_NETPLAY_PAD_BYTES;
}

bool recomp_replay_open(RecompReplayReader *r, const uint8_t *data, size_t size, RecompReplayInfo *info,
    const uint8_t **host_blob, const uint8_t **joiner_blob)
{
    memset(r, 0, sizeof *r);
    memset(info, 0, sizeof *info);
    if (size < RECOMP_REPLAY_HEADER_BYTES || memcmp(data, "DXBR", 4u) != 0 ||
        get(data + 4, 2u) != RECOMP_REPLAY_VERSION)
        return false;
    const uint8_t *at = data + 6;
    memcpy(info->digest, at, sizeof info->digest);
    at += sizeof info->digest;
    info->player = at[0];
    info->delay = at[1];
    info->flags = at[2];
    info->start_frame = get(at + 3, 4u);
    at += 7;
    for (unsigned i = 0; i < 2u; ++i) {
        if ((size_t)(data + size - at) < 4u) return false;
        const uint32_t blob = get(at, 4u);
        at += 4;
        if (blob > RECOMP_NETPLAY_MAX_PROFILE || (size_t)(data + size - at) < blob) return false;
        info->blob_size[i] = blob;
        *(i == 0u ? host_blob : joiner_blob) = at;
        at += blob;
    }
    if (info->player > 1u || info->delay > RECOMP_ROLLBACK_WINDOW) return false;
    r->data = data;
    r->size = size;
    r->at = (size_t)(at - data);
    return true;
}

bool recomp_replay_next(RecompReplayReader *r, RecompInputGamepad pads[2])
{
    while (r->repeat == 0u) {
        if (r->at >= r->size) return false;
        const uint8_t tag = r->data[r->at];
        if (tag != 0u) {
            r->repeat = tag;
            ++r->at;
            break;
        }
        if (r->size - r->at < 1u + 2u * RECOMP_NETPLAY_PAD_BYTES) return false;
        get_pad(r->data + r->at + 1u, &r->last[0]);
        get_pad(r->data + r->at + 1u + RECOMP_NETPLAY_PAD_BYTES, &r->last[1]);
        r->at += 1u + 2u * RECOMP_NETPLAY_PAD_BYTES;
        pads[0] = r->last[0];
        pads[1] = r->last[1];
        return true;
    }
    --r->repeat;
    pads[0] = r->last[0];
    pads[1] = r->last[1];
    return true;
}
