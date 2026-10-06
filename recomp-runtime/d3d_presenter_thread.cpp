#include "d3d_presenter_capture.h"
#include "d3d_presenter_d3d11_backend.h"
#include "d3d_pose_replay.h"
#include "d3d_split_pose.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <condition_variable>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <system_error>
#include <stdexcept>
#include <thread>
#include <vector>


namespace {

struct SplitPresentRecord {
    uint32_t frame;
    float fraction;
    unsigned camera_draws, pose_draws;
    double deadline, start, end, published, picked;
    double split_ms;
    unsigned records;
};

struct PresenterThread {
    DWORD owner_thread = GetCurrentThreadId();
    std::thread worker;
    std::mutex mutex;
    std::condition_variable changed;
    HANDLE wake = nullptr;
    HANDLE split_timer = nullptr;
    D3dCapturePacket packets[3];
    unsigned open = 0;
    unsigned front = 0;
    unsigned pending = 0; // Includes the packet executing outside the mutex.
    bool started = false;
    bool shutdown = false;
    RecompD3dPresenterError created = RECOMP_D3D_PRESENTER_OK;
    RecompD3dPresenterError status = RECOMP_D3D_PRESENTER_OK;
    RecompD3dPresenterError destroyed = RECOMP_D3D_PRESENTER_OK;
    unsigned draw_declines = 0;
    uint32_t verify_at = 0, verify_count = 0, capture_frame = 0;
    // RECOMP_PERF_COUNTER pacing on the game side, reported once per second.
    bool pacing = false;
    double split_rate = 0, split_next = 0;
    uint32_t split_first_frame = 0;
    // Visual clock: when the last shown tick's interpolation started.
    uint32_t split_clock_frame = 0;
    double split_clock_start = 0;
    bool split_trace = false;
    size_t split_trace_limit = 0;
    unsigned split_presents = 0, split_camera_draws = 0, split_pose_draws = 0, split_fallbacks = 0;
    double trace_split_ms = 0;
    unsigned trace_records = 0;
    // Buffered so tracing does not write to stderr while presents are paced.
    std::vector<SplitPresentRecord> split_records;
    double prepare_published = 0;
    size_t prepare_cursor = 0;
    double last_frame_ms = 0.0;
    double frame_max_ms = 0.0;
    double queue_wait_ms = 0.0, seal_ms = 0.0, add_ms = 0.0, status_ms = 0.0;
    unsigned captured_frames = 0;
    ULONGLONG pacing_start = 0u;
    double pacing_cpu_ms = 0.0; // Game thread CPU time when the window began.
    double capture_kb = 0.0, capture_records = 0.0;

    ~PresenterThread() { if (wake != nullptr) CloseHandle(wake); if (split_timer != nullptr) CloseHandle(split_timer); }
};

PresenterThread *active_thread;
double clock_ms();

void dump_split_trace(PresenterThread &thread)
{
    if (!thread.split_trace) return;
    FILETIME now;
    GetSystemTimePreciseAsFileTime(&now);
    const double local = (static_cast<double>((static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime)/10000.0) - 11644473600000.0;
    std::fprintf(stderr, "recomp split clock: steady_ms=%.3f unix_ms=%.3f\n", clock_ms(), local);
    for (size_t i = 0; i < thread.split_records.size(); ++i) {
        const auto &r = thread.split_records[i];
        std::fprintf(stderr, "recomp split present: n=%zu frame=%u ms=%.3f fraction=%.4f camera_draws=%u pose_draws=%u "
            "deadline=%.3f end=%.3f published=%.3f picked=%.3f split_ms=%.3f records=%u\n", i, r.frame, r.start, r.fraction,
            r.camera_draws, r.pose_draws, r.deadline, r.end, r.published, r.picked, r.split_ms, r.records);
    }
    thread.split_trace = false;
}

/* Uses idle time before a split present to create the next packet's new
   textures, so a camera cut does not upload them all inside one present.
   Returns false when there is nothing left to do before the deadline. */
bool prepare_next(PresenterThread &thread, RecompD3dPresenter *backend, double deadline)
{
    const D3dCapturePacket *next = nullptr;
    {
        std::lock_guard<std::mutex> lock(thread.mutex);
        if (thread.pending > 1) next = &thread.packets[(thread.front+1)%3];
    }
    if (next == nullptr) return false;
    if (next->published_ms != thread.prepare_published) {
        thread.prepare_published = next->published_ms;
        thread.prepare_cursor = 0;
    }
    bool progressed = false;
    while (thread.prepare_cursor < next->count() && deadline-clock_ms() > 1.5) {
        const size_t i = thread.prepare_cursor;
        if (next->record(i).kind == D3dCapturePacket::COMMAND) {
            const auto &command = next->command(i);
            // ponytail: upload cost estimated at ~4 ms per MiB of new texels;
            // anything longer than a present slot is left to its draw.
            // Textures that already exist cost nothing.
            const uint64_t bytes = d3d11_backend_prepare_bytes(backend, &command);
            const double cost = bytes*4.0/1048576.0;
            if (bytes == 0) {
                ++thread.prepare_cursor;
                continue;
            }
            if (cost > 1000.0/thread.split_rate) {
                ++thread.prepare_cursor;
                continue;
            }
            if (deadline-clock_ms()-1.0 < cost) break;
            d3d11_backend_prepare(backend, &command);
        }
        ++thread.prepare_cursor;
        progressed = true;
    }
    return progressed;
}

RecompD3dPresenterError validate(RecompD3dPresenter *presenter)
{
    if (presenter == nullptr ||
        presenter != reinterpret_cast<RecompD3dPresenter *>(active_thread)) {
        return RECOMP_D3D_PRESENTER_NOT_INITIALIZED;
    }
    return GetCurrentThreadId() == active_thread->owner_thread
        ? RECOMP_D3D_PRESENTER_OK : RECOMP_D3D_PRESENTER_WRONG_THREAD;
}

RecompD3dPresenterError status(PresenterThread &thread)
{
    std::lock_guard<std::mutex> lock(thread.mutex);
    return thread.status;
}

void fail(PresenterThread &thread, RecompD3dPresenterError error)
{
    if (error == RECOMP_D3D_PRESENTER_OK) return;
    {
        std::lock_guard<std::mutex> lock(thread.mutex);
        if (thread.status == RECOMP_D3D_PRESENTER_OK) thread.status = error;
    }
    thread.changed.notify_all();
}

void pump(PresenterThread &thread)
{
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0u, 0u, PM_REMOVE)) {
        if (message.message == WM_QUIT) {
            fail(thread, RECOMP_D3D_PRESENTER_HOST_FAILURE);
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
        if (message.message == WM_CLOSE && message.hwnd != nullptr) {
            fail(thread, RECOMP_D3D_PRESENTER_CLOSED);
        }
    }
}

void execute(PresenterThread &thread, RecompD3dPresenter *backend,
    const D3dCapturePacket &packet, int phase = -1, float fraction = -1)
{
    RecompBoneMatrix sampled[4][32];
    bool sampled_valid[4] = {};
    std::vector<uint8_t> vertex_scratch;
    unsigned replay_vertices = 0, visible_vertices = 0;
    float transform_delta = 0;
    for (size_t i = 0; i < packet.count(); ++i) {
        if (status(thread) != RECOMP_D3D_PRESENTER_OK) break;
        const auto &record = packet.record(i);
        RecompD3dPresenterError error = RECOMP_D3D_PRESENTER_OK;
        bool draw = false;
        try {
            switch (record.kind) {
            case D3dCapturePacket::COMMAND: {
                const auto &command = packet.command(i);
                draw = command.type == RECOMP_D3D_PRESENTER_COMMAND_DRAW;
                if (fraction >= 0 && draw && command.data.draw.split_pose) {
                    const auto *split = static_cast<const RecompSplitDraw *>(command.data.draw.split_pose);
                    const double split_start = thread.split_trace ? clock_ms() : 0;
                    thread.split_camera_draws += split->camera;
                    thread.split_pose_draws += split->pose;
                    const RecompBoneMatrix *bones = nullptr;
                    const char *split_error = nullptr;
                    if (split->pose) {
                        if (split->actor >= 4) split_error = "actor";
                        else if (!sampled_valid[split->actor] &&
                            !recomp_animation_visual_sample(&split->visual, fraction, sampled[split->actor]))
                            split_error = "sample";
                        else {
                            sampled_valid[split->actor] = true;
                            bones = sampled[split->actor];
                        }
                    }
                    if (split->seam) vertex_scratch.resize(
                        size_t(command.data.draw.vertex_count)*command.data.draw.vertex_stride);
                    RecompD3dPresenterCommand replay;
                    replay.type = command.type;
                    if (!split_error && !recomp_d3d_split_draw(&command.data.draw, fraction, bones,
                        vertex_scratch.data(), &replay.data.draw))
                        split_error = "draw";
                    if (thread.split_trace) thread.trace_split_ms += clock_ms()-split_start;
                    if (split_error) {
                        // Guest data can be degenerate (the island map draws an object whose
                        // world matrix holds NaN). Submit the captured draw unchanged, exactly
                        // as the 60 Hz path does, instead of stopping the presenter.
                        if (thread.split_fallbacks++ < 16)
                            std::fprintf(stderr, "recomp split: fallback reason=%s camera=%u pose=%u seam=%u ball=%u\n",
                                split_error, unsigned(split->camera), unsigned(split->pose),
                                unsigned(split->seam), unsigned(split->ball));
                        error = d3d11_backend_submit(backend, &command);
                    } else error = d3d11_backend_submit(backend, &replay);
                } else if (phase >= 0 && draw && command.data.draw.pose_replay != nullptr) {
                    RecompD3dPresenterCommand replay;
                    replay.type = command.type;
                    if (!recomp_d3d_pose_replay_draw(&command.data.draw,
                            static_cast<unsigned>(phase), &replay.data.draw)) {
                        fail(thread, RECOMP_D3D_PRESENTER_INVALID_ARGUMENT);
                        return;
                    }
                    const auto &draw = replay.data.draw;
                    for (unsigned j = 0; j < 16; ++j)
                        transform_delta = (std::max)(transform_delta,
                            std::fabs(draw.transform[j]-command.data.draw.transform[j]));
                    for (unsigned vertex = 0; vertex < draw.vertex_count; ++vertex) {
                        const auto *p = reinterpret_cast<const float *>(
                            static_cast<const uint8_t *>(draw.vertex_bytes)+size_t(vertex)*draw.vertex_stride);
                        float point[4]{}, remaining = 1;
                        for (unsigned bone = 0; bone <= draw.blend_weight_count; ++bone) {
                            float weight = bone == draw.blend_weight_count ? remaining : p[3+bone];
                            remaining -= weight;
                            const float *m = bone == 0 ? draw.transform : draw.blend_transforms[bone-1];
                            for (unsigned axis = 0; axis < 4; ++axis)
                                point[axis] += weight*(p[0]*m[axis]+p[1]*m[4+axis]+p[2]*m[8+axis]+m[12+axis]);
                        }
                        ++replay_vertices;
                        if (point[3] > 0 && std::fabs(point[0]) <= point[3] &&
                            std::fabs(point[1]) <= point[3] && point[2] >= 0 && point[2] <= point[3])
                            ++visible_vertices;
                    }
                    error = d3d11_backend_submit(backend, &replay);
                } else error = d3d11_backend_submit(backend, &command);
                thread.trace_records += draw;
                break;
            }
            case D3dCapturePacket::RELEASE:
                error = d3d11_backend_release_memory(backend, record.base, record.size);
                break;
            case D3dCapturePacket::REPORT:
                d3d11_backend_report_draw_textures();
                break;
            }
        } catch (const std::bad_alloc &) {
            error = RECOMP_D3D_PRESENTER_OUT_OF_MEMORY;
        } catch (...) {
            error = RECOMP_D3D_PRESENTER_HOST_FAILURE;
        }
        if (draw && error != RECOMP_D3D_PRESENTER_OK &&
            error != RECOMP_D3D_PRESENTER_CLOSED) {
            ++thread.draw_declines;
        } else {
            fail(thread, error);
        }
    }
    if (phase >= 0) std::fprintf(stderr,
        "recomp pose replay coverage: phase=%d vertices=%u inside_clip=%u transform_delta=%.9g\n",
        phase, replay_vertices, visible_vertices, transform_delta);
}

double clock_ms();

void run(PresenterThread &thread, RecompD3dPresenterConfig config)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    RecompD3dPresenter *backend = nullptr;
    RecompD3dPresenterError created;
    try {
        created = d3d11_backend_create(&config, &backend);
    } catch (const std::bad_alloc &) {
        created = RECOMP_D3D_PRESENTER_OUT_OF_MEMORY;
    } catch (...) {
        created = RECOMP_D3D_PRESENTER_HOST_FAILURE;
    }
    {
        std::lock_guard<std::mutex> lock(thread.mutex);
        thread.created = created;
        thread.status = created;
        thread.started = true;
    }
    thread.changed.notify_all();
    if (created != RECOMP_D3D_PRESENTER_OK) return;

    double render_start = clock_ms(), render_work = 0;
    float fraction_min = 1, fraction_max = 0;
    unsigned render_packets = 0, render_presents = 0;
    for (;;) {
        pump(thread);
        D3dCapturePacket *packet = nullptr;
        {
            std::lock_guard<std::mutex> lock(thread.mutex);
            if (thread.pending != 0) packet = &thread.packets[thread.front];
            else if (thread.shutdown) break;
        }
        if (packet == nullptr) {
            if (MsgWaitForMultipleObjects(1, &thread.wake, FALSE,
                    INFINITE, QS_ALLINPUT) == WAIT_FAILED) {
                fail(thread, RECOMP_D3D_PRESENTER_HOST_FAILURE);
            }
            continue;
        }
        uint32_t frame = 0;
        bool released = false;
        for (size_t i = 0; i < packet->count(); ++i) {
            if (packet->record(i).kind == D3dCapturePacket::RELEASE) released = true;
            if (packet->record(i).kind == D3dCapturePacket::COMMAND &&
                packet->command(i).type == RECOMP_D3D_PRESENTER_COMMAND_PRESENT)
                frame = packet->command(i).data.present.swap_counter;
        }
        if (thread.split_rate && frame) {
            constexpr double tick_ms = 1000.0/60.0;
            const double picked = clock_ms();
            if (!thread.split_first_frame) {
                thread.split_first_frame = frame;
                thread.split_next = clock_ms();
            }
            double now = clock_ms();
            // Interpolation for a tick starts a fixed lead after its packet is
            // published, so idle time before the switch can prepare it. Consecutive
            // ticks continue one clock exactly one tick apart; publish jitter would
            // otherwise make visual steps uneven. Resynchronize after a gap or drift.
            constexpr double lead_ms = 4.0;
            double start = packet->published_ms+lead_ms;
            const double continued = thread.split_clock_start+tick_ms;
            if (frame == thread.split_clock_frame+1 && std::fabs(continued-start) < tick_ms/4)
                start = continued;
            if (start+tick_ms <= now) start = now;
            thread.split_clock_frame = frame;
            thread.split_clock_start = start;
            double end = start+tick_ms;
            // A slightly late producer must not discard an otherwise due present.
            // Only abandon the clock after a full simulation interval was missed.
            if (thread.split_next+tick_ms < now) thread.split_next = now;
            bool presented = false;
            // Past the tick, hold the end pose at each deadline until the next
            // packet arrives and its new textures exist, so neither a late
            // producer nor a camera cut's uploads stall present cadence. A
            // packet waits at most one tick for preparation.
            const auto next_ready = [&thread, tick_ms] {
                std::lock_guard<std::mutex> lock(thread.mutex);
                if (thread.shutdown) return true;
                if (thread.pending < 2) return false;
                const auto &next = thread.packets[(thread.front+1)%3];
                return (next.published_ms == thread.prepare_published &&
                        thread.prepare_cursor >= next.count()) ||
                    clock_ms()-next.published_ms > tick_ms;
            };
            while ((thread.split_next < end || !next_ready()) &&
                   status(thread) == RECOMP_D3D_PRESENTER_OK) {
                for (;;) {
                    double remaining = thread.split_next-clock_ms();
                    if (remaining <= 0) break;
                    pump(thread);
                    if (status(thread) != RECOMP_D3D_PRESENTER_OK) break;
                    if (prepare_next(thread, backend, thread.split_next)) continue;
                    if (remaining > 0.5 && thread.split_timer) {
                        LARGE_INTEGER due{};
                        due.QuadPart = -static_cast<LONGLONG>((remaining-0.5)*10000.0);
                        if (SetWaitableTimer(thread.split_timer, &due, 0, nullptr, nullptr, FALSE))
                            WaitForSingleObject(thread.split_timer, INFINITE);
                    } else YieldProcessor();
                }
                if (thread.split_next >= end && next_ready()) break;
                // Sample at the scheduled display time; wake-up jitter is not motion.
                double elapsed = (thread.split_next-start)/tick_ms;
                float fraction = static_cast<float>((std::max)(0.0, (std::min)(elapsed, 0.999999)));
                fraction_min = (std::min)(fraction_min, fraction);
                fraction_max = (std::max)(fraction_max, fraction);
                double draw_start = clock_ms();
                const double deadline = thread.split_next;
                thread.split_camera_draws = thread.split_pose_draws = 0;
                thread.trace_split_ms = 0; thread.trace_records = 0;
                execute(thread, backend, *packet, -1, fraction);
                render_work += clock_ms()-draw_start; ++render_presents;
                if (thread.split_trace) thread.split_records.push_back({frame, fraction,
                    thread.split_camera_draws, thread.split_pose_draws, deadline, draw_start,
                    clock_ms(), packet->published_ms, picked, thread.trace_split_ms,
                    thread.trace_records});
                if (thread.split_trace && thread.split_records.size() == thread.split_trace_limit)
                    dump_split_trace(thread);
                ++thread.split_presents;
                presented = true;
                thread.split_next += 1000.0/thread.split_rate;
                // Do not submit bursts of expired presents when the GPU is late.
                double now = clock_ms();
                if (thread.split_next+1000.0/thread.split_rate < now)
                    thread.split_next = now;
            }
            if (!presented && released) execute(thread, backend, *packet);
        } else if (thread.verify_at && frame >= thread.verify_at && frame-thread.verify_at < thread.verify_count) {
            if (released || packet->hasPoseReplay()) {
                std::fprintf(stderr, "recomp replay identity: frame=%u rejected=packet-kind\n", frame);
                fail(thread, RECOMP_D3D_PRESENTER_INVALID_ARGUMENT);
            } else {
                d3d11_backend_verify_replay(backend, frame, false);
                execute(thread, backend, *packet);
                d3d11_backend_verify_replay(backend, frame, true);
                execute(thread, backend, *packet);
            }
        } else if (packet->hasPoseReplay()) {
            bool released = false;
            unsigned draws = 0, frame = 0;
            for (size_t i = 0; i < packet->count(); ++i) {
                if (packet->record(i).kind == D3dCapturePacket::RELEASE) released = true;
                if (packet->record(i).kind != D3dCapturePacket::COMMAND) continue;
                const auto &command = packet->command(i);
                if (command.type == RECOMP_D3D_PRESENTER_COMMAND_DRAW && command.data.draw.pose_replay) {
                    ++draws;
                    frame = static_cast<const RecompD3dPoseReplay *>(command.data.draw.pose_replay)->frame;
                }
            }
            if (released) {
                std::fprintf(stderr, "recomp pose experiment: frame=%u rejected=resource-release\n", frame);
                execute(thread, backend, *packet);
            } else for (int phase = 0; phase < RECOMP_POSE_REPLAY_SAMPLES; ++phase) {
                std::fprintf(stderr, "recomp pose experiment: frame=%u phase=%d draws=%u bytes=%llu\n",
                    frame, phase, draws, static_cast<unsigned long long>(packet->bytes()));
                execute(thread, backend, *packet, phase);
            }
        } else execute(thread, backend, *packet);
        if (thread.split_rate && thread.pacing) {
            ++render_packets;
            double now = clock_ms();
            if (now-render_start >= 1000) {
                std::fprintf(stderr, "recomp split workload: packets=%u presents=%u draw_ms=%.3f last_packet_bytes=%llu fraction=%.3f..%.3f\n",
                    render_packets, render_presents, render_presents ? render_work/render_presents : 0,
                    static_cast<unsigned long long>(packet->bytes()), fraction_min, fraction_max);
                fraction_min = 1; fraction_max = 0;
                render_packets = render_presents = 0; render_work = 0; render_start = now;
            }
        }
        packet->clear();
        {
            std::lock_guard<std::mutex> lock(thread.mutex);
            thread.front = (thread.front + 1) % 3;
            --thread.pending;
        }
        thread.changed.notify_all();
    }
    thread.destroyed = d3d11_backend_destroy(&backend);
    dump_split_trace(thread);
    std::fprintf(stderr, "recomp d3d presenter thread: draw declines=%u\n",
        thread.draw_declines);
}

double clock_ms()
{
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

/* Separates a slow game frame (tick_max_ms high) from a render thread that
   cannot keep up (queue_wait_ms high). */
void notePacing(PresenterThread &thread, double wait_start_ms, double wait_end_ms)
{
    ++thread.captured_frames;
    if (thread.last_frame_ms != 0.0) {
        thread.frame_max_ms = (std::max)(thread.frame_max_ms, wait_start_ms - thread.last_frame_ms);
    }
    thread.last_frame_ms = wait_end_ms;
    thread.queue_wait_ms += wait_end_ms - wait_start_ms;
    const ULONGLONG now = GetTickCount64();
    // Game thread CPU time excludes the vblank timer sleep, so it measures game work.
    FILETIME created, exited, kernel, user;
    GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user);
    const double cpu_ms = ((uint64_t(kernel.dwHighDateTime) << 32 | kernel.dwLowDateTime) +
        (uint64_t(user.dwHighDateTime) << 32 | user.dwLowDateTime))/10000.0;
    if (thread.pacing_start == 0u) { thread.pacing_start = now; thread.pacing_cpu_ms = cpu_ms; }
    if (now - thread.pacing_start < 1000u) return;
    std::fprintf(stderr, "recomp pacing: tick_ms=%llu tick_max_ms=%.1f queue_wait_ms=%.1f capture_fps=%.2f seal_ms=%.3f add_ms=%.3f status_ms=%.3f cpu_ms=%.3f packet_kb=%.0f records=%.0f\n",
        static_cast<unsigned long long>(now), thread.frame_max_ms, thread.queue_wait_ms,
        thread.captured_frames*1000.0/(now-thread.pacing_start), thread.seal_ms/thread.captured_frames, thread.add_ms/thread.captured_frames, thread.status_ms/thread.captured_frames,
        (cpu_ms-thread.pacing_cpu_ms)/thread.captured_frames, thread.capture_kb/thread.captured_frames,
        thread.capture_records/thread.captured_frames);
    thread.pacing_start = now;
    thread.pacing_cpu_ms = cpu_ms;
    thread.capture_kb = thread.capture_records = 0.0;
    thread.frame_max_ms = 0.0;
    thread.queue_wait_ms = 0.0;
    thread.seal_ms = thread.add_ms = thread.status_ms = 0.0; thread.captured_frames = 0;
}

RecompD3dPresenterError publish(PresenterThread &thread, bool destroying)
{
    const double wait_start_ms = thread.pacing ? clock_ms() : 0.0;
    std::unique_lock<std::mutex> lock(thread.mutex);
    thread.changed.wait(lock, [&] {
        return thread.pending < 2 ||
            (!destroying && thread.status != RECOMP_D3D_PRESENTER_OK);
    });
    if (thread.pacing) notePacing(thread, wait_start_ms, clock_ms());
    if (!destroying && thread.status != RECOMP_D3D_PRESENTER_OK) return thread.status;
    auto &packet = thread.packets[thread.open];
    if (thread.pacing) {
        thread.capture_kb += packet.bytes()/1024.0;
        thread.capture_records += double(packet.count());
    }
    const bool verify = thread.verify_at && thread.capture_frame >= thread.verify_at &&
        thread.capture_frame-thread.verify_at < thread.verify_count;
    // The open slot is invisible to the worker until pending counts it, so
    // copy textures without holding the lock the worker checks every present.
    lock.unlock();
    try {
        double seal_start = clock_ms();
        packet.seal(packet.hasPoseReplay() || verify || thread.split_rate != 0);
        thread.seal_ms += clock_ms()-seal_start;
        packet.published_ms = clock_ms();
    } catch (const std::bad_alloc &) {
        return RECOMP_D3D_PRESENTER_OUT_OF_MEMORY;
    } catch (const std::length_error &) {
        return RECOMP_D3D_PRESENTER_OUT_OF_MEMORY;
    }
    lock.lock();
    thread.open = (thread.open + 1) % 3;
    ++thread.pending;
    lock.unlock();
    SetEvent(thread.wake);
    return RECOMP_D3D_PRESENTER_OK;
}

} // namespace

RecompD3dPresenterError recomp_d3d_presenter_create(
    const RecompD3dPresenterConfig *config, RecompD3dPresenter **presenter)
{
    if (config == nullptr || presenter == nullptr || config->width == 0 ||
        config->height == 0 ||
        config->color_format != RECOMP_D3D_PRESENTER_COLOR_FORMAT_BGRA8_UNORM ||
        config->depth_format != RECOMP_D3D_PRESENTER_DEPTH_FORMAT_D24S8) {
        return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    }
    if (*presenter != nullptr || active_thread != nullptr) {
        return RECOMP_D3D_PRESENTER_ALREADY_INITIALIZED;
    }
    std::unique_ptr<PresenterThread> thread;
    try {
        thread = std::make_unique<PresenterThread>();
        thread->split_rate = recomp_split_rate(std::getenv("RECOMP_SPLIT_RATE"));
        d3d11_backend_set_split_presentation(thread->split_rate != 0);
        thread->split_trace = std::getenv("RECOMP_SPLIT_TRACE") != nullptr;
        if (const char *limit = std::getenv("RECOMP_SPLIT_TRACE_LIMIT"))
            thread->split_trace_limit = std::strtoul(limit, nullptr, 10);
        if (thread->split_rate) thread->split_timer = CreateWaitableTimerExW(nullptr, nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (thread->split_rate) std::fprintf(stderr, "recomp split rate: target=%.6g gameplay=60 visual_delay_ticks=1\n", thread->split_rate);
        if (const char *at = std::getenv("RECOMP_REPLAY_VERIFY_AT")) {
            thread->verify_at = static_cast<uint32_t>(std::strtoul(at, nullptr, 10));
            thread->verify_count = 120;
        }
        const char *performance = std::getenv("RECOMP_PERF_COUNTER");
        thread->pacing = performance != nullptr && std::strcmp(performance, "1") == 0;
        thread->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (thread->wake == nullptr) return RECOMP_D3D_PRESENTER_HOST_FAILURE;
        thread->worker = std::thread(run, std::ref(*thread), *config);
    } catch (const std::bad_alloc &) {
        return RECOMP_D3D_PRESENTER_OUT_OF_MEMORY;
    } catch (const std::system_error &) {
        return RECOMP_D3D_PRESENTER_HOST_FAILURE;
    }
    std::unique_lock<std::mutex> lock(thread->mutex);
    thread->changed.wait(lock, [&] { return thread->started; });
    const auto error = thread->created;
    lock.unlock();
    if (error != RECOMP_D3D_PRESENTER_OK) {
        thread->worker.join();
        return error;
    }
    active_thread = thread.release();
    *presenter = reinterpret_cast<RecompD3dPresenter *>(active_thread);
    return RECOMP_D3D_PRESENTER_OK;
}

RecompD3dPresenterError recomp_d3d_presenter_submit(
    RecompD3dPresenter *presenter, const RecompD3dPresenterCommand *command)
{
    auto error = validate(presenter);
    if (error != RECOMP_D3D_PRESENTER_OK) return error;
    auto &thread = *active_thread;
    double status_start = thread.pacing ? clock_ms() : 0;
    error = status(thread);
    if (thread.pacing) thread.status_ms += clock_ms()-status_start;
    if (error != RECOMP_D3D_PRESENTER_OK) return error;
    if (command == nullptr) return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    if (command->type == RECOMP_D3D_PRESENTER_COMMAND_PRESENT)
        thread.capture_frame = command->data.present.swap_counter;
    double add_start = thread.pacing ? clock_ms() : 0;
    error = thread.packets[thread.open].add(*command);
    if (thread.pacing) thread.add_ms += clock_ms()-add_start;
    if (error != RECOMP_D3D_PRESENTER_OK) return error;
    return command->type == RECOMP_D3D_PRESENTER_COMMAND_PRESENT
        ? publish(thread, false) : RECOMP_D3D_PRESENTER_OK;
}

RecompD3dPresenterError recomp_d3d_presenter_release_memory(
    RecompD3dPresenter *presenter, uint32_t base, uint32_t size)
{
    auto error = validate(presenter);
    if (error != RECOMP_D3D_PRESENTER_OK) return error;
    auto &thread = *active_thread;
    error = status(thread);
    if (error != RECOMP_D3D_PRESENTER_OK) return error;
    return thread.packets[thread.open].addRelease(base, size);
}

RecompD3dPresenterError recomp_d3d_presenter_destroy(RecompD3dPresenter **presenter)
{
    if (presenter == nullptr) return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    const auto error = validate(*presenter);
    if (error != RECOMP_D3D_PRESENTER_OK) return error;
    auto &thread = *active_thread;
    if (thread.packets[thread.open].count() != 0) publish(thread, true);
    {
        std::lock_guard<std::mutex> lock(thread.mutex);
        thread.shutdown = true;
    }
    SetEvent(thread.wake);
    thread.worker.join();
    // A close already reached the owner through PRESENT; report other pending work errors.
    const auto pending = thread.status == RECOMP_D3D_PRESENTER_CLOSED
        ? RECOMP_D3D_PRESENTER_OK : thread.status;
    const auto destroyed = thread.destroyed != RECOMP_D3D_PRESENTER_OK
        ? thread.destroyed : pending;
    delete active_thread;
    active_thread = nullptr;
    *presenter = nullptr;
    return destroyed;
}

void recomp_d3d_presenter_set_immediate_present(bool enabled)
{
    d3d11_backend_set_immediate_present(enabled);
}

void recomp_d3d_presenter_report_draw_textures()
{
    if (active_thread == nullptr || GetCurrentThreadId() != active_thread->owner_thread) return;
    auto &thread = *active_thread;
    if (status(thread) == RECOMP_D3D_PRESENTER_OK) {
        fail(thread, thread.packets[thread.open].addReport());
    }
}
