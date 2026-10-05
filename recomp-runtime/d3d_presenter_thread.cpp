#include "d3d_presenter_capture.h"
#include "d3d_frame_capture.h"
#include "d3d_presenter_d3d11_backend.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <condition_variable>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <system_error>
#include <thread>


namespace {

struct PresenterThread {
    DWORD owner_thread = GetCurrentThreadId();
    std::thread worker;
    std::mutex mutex;
    std::condition_variable changed;
    HANDLE wake = nullptr;
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
    // RECOMP_PERF_COUNTER pacing on the game side, reported once per second.
    bool pacing = false;
    double last_frame_ms = 0.0;
    double frame_max_ms = 0.0;
    double queue_wait_ms = 0.0;
    ULONGLONG pacing_start = 0u;

    ~PresenterThread() { if (wake != nullptr) CloseHandle(wake); }
};

PresenterThread *active_thread;

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
    const D3dCapturePacket &packet)
{
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
                error = d3d11_backend_submit(backend, &command);
                if (command.capture_frame && !draw) {
                    char path[1024];
                    if (recomp_d3d_capture_path(path, sizeof path, command.capture_frame, "host.jsonl")) {
                        if (FILE *file = std::fopen(path, "ab")) {
                            std::fprintf(file, "{\"call\":%u,\"type\":%u,\"error\":%u}\n",
                                command.capture_call, command.type, error);
                            std::fclose(file);
                        }
                    }
                }
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
}

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
        execute(thread, backend, *packet);
        packet->clear();
        {
            std::lock_guard<std::mutex> lock(thread.mutex);
            thread.front = (thread.front + 1) % 3;
            --thread.pending;
        }
        thread.changed.notify_all();
    }
    thread.destroyed = d3d11_backend_destroy(&backend);
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
    if (thread.last_frame_ms != 0.0) {
        thread.frame_max_ms = (std::max)(thread.frame_max_ms, wait_start_ms - thread.last_frame_ms);
    }
    thread.last_frame_ms = wait_end_ms;
    thread.queue_wait_ms += wait_end_ms - wait_start_ms;
    const ULONGLONG now = GetTickCount64();
    if (thread.pacing_start == 0u) thread.pacing_start = now;
    if (now - thread.pacing_start < 1000u) return;
    std::fprintf(stderr, "recomp pacing: tick_ms=%llu tick_max_ms=%.1f queue_wait_ms=%.1f\n",
        static_cast<unsigned long long>(now), thread.frame_max_ms, thread.queue_wait_ms);
    thread.pacing_start = now;
    thread.frame_max_ms = 0.0;
    thread.queue_wait_ms = 0.0;
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
    thread.packets[thread.open].seal();
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
    error = status(thread);
    if (error != RECOMP_D3D_PRESENTER_OK) return error;
    if (command == nullptr) return RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    error = thread.packets[thread.open].add(*command);
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
