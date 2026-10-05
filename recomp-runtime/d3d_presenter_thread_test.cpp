#include "d3d_presenter.h"
#include "d3d_draw_model.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

static const RecompD3dPresenterConfig config = {
    320u, 240u, RECOMP_D3D_PRESENTER_COLOR_FORMAT_BGRA8_UNORM,
    RECOMP_D3D_PRESENTER_DEPTH_FORMAT_D24S8};

static bool expect(const char *label, RecompD3dPresenterError actual,
    RecompD3dPresenterError expected = RECOMP_D3D_PRESENTER_OK)
{
    if (actual == expected) return true;
    std::fprintf(stderr, "FAIL %s actual=%u expected=%u\n", label,
        static_cast<unsigned>(actual), static_cast<unsigned>(expected));
    return false;
}

static RecompD3dPresenterCommand clearCommand()
{
    RecompD3dPresenterCommand command{};
    command.type = RECOMP_D3D_PRESENTER_COMMAND_CLEAR;
    command.data.clear = {true, true, false, 0xff204060u, 1.0f, 0u};
    return command;
}

static RecompD3dPresenterError tick(RecompD3dPresenter *presenter, uint32_t swap)
{
    auto command = clearCommand();
    const auto error = recomp_d3d_presenter_submit(presenter, &command);
    if (error != RECOMP_D3D_PRESENTER_OK) return error;
    command = {};
    command.type = RECOMP_D3D_PRESENTER_COMMAND_PRESENT;
    command.data.present = {5u, swap};
    return recomp_d3d_presenter_submit(presenter, &command);
}

static bool destroy(RecompD3dPresenter *&presenter)
{
    const bool passed = expect("destroy", recomp_d3d_presenter_destroy(&presenter));
    if (presenter != nullptr) std::fprintf(stderr, "FAIL destroy retained handle\n");
    return passed && presenter == nullptr;
}

static HWND presenterWindow()
{
    HWND window = nullptr;
    while ((window = FindWindowExW(nullptr, window,
            L"DOAXBVRecompPresenterWindow", nullptr)) != nullptr) {
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        if (process == GetCurrentProcessId()) return window;
    }
    return nullptr;
}

static bool testTicks()
{
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("null config", recomp_d3d_presenter_create(nullptr, &presenter),
        RECOMP_D3D_PRESENTER_INVALID_ARGUMENT);
    if (!expect("create ticks", recomp_d3d_presenter_create(&config, &presenter))) return false;
    RecompD3dPresenter *second = nullptr;
    passed &= expect("singleton", recomp_d3d_presenter_create(&config, &second),
        RECOMP_D3D_PRESENTER_ALREADY_INITIALIZED);
    std::thread other([&] {
        passed &= expect("owner thread", tick(presenter, 1), RECOMP_D3D_PRESENTER_WRONG_THREAD);
    });
    other.join();
    for (uint32_t i = 1; i <= 5 && passed; ++i) passed &= expect("tick", tick(presenter, i));
    auto clear = clearCommand();
    passed &= expect("partial clear", recomp_d3d_presenter_submit(presenter, &clear));
    passed &= expect("partial release", recomp_d3d_presenter_release_memory(
        presenter, 0x00200000u, 4096u));
    recomp_d3d_presenter_report_draw_textures();
    passed &= destroy(presenter);
    if (passed) std::puts("PASS five CLEAR/PRESENT ticks and partial shutdown");
    return passed;
}

static bool testCapturedDraw()
{
    RecompD3dPresenter *presenter = nullptr;
    if (!expect("create draw", recomp_d3d_presenter_create(&config, &presenter))) return false;
    struct Vertex { float x, y, z, rhw; uint32_t diffuse; float u, v; };
    Vertex vertices[] = {
        {20, 20, 0.25f, 1, 0xffff8040u, 0, 0},
        {200, 20, 0.25f, 1, 0xffff8040u, 1, 0},
        {20, 200, 0.25f, 1, 0xffff8040u, 0, 1},
        {200, 200, 0.25f, 1, 0xffff8040u, 1, 1},
    };
    uint16_t indices[] = {0, 1, 2, 3};
    auto clear = clearCommand();
    bool passed = expect("draw clear", recomp_d3d_presenter_submit(presenter, &clear));
    RecompD3dPresenterCommand command{};
    command.type = RECOMP_D3D_PRESENTER_COMMAND_DRAW;
    auto &draw = command.data.draw;
    draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
    draw.index_count = draw.vertex_count = 4;
    draw.triangle_count = 2;
    draw.vertex_stride = sizeof(Vertex);
    draw.fvf = 0x144u;
    draw.vertex_bytes = vertices;
    draw.index_bytes = indices;
    draw.blend.color_write_mask = 15;
    passed &= expect("capture draw", recomp_d3d_presenter_submit(presenter, &command));
    // No PRESENT has published this packet, so these writes precede execution.
    std::memset(vertices, 0xff, sizeof vertices);
    std::memset(indices, 0xff, sizeof indices);
    command = {};
    command.type = RECOMP_D3D_PRESENTER_COMMAND_PRESENT;
    command.data.present = {5, 1};
    passed &= expect("draw present", recomp_d3d_presenter_submit(presenter, &command));
    recomp_d3d_presenter_report_draw_textures();
    passed &= destroy(presenter);
    if (passed) std::puts("PASS captured draw survives source overwrite");
    return passed;
}

static bool testClose(bool idle)
{
    RecompD3dPresenter *presenter = nullptr;
    if (!expect("create close", recomp_d3d_presenter_create(&config, &presenter))) return false;
    const HWND window = presenterWindow();
    bool passed = window != nullptr;
    DWORD owner = GetWindowThreadProcessId(window, nullptr);
    passed &= owner != 0 && owner != GetCurrentThreadId();
    if (passed) passed = PostMessageW(window, WM_CLOSE, 0, 0) != 0;
    if (passed && idle) {
        const ULONGLONG deadline = GetTickCount64() + 2000;
        while (IsWindow(window) && GetTickCount64() < deadline) Sleep(1);
        passed = !IsWindow(window);
    }
    RecompD3dPresenterError error = RECOMP_D3D_PRESENTER_OK;
    uint32_t ticks = 0;
    while (passed && error == RECOMP_D3D_PRESENTER_OK && ticks < 600) {
        error = tick(presenter, ++ticks);
    }
    passed &= expect("close status", error, RECOMP_D3D_PRESENTER_CLOSED);
    passed &= expect("sticky close", recomp_d3d_presenter_submit(presenter, nullptr),
        RECOMP_D3D_PRESENTER_CLOSED);
    passed &= destroy(presenter);
    if (passed) std::printf("PASS %s close ticks=%u\n", idle ? "idle" : "queued", ticks);
    else std::fprintf(stderr, "FAIL close idle=%d window=%p owner=%lu\n", idle,
        static_cast<void *>(window), owner);
    return passed;
}

int main()
{
    recomp_d3d_presenter_set_immediate_present(false);
    if (!testTicks()) return 1;
    _putenv_s("RECOMP_REPLAY_VERIFY_AT", "1");
    bool captured = testCapturedDraw();
    _putenv_s("RECOMP_REPLAY_VERIFY_AT", "");
    if (!captured || !testClose(false) || !testClose(true)) return 1;
    std::puts("PASS presenter render thread");
    return 0;
}
