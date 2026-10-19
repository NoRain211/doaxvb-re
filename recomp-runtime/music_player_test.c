#include "music_player.h"

#include <stdio.h>
#include <string.h>

enum {
    BASE = 0x00010000u,
    PLAYER = BASE,
    SETTINGS = BASE + 0x1000u,
    STREAM_A = BASE + 0x2000u,
    STREAM_B = BASE + 0x2100u,
    VTABLE = BASE + 0x2200u,
    STACK_TOP = BASE + 0x3ff0u,
    MEMORY_SIZE = 0x4000u,
    METHODS = 0x00400000u,

    /* Fake stream fields beside the real track field at +4. */
    FAKE_STATE = 0x10u,
    FAKE_LOOPING = 0x14u,

    OPEN = 0x04u, STOP = 0x0cu, SET_LOOP = 0x18u, IS_LOOPING = 0x1cu,
    SET_VOLUME = 0x20u, SET_CHANNEL = 0x28u, STATE = 0x3cu, SET_40 = 0x40u,
    FADE_IN = 0x44u, FADE_OUT = 0x48u, IS_STOPPING = 0x50u,

    ONE = 0x3f800000u,
    HALF = 0x3f000000u
};

typedef struct Call {
    uint32_t stream;
    uint32_t method;
    uint32_t first;
    uint32_t second;
} Call;

static Call calls[32];
static size_t call_count;

static uint32_t guest(uint32_t address)
{
    return *recomp_memory_u32(address);
}

static void put(uint32_t address, uint32_t value)
{
    *recomp_memory_u32(address) = value;
}

/* A stdcall stream method: record it, model the state it reports, and pop
   the return address and arguments as the callee does. */
static void record(uint32_t method, unsigned argc)
{
    uint32_t esp = recomp_runtime.registers.esp;
    uint32_t stream = recomp_runtime.registers.ecx;
    Call call = {stream, method,
        argc > 0u ? guest(esp + 4u) : 0u, argc > 1u ? guest(esp + 8u) : 0u};

    if (call_count < sizeof calls / sizeof calls[0]) {
        calls[call_count] = call;
    }
    ++call_count;
    recomp_runtime.registers.eax = 0u;
    if (method == OPEN) {
        put(stream + 4u, call.first);
        put(stream + FAKE_STATE, 2u);
    } else if (method == STOP) {
        put(stream + FAKE_STATE, 0u);
    } else if (method == SET_LOOP) {
        put(stream + FAKE_LOOPING, call.first);
    } else if (method == STATE) {
        recomp_runtime.registers.eax = guest(stream + FAKE_STATE);
    } else if (method == IS_LOOPING) {
        recomp_runtime.registers.eax = 0xabcd0000u | guest(stream + FAKE_LOOPING);
    }
    recomp_runtime.registers.esp = esp + 4u + 4u * argc;
}

#define METHOD(name, offset, argc) \
    static void name(void) { record(offset, argc); }
METHOD(open_method, OPEN, 1u)
METHOD(stop_method, STOP, 0u)
METHOD(set_loop_method, SET_LOOP, 1u)
METHOD(is_looping_method, IS_LOOPING, 0u)
METHOD(set_volume_method, SET_VOLUME, 1u)
METHOD(set_channel_method, SET_CHANNEL, 2u)
METHOD(state_method, STATE, 0u)
METHOD(set_40_method, SET_40, 1u)
METHOD(fade_in_method, FADE_IN, 1u)
METHOD(fade_out_method, FADE_OUT, 1u)
METHOD(is_stopping_method, IS_STOPPING, 0u)
#undef METHOD

static int play(uint32_t request)
{
    recomp_runtime.registers.esp = STACK_TOP - 8u;
    put(STACK_TOP - 4u, request);
    put(STACK_TOP - 8u, 0u);
    recomp_runtime.registers.eax = PLAYER;
    call_count = 0u;
    recomp_music_play();
    if (recomp_runtime.registers.esp != STACK_TOP) {
        fprintf(stderr, "music player: esp 0x%08x after ret 4\n",
            (unsigned)recomp_runtime.registers.esp);
        return 0;
    }
    return 1;
}

static int expect_calls(const char *name, const Call *expected, size_t count)
{
    if (call_count != count) {
        fprintf(stderr, "music player %s: %zu calls, expected %zu\n", name, call_count, count);
        return 0;
    }
    for (size_t i = 0u; i < count; ++i) {
        if (memcmp(&calls[i], &expected[i], sizeof expected[i]) != 0) {
            fprintf(stderr, "music player %s: call %zu is %08x+%02x(%08x, %08x)\n", name, i,
                (unsigned)calls[i].stream, (unsigned)calls[i].method,
                (unsigned)calls[i].first, (unsigned)calls[i].second);
            return 0;
        }
    }
    return 1;
}

static int expect(const char *name, uint32_t actual, uint32_t wanted)
{
    if (actual != wanted) {
        fprintf(stderr, "music player %s: 0x%08x, expected 0x%08x\n",
            name, (unsigned)actual, (unsigned)wanted);
        return 0;
    }
    return 1;
}

static void add_entry(int index, uint8_t volume, uint8_t fade_out, uint8_t fade_in,
                      uint16_t locations, uint32_t track)
{
    uint8_t *entry = recomp_memory(PLAYER + 4u + (uint32_t)index * 0x10u, 0x10u);

    entry[1] = volume;
    entry[2] = fade_out;
    entry[4] = fade_in;
    memcpy(entry + 6, &locations, sizeof locations);
    memcpy(entry + 12, &track, sizeof track);
}

int recomp_music_player_test(void)
{
    static uint8_t memory[MEMORY_SIZE];
    const RecompMemoryRegion region = {BASE, sizeof memory, memory};
    const RecompFunctionEntry functions[] = {
        {METHODS + OPEN, open_method},
        {METHODS + STOP, stop_method},
        {METHODS + SET_LOOP, set_loop_method},
        {METHODS + IS_LOOPING, is_looping_method},
        {METHODS + SET_VOLUME, set_volume_method},
        {METHODS + SET_CHANNEL, set_channel_method},
        {METHODS + STATE, state_method},
        {METHODS + SET_40, set_40_method},
        {METHODS + FADE_IN, fade_in_method},
        {METHODS + FADE_OUT, fade_out_method},
        {METHODS + IS_STOPPING, is_stopping_method},
    };
    const float one = 1.0f;
    int passed = 1;

    passed &= expect("clamp high", (uint32_t)recomp_music_playlist_clamp(5, 3), 2u);
    passed &= expect("clamp low", (uint32_t)recomp_music_playlist_clamp(-1, 3), 0u);
    passed &= expect("step wraps", (uint32_t)recomp_music_playlist_step(2, 3), 0u);
    passed &= expect("step", (uint32_t)recomp_music_playlist_step(0, 3), 1u);
    passed &= expect("step past end", (uint32_t)recomp_music_playlist_step(7, 3), 2u);
    passed &= expect("step negative", (uint32_t)recomp_music_playlist_step(-4, 3), 0u);

    memset(memory, 0, sizeof memory);
    recomp_runtime_init(&region, 1u, NULL, 0u, functions,
        sizeof functions / sizeof functions[0]);
    for (uint32_t offset = 0u; offset < 0x204u; offset += 4u) {
        recomp_guest_store(SETTINGS + offset, &one, sizeof one);
    }
    for (uint32_t offset = 0u; offset <= IS_STOPPING; offset += 4u) {
        put(VTABLE + offset, METHODS + offset);
    }
    put(STREAM_A, VTABLE);
    put(STREAM_A + 4u, UINT32_MAX);
    put(STREAM_B, VTABLE);
    put(STREAM_B + 4u, UINT32_MAX);
    put(STREAM_B + FAKE_STATE, 2u);

    /* The player's own playlist: entry 1 is not allowed at location 0. */
    put(PLAYER, 3u);
    put(PLAYER + 0x644u, PLAYER + 4u);
    put(PLAYER + 0x64cu, 1u);
    add_entry(0, 100u, 0u, 0u, 0x1u, 0x20u);
    add_entry(1, 100u, 0u, 0u, 0x2u, 0x21u);
    add_entry(2, 50u, 7u, 3u, 0x1u, 0x22u);
    put(PLAYER + 0x654u, SETTINGS);
    put(PLAYER + 0x658u, PLAYER);
    *recomp_memory(PLAYER + 0x67du, 1u) = 1u;
    put(PLAYER + 0x680u, STREAM_A);
    put(PLAYER + 0x698u, STREAM_B);
    recomp_guest_store(PLAYER + 0x6b0u, &one, sizeof one);
    put(PLAYER + 0x6e4u, 5u);

    {
        const Call expected[] = {
            {STREAM_A, SET_LOOP, 0u, 0u}, {STREAM_A, SET_VOLUME, HALF, 0u},
            {STREAM_A, SET_CHANNEL, 0u, ONE}, {STREAM_A, SET_CHANNEL, 1u, ONE},
            {STREAM_A, SET_CHANNEL, 2u, ONE}, {STREAM_A, SET_CHANNEL, 3u, ONE},
            {STREAM_A, SET_CHANNEL, 4u, ONE}, {STREAM_A, SET_CHANNEL, 5u, ONE},
            {STREAM_A, SET_40, 0u, 0u}, {STREAM_A, FADE_IN, 30u, 0u},
            {STREAM_A, FADE_OUT, 70u, 0u}, {STREAM_A, OPEN, 0x22u, 0u},
            {STREAM_B, STATE, 0u, 0u}, {STREAM_B, STOP, 0u, 0u},
        };

        passed &= play(RECOMP_MUSIC_PLAY_PLAYLIST);
        passed &= expect_calls("playlist", expected, sizeof expected / sizeof expected[0]);
        passed &= expect("playlist next", guest(PLAYER + 0x64cu), 0u);
        passed &= expect("playlist playing", guest(PLAYER + 0x650u), 2u);
        passed &= expect("playlist deck", *recomp_memory(PLAYER + 0x67du, 1u), 0u);
        passed &= expect("playlist mode", *recomp_memory(PLAYER + 0x67cu, 1u), 1u);
        passed &= expect("deck play id", guest(PLAYER + 0x684u), 5u);
        passed &= expect("next play id", guest(PLAYER + 0x6e4u), 6u);
        passed &= expect("deck request", guest(PLAYER + 0x688u), RECOMP_MUSIC_PLAY_PLAYLIST);
        passed &= expect("playlist track", guest(PLAYER + 0x6dcu), 0x22u);
    }
    {
        const Call expected[] = {
            {STREAM_B, SET_LOOP, 1u, 0u}, {STREAM_B, SET_VOLUME, ONE, 0u},
            {STREAM_B, SET_CHANNEL, 0u, ONE}, {STREAM_B, SET_CHANNEL, 1u, ONE},
            {STREAM_B, SET_CHANNEL, 2u, ONE}, {STREAM_B, SET_CHANNEL, 3u, ONE},
            {STREAM_B, SET_CHANNEL, 4u, ONE}, {STREAM_B, SET_CHANNEL, 5u, ONE},
            {STREAM_B, SET_40, 0u, 0u}, {STREAM_B, FADE_IN, 0u, 0u},
            {STREAM_B, FADE_OUT, 0u, 0u}, {STREAM_B, OPEN, 7u, 0u},
            {STREAM_A, STATE, 0u, 0u}, {STREAM_A, STOP, 0u, 0u},
        };

        passed &= play(7u);
        passed &= expect_calls("track", expected, sizeof expected / sizeof expected[0]);
        passed &= expect("track mode", *recomp_memory(PLAYER + 0x67cu, 1u), 0u);
        passed &= expect("track deck volume", *recomp_memory(PLAYER + 0x6a5u, 1u), 100u);
        passed &= expect("track request", guest(PLAYER + 0x6d8u), 7u);
        passed &= expect("track playlist track", guest(PLAYER + 0x6dcu), UINT32_MAX);
    }
    {
        /* The same track again keeps the playing, looping stream. */
        const Call expected[] = {
            {STREAM_B, STATE, 0u, 0u}, {STREAM_B, IS_STOPPING, 0u, 0u},
            {STREAM_B, IS_LOOPING, 0u, 0u},
        };

        passed &= play(7u);
        passed &= expect_calls("same track", expected, sizeof expected / sizeof expected[0]);
        passed &= expect("same track deck", *recomp_memory(PLAYER + 0x67du, 1u), 1u);
    }
    passed &= play(UINT32_MAX);
    passed &= expect_calls("negative track", NULL, 0u);

    recomp_runtime_init(NULL, 0u, NULL, 0u, NULL, 0u);
    return passed;
}
