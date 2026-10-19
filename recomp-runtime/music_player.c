#include "music_player.h"
#include "kernel_abi.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* The game's music player is one object at guest 0x00B5DFD8, set up by
   0x000B8C70. It starts with its own playlist, points at the active one,
   and owns stream decks. Music_Play alternates between two decks so the
   previous song fades out while the next one starts. */
enum {
    PLAYER_SETTINGS = 0x654u,      /* float volume factors */
    PLAYER_PLAYLIST = 0x658u,
    PLAYER_PLAYLIST_MODE = 0x67cu, /* u8: the last start used the playlist */
    PLAYER_DECK = 0x67du,          /* u8: deck started last */
    PLAYER_DECKS = 0x680u,         /* two decks of DECK_SIZE */
    PLAYER_VOLUME = 0x6b0u,        /* float */
    PLAYER_TRACK = 0x6d8u,         /* track of the last direct start */
    PLAYER_PLAYLIST_TRACK = 0x6dcu,
    PLAYER_LOCATION = 0x6e0u,      /* s8: 0-7, negative for none */
    PLAYER_PLAY_ID = 0x6e4u,       /* next play id, 1-0x7FFF */

    DECK_SIZE = 0x18u,
    DECK_STREAM = 0x0u,
    DECK_PLAY_ID = 0x4u,
    DECK_REQUEST = 0x8u,           /* track id or RECOMP_MUSIC_PLAY_PLAYLIST */
    DECK_ENTRY = 0xcu,             /* first DECK_ENTRY_SIZE bytes of an entry */
    DECK_ENTRY_SIZE = 0xcu,

    PLAYLIST_COUNT = 0x0u,
    PLAYLIST_INLINE = 0x4u,        /* the array ENTRIES normally points at */
    PLAYLIST_ENTRIES = 0x644u,
    PLAYLIST_NEXT = 0x64cu,
    PLAYLIST_PLAYING = 0x650u,

    ENTRY_SIZE = 0x10u,
    ENTRY_VOLUME = 0x1u,           /* s8 percent */
    ENTRY_FADE_OUT = 0x2u,         /* u8, tens of milliseconds */
    ENTRY_FADE_IN = 0x4u,          /* u8, tens of milliseconds */
    ENTRY_LOCATIONS = 0x6u,        /* u16 mask of locations allowed to play it */
    ENTRY_UNKNOWN_8 = 0x8u,        /* u32, -1 for direct starts */
    ENTRY_TRACK = 0xcu
};

/* Stream objects are the three static players at 0x00CAEF70. Field and
   method names are inferred from how the music player uses them. */
enum {
    STREAM_TRACK = 0x4u,

    STREAM_OPEN = 0x04u,           /* (track id) */
    STREAM_STOP = 0x0cu,
    STREAM_SET_LOOP = 0x18u,       /* (bool) */
    STREAM_IS_LOOPING = 0x1cu,
    STREAM_SET_VOLUME = 0x20u,     /* (float) */
    STREAM_SET_CHANNEL_VOLUME = 0x28u, /* (channel 0-5, float) */
    STREAM_STATE = 0x3cu,
    STREAM_SET_40 = 0x40u,         /* always 0 here; purpose unknown */
    STREAM_SET_FADE_IN = 0x44u,    /* (milliseconds) */
    STREAM_SET_FADE_OUT = 0x48u,   /* (milliseconds) */
    STREAM_IS_STOPPING = 0x50u,

    STREAM_PLAYING = 2
};

/* A direct start with this id prepares the deck without opening a track. */
#define MUSIC_TRACK_NONE 0x7ffffffeu

static uint32_t u32(uint32_t address)
{
    return *recomp_memory_u32(address);
}

static int32_t s32(uint32_t address)
{
    return (int32_t)u32(address);
}

static uint8_t u8(uint32_t address)
{
    return *recomp_memory(address, 1u);
}

static void put_u32(uint32_t address, uint32_t value)
{
    *recomp_memory_u32(address) = value;
}

static void put_u8(uint32_t address, uint8_t value)
{
    *recomp_memory(address, 1u) = value;
}

static double guest_float(uint32_t address)
{
    float value;

    recomp_guest_load(&value, address, sizeof value);
    return value;
}

static uint32_t float_bits(double value)
{
    float narrowed = (float)value;
    uint32_t bits;

    memcpy(&bits, &narrowed, sizeof bits);
    return bits;
}

/* Stream methods are thiscall: ECX is the stream, the callee pops args. */
static uint32_t stream_call(
    uint32_t stream, uint32_t method, const uint32_t *args, unsigned count)
{
    uint32_t target = u32(u32(stream) + method);

    recomp_runtime.registers.ecx = stream;
    kernel_call_guest(target, args, count);
    return recomp_runtime.registers.eax;
}

static uint32_t stream_get(uint32_t stream, uint32_t method)
{
    return stream_call(stream, method, NULL, 0u);
}

/* Boolean methods return in AL. */
static bool stream_test(uint32_t stream, uint32_t method)
{
    return (stream_get(stream, method) & 0xffu) != 0u;
}

static void stream_set(uint32_t stream, uint32_t method, uint32_t value)
{
    stream_call(stream, method, &value, 1u);
}

int recomp_music_playlist_clamp(int index, int count)
{
    if (index > count - 1) {
        return count - 1;
    }
    return index < 0 ? 0 : index;
}

int recomp_music_playlist_step(int index, int count)
{
    return index == count - 1 ? 0 : recomp_music_playlist_clamp(index + 1, count);
}

static uint32_t playlist_entry(uint32_t list, int index)
{
    int slot = recomp_music_playlist_clamp(index, s32(list + PLAYLIST_COUNT));

    return u32(list + PLAYLIST_ENTRIES) + (uint32_t)slot * ENTRY_SIZE;
}

static void playlist_advance(uint32_t list)
{
    int count = s32(list + PLAYLIST_COUNT);

    if (count > 0) {
        put_u32(list + PLAYLIST_NEXT,
            (uint32_t)recomp_music_playlist_step(s32(list + PLAYLIST_NEXT), count));
    }
}

/* True when the next entry may not play at the player's location. */
static bool next_entry_blocked(uint32_t player, uint32_t list)
{
    int8_t location = (int8_t)u8(player + PLAYER_LOCATION);
    uint16_t allowed;

    if (s32(list + PLAYLIST_COUNT) <= 0 || location < 0) {
        return false;
    }
    recomp_guest_load(&allowed,
        playlist_entry(list, s32(list + PLAYLIST_NEXT)) + ENTRY_LOCATIONS,
        sizeof allowed);
    return (allowed & (1u << (location & 31))) == 0u;
}

/* The factor order matches the original so the rounding does too. */
static double master_volume(uint32_t settings)
{
    return guest_float(settings + 0x28u) * guest_float(settings + 0x14u) *
        guest_float(settings + 0x10u) * guest_float(settings + 0x4u) *
        guest_float(settings);
}

static double channel_volume(uint32_t settings, uint32_t channel)
{
    uint32_t offset = channel * 4u;

    return guest_float(settings + 0x1ecu + offset) *
        guest_float(settings + 0x174u + offset) *
        guest_float(settings + 0x15cu + offset) *
        guest_float(settings + 0x114u + offset);
}

static void set_mix(uint32_t player, uint32_t stream, double volume)
{
    uint32_t settings = u32(player + PLAYER_SETTINGS);

    stream_set(stream, STREAM_SET_VOLUME, float_bits(volume));
    for (uint32_t channel = 0u; channel < 6u; ++channel) {
        uint32_t args[2] = {channel, float_bits(channel_volume(settings, channel))};

        stream_call(stream, STREAM_SET_CHANNEL_VOLUME, args, 2u);
    }
    stream_set(stream, STREAM_SET_40, 0u);
}

/* Guest 0x000B8F20, Music_NextPlayId. */
static void next_play_id(uint32_t player)
{
    int32_t id = (int32_t)(u32(player + PLAYER_PLAY_ID) + 1u);

    put_u32(player + PLAYER_PLAY_ID, id < 1 || id > 0x7fff ? 1u : (uint32_t)id);
}

static void begin_deck(uint32_t player, uint32_t deck, uint32_t request)
{
    put_u32(deck + DECK_PLAY_ID, u32(player + PLAYER_PLAY_ID));
    next_play_id(player);
    put_u32(deck + DECK_REQUEST, request);
}

/* A direct start of a track a deck is still playing keeps that stream and
   makes it loop. */
static bool keep_playing(uint32_t player, int32_t track)
{
    for (uint32_t i = 0u; i < 2u; ++i) {
        uint32_t stream = u32(player + PLAYER_DECKS + i * DECK_SIZE + DECK_STREAM);

        if (s32(stream + STREAM_TRACK) == track &&
                stream_get(stream, STREAM_STATE) == STREAM_PLAYING &&
                !stream_test(stream, STREAM_IS_STOPPING)) {
            if (!stream_test(stream, STREAM_IS_LOOPING)) {
                stream_set(stream, STREAM_SET_LOOP, 1u);
            }
            return true;
        }
    }
    return false;
}

static void start_track(uint32_t player, uint32_t deck, uint32_t stream, int32_t track)
{
    uint32_t entry = deck + DECK_ENTRY;

    put_u8(player + PLAYER_PLAYLIST_MODE, 0u);
    memset(recomp_memory(entry, ENTRY_LOCATIONS), 0, ENTRY_LOCATIONS);
    put_u8(entry + ENTRY_VOLUME, 100u);
    recomp_guest_store(entry + ENTRY_LOCATIONS, &(uint16_t){0xffffu}, sizeof(uint16_t));
    put_u32(entry + ENTRY_UNKNOWN_8, UINT32_MAX);
    begin_deck(player, deck, (uint32_t)track);
    stream_set(stream, STREAM_SET_LOOP, 1u);
    set_mix(player, stream,
        master_volume(u32(player + PLAYER_SETTINGS)) * guest_float(player + PLAYER_VOLUME));
    stream_set(stream, STREAM_SET_FADE_IN, 0u);
    stream_set(stream, STREAM_SET_FADE_OUT, 0u);
    if ((uint32_t)track != MUSIC_TRACK_NONE) {
        stream_set(stream, STREAM_OPEN, (uint32_t)track);
    }
    put_u32(player + PLAYER_TRACK, (uint32_t)track);
    put_u32(player + PLAYER_PLAYLIST_TRACK, UINT32_MAX);
    fprintf(stderr, "[music] play track=%" PRId32 "\n", track);
}

static void start_playlist_song(uint32_t player, uint32_t deck, uint32_t stream)
{
    uint32_t list = u32(player + PLAYER_PLAYLIST);
    int count = s32(list + PLAYLIST_COUNT);
    int next;
    double volume;
    uint32_t track;

    put_u8(player + PLAYER_PLAYLIST_MODE, 1u);
    if (count <= 0) {
        stream_get(stream, STREAM_STOP);
        return;
    }
    /* Skip entries this location may not play, trying each at most once. */
    if (next_entry_blocked(player, list)) {
        for (int tried = 1; tried <= count; ++tried) {
            playlist_advance(list);
            if (!next_entry_blocked(player, list)) {
                break;
            }
        }
        if (next_entry_blocked(player, list)) {
            return;
        }
    }
    next = s32(list + PLAYLIST_NEXT);
    memcpy(recomp_memory(deck + DECK_ENTRY, DECK_ENTRY_SIZE),
        recomp_memory(list + PLAYLIST_INLINE +
            (uint32_t)recomp_music_playlist_clamp(next, count) * ENTRY_SIZE,
            DECK_ENTRY_SIZE),
        DECK_ENTRY_SIZE);
    begin_deck(player, deck, RECOMP_MUSIC_PLAY_PLAYLIST);
    stream_set(stream, STREAM_SET_LOOP, 0u);

    volume = (int8_t)u8(deck + DECK_ENTRY + ENTRY_VOLUME) * (double)0.01f;
    volume = volume > 1.0 ? 1.0 : volume < 0.0 ? 0.0 : volume;
    set_mix(player, stream, volume * guest_float(player + PLAYER_VOLUME) *
        master_volume(u32(player + PLAYER_SETTINGS)));
    stream_set(stream, STREAM_SET_FADE_IN, u8(deck + DECK_ENTRY + ENTRY_FADE_IN) * 10u);
    stream_set(stream, STREAM_SET_FADE_OUT, u8(deck + DECK_ENTRY + ENTRY_FADE_OUT) * 10u);

    put_u32(list + PLAYLIST_PLAYING, (uint32_t)next);
    track = u32(playlist_entry(list, next) + ENTRY_TRACK);
    stream_set(stream, STREAM_OPEN, track);
    put_u32(player + PLAYER_PLAYLIST_TRACK, track);
    playlist_advance(list);
    fprintf(stderr, "[music] play playlist entry=%d track=%" PRIu32 "\n", next, track);
}

static void music_play(uint32_t player, int32_t track)
{
    bool from_playlist = (uint32_t)track == RECOMP_MUSIC_PLAY_PLAYLIST;
    bool playlist_mode = u8(player + PLAYER_PLAYLIST_MODE) != 0u;
    uint8_t deck_index;
    uint32_t deck, previous;

    if (from_playlist ? playlist_mode : track < 0) {
        return;
    }
    if (!playlist_mode && keep_playing(player, track)) {
        return;
    }
    deck_index = u8(player + PLAYER_DECK) == 0u;
    put_u8(player + PLAYER_DECK, deck_index);
    deck = player + PLAYER_DECKS + deck_index * DECK_SIZE;
    previous = u32(player + PLAYER_DECKS + (deck_index ^ 1u) * DECK_SIZE + DECK_STREAM);

    if (from_playlist) {
        start_playlist_song(player, deck, u32(deck + DECK_STREAM));
    } else {
        start_track(player, deck, u32(deck + DECK_STREAM), track);
    }
    if (stream_get(previous, STREAM_STATE) == STREAM_PLAYING) {
        stream_get(previous, STREAM_STOP);
    }
}

void recomp_music_play(void)
{
    uint32_t player = recomp_runtime.registers.eax;
    int32_t track = (int32_t)kernel_arg(1u);

    music_play(player, track);
    recomp_runtime.registers.esp += 8u; /* ret 4 */
}
