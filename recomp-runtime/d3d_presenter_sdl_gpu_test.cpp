// SPDX-License-Identifier: GPL-3.0-or-later
#include "d3d_presenter.h"
#include "d3d_draw_model.h"

#include <SDL3/SDL.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <thread>

static const RecompD3dPresenterConfig kTestConfig = {
    320u, 240u,
    RECOMP_D3D_PRESENTER_COLOR_FORMAT_BGRA8_UNORM,
    RECOMP_D3D_PRESENTER_DEPTH_FORMAT_D24S8
};

static bool expect(const char *label, RecompD3dPresenterError actual,
    RecompD3dPresenterError expected = RECOMP_D3D_PRESENTER_OK)
{
    if (actual == expected) return true;
    std::fprintf(stderr, "FAIL %s: actual=%u expected=%u\n", label,
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

static bool testCreateAndTicks()
{
    RecompD3dPresenter *presenter = nullptr;
    bool passed = expect("null config",
        recomp_d3d_presenter_create(nullptr, &presenter),
        RECOMP_D3D_PRESENTER_INVALID_ARGUMENT);

    RecompD3dPresenterConfig invalid_config = kTestConfig;
    invalid_config.width = 0;
    passed &= expect("zero width",
        recomp_d3d_presenter_create(&invalid_config, &presenter),
        RECOMP_D3D_PRESENTER_INVALID_ARGUMENT);

    if (!expect("create presenter", recomp_d3d_presenter_create(&kTestConfig, &presenter))) {
        return false;
    }

    RecompD3dPresenter *second = nullptr;
    passed &= expect("singleton check",
        recomp_d3d_presenter_create(&kTestConfig, &second),
        RECOMP_D3D_PRESENTER_ALREADY_INITIALIZED);

    // Thread check
    std::thread other([&] {
        passed &= expect("wrong thread check",
            tick(presenter, 1),
            RECOMP_D3D_PRESENTER_WRONG_THREAD);
    });
    other.join();

    // 5 ticks of Clear + Present
    for (uint32_t i = 1; i <= 5 && passed; ++i) {
        passed &= expect("tick", tick(presenter, i));
    }

    auto clear = clearCommand();
    passed &= expect("submit clear", recomp_d3d_presenter_submit(presenter, &clear));

    passed &= expect("release memory",
        recomp_d3d_presenter_release_memory(presenter, 0x00200000u, 4096u));

    recomp_d3d_presenter_report_draw_textures();

    passed &= expect("destroy presenter",
        recomp_d3d_presenter_destroy(&presenter));
    passed &= (presenter == nullptr);

    if (passed) {
        std::puts("PASS testCreateAndTicks");
    }
    return passed;
}

static bool testDraw()
{
    RecompD3dPresenter *presenter = nullptr;
    if (!expect("create for draw", recomp_d3d_presenter_create(&kTestConfig, &presenter))) {
        return false;
    }

    struct Vertex {
        float x, y, z, rhw;
        uint32_t diffuse;
        float u, v;
    };
    Vertex vertices[] = {
        {20.0f,  20.0f,  0.25f, 1.0f, 0xffff8040u, 0.0f, 0.0f},
        {200.0f, 20.0f,  0.25f, 1.0f, 0xffff8040u, 1.0f, 0.0f},
        {20.0f,  200.0f, 0.25f, 1.0f, 0xffff8040u, 0.0f, 1.0f},
        {200.0f, 200.0f, 0.25f, 1.0f, 0xffff8040u, 1.0f, 1.0f},
    };
    uint16_t indices[] = {0, 1, 2, 3};

    auto clear = clearCommand();
    bool passed = expect("draw clear", recomp_d3d_presenter_submit(presenter, &clear));

    RecompD3dPresenterCommand draw_cmd{};
    draw_cmd.type = RECOMP_D3D_PRESENTER_COMMAND_DRAW;
    auto &draw = draw_cmd.data.draw;
    draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
    draw.index_count = draw.vertex_count = 4;
    draw.triangle_count = 2;
    draw.vertex_stride = sizeof(Vertex);
    draw.fvf = 0x144u; // D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1
    draw.vertex_bytes = vertices;
    draw.index_bytes = indices;
    draw.blend.color_write_mask = 15;

    passed &= expect("submit 2D draw", recomp_d3d_presenter_submit(presenter, &draw_cmd));

    // Test FVF 0x104 (movie quad / UI: XYZRHW + TEX1 without diffuse) with linear BGRA texture
    struct VertexMovie {
        float x, y, z, rhw;
        float u, v;
    };
    VertexMovie movie_verts[] = {
        {0.0f,   0.0f,   0.0f, 1.0f, 0.0f, 0.0f},
        {320.0f, 0.0f,   0.0f, 1.0f, 1.0f, 0.0f},
        {0.0f,   240.0f, 0.0f, 1.0f, 0.0f, 1.0f},
        {320.0f, 240.0f, 0.0f, 1.0f, 1.0f, 1.0f},
    };
    uint32_t movie_pixels[4] = {0xff112233u, 0xff445566u, 0xff778899u, 0xffaabbccu};
    draw_cmd = {};
    draw_cmd.type = RECOMP_D3D_PRESENTER_COMMAND_DRAW;
    auto &draw_movie = draw_cmd.data.draw;
    draw_movie.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
    draw_movie.index_count = draw_movie.vertex_count = 4;
    draw_movie.triangle_count = 2;
    draw_movie.vertex_stride = sizeof(VertexMovie);
    draw_movie.fvf = 0x104u;
    draw_movie.vertex_bytes = movie_verts;
    draw_movie.index_bytes = indices;
    draw_movie.has_texture = true;
    draw_movie.texture.data = 0x82000000u;
    draw_movie.texture.format_byte = 0x12u; // Linear BGRA
    draw_movie.texture.linear = true;
    draw_movie.texture.width = 2;
    draw_movie.texture.height = 2;
    draw_movie.texture.pitch = 8;
    draw_movie.texture_bytes = movie_pixels;
    draw_movie.texture_byte_count = sizeof(movie_pixels);
    draw_movie.blend.blend_enable = false; // Opaque video
    passed &= expect("submit movie 0x104 draw", recomp_d3d_presenter_submit(presenter, &draw_cmd));

    // Present frame 1 so the draw and upload are completed before mutating guest texture
    RecompD3dPresenterCommand present_cmd{};
    present_cmd.type = RECOMP_D3D_PRESENTER_COMMAND_PRESENT;
    present_cmd.data.present.effective_flags = 5u;
    present_cmd.data.present.swap_counter = 1u;
    passed &= expect("present movie frame 1", recomp_d3d_presenter_submit(presenter, &present_cmd));

    // Re-upload modified video pixels to test linear BGRA dynamic cache update across frames
    movie_pixels[0] = 0xffffffffu;
    passed &= expect("submit movie frame 2 dynamic upload", recomp_d3d_presenter_submit(presenter, &draw_cmd));
    present_cmd.data.present.swap_counter = 2u;
    passed &= expect("present movie frame 2", recomp_d3d_presenter_submit(presenter, &present_cmd));

    // Test FVF 0x112 (3D mesh: XYZ + NORMAL + TEX1 with WVP transform)
    struct Vertex3D {
        float x, y, z;
        float nx, ny, nz;
        float u, v;
    };
    Vertex3D mesh_verts[] = {
        {-1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f},
        { 1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f},
        {-1.0f,  1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f},
        { 1.0f,  1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f},
    };
    draw_cmd = {};
    draw_cmd.type = RECOMP_D3D_PRESENTER_COMMAND_DRAW;
    auto &draw_3d = draw_cmd.data.draw;
    draw_3d.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
    draw_3d.index_count = draw_3d.vertex_count = 4;
    draw_3d.triangle_count = 2;
    draw_3d.vertex_stride = sizeof(Vertex3D);
    draw_3d.fvf = 0x112u;
    draw_3d.vertex_bytes = mesh_verts;
    draw_3d.index_bytes = indices;
    draw_3d.has_transform = true;
    draw_3d.transform[0] = draw_3d.transform[5] = draw_3d.transform[10] = draw_3d.transform[15] = 1.0f;
    draw_3d.depth.depth_test_enable = true;
    draw_3d.depth.depth_write_enable = true;
    draw_3d.depth.depth_func = RECOMP_D3D_COMPARE_LESS_EQUAL;
    draw_3d.blend.blend_enable = false;
    draw_3d.blend.color_write_mask = 15;
    passed &= expect("submit 3D 0x112 draw", recomp_d3d_presenter_submit(presenter, &draw_cmd));

    // Test vertex program with FVF 0x112
    draw_cmd = {};
    draw_cmd.type = RECOMP_D3D_PRESENTER_COMMAND_DRAW;
    auto &draw_prog = draw_cmd.data.draw;
    draw_prog.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
    draw_prog.index_count = draw_prog.vertex_count = 4;
    draw_prog.triangle_count = 2;
    draw_prog.vertex_stride = sizeof(Vertex3D);
    draw_prog.fvf = 0x112u;
    draw_prog.vertex_bytes = mesh_verts;
    draw_prog.index_bytes = indices;
    draw_prog.has_transform = true;
    draw_prog.blend.color_write_mask = 15;
    draw_prog.program_count = 3;
    const unsigned attributes[] = {0, 0, 9}, outputs[] = {0, 3, 9};
    for (unsigned i = 0; i < 3; ++i) {
        draw_prog.program[i][1] = (1u << 21) | (attributes[i] << 9) | 0x1bu;
        draw_prog.program[i][2] = (i == 1 ? 3u : 2u) << 26;
        draw_prog.program[i][3] = (15u << 12) | (1u << 11) | (outputs[i] << 3) | (i == 2 ? 1u : 0u);
    }
    draw_prog.program[1][1] |= 109u << 13;
    draw_prog.program_constants[58][0] = 2.0f;
    draw_prog.program_constants[58][1] = -2.0f;
    draw_prog.program_constants[58][2] = 1.0f;
    draw_prog.program_constants[59][0] = draw_prog.program_constants[59][1] = 1.5f;
    draw_prog.program_constants[109][0] = 1.0f;
    draw_prog.program_constants[109][3] = 1.0f;
    passed &= expect("submit vertex program draw", recomp_d3d_presenter_submit(presenter, &draw_cmd));

    RecompD3dPresenterCommand pres_cmd{};
    pres_cmd.type = RECOMP_D3D_PRESENTER_COMMAND_PRESENT;
    pres_cmd.data.present = {5u, 1u};
    passed &= expect("draw present", recomp_d3d_presenter_submit(presenter, &pres_cmd));

    passed &= expect("destroy after draw", recomp_d3d_presenter_destroy(&presenter));
    if (passed) {
        std::puts("PASS testDraw");
    }
    return passed;
}

static bool testClose()
{
    RecompD3dPresenter *presenter = nullptr;
    if (!expect("create for close", recomp_d3d_presenter_create(&kTestConfig, &presenter))) {
        return false;
    }

    // Push close event
    SDL_Event event{};
    event.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
    SDL_PushEvent(&event);

    RecompD3dPresenterError error = tick(presenter, 1);
    bool passed = expect("close status", error, RECOMP_D3D_PRESENTER_CLOSED);

    // Sticky close check
    passed &= expect("sticky close",
        recomp_d3d_presenter_submit(presenter, nullptr),
        RECOMP_D3D_PRESENTER_CLOSED);

    passed &= expect("destroy after close", recomp_d3d_presenter_destroy(&presenter));
    if (passed) {
        std::puts("PASS testClose");
    }
    return passed;
}

static bool testMsaa()
{
    setenv("RECOMP_D3D_MSAA", "4", 1);
    RecompD3dPresenter *presenter = nullptr;
    if (!expect("create for msaa", recomp_d3d_presenter_create(&kTestConfig, &presenter))) {
        unsetenv("RECOMP_D3D_MSAA");
        return false;
    }

    struct Vertex {
        float x, y, z, rhw;
        uint32_t diffuse;
        float u, v;
    };
    Vertex vertices[] = {
        {20.0f,  20.0f,  0.25f, 1.0f, 0xffff8040u, 0.0f, 0.0f},
        {200.0f, 20.0f,  0.25f, 1.0f, 0xffff8040u, 1.0f, 0.0f},
        {20.0f,  200.0f, 0.25f, 1.0f, 0xffff8040u, 0.0f, 1.0f},
        {200.0f, 200.0f, 0.25f, 1.0f, 0xffff8040u, 1.0f, 1.0f},
    };
    uint16_t indices[] = {0, 1, 2, 3};

    auto clear = clearCommand();
    bool passed = expect("msaa clear", recomp_d3d_presenter_submit(presenter, &clear));

    RecompD3dPresenterCommand draw_cmd{};
    draw_cmd.type = RECOMP_D3D_PRESENTER_COMMAND_DRAW;
    auto &draw = draw_cmd.data.draw;
    draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
    draw.index_count = draw.vertex_count = 4;
    draw.triangle_count = 2;
    draw.vertex_stride = sizeof(Vertex);
    draw.fvf = 0x144u;
    draw.vertex_bytes = vertices;
    draw.index_bytes = indices;
    draw.blend.color_write_mask = 15;
    passed &= expect("submit msaa 2D draw", recomp_d3d_presenter_submit(presenter, &draw_cmd));

    // Test backbuffer sampling under MSAA
    draw_cmd = {};
    draw_cmd.type = RECOMP_D3D_PRESENTER_COMMAND_DRAW;
    auto &draw_bb = draw_cmd.data.draw;
    draw_bb.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
    draw_bb.index_count = draw_bb.vertex_count = 4;
    draw_bb.triangle_count = 2;
    draw_bb.vertex_stride = sizeof(Vertex);
    draw_bb.fvf = 0x144u;
    draw_bb.vertex_bytes = vertices;
    draw_bb.index_bytes = indices;
    draw_bb.blend.color_write_mask = 15;
    draw_bb.has_texture = true;
    draw_bb.texture_is_backbuffer = true;
    draw_bb.texture.width = 320;
    draw_bb.texture.height = 240;
    draw_bb.texture.data = 0x80000000u;
    passed &= expect("submit msaa backbuffer sample draw", recomp_d3d_presenter_submit(presenter, &draw_cmd));

    RecompD3dPresenterCommand pres_cmd{};
    pres_cmd.type = RECOMP_D3D_PRESENTER_COMMAND_PRESENT;
    pres_cmd.data.present = {5u, 1u};
    passed &= expect("msaa present", recomp_d3d_presenter_submit(presenter, &pres_cmd));

    passed &= expect("destroy after msaa", recomp_d3d_presenter_destroy(&presenter));
    unsetenv("RECOMP_D3D_MSAA");

    if (passed) {
        std::puts("PASS testMsaa");
    }
    return passed;
}

int main()
{
    // Run tests headless so no windows pop up during CI / test runs
    setenv("RECOMP_HEADLESS", "1", 1);
    recomp_d3d_presenter_set_immediate_present(false);

    if (!testCreateAndTicks() || !testDraw() || !testMsaa() || !testClose()) {
        std::fprintf(stderr, "SDL_GPU presenter tests failed!\n");
        return 1;
    }

    std::puts("All SDL_GPU presenter tests passed!");
    return 0;
}
