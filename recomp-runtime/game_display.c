#include "game_display.h"
#include "game_scheduler.h"
#include "d3d_split_pose.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Only continuous render inputs are sampled. Pointers, scripts and clocks
   remain at the completed tick. The main loop rolls every display write back. */
enum { FLOATS, ANGLES, MATRIX, MAX_SAMPLES = 8192 };
typedef struct Sample {
    uint32_t address, kind, count;
    uint32_t before[16], after[16];
} Sample;
static Sample samples[MAX_SAMPLES];
static unsigned sample_count, cursor;
static bool continuous;
static uint32_t last_tick;
static uint8_t last_scene;
static float display_alpha;
static bool camera_cut;
static uint8_t camera_mode[5], camera_active[5], actor_type[4], actor_active[4], last_view;
static uint32_t camera_age[5];

bool recomp_game_display_jump(const float before[3], const float after[3], float limit)
{
    double squared = 0;
    for (unsigned i = 0; i < 3; ++i) {
        const double delta = (double)after[i] - before[i];
        if (!isfinite(delta)) return true;
        squared += delta * delta;
    }
    return squared > (double)limit * limit;
}

static void snap(unsigned first, unsigned end)
{
    for (unsigned i = first; i < end; ++i)
        memcpy(samples[i].before, samples[i].after, sizeof samples[i].before);
}

float recomp_game_display_lerp(float a, float b, float alpha)
{
    if (!isfinite(a) || !isfinite(b)) return b;
    if (alpha <= 0) return a;
    if (alpha >= 1) return b;
    return (float)(a + ((double)b - a) * alpha);
}

uint32_t recomp_game_display_angle(uint32_t a, uint32_t b, float alpha)
{
    if (alpha <= 0) return a;
    if (alpha >= 1) return b;
    return (a + (int32_t)((int16_t)(b - a) * alpha)) & 0xffffu;
}

static void sample(uint32_t address, unsigned count, unsigned kind)
{
    if (cursor == MAX_SAMPLES) recomp_stop(1, "game:display-capacity");
    Sample *s = samples + cursor;
    const bool same = continuous && cursor < sample_count &&
        s->address == address && s->count == count && s->kind == kind;
    memcpy(s->before, s->after, sizeof s->before);
    s->address = address; s->count = count; s->kind = kind;
    recomp_guest_load(s->after, address, count * 4u);
    if (!same) memcpy(s->before, s->after, sizeof s->before);
    ++cursor;
}

static uint32_t uv_resources[32], uv_tick;
static unsigned uv_count;
void recomp_game_display_uv(uint32_t resource)
{
    if (!recomp_game_authored_pass()) return;
    if (uv_tick != recomp_game_tick()) { uv_count = 0; uv_tick = recomp_game_tick(); }
    for (unsigned i = 0; i < uv_count; ++i) if (uv_resources[i] == resource) return;
    if (uv_count == 32) recomp_stop(1, "game:display-uv-capacity");
    uv_resources[uv_count++] = resource;
}

/* The SDK UV helper walks format descriptions from guest memory. Keep the
   vertex layout data private and sample just its two UV coordinates. */
static void sample_uv(uint32_t resource)
{
    if (!resource) return;
    const unsigned groups = *recomp_memory_u32(resource + 8u);
    if (groups > 16) recomp_stop(1, "game:display-uv-groups");
    for (unsigned group = 0; group < groups; ++group) {
        const uint32_t record = resource + 0x20u + group * 16u;
        const unsigned count = *recomp_memory_u32(record);
        if (!count) continue;
        const uint32_t vb = *recomp_memory_u32(record + 4u);
        const uint32_t layout = 0x00286ff8u + group * 12u;
        const unsigned prefix = *recomp_memory_u32(layout) + *recomp_memory_u32(layout + 4u);
        const unsigned stride = prefix + *recomp_memory_u32(layout + 8u);
        const uint32_t data = *recomp_memory_u32(vb + 4u);
        for (unsigned vertex = 0; vertex < count; ++vertex)
            sample(data + (vertex * stride + prefix) * 4u, 2, FLOATS);
    }
}

void recomp_game_display_commit(void)
{
    const uint8_t scene = *recomp_memory(0x005dece0u, 1);
    continuous = last_tick + 1 == recomp_game_tick() && scene == last_scene;
    last_tick = recomp_game_tick(); last_scene = scene; cursor = 0;
    sample(0x00416630u, 3, FLOATS);
    sample(0x00416690u, 3, ANGLES);
    if (recomp_game_display_jump((const float *)samples[0].before,
            (const float *)samples[0].after, 2.0f)) snap(0, cursor);
    for (unsigned actor = 0; actor < 4; ++actor) {
        const unsigned first = cursor;
        const uint8_t active = *recomp_memory(0x004d6fd0u + actor, 1);
        const uint8_t type = *recomp_memory(0x004d6ff1u + actor * 0xbcu, 1);
        sample(0x004d700cu + actor * 0xbcu, 3, FLOATS);
        sample(0x004d7004u + actor * 0xbcu, 1, ANGLES);
        for (unsigned bone = 0; bone < 32; ++bone) {
            sample(0x004d3650u + actor * 0x800u + bone * 64u, 16, MATRIX);
            sample(0x004d5700u + actor * 0x400u + bone * 32u, 8, FLOATS);
            sample(0x004d6700u + actor * 0x200u + bone * 16u, 4, FLOATS);
        }
        if (active != actor_active[actor] || type != actor_type[actor] ||
            recomp_game_display_jump((const float *)samples[first].before,
                (const float *)samples[first].after, 0.5f)) snap(first, cursor);
        actor_active[actor] = active; actor_type[actor] = type;
    }
    const uint8_t view = *recomp_memory(0x0041efe7u, 1);
    camera_cut = !continuous || last_view != view;
    last_view = view;
    for (unsigned camera = 0; camera < 5; ++camera) {
        const uint32_t base = 0x0041a800u + camera * 0xae0u;
        const unsigned first = cursor;
        const uint8_t mode = *recomp_memory(base + 0xaccu, 1);
        const uint8_t active = *recomp_memory(base + 0xad8u, 1);
        const uint32_t age = *recomp_memory_u32(base + 0x954u);
        sample(base, 8, FLOATS); sample(base + 32u, 3, ANGLES);
        sample(base + 44u, 3, FLOATS);
        const float *before = (const float *)samples[first].before;
        const float *after = (const float *)samples[first].after;
        float distance = 0;
        for (unsigned i = 0; i < 3; ++i) distance += (after[i] - after[i+4]) * (after[i] - after[i+4]);
        const float limit = fmaxf(1.0f, sqrtf(distance) * 0.25f);
        const bool cut = active != camera_active[camera] || mode != camera_mode[camera] ||
            age < camera_age[camera] || recomp_game_display_jump(before, after, limit) ||
            recomp_game_display_jump(before + 4, after + 4, limit);
        if (cut) snap(first, cursor);
        if (camera == view && cut) camera_cut = true;
        camera_mode[camera] = mode; camera_age[camera] = age; camera_active[camera] = active;
    }
    sample(0x00327cd0u, 4, FLOATS); sample(0x00327d10u, 5, ANGLES);
    sample(0x005bb980u, 4, FLOATS); sample(0x005bb97cu, 1, ANGLES);
    for (unsigned boat = 0; boat < 3; ++boat) {
        sample(0x00327520u + boat * 0x150u, 4, FLOATS);
        sample(0x00327540u + boat * 0x150u, 3, ANGLES);
    }
    for (unsigned record = 0; record < 4; ++record) {
        const uint32_t base = 0x009d5240u + record * 0x58u;
        sample(base + 4, 8, FLOATS); sample(base + 36, 2, ANGLES);
        sample(base + 44, 3, FLOATS);
        const uint32_t work = 0x009d50d8u + record * 0x58u;
        sample(work + 4, 8, FLOATS); sample(work + 36, 2, ANGLES);
        sample(work + 44, 3, FLOATS);
    }
    /* Foliage and decorative instances retain identity through their address. */
    for (unsigned group = 0; group < 2; ++group) {
        uint32_t descriptor = *recomp_memory_u32(0x00326fa0u + group * 4u);
        if (descriptor) {
            const unsigned count = *recomp_memory_u16(descriptor + 16);
            const uint32_t records = *recomp_memory_u32(descriptor);
            for (unsigned i = 0; records && i < count; ++i) {
                sample(records + i * 64u + 44u, 1, ANGLES);
                sample(records + i * 64u + 56u, 1, ANGLES);
            }
        }
        descriptor = *recomp_memory_u32(0x00327408u + group * 4u);
        if (descriptor) {
            const int count = (int16_t)*recomp_memory_u16(descriptor + 4);
            const uint32_t records = *recomp_memory_u32(descriptor);
            for (int i = 0; records && i < count; ++i)
                sample(records + i * 0x2cu + 0x24u, 2, ANGLES);
        }
    }
    if (uv_tick == recomp_game_tick())
        for (unsigned i = 0; i < uv_count; ++i) sample_uv(uv_resources[i]);
    if (camera_cut) snap(0, cursor);
    sample_count = cursor;
    if (camera_cut && getenv("RECOMP_GAME_DISPLAY_TRACE"))
        fprintf(stderr, "hfr-cut tick=%u view=%u mode=%u state=%u\n", recomp_game_tick(),
            view, view < 5 ? camera_mode[view] : 0, *recomp_memory(0x004d56dau, 1));
}

void recomp_game_display_apply(float alpha)
{
    display_alpha = fminf(1, fmaxf(0, alpha));
    for (unsigned n = 0; n < sample_count; ++n) {
        const Sample *s = samples + n;
        uint32_t out[16];
        if (memcmp(s->before, s->after, s->count * 4u) == 0) {
            memcpy(out, s->after, s->count * 4u);
        } else if (s->kind == MATRIX) {
            if (!recomp_split_rigid_matrix((const float *)s->before,
                    (const float *)s->after, display_alpha, (float *)out))
                memcpy(out, s->after, sizeof out);
        } else for (unsigned i = 0; i < s->count; ++i) {
            if (s->kind == ANGLES)
                out[i] = recomp_game_display_angle(s->before[i], s->after[i], display_alpha);
            else {
                float a, b, value;
                memcpy(&a, s->before + i, 4); memcpy(&b, s->after + i, 4);
                value = recomp_game_display_lerp(a, b, display_alpha);
                memcpy(out + i, &value, 4);
            }
        }
        recomp_guest_store(s->address, out, s->count * 4u);
    }
}

void recomp_game_display_trace(bool tick)
{
    static int enabled = -1;
    if (enabled < 0) enabled = getenv("RECOMP_GAME_DISPLAY_TRACE") != NULL;
    if (!enabled) return;
    char line[4096];
    int used = 0;
    const unsigned view = *recomp_memory(0x0041efe7u, 1);
    used += snprintf(line + used, sizeof line - used, "hfr-state tick=%u pass=%u authored=%u alpha=%.9g state=%u cut=%u view=%u",
        recomp_game_tick(), recomp_game_pass(), tick, tick ? 1 : display_alpha,
        *recomp_memory(0x004d56dau, 1), !tick && camera_cut, view);
    const uint32_t ranges[][2] = {{0x00416630u, 3}, {0x004d6ff0u, 188},
        {0x0041a800u + (view < 5 ? view : 0) * 0xae0u, 14}};
    for (unsigned r = 0; r < 3; ++r) {
        used += snprintf(line + used, sizeof line - used, " | ");
        for (unsigned i = 0; i < ranges[r][1]; ++i)
            used += snprintf(line + used, sizeof line - used, "%08x ", *recomp_memory_u32(ranges[r][0] + i * 4u));
    }
    used += snprintf(line + used, sizeof line - used, " | ");
    for (unsigned actor = 0; actor < 4; ++actor) {
        const uint8_t *bones = recomp_memory(0x004d3650u + actor * 0x800u, 0x800u);
        uint32_t hash = 2166136261u;
        for (unsigned i = 0; i < 0x800u; ++i) hash = (hash ^ bones[i]) * 16777619u;
        used += snprintf(line + used, sizeof line - used, "%08x ", hash);
    }
    used += snprintf(line + used, sizeof line - used, " | ");
    for (unsigned actor = 0; actor < 4; ++actor)
        for (unsigned axis = 0; axis < 3; ++axis)
            used += snprintf(line + used, sizeof line - used, "%08x ",
                *recomp_memory_u32(0x004d3680u + actor * 0x800u + axis * 4u));
    snprintf(line + used, sizeof line - used, "\n");
    fputs(line, stderr);
}
