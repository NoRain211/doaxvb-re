#include "movie_player.h"

#include "kernel_abi.h"
#include "movie_video.h"
#include "stop_report.h"
#include "xapi_time_adapter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

/* The game keeps calling the player through the table at 0x00284168, so a
   handle is a guest block whose first word points at it. Decoding and frames
   stay on the host. One movie plays at a time. */
enum {
    PLAYER_TABLE = 0x00284168u,
    SLOTS = 4u,
    MAX_WIDTH = 720u,
    MAX_HEIGHT = 480u,
};

enum { STOPPED = 0, PREPARING = 1, PLAYING = 2, ENDED = 3, FAILED = 4 };

#ifdef _WIN32
static SRWLOCK lock = SRWLOCK_INIT;
#define LOCK() AcquireSRWLockExclusive(&lock)
#define UNLOCK() ReleaseSRWLockExclusive(&lock)
typedef HANDLE Worker;
#else
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&lock)
#define UNLOCK() pthread_mutex_unlock(&lock)
typedef pthread_t Worker;
#endif
#ifdef __SWITCH__
/* libnx; declared here so this file needs no switch.h. */
uint32_t svcSetThreadCoreMask(uint32_t handle, int32_t preferred_core, uint32_t affinity_mask);
#endif

/* Fields under lock are shared with the decode worker. Slots [head,
   head + ready) hold decoded frames in order; the game holds slot head while
   held is set, so the worker never writes it. */
static struct {
    uint32_t handle;
    uint32_t out_pitch;
    int running;
    int held;
    int64_t start_ns;
    Worker worker;
    char path[512];
    /* under lock */
    int stop, ended, failed;
    unsigned head, ready;
    uint32_t width, height;
    double time[SLOTS];
    uint8_t *pixels[SLOTS];
} movie;

static int16_t color_table[RECOMP_MOVIE_COLOR_TABLE_ENTRIES];

/* BT.601 studio range, as MPEG-1 is coded, in the 6-bit fixed point the
   converter sums per channel; channels are B, G, R, A in memory. Luma stops
   at full white so a sum never wraps 16 bits. */
static void build_color_table(void)
{
    for (int value = 0; value < 256; ++value) {
        double y = 1.164383 * (value - 16) * 64.0, c = value - 128;
        if (y > 255.0 * 64.0) y = 255.0 * 64.0;
        int16_t *luma = color_table + value * 4, *u = luma + 1024, *v = luma + 2048;
        luma[0] = luma[1] = luma[2] = (int16_t)(y < 0 ? y - 0.5 : y + 0.5);
        luma[3] = 255 * 64;
        u[0] = (int16_t)(2.017232 * c * 64.0);
        u[1] = (int16_t)(-0.391762 * c * 64.0);
        v[1] = (int16_t)(-0.812968 * c * 64.0);
        v[2] = (int16_t)(1.596027 * c * 64.0);
    }
}

static void sleep_briefly(void)
{
#ifdef _WIN32
    Sleep(2u);
#else
    struct timespec pause = {0, 2 * 1000 * 1000};
    nanosleep(&pause, NULL);
#endif
}

static int64_t now_ns(void) { return (int64_t)recomp_xapi_performance_counter(); }

/* Decodes and converts ahead of the game, so a slow frame is absorbed by the
   frames already queued. */
#ifdef _WIN32
static DWORD WINAPI decode_worker(void *unused)
#else
static void *decode_worker(void *unused)
#endif
{
    (void)unused;
#ifdef __SWITCH__
    /* Off the game's core; the game, presenter and mixer share core 0. */
    svcSetThreadCoreMask(0xffff8000u, 1, 1u << 1);
#endif
    RecompMovieVideo *video = recomp_movie_video_open(movie.path);
    int failed = video == NULL;

    while (!failed) {
        LOCK();
        int stop = movie.stop, full = movie.ready == SLOTS;
        unsigned slot = (movie.head + movie.ready) % SLOTS;
        UNLOCK();
        if (stop) break;
        if (full) {
            sleep_briefly();
            continue;
        }
        RecompMovieFrame frame;
        if (!recomp_movie_video_next(video, &frame)) break;
        if (frame.width == 0u || frame.width > MAX_WIDTH ||
            frame.height == 0u || frame.height > MAX_HEIGHT ||
            (movie.width != 0u && (frame.width != movie.width || frame.height != movie.height))) {
            failed = 1;
            break;
        }
        const size_t pitch = (size_t)frame.width * 4u, bytes = pitch * frame.height;
        if (movie.pixels[slot] == NULL && (movie.pixels[slot] = malloc(bytes)) == NULL) {
            failed = 1;
            break;
        }
        if (!recomp_movie_color_convert(frame.planes, frame.width, frame.height, color_table,
                RECOMP_MOVIE_COLOR_TABLE_ENTRIES, movie.pixels[slot], bytes, pitch)) {
            failed = 1;
            break;
        }
        LOCK();
        movie.width = frame.width;
        movie.height = frame.height;
        movie.time[slot] = frame.time;
        ++movie.ready;
        UNLOCK();
    }
    recomp_movie_video_close(video);
    LOCK();
    movie.ended = 1;
    movie.failed |= failed;
    UNLOCK();
    return 0;
}

static void stop_movie(void)
{
    if (movie.running) {
        LOCK();
        movie.stop = 1;
        UNLOCK();
#ifdef _WIN32
        WaitForSingleObject(movie.worker, INFINITE);
        CloseHandle(movie.worker);
#else
        pthread_join(movie.worker, NULL);
#endif
    }
    for (unsigned i = 0u; i < SLOTS; ++i) free(movie.pixels[i]);
    const uint32_t handle = movie.handle;
    memset(&movie, 0, sizeof movie);
    movie.handle = handle;
}

static uint32_t argument(unsigned index)
{
    return *recomp_memory_u32(recomp_runtime.registers.esp + 4u * index);
}

/* Every player call is cdecl; the caller pops the arguments. */
static void finish(uint32_t value)
{
    recomp_runtime.registers.eax = value;
    recomp_runtime.registers.esp += 4u;
}

static void check_handle(void)
{
    if (movie.handle == 0u || argument(1) != movie.handle) {
        recomp_stop(2, "movie-player:unknown-handle");
    }
}

/* mwPlyInitSfdFx: decoding runs on the worker, so there is no server to
   register. */
static void player_init(void) { finish(0u); }

/* mwPlyCalcWorkCprmSfd: the guest work block only holds the handle word. */
static void player_work_size(void) { finish(16u); }

/* mwPlyCreateSofdec: the creation parameters hold the work block at +0x18. */
static void player_create(void)
{
    const uint32_t work = *recomp_memory_u32(argument(1) + 0x18u);

    stop_movie();
    if (work == 0u) {
        finish(0u);
        return;
    }
    *recomp_memory_u32(work) = PLAYER_TABLE;
    movie.handle = work;
    finish(work);
}

static void player_destroy(void)
{
    check_handle();
    stop_movie();
    movie.handle = 0u;
    finish(0u);
}

/* mwPlyStartFname(h, "d:\\name.sfd"): the d: drive is the disc root. */
static void player_start(void)
{
    check_handle();
    stop_movie();
    build_color_table();
    const char *name = (const char *)recomp_memory_i8(argument(2));
    if ((name[0] == 'd' || name[0] == 'D') && name[1] == ':') name += 2;
    while (*name == '\\' || *name == '/') ++name;
    int length = snprintf(movie.path, sizeof movie.path, "%s/%s",
        recomp_disc_root_path != NULL ? recomp_disc_root_path : ".", name);
    if (length < 0 || (size_t)length >= sizeof movie.path) {
        movie.failed = 1;
        finish(0u);
        return;
    }
    for (char *c = movie.path; *c != '\0'; ++c) {
        if (*c == '\\') *c = '/';
    }
#ifdef _WIN32
    movie.worker = CreateThread(NULL, 0u, decode_worker, NULL, 0u, NULL);
    movie.running = movie.worker != NULL;
#else
    movie.running = pthread_create(&movie.worker, NULL, decode_worker, NULL) == 0;
#endif
    if (!movie.running) movie.failed = 1;
    finish(0u);
}

static void player_stop(void)
{
    check_handle();
    stop_movie();
    finish(0u);
}

static void player_status(void)
{
    uint32_t status = STOPPED;

    check_handle();
    LOCK();
    if (movie.failed) status = FAILED;
    else if (movie.ended && movie.ready == 0u) status = ENDED;
    else if (movie.running) status = movie.start_ns != 0 ? PLAYING : PREPARING;
    UNLOCK();
    if (status == ENDED || status == FAILED) {
        recomp_stop_at_boundary("cri-movie-status:terminal");
    }
    finish(status);
}

/* mwPlyPause: the game only ever resumes, right after creating the player. */
static void player_pause(void) { finish(0u); }

/* shortcut: movies are silent until the ADX track is decoded (step 3); then
   this volume applies to it. */
static void player_volume(void) { finish(0u); }

/* mwPlyGetAdxtHn: 0 makes the game skip its mixer routing. */
static void player_adxt(void) { finish(0u); }

static void player_audio_switch(void) { finish(0u); }

/* mwPlyGetCurFrm(h, frame): frame +0 is nonzero when a frame is ready, +8 and
   +0xC are its size. Frames are released on a wall clock started by the
   first one; late frames are dropped. */
static void player_get_frame(void)
{
    const uint32_t object = argument(2);
    uint32_t fields[16] = {0};

    check_handle();
    LOCK();
    if (!movie.held && movie.ready != 0u) {
        const int64_t now = now_ns();
        if (movie.start_ns == 0) movie.start_ns = now - (int64_t)(movie.time[movie.head] * 1e9);
        const double elapsed = (now - movie.start_ns) / 1e9;
        while (movie.ready > 1u && movie.time[(movie.head + 1u) % SLOTS] <= elapsed) {
            movie.head = (movie.head + 1u) % SLOTS;
            --movie.ready;
        }
        if (movie.time[movie.head] <= elapsed) {
            movie.held = 1;
            fields[0] = 1u;
            fields[2] = movie.width;
            fields[3] = movie.height;
        }
    }
    UNLOCK();
    recomp_guest_store(object, fields, sizeof fields);
    finish(0u);
}

static void player_release_frame(void)
{
    check_handle();
    LOCK();
    if (movie.held) {
        movie.held = 0;
        movie.head = (movie.head + 1u) % SLOTS;
        --movie.ready;
    }
    UNLOCK();
    finish(0u);
}

/* mwPlyFxSetOutBufSize(h, pitch, height) */
static void player_output_size(void)
{
    check_handle();
    movie.out_pitch = argument(2);
    finish(0u);
}

/* mwPlyFxCnvFrmARGB8888(h, frame, bits): copies the held frame, which the
   worker already converted, into the locked texture. */
static void player_convert(void)
{
    check_handle();
    const uint32_t bits = argument(3);
    if (movie.held && bits != 0u) {
        const size_t row = (size_t)movie.width * 4u;
        if (movie.out_pitch < row) recomp_stop(2, "movie-player:output-pitch");
        const uint8_t *source = movie.pixels[movie.head];
        uint8_t *destination = recomp_memory(bits, (size_t)movie.out_pitch * (movie.height - 1u) + row);
        for (uint32_t y = 0u; y < movie.height; ++y) {
            memcpy(destination + (size_t)y * movie.out_pitch, source + y * row, row);
        }
    }
    finish(0u);
}

RecompFunction recomp_movie_player_lookup_manual(uint32_t guest_address)
{
    switch (guest_address) {
    case 0x00199cb0u: return player_init;
    case 0x00198f80u: return player_work_size;
    case 0x00199760u: return player_create;
    case 0x00198550u: return player_adxt;
    case 0x001984e0u: return player_audio_switch;
    case 0x00198190u: return player_get_frame;
    case 0x00197ec0u: return player_release_frame;
    case 0x001985f0u: return player_output_size;
    case 0x001989f0u: return player_convert;
    /* Through the table at 0x00284168. */
    case 0x00199680u: return player_destroy;
    case 0x001a34a0u: return player_start;
    case 0x001a3060u: return player_stop;
    case 0x00198320u: return player_status;
    case 0x001a30b0u: return player_pause;
    case 0x00198420u: return player_volume;
    default: return NULL;
    }
}
