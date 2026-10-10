#include "dsound_service_adapter.h"
#include "xbox_memory_layout.h"
#include "stop_report.h"
#include "audio_output.h"
#include "xbox_adpcm.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#endif

enum {
    DIRECT_SOUND_CREATE_ADDRESS = 0x001fa27cu,
    DIRECT_SOUND_COMMIT_DEFERRED_SETTINGS_ADDRESS = 0x001f974fu,
    DIRECT_SOUND_DO_WORK_ADDRESS = 0x001f90e0u,
    DIRECT_SOUND_DOWNLOAD_EFFECTS_IMAGE_ADDRESS = 0x001f8f21u,
    DIRECT_SOUND_SET_MIX_BIN_HEADROOM_ADDRESS = 0x001f8f48u,
    DIRECT_SOUND_SET_POSITION_ADDRESS = 0x001f9dd4u,
    DIRECT_SOUND_SET_VELOCITY_ADDRESS = 0x001f9e09u,
    DIRECT_SOUND_MANAGER_GLOBAL = 0x00214708u,
    DIRECT_SOUND_MANAGER_VTABLE = 0x00239decu,
    DIRECT_SOUND_DEVICE_VTABLE = 0x00239e1cu,
    DIRECT_SOUND_APU_VTABLE = 0x00239e44u,
    DIRECT_SOUND_APU_INNER_VTABLE = 0x00239e40u,
    DIRECT_SOUND_APU_PAGE_POOL_VTABLE = 0x00239e98u,
    /* sub_001FB4C2 publishes the addresses of these two counters at APU+0x2F8
       and APU+0x2FC and seeds them. Nothing in this image reads them directly;
       generated code reaches them through the published pointers, so the seed
       values matter even though their meaning is unresolved. */
    DIRECT_SOUND_APU_COUNTER_A_GLOBAL = 0x0021406cu,
    DIRECT_SOUND_APU_COUNTER_B_GLOBAL = 0x00214070u,
    DIRECT_SOUND_APU_COUNTER_A_SEED = 0xc0u,
    DIRECT_SOUND_APU_COUNTER_B_SEED = 0x40u,
    DIRECT_SOUND_VOICE_STATE_GLOBAL = 0x002147e8u,
    DIRECT_SOUND_VOICE_STATE_ALIGNMENT = 0x8000u,
    DIRECT_SOUND_APU_PAGE_POOL_TAG = 0x00214074u,
    DIRECT_SOUND_APU_MIXER_DEVICE_FIELD = 0x34u,
    DIRECT_SOUND_APU_DEVICE_TAIL_FIELD = 0x74u,
    DIRECT_SOUND_APU_PAGE_POOL_BLOCK_LIST = 0x04u,
    DIRECT_SOUND_APU_PAGE_POOL_SECOND_LIST = 0x0cu,
    DIRECT_SOUND_APU_PAGE_POOL_TAG_OFFSET = 0x1cu,
    DIRECT_SOUND_APU_TAIL_LIST_FIRST = 0x728u,
    DIRECT_SOUND_APU_TAIL_LIST_LAST = 0x750u,
};

static RecompDsoundServiceModel dsound_service_model;

typedef struct BufferClock {
    uint32_t address;
    uint32_t data, source_size, format;
    int32_t volume;
    /* Resolved on the game thread; the output pump never calls guest memory
       accessors, which are single-threaded. */
    const uint8_t *pcm;
    RecompDsoundBufferModel model;
    RecompDsoundBufferModel output_model;
} BufferClock;

static BufferClock buffer_clocks[RECOMP_DSOUND_VOICE_STATE_COUNT];

/* Guards buffer_clocks and every host output call; the output pump thread
   shares them with the game thread. Never held across generated code. */
#ifdef _WIN32
static SRWLOCK clock_lock = SRWLOCK_INIT;
static volatile DWORD clock_owner;
#define LOCK_CLOCKS() \
    (AcquireSRWLockExclusive(&clock_lock), clock_owner = GetCurrentThreadId())
#define UNLOCK_CLOCKS() (clock_owner = 0u, ReleaseSRWLockExclusive(&clock_lock))
static inline int is_clock_owner(void) { return clock_owner == GetCurrentThreadId(); }
#else
static pthread_mutex_t clock_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile pthread_t clock_owner;
static volatile int clock_locked = 0;
#define LOCK_CLOCKS() \
    (pthread_mutex_lock(&clock_lock), clock_owner = pthread_self(), clock_locked = 1)
#define UNLOCK_CLOCKS() \
    (clock_locked = 0, pthread_mutex_unlock(&clock_lock))
static inline int is_clock_owner(void) {
    return clock_locked && pthread_equal(clock_owner, pthread_self());
}
#endif

static void retire_buffer(BufferClock *clock)
{
    recomp_audio_output_reset_voice((uint32_t)(clock - buffer_clocks));
    *clock = (BufferClock){0};
}

/* Copies the ring from the last output position up to `now` into host output.
   Guest-facing calls pass `force`: everything up to the cursor the guest is
   about to observe must be copied first, because the guest's streaming
   decoder refills the ring right behind that cursor. The background pump
   batches into >= 10 ms chunks instead. */
static void pump_buffer(BufferClock *clock, uint64_t now, int force)
{
    RecompDsoundBufferModel *output = &clock->output_model;
    if (!output->playing || clock->pcm == NULL || now <= output->last_ms ||
        (!force && now - output->last_ms < 10u)) {
        return;
    }
    uint32_t channels = (clock->format >> 16u) & 0xffu;
    uint32_t bits = clock->format >> 24u;
    int adpcm = (clock->format & 0xffffu) == 0x69u;
    uint32_t offset = 0u;
    uint32_t bytes = recomp_dsound_buffer_consume(output, now, &offset);
    if (bytes == 0u) {
        return;
    }
    /* Host output owns its copy; the decoder may refill the guest ring immediately. */
    uint8_t pcm[80000];
    if (bytes > sizeof pcm) {
        return;
    }
    if (adpcm) {
        uint32_t block_bytes = XBOX_ADPCM_BLOCK_BYTES * channels;
        uint32_t decoded_bytes = XBOX_ADPCM_BLOCK_SAMPLES * channels * 2u;
        for (uint32_t written = 0u; written < bytes;) {
            uint8_t block[XBOX_ADPCM_BLOCK_BYTES * 2];
            int16_t decoded[XBOX_ADPCM_BLOCK_SAMPLES * 2];
            uint32_t within = offset % decoded_bytes;
            uint32_t count = decoded_bytes - within;
            if (count > bytes - written) count = bytes - written;
            memcpy(block, clock->pcm + offset / decoded_bytes * block_bytes,
                block_bytes);
            if (!xbox_adpcm_decode_block(block, block_bytes, channels,
                    decoded, XBOX_ADPCM_BLOCK_SAMPLES * 2u)) {
                output->playing = 0u;
                return;
            }
            memcpy(pcm + written, (uint8_t *)decoded + within, count);
            written += count;
            offset += count;
            if (offset == output->size_bytes) offset = output->loop_start_bytes;
        }
        bits = 16u;
    } else {
        for (uint32_t written = 0u; written < bytes;) {
            uint32_t count = output->size_bytes - offset;
            if (count > bytes - written) count = bytes - written;
            memcpy(pcm + written, clock->pcm + offset, count);
            written += count;
            offset += count;
            if (offset == output->size_bytes) offset = output->loop_start_bytes;
        }
    }
    recomp_audio_output_submit((uint32_t)(clock - buffer_clocks), pcm, bytes,
        output->sample_rate, channels, bits, clock->volume);
}

static uint64_t buffer_now_ms(void)
{
#ifdef RECOMP_DSOUND_TEST_CLOCK
    uint64_t recomp_test_dsound_now_ms(void);
    return recomp_test_dsound_now_ms();
#elif defined(_WIN32)
    return GetTickCount64();
#elif defined(__APPLE__) || defined(_POSIX_TIMERS)
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
#else
    struct timespec now;
    timespec_get(&now, TIME_UTC);
    return (uint64_t)now.tv_sec * 1000u + (uint64_t)now.tv_nsec / 1000000u;
#endif
}

static BufferClock *find_buffer_clock(uint32_t address)
{
    for (size_t i = 0; i < sizeof buffer_clocks / sizeof buffer_clocks[0]; ++i) {
        if (buffer_clocks[i].address == address && address != 0u) {
            return &buffer_clocks[i];
        }
    }
    return NULL;
}

static BufferClock *buffer_clock(uint32_t address)
{
    BufferClock *clock = find_buffer_clock(address);
    if (address == 0u) return NULL;
    uint32_t settings = *recomp_memory_u32(address);
    uint32_t size = *recomp_memory_u32(settings + 0xbcu);
    uint32_t data = *recomp_memory_u32(settings + 0xb8u);
    uint32_t format = *recomp_memory_u32(settings + 0xcu);
    uint32_t tag = format & 0xffffu;
    uint32_t channels = (format >> 16u) & 0xffu;
    uint32_t bits = format >> 24u;
    uint32_t align = *recomp_memory_u32(settings + 0x14u);
    uint32_t rate = *recomp_memory_u32(settings + 0x10u);
    uint32_t loop_start = *recomp_memory_u32(settings + 0xc8u);
    if (clock != NULL && (clock->data != data || clock->source_size != size ||
            clock->format != format)) {
        retire_buffer(clock);
        clock = NULL;
    }
    /* ponytail: full play regions and tail loops cover the active route;
       extend non-tail loop ends when an observed sound needs them. */
    if ((tag != 1u && tag != 0x69u) || size == 0u ||
        *recomp_memory_u32(settings + 0xc0u) != 0u ||
        *recomp_memory_u32(settings + 0xc4u) != size ||
        loop_start >= size ||
        *recomp_memory_u32(settings + 0xccu) != size - loop_start) {
        if (clock != NULL) retire_buffer(clock);
        return NULL;
    }
    if (channels < 1u || channels > 2u || rate < 1000u || rate > 200000u ||
        (tag == 1u && ((bits != 8u && bits != 16u) || align != channels * bits / 8u)) ||
        (tag == 0x69u && (bits != 4u || align != XBOX_ADPCM_BLOCK_BYTES * channels)) ||
        align == 0u || size % align != 0u || loop_start % align != 0u) {
        return NULL;
    }
    uint64_t decoded_size = tag == 0x69u
        ? (uint64_t)(size / align) * XBOX_ADPCM_BLOCK_SAMPLES * channels * 2u : size;
    if (decoded_size > UINT32_MAX) return NULL;
    uint32_t decoded_loop_start = tag == 0x69u
        ? loop_start / align * XBOX_ADPCM_BLOCK_SAMPLES * channels * 2u : loop_start;
    if (clock != NULL) {
        if (clock->model.loop_start_bytes != decoded_loop_start) {
            uint64_t now = buffer_now_ms();
            pump_buffer(clock, now, 1);
            recomp_dsound_buffer_cursor(&clock->model, now);
            recomp_audio_output_reset_voice((uint32_t)(clock - buffer_clocks));
            clock->model.loop_start_bytes = decoded_loop_start;
            clock->output_model = clock->model;
        }
        clock->volume = (int32_t)*recomp_memory_u32(settings + 0x1cu);
        return clock;
    }
    for (size_t i = 0; i < sizeof buffer_clocks / sizeof buffer_clocks[0]; ++i) {
        if (buffer_clocks[i].address == 0u) {
            clock = &buffer_clocks[i];
            if (recomp_dsound_buffer_configure(&clock->model, (uint32_t)decoded_size,
                    rate, tag == 0x69u ? channels * 2u : align, buffer_now_ms()) != 0u) {
                return NULL;
            }
            clock->address = address;
            clock->data = data;
            clock->source_size = size;
            clock->format = format;
            clock->volume = (int32_t)*recomp_memory_u32(settings + 0x1cu);
            clock->model.loop_start_bytes = decoded_loop_start;
            clock->output_model = clock->model;
            return clock;
        }
    }
    recomp_stop(2, "dsound-buffer:voice-capacity");
    return NULL;
}

static void original_buffer_call(uint32_t address)
{
#ifdef RECOMP_FULL_PROGRAM
    void sub_001F8FD8(void);
    void sub_001F8FFC(void);
    void sub_001F9014(void);
    void sub_001F9058(void);
    void sub_001F9074(void);
    void sub_001F9094(void);
    void sub_001F9767(void);
    void sub_001F84B0(void);
    void sub_001F9E5E(void);
    switch (address) {
    case 0x001f8fd8u: sub_001F8FD8(); return;
    case 0x001f8ffcu: sub_001F8FFC(); return;
    case 0x001f9014u: sub_001F9014(); return;
    case 0x001f9058u: sub_001F9058(); return;
    case 0x001f9074u: sub_001F9074(); return;
    case 0x001f9094u: sub_001F9094(); return;
    case 0x001f9767u: sub_001F9767(); return;
    case 0x001f84b0u: sub_001F84B0(); return;
    case 0x001f9e5eu: sub_001F9E5E(); return;
    }
#elif defined(RECOMP_DSOUND_TEST_CLOCK)
    if (address == 0x001f9e5eu) {
        void recomp_test_dsound_set_data(void);
        recomp_test_dsound_set_data();
        return;
    }
#endif
    recomp_stop(2, "dsound-buffer:original-unavailable:%08" PRIx32, address);
}

static uint32_t stack_argument(uint32_t entry_esp, uint32_t index)
{
    return *recomp_memory_u32(entry_esp + 4u + index * 4u);
}

static float stack_float(uint32_t entry_esp, uint32_t index)
{
    uint32_t bits = stack_argument(entry_esp, index);
    float value;

    memcpy(&value, &bits, sizeof value);
    return value;
}

static void buffer_call(uint32_t operation, uint32_t argument_count)
{
    uint32_t entry = recomp_runtime.registers.esp;
    uint32_t address = stack_argument(entry, 0u);
    BufferClock *clock;
    uint32_t result = RECOMP_DSOUND_OK;
    uint64_t now;
    int continue_output = 0;
    static uint32_t reported_plays;

    if (address == 0u) {
        recomp_runtime.registers.eax = RECOMP_DSOUND_POINTER_ERROR;
        recomp_runtime.registers.esp = entry + 4u + argument_count * 4u;
        return;
    }
    if (operation == 0x001f9e5eu) {
        clock = find_buffer_clock(address);
        original_buffer_call(operation);
        uint32_t settings = *recomp_memory_u32(address);
        if (clock != NULL && (clock->data != *recomp_memory_u32(settings + 0xb8u) ||
                clock->source_size != *recomp_memory_u32(settings + 0xbcu))) {
            LOCK_CLOCKS();
            retire_buffer(clock);
            UNLOCK_CLOCKS();
        }
        return;
    }
    if (operation == 0x001f84b0u) {
        clock = find_buffer_clock(address);
        original_buffer_call(operation);
        if (clock != NULL && recomp_runtime.registers.eax == 0u) {
            LOCK_CLOCKS();
            retire_buffer(clock);
            UNLOCK_CLOCKS();
        }
        return;
    }
    LOCK_CLOCKS();
    clock = buffer_clock(address);
    if (operation == 0x001f8fd8u && reported_plays++ < 16u) {
        uint32_t settings = *recomp_memory_u32(address);
        fprintf(stderr, "recomp audio: Play buffer=%08" PRIx32
            " format=%08" PRIx32 " bytes=%" PRIu32 " rate=%" PRIu32
            " flags=%" PRIu32 " pcm_clock=%u\n", address,
            *recomp_memory_u32(settings + 0xcu),
            *recomp_memory_u32(settings + 0xbcu),
            *recomp_memory_u32(settings + 0x10u), stack_argument(entry, 3u),
            clock != NULL);
    }
    if (clock == NULL) {
        UNLOCK_CLOCKS();
        original_buffer_call(operation);
        return;
    }
    now = buffer_now_ms();
    pump_buffer(clock, now, 1);
    switch (operation) {
    case 0x001f8fd8u:
        recomp_dsound_buffer_cursor(&clock->model, now);
        continue_output = clock->model.playing && clock->output_model.playing &&
            !(stack_argument(entry, 3u) & RECOMP_DSOUND_PLAY_FROMSTART);
        result = recomp_dsound_buffer_play(
            &clock->model, stack_argument(entry, 3u), now);
        break;
    case 0x001f8ffcu:
        recomp_dsound_buffer_stop(&clock->model, now);
        break;
    case 0x001f9014u:
        /* StopEx(buffer, timestamp, flags). ENVELOPE|RELEASEWAVEFORM on a
           loop exits the loop and plays to the end; other flags stop now.
           ponytail: the game's callers pass a zero timestamp and no envelope;
           schedule timestamps and model release envelopes if a caller needs them. */
        if ((stack_argument(entry, 3u) & 3u) == 3u && clock->model.playing &&
            (clock->model.play_flags & RECOMP_DSOUND_PLAY_LOOPING)) {
            recomp_dsound_buffer_cursor(&clock->model, now);
            clock->model.play_flags &= ~RECOMP_DSOUND_PLAY_LOOPING;
            continue_output = clock->model.playing && clock->output_model.playing;
        } else {
            recomp_dsound_buffer_stop(&clock->model, now);
        }
        break;
    case 0x001f9058u: {
        uint32_t output = stack_argument(entry, 1u);
        recomp_dsound_buffer_cursor(&clock->model, now);
        if (output == 0u) {
            result = RECOMP_DSOUND_POINTER_ERROR;
        } else {
            *recomp_memory_u32(output) = clock->model.playing
                ? 1u | ((clock->model.play_flags & 1u) ? 4u : 0u) : 0u;
        }
        break;
    }
    case 0x001f9074u: {
        uint32_t cursor = recomp_dsound_buffer_cursor(&clock->model, now);
        for (uint32_t i = 1u; i <= 2u; ++i) {
            uint32_t output = stack_argument(entry, i);
            if (output != 0u) {
                /* Output queues lag this clock; they never drive guest progress. */
                *recomp_memory_u32(output) = (clock->format & 0xffffu) == 0x69u
                    ? cursor / (XBOX_ADPCM_BLOCK_SAMPLES * clock->model.block_align) *
                        (XBOX_ADPCM_BLOCK_BYTES * clock->model.block_align / 2u)
                    : cursor;
            }
        }
        break;
    }
    case 0x001f9094u: {
        uint32_t position = stack_argument(entry, 1u);
        if ((clock->format & 0xffffu) == 0x69u) {
            uint32_t block = XBOX_ADPCM_BLOCK_BYTES * clock->model.block_align / 2u;
            if (position >= clock->source_size || position % block != 0u) {
                result = RECOMP_DSOUND_INVALID_PARAM;
                break;
            }
            position = position / block * XBOX_ADPCM_BLOCK_SAMPLES * clock->model.block_align;
        }
        result = recomp_dsound_buffer_set_position(&clock->model, position, now);
        break;
    }
    case 0x001f9767u:
        result = recomp_dsound_buffer_set_frequency(
            &clock->model, stack_argument(entry, 1u), now);
        break;
    }
    if (result == RECOMP_DSOUND_OK && operation != 0x001f9058u &&
        operation != 0x001f9074u) {
        if (!continue_output) {
            recomp_audio_output_reset_voice((uint32_t)(clock - buffer_clocks));
        }
        if (continue_output) {
            clock->output_model.play_flags = clock->model.play_flags;
        } else {
            clock->output_model = clock->model;
        }
    }
    if (operation == 0x001f8fd8u && result == RECOMP_DSOUND_OK &&
        clock->pcm == NULL && clock->data != 0u &&
        clock->data <= UINT32_MAX - clock->source_size) {
        clock->pcm = recomp_memory(clock->data, clock->source_size);
    }
    UNLOCK_CLOCKS();
    recomp_runtime.registers.eax = result;
    recomp_runtime.registers.esp = entry + 4u + argument_count * 4u;
}

static void buffer_play(void) { buffer_call(0x001f8fd8u, 4u); }
static void buffer_stop(void) { buffer_call(0x001f8ffcu, 1u); }
static void buffer_stop_ex(void) { buffer_call(0x001f9014u, 4u); }
static void buffer_status(void) { buffer_call(0x001f9058u, 2u); }
static void buffer_position(void) { buffer_call(0x001f9074u, 3u); }
static void buffer_seek(void) { buffer_call(0x001f9094u, 2u); }
static void buffer_frequency(void) { buffer_call(0x001f9767u, 2u); }
static void buffer_set_data(void) { buffer_call(0x001f9e5eu, 3u); }
static void buffer_release(void) { buffer_call(0x001f84b0u, 1u); }

/* A list the constructors leave empty: both links point at the entry itself,
   which is how the generated walkers recognise the end. */
static void write_empty_list(uint32_t entry)
{
    *recomp_memory_u32(entry) = entry;
    *recomp_memory_u32(entry + 4u) = entry;
}

/* The 0x7E0-byte CMcpxAPU that sub_001FB4C2 builds and sub_001FA0E6 links at
   manager+0xC. Without it a sound buffer's APU pointer is zero, and the page
   walk in CMcpxBuffer_Play starts from 0x300 and runs off into unmapped
   memory. Only the constructor's own writes are reproduced, including the
   page pool sub_001FE73B builds at APU+0x300. The sub-objects that need
   their own allocations (APU+0x58) and the hardware bring-up in
   sub_001FBF1C are deliberately absent. */
static void write_apu_object(const RecompDsoundServiceModel *model)
{
    uint32_t apu = model->apu;
    uint32_t page_pool = apu + RECOMP_DSOUND_APU_PAGE_POOL_OFFSET;
    uint32_t offset;

    recomp_guest_memset(apu, 0, RECOMP_DSOUND_APU_SIZE);
    *recomp_memory_u32(apu) = DIRECT_SOUND_APU_VTABLE;
    *recomp_memory_u32(apu + 4u) = 1u;
    *recomp_memory_u32(apu + RECOMP_DSOUND_APU_INNER_OFFSET) =
        DIRECT_SOUND_APU_INNER_VTABLE;
    *recomp_memory_u32(apu + RECOMP_DSOUND_APU_DEVICE_OFFSET) = model->device;
    *recomp_memory_u32(apu + RECOMP_DSOUND_APU_MIXER_DEVICE_OFFSET) =
        model->device + DIRECT_SOUND_APU_MIXER_DEVICE_FIELD;
    *recomp_memory_u32(apu + RECOMP_DSOUND_APU_DEVICE_TAIL_OFFSET) =
        model->device + DIRECT_SOUND_APU_DEVICE_TAIL_FIELD;
    *recomp_memory_u32(apu + RECOMP_DSOUND_APU_COUNTER_A_POINTER_OFFSET) =
        DIRECT_SOUND_APU_COUNTER_A_GLOBAL;
    *recomp_memory_u32(apu + RECOMP_DSOUND_APU_COUNTER_B_POINTER_OFFSET) =
        DIRECT_SOUND_APU_COUNTER_B_GLOBAL;
    *recomp_memory_u32(DIRECT_SOUND_APU_COUNTER_A_GLOBAL) =
        DIRECT_SOUND_APU_COUNTER_A_SEED;
    *recomp_memory_u32(DIRECT_SOUND_APU_COUNTER_B_GLOBAL) =
        DIRECT_SOUND_APU_COUNTER_B_SEED;

    /* sub_001FE73B. The largest-free-block cache at pool+0x18 stays zero, as
       the constructor leaves it, so sub_001FE808 finds no pages and returns
       null rather than mapping anything. Play then fails with
       DSERR_OUTOFMEMORY instead of faulting. */
    *recomp_memory_u32(page_pool) = DIRECT_SOUND_APU_PAGE_POOL_VTABLE;
    write_empty_list(page_pool + DIRECT_SOUND_APU_PAGE_POOL_BLOCK_LIST);
    write_empty_list(page_pool + DIRECT_SOUND_APU_PAGE_POOL_SECOND_LIST);
    *recomp_memory_u32(page_pool + DIRECT_SOUND_APU_PAGE_POOL_TAG_OFFSET) =
        DIRECT_SOUND_APU_PAGE_POOL_TAG;

    /* Six consecutive empty lists; the original writes three in a loop and
       the remaining three one at a time. */
    for (offset = DIRECT_SOUND_APU_TAIL_LIST_FIRST;
         offset <= DIRECT_SOUND_APU_TAIL_LIST_LAST;
         offset += 8u) {
        write_empty_list(apu + offset);
    }
}

/* sub_00200468 descriptor 3 allocates this 0x8000-byte, 0x8000-aligned
   software voice-state table and publishes it at 0x002147E8. sub_001FFBD6
   then seeds the voice index at +0x7C in each 0x80-byte record. Generated
   stream cleanup uses the table even under the no-audio policy. */
static void write_voice_state_table(const RecompDsoundServiceModel *model)
{
    uint32_t index;

    recomp_guest_memset(
        model->voice_state_table, 0, RECOMP_DSOUND_VOICE_STATE_TABLE_SIZE);
    for (index = 0u; index < RECOMP_DSOUND_VOICE_STATE_COUNT; ++index) {
        *recomp_memory_u32(
            model->voice_state_table + index * RECOMP_DSOUND_VOICE_STATE_SIZE +
            RECOMP_DSOUND_VOICE_STATE_INDEX_OFFSET) = index;
    }
    *recomp_memory_u32(DIRECT_SOUND_VOICE_STATE_GLOBAL) =
        model->voice_state_table;
}

static void write_created_objects(const RecompDsoundServiceModel *model)
{
    uint32_t list_head =
        model->manager + RECOMP_DSOUND_MANAGER_LIST_FORWARD_OFFSET;

    /* Current generated consumers require the sub_001F855B manager header and
       the sub_001F987D device header. sub_001FA106 links the device at
       manager+8; the returned public interface is that cell. */
    recomp_guest_memset(model->manager, 0, RECOMP_DSOUND_MANAGER_SIZE);
    recomp_guest_memset(model->device, 0, RECOMP_DSOUND_DEVICE_SIZE);
    *recomp_memory_u32(model->manager) = DIRECT_SOUND_MANAGER_VTABLE;
    *recomp_memory_u32(model->manager + 4u) =
        model->manager_reference_count;
    *recomp_memory_u32(
        model->manager + RECOMP_DSOUND_MANAGER_DEVICE_OFFSET) = model->device;
    *recomp_memory_u32(
        model->manager + RECOMP_DSOUND_MANAGER_APU_OFFSET) = model->apu;
    *recomp_memory_u32(
        model->manager + RECOMP_DSOUND_MANAGER_LIST_FORWARD_OFFSET) =
        list_head;
    *recomp_memory_u32(
        model->manager + RECOMP_DSOUND_MANAGER_LIST_BACK_OFFSET) = list_head;
    *recomp_memory_u32(model->device) = DIRECT_SOUND_DEVICE_VTABLE;
    *recomp_memory_u32(model->device + 4u) =
        model->device_reference_count;
    /* sub_001FB8A7 tests device+0xC against 0xFFFFFFFF before it uses the
       effects path. The guest writes that sentinel when no effects image is
       loaded (sub_001E47A4); a memset leaves 0, which reads as a live handle
       and sends sub_001FB36B chasing the null object at apu+0x14. */
    *recomp_memory_u32(
        model->device + RECOMP_DSOUND_DEVICE_EFFECTS_HANDLE_OFFSET) =
        RECOMP_DSOUND_DEVICE_EFFECTS_HANDLE_NONE;
    write_apu_object(model);
    write_voice_state_table(model);
    *recomp_memory_u32(DIRECT_SOUND_MANAGER_GLOBAL) = model->manager;
}

#if defined(RECOMP_FULL_PROGRAM)
#ifdef _WIN32
static volatile LONG output_pump_stop;
static HANDLE output_pump;

/* Feeds host output while the game thread is busy, as the APU plays from
   guest rings on its own. A frame hitch then delays only the guest's refills,
   not audio it has already written. */
static DWORD WINAPI output_pump_loop(void *unused)
{
    (void)unused;
    while (!output_pump_stop) {
        Sleep(5u);
        LOCK_CLOCKS();
        uint64_t now = buffer_now_ms();
        for (uint32_t i = 0u; i < RECOMP_DSOUND_VOICE_STATE_COUNT; ++i) {
            if (buffer_clocks[i].address != 0u) pump_buffer(&buffer_clocks[i], now, 0);
        }
        UNLOCK_CLOCKS();
    }
    return 0u;
}

static void stop_output_pump(void)
{
    output_pump_stop = 1;
    /* Output teardown follows this handler, so the pump must be out of its
       pass. Exiting under the lock (recomp_stop in a locked adapter path)
       already parks it for good; joining then would deadlock. */
    if (clock_owner != GetCurrentThreadId()) {
        WaitForSingleObject(output_pump, INFINITE);
    }
}

static void start_output_pump(void)
{
    if (output_pump != NULL || !recomp_audio_output_enabled()) return;
    output_pump = CreateThread(NULL, 0u, output_pump_loop, NULL, 0u, NULL);
    if (output_pump != NULL) {
        SetThreadPriority(output_pump, THREAD_PRIORITY_TIME_CRITICAL);
        atexit(stop_output_pump);
    }
}
#else
static atomic_int output_pump_stop;
static pthread_t output_pump;
static int output_pump_started;

static void *output_pump_loop(void *unused)
{
    (void)unused;
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    struct timespec sleep_time = { .tv_sec = 0, .tv_nsec = 5 * 1000 * 1000 }; /* 5ms */
    while (!atomic_load(&output_pump_stop)) {
        nanosleep(&sleep_time, NULL);
        LOCK_CLOCKS();
        uint64_t now = buffer_now_ms();
        for (uint32_t i = 0u; i < RECOMP_DSOUND_VOICE_STATE_COUNT; ++i) {
            if (buffer_clocks[i].address != 0u) pump_buffer(&buffer_clocks[i], now, 0);
        }
        UNLOCK_CLOCKS();
    }
    return NULL;
}

static void stop_output_pump(void)
{
    atomic_store(&output_pump_stop, 1);
    if (output_pump_started) {
        if (!is_clock_owner()) {
            pthread_join(output_pump, NULL);
        }
        output_pump_started = 0;
    }
}

static void start_output_pump(void)
{
    if (output_pump_started || !recomp_audio_output_enabled()) return;
    output_pump_stop = 0;
    if (pthread_create(&output_pump, NULL, output_pump_loop, NULL) == 0) {
        output_pump_started = 1;
        atexit(stop_output_pump);
    }
}
#endif
#endif

static void recomp_dsound_create_adapter(void)
{
    uint32_t entry_esp = recomp_runtime.registers.esp;
    uint32_t output_address = stack_argument(entry_esp, 1u);
    uint32_t heap_checkpoint = xbox_HeapCheckpoint();
    RecompDsoundCreateResources resources = {0};
    uint32_t public_device = 0u;
    uint32_t result = RECOMP_DSOUND_POINTER_ERROR;

    if (output_address != 0u) {
        *recomp_memory_u32(output_address) = 0u;
        resources.manager = xbox_HeapAlloc(RECOMP_DSOUND_MANAGER_SIZE, 16u);
        if (resources.manager != 0u) {
            resources.device = xbox_HeapAlloc(RECOMP_DSOUND_DEVICE_SIZE, 16u);
        }
        if (resources.device != 0u) {
            resources.apu = xbox_HeapAlloc(RECOMP_DSOUND_APU_SIZE, 16u);
        }
        if (resources.apu != 0u) {
            resources.voice_state_table = xbox_ContiguousAlloc(
                RECOMP_DSOUND_VOICE_STATE_TABLE_SIZE,
                0u,
                UINT32_MAX,
                DIRECT_SOUND_VOICE_STATE_ALIGNMENT);
        }
        result = recomp_dsound_create(
            &dsound_service_model, &resources, &public_device);
    }
    if (result == RECOMP_DSOUND_OK) {
        write_created_objects(&dsound_service_model);
        *recomp_memory_u32(output_address) = public_device;
#if defined(RECOMP_FULL_PROGRAM)
        start_output_pump();
#endif
    } else if (resources.manager != 0u &&
               !xbox_HeapRestore(heap_checkpoint)) {
        fprintf(stderr, "recomp dsound: could not restore guest heap\n");
    }

    fprintf(
        stderr,
        "recomp dsound: DirectSoundCreate policy=host-pcm result=0x%08"
        PRIx32 " device=0x%08" PRIx32 " apu=0x%08" PRIx32
        " voices=0x%08" PRIx32 "\n",
        result,
        public_device,
        dsound_service_model.apu,
        dsound_service_model.voice_state_table);
    recomp_runtime.registers.eax = result;
    recomp_runtime.registers.esp = entry_esp + 16u;
}

void recomp_dsound_service_adapter_reset(void)
{
    LOCK_CLOCKS();
    for (uint32_t i = 0u; i < RECOMP_DSOUND_VOICE_STATE_COUNT; ++i) {
        if (buffer_clocks[i].address != 0u) {
            recomp_audio_output_reset_voice(i);
        }
    }
    recomp_dsound_service_reset(&dsound_service_model);
    memset(buffer_clocks, 0, sizeof buffer_clocks);
    UNLOCK_CLOCKS();
}

const RecompDsoundServiceModel *recomp_dsound_service_adapter_model(void)
{
    return &dsound_service_model;
}

static void recomp_dsound_do_work_adapter(void)
{
    uint32_t entry_esp = recomp_runtime.registers.esp;

    recomp_dsound_do_work(&dsound_service_model);
    LOCK_CLOCKS();
    uint64_t now = buffer_now_ms();
    for (uint32_t i = 0u; i < RECOMP_DSOUND_VOICE_STATE_COUNT; ++i) {
        if (buffer_clocks[i].address != 0u) {
            BufferClock *clock = buffer_clock(buffer_clocks[i].address);
            if (clock != NULL) pump_buffer(clock, now, 1);
        }
    }
    UNLOCK_CLOCKS();
    if (dsound_service_model.work_count == 1u) {
        fprintf(
            stderr,
            "recomp dsound: DirectSoundDoWork policy=host-pcm-service"
            " count=%" PRIu32 "\n",
            dsound_service_model.work_count);
    }
    recomp_runtime.registers.esp = entry_esp + 4u;
}

static void recomp_dsound_download_effects_image_adapter(void)
{
    uint32_t entry_esp = recomp_runtime.registers.esp;
    uint32_t image_description = stack_argument(entry_esp, 4u);

    recomp_runtime.registers.eax = recomp_dsound_download_effects_image(
        &dsound_service_model,
        stack_argument(entry_esp, 1u),
        stack_argument(entry_esp, 2u),
        stack_argument(entry_esp, 3u));
    if (image_description != 0u) {
        *recomp_memory_u32(image_description) = 0u;
    }
    fprintf(
        stderr,
        "recomp dsound: DownloadEffectsImage policy=no-audio"
        " buffer=0x%08" PRIx32 " size=%" PRIu32 " result=0x%08"
        PRIx32 "\n",
        dsound_service_model.effects_image_buffer,
        dsound_service_model.effects_image_size,
        recomp_runtime.registers.eax);
    recomp_runtime.registers.esp = entry_esp + 24u;
}

static void recomp_dsound_set_mix_bin_headroom_adapter(void)
{
    uint32_t entry_esp = recomp_runtime.registers.esp;

    recomp_dsound_set_mix_bin_headroom(
        &dsound_service_model,
        stack_argument(entry_esp, 1u),
        stack_argument(entry_esp, 2u));
    recomp_runtime.registers.eax = RECOMP_DSOUND_OK;
    recomp_runtime.registers.esp = entry_esp + 16u;
}

static void recomp_dsound_set_position_adapter(void)
{
    uint32_t entry_esp = recomp_runtime.registers.esp;
    RecompDsoundVector position = {
        .x = stack_float(entry_esp, 1u),
        .y = stack_float(entry_esp, 2u),
        .z = stack_float(entry_esp, 3u),
    };

    recomp_dsound_set_listener_position(
        &dsound_service_model, position, stack_argument(entry_esp, 4u));
    recomp_runtime.registers.eax = RECOMP_DSOUND_OK;
    recomp_runtime.registers.esp = entry_esp + 24u;
}

static void recomp_dsound_set_velocity_adapter(void)
{
    uint32_t entry_esp = recomp_runtime.registers.esp;
    RecompDsoundVector velocity = {
        .x = stack_float(entry_esp, 1u),
        .y = stack_float(entry_esp, 2u),
        .z = stack_float(entry_esp, 3u),
    };

    recomp_dsound_set_listener_velocity(
        &dsound_service_model, velocity, stack_argument(entry_esp, 4u));
    recomp_runtime.registers.eax = RECOMP_DSOUND_OK;
    recomp_runtime.registers.esp = entry_esp + 24u;
}

static void recomp_dsound_commit_deferred_settings_adapter(void)
{
    uint32_t entry_esp = recomp_runtime.registers.esp;

    recomp_dsound_commit_listener(&dsound_service_model);
    recomp_runtime.registers.eax = RECOMP_DSOUND_OK;
    recomp_runtime.registers.esp = entry_esp + 8u;
}

RecompFunction recomp_dsound_service_lookup_manual(uint32_t guest_address)
{
    switch (guest_address) {
    case 0x001f8fd8u: return buffer_play;
    case 0x001f8ffcu: return buffer_stop;
    case 0x001f9014u: return buffer_stop_ex;
    case 0x001f9058u: return buffer_status;
    case 0x001f9074u: return buffer_position;
    case 0x001f9094u: return buffer_seek;
    case 0x001f9767u: return buffer_frequency;
    case 0x001f84b0u: return buffer_release;
    case 0x001f9e5eu: return buffer_set_data;
    case DIRECT_SOUND_CREATE_ADDRESS:
        return recomp_dsound_create_adapter;
    case DIRECT_SOUND_DO_WORK_ADDRESS:
        return recomp_dsound_do_work_adapter;
    case DIRECT_SOUND_DOWNLOAD_EFFECTS_IMAGE_ADDRESS:
        return recomp_dsound_download_effects_image_adapter;
    case DIRECT_SOUND_SET_MIX_BIN_HEADROOM_ADDRESS:
        return recomp_dsound_set_mix_bin_headroom_adapter;
    case DIRECT_SOUND_COMMIT_DEFERRED_SETTINGS_ADDRESS:
        return recomp_dsound_commit_deferred_settings_adapter;
    case DIRECT_SOUND_SET_POSITION_ADDRESS:
        return recomp_dsound_set_position_adapter;
    case DIRECT_SOUND_SET_VELOCITY_ADDRESS:
        return recomp_dsound_set_velocity_adapter;
    default:
        return NULL;
    }
}
