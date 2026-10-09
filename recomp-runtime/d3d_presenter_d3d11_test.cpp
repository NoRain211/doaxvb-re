#include "d3d_presenter_d3d11.cpp"

static int finish(RecompD3dPresenter *presenter, int status, uint32_t detail)
{
    const uint32_t cached = presenter->texture_count;
    if (presenter->texture_count > kTextureSlots) {
        presenter->texture_count = kTextureSlots;
    }
    releaseGraphics(presenter);
    bool released = presenter->texture_count == 0u &&
        presenter->next_texture_slot == 0u && presenter->device == nullptr &&
        presenter->context == nullptr && presenter->render_targets.size() == 0u &&
        presenter->depth_targets.size() == 0u && presenter->target_bytes == 0u &&
        presenter->render_target_view == nullptr && presenter->depth_view == nullptr &&
        presenter->depth_texture == nullptr && presenter->gamma_buffer == nullptr &&
        presenter->gamma_vertex_shader == nullptr && presenter->gamma_pixel_shader == nullptr &&
        presenter->present_target_view == nullptr;
    for (uint32_t i = 0u; i < kTextureSlots; ++i) {
        released = released && !presenter->textures[i].used &&
            presenter->textures[i].view == nullptr;
    }
    for (auto state : presenter->draw_rasterizer_states) released = released && state == nullptr;
    for (uint32_t i = 0u; i < presenter->render_targets.size(); ++i) {
        released = released && presenter->render_targets[i].render_view == nullptr &&
            presenter->render_targets[i].sample_view == nullptr;
    }
    for (uint32_t i = 0u; i < presenter->depth_targets.size(); ++i) {
        released = released && presenter->depth_targets[i].view == nullptr;
    }
    if (!released) {
        std::fprintf(stderr, "FAIL release status=%d cached=%u\n", status, cached);
        return 90;
    }
    if (status != 0) {
        std::fprintf(stderr, "FAIL status=%d detail=%u cached=%u\n",
            status, detail, cached);
    } else {
        std::printf("PASS uploads=%u cached=%u reloaded=0x%08x\n",
            kTextureSlots + 1u, cached, detail);
    }
    return status;
}

static bool containsData(const RecompD3dPresenter &presenter, uint32_t data)
{
    for (uint32_t i = 0u; i < presenter.texture_count; ++i) {
        if (presenter.textures[i].used && presenter.textures[i].data == data) {
            return true;
        }
    }
    return false;
}

static bool createTestTargets(
    RecompD3dPresenter *presenter,
    ID3D11Texture2D **color,
    ID3D11Texture2D **readback)
{
    presenter->config = {
        4u, 4u, RECOMP_D3D_PRESENTER_COLOR_FORMAT_BGRA8_UNORM,
        RECOMP_D3D_PRESENTER_DEPTH_FORMAT_D24S8};
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 4u;
    desc.Height = 4u;
    desc.MipLevels = 1u;
    desc.ArraySize = 1u;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1u;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(presenter->device->CreateTexture2D(&desc, nullptr, color)) ||
        FAILED(presenter->device->CreateRenderTargetView(
            *color, nullptr, &presenter->render_target_view))) {
        return false;
    }
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0u;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(presenter->device->CreateTexture2D(&desc, nullptr, readback))) {
        return false;
    }
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    desc.CPUAccessFlags = 0u;
    if (FAILED(presenter->device->CreateTexture2D(
            &desc, nullptr, &presenter->depth_texture)) ||
        FAILED(presenter->device->CreateDepthStencilView(
            presenter->depth_texture, nullptr, &presenter->depth_view))) {
        return false;
    }
    const D3D11_VIEWPORT viewport = {0.0f, 0.0f, 4.0f, 4.0f, 0.0f, 1.0f};
    presenter->context->RSSetViewports(1u, &viewport);
    return true;
}

static bool checkPixels(
    RecompD3dPresenter *presenter,
    ID3D11Texture2D *color,
    ID3D11Texture2D *readback,
    const char *label,
    const uint32_t expected[4])
{
    presenter->context->CopyResource(readback, color);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(presenter->context->Map(
            readback, 0u, D3D11_MAP_READ, 0u, &mapped))) {
        std::fprintf(stderr, "FAIL %s readback\n", label);
        return false;
    }
    bool passed = true;
    for (uint32_t y = 0u; y < 4u; ++y) {
        const auto *row = reinterpret_cast<const uint32_t *>(
            static_cast<const uint8_t *>(mapped.pData) + y * mapped.RowPitch);
        for (uint32_t x = 0u; x < 4u; ++x) {
            if (row[x] != expected[x]) {
                std::fprintf(stderr,
                    "FAIL %s pixel=(%u,%u) got=%08x expected=%08x\n",
                    label, x, y, row[x], expected[x]);
                passed = false;
                break;
            }
        }
        if (!passed) {
            break;
        }
    }
    presenter->context->Unmap(readback, 0u);
    return passed;
}

static bool testOffscreenDepth(
    RecompD3dPresenter *presenter,
    ID3D11Texture2D *color,
    ID3D11Texture2D *readback,
    RecompD3dPresenterDrawCommand draw,
    RecompD3dPresenterTarget target)
{
    const uint32_t blue[4] = {
        0xff0000ffu, 0xff0000ffu, 0xff0000ffu, 0xff0000ffu};
    const uint32_t green[4] = {
        0xff00ff00u, 0xff00ff00u, 0xff00ff00u, 0xff00ff00u};
    const uint32_t red = 0xffff0000u;
    draw.texture = {};
    draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
    draw.texture.bits_per_pixel = 32u;
    draw.texture.width = draw.texture.height = 1u;
    draw.texture.data = 0x00400000u;
    draw.texture_bytes = &red;
    draw.texture_byte_count = sizeof red;
    draw.depth.depth_test_enable = draw.depth.depth_write_enable = true;
    draw.depth.depth_func = RECOMP_D3D_COMPARE_LESS;
    target.no_depth = false;
    target.custom_depth = true;
    target.depth = target.color;
    target.depth.data += 0x00020000u;
    target.depth.format_byte = target.color.width == 4u ? 0x2au : 0x2eu;
    target.depth.depth = true;
    target.depth.linear = target.depth.format_byte == 0x2eu;

    const auto checkOffscreen = [&](const char *label, const uint32_t expected[4]) {
        RecompD3dPresenterDrawCommand sample = draw;
        sample.texture = draw.target.color;
        sample.texture_bytes = nullptr;
        sample.texture_byte_count = 0u;
        sample.has_texture = true;
        sample.target = {};
        sample.depth.depth_test_enable = false;
        sample.depth.depth_write_enable = false;
        return submitDraw(presenter, sample) == RECOMP_D3D_PRESENTER_OK &&
            checkPixels(presenter, color, readback, label, expected);
    };

    RecompD3dPresenterClearCommand clear = {
        true, true, false, blue[0], 0.5f, 0u};
    if (submitClear(presenter, clear) != RECOMP_D3D_PRESENTER_OK) {
        return false;
    }
    clear.target = target;
    clear.z = 1.0f;
    draw.target = target;
    draw.transform[14] = 0.25f;
    draw.has_texture = false;
    if (submitClear(presenter, clear) != RECOMP_D3D_PRESENTER_OK ||
        submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK) {
        return false;
    }
    draw.transform[14] = 0.75f;
    draw.has_texture = true;
    if (submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
        !checkOffscreen("offscreen depth occludes farther red", green)) {
        return false;
    }

    /* An offscreen clear must not raise main depth to 1; an offscreen write
       must not lower it to .25. Probe both sides without changing depth. */
    clear.target = {};
    clear.clear_depth = false;
    draw.target = {};
    draw.depth.depth_write_enable = false;
    if (submitClear(presenter, clear) != RECOMP_D3D_PRESENTER_OK ||
        submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
        !checkPixels(presenter, color, readback,
            "offscreen clear preserves main depth", blue)) {
        return false;
    }
    draw.transform[14] = 0.4f;
    draw.has_texture = false;
    if (submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
        !checkPixels(presenter, color, readback,
            "offscreen write preserves main depth", green)) {
        return false;
    }

    /* A new color target shares the written depth. EQUAL distinguishes the
       retained .25 from a newly allocated or reset depth surface. */
    target.color.data += 0x00010000u;
    clear.target = target;
    draw.target = target;
    draw.depth.depth_write_enable = true;
    draw.transform[14] = 0.75f;
    draw.has_texture = true;
    if (submitClear(presenter, clear) != RECOMP_D3D_PRESENTER_OK ||
        submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
        !checkOffscreen("shared depth rejects farther red", blue)) {
        return false;
    }
    draw.transform[14] = 0.25f;
    draw.depth.depth_func = RECOMP_D3D_COMPARE_EQUAL;
    draw.has_texture = false;
    return submitDraw(presenter, draw) == RECOMP_D3D_PRESENTER_OK &&
        checkOffscreen("shared depth retains exact written value", green);
}

static bool testOffscreenRendering(
    RecompD3dPresenter *presenter,
    ID3D11Texture2D *color,
    ID3D11Texture2D *readback,
    bool seed)
{
    float vertices[4][8] = {
        {-1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f},
        { 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f},
        {-1.0f,  1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f},
        { 1.0f,  1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f},
    };
    if (seed) {
        vertices[1][0] = vertices[3][0] = 0.0f;
    }
    const uint16_t indices[] = {0u, 1u, 2u, 3u};
    RecompD3dPresenterDrawCommand draw{};
    draw.blend.color_write_mask = 15u;
    draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
    draw.index_count = draw.vertex_count = 4u;
    draw.triangle_count = 2u;
    draw.vertex_stride = sizeof vertices[0];
    draw.fvf = 0x112u;
    draw.vertex_bytes = vertices;
    draw.index_bytes = indices;
    draw.has_transform = true;
    draw.transform[0] = draw.transform[5] =
        draw.transform[10] = draw.transform[15] = 1.0f;
    RecompD3dPresenterClearCommand clear = {
        true, true, false, 0xff0000ffu, 1.0f, 0u};
    const uint32_t blue[4] = {
        0xff0000ffu, 0xff0000ffu, 0xff0000ffu, 0xff0000ffu};
    if (seed && submitClear(presenter, clear) != RECOMP_D3D_PRESENTER_OK) {
        return false;
    }
    for (uint32_t size = 4u; size >= 2u; size -= 2u) {
        RecompD3dPresenterTarget target{};
        target.offscreen = target.no_depth = true;
        target.color.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
        target.color.bits_per_pixel = 32u;
        target.color.render_target = true;
        target.color.width = target.color.height = size;
        target.color.pitch = size * 4u;
        target.color.data = 0x00300000u + size * 0x100u;
        clear.target = target;
        clear.clear_depth = clear.clear_stencil = seed;
        if (seed) {
            /* Green geometry covers half of a red-cleared target. Neither
               operation may touch the blue back buffer. Depth/stencil clear
               flags must be ignored when no attachment is bound. */
            clear.color = 0xffff0000u;
            draw.target = target;
            if (submitClear(presenter, clear) != RECOMP_D3D_PRESENTER_OK ||
                submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
                !checkPixels(presenter, color, readback,
                    "offscreen clear/draw isolation", blue)) {
                return false;
            }
            clear.clear_color = false;
            if (submitClear(presenter, clear) != RECOMP_D3D_PRESENTER_OK ||
                !checkPixels(presenter, color, readback,
                    "no-depth clear is a no-op", blue)) {
                return false;
            }
            clear.clear_color = true;
        } else {
            /* No CPU bytes exist for these textures. Sampling must use the
               rendered contents retained through the immutable FIFO churn. */
            draw.has_texture = true;
            draw.texture = target.color;
            const uint32_t expected[4] = {
                size == 4u ? 0xff00ff00u : 0xff40bf00u,
                size == 4u ? 0xff00ff00u : 0xff40bf00u,
                size == 4u ? 0xffff0000u : 0xffbf4000u,
                size == 4u ? 0xffff0000u : 0xffbf4000u,
            };
            if (submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
                !checkPixels(presenter, color, readback,
                    "retained offscreen pixels and restored viewport", expected)) {
                return false;
            }
            /* Retarget while this resource is still bound for sampling. */
            clear.color = blue[0];
            if (submitClear(presenter, clear) != RECOMP_D3D_PRESENTER_OK ||
                submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
                !checkPixels(presenter, color, readback,
                    "sampled target cleared and resampled", blue)) {
                return false;
            }
            if (!testOffscreenDepth(presenter, color, readback, draw, target)) {
                return false;
            }
        }
    }
    return true;
}

static bool testCompressedMips(RecompD3dPresenter *presenter,
    ID3D11Texture2D *color, ID3D11Texture2D *readback)
{
    struct Vertex { float x, y, z, rhw; uint32_t color; float u, v; };
    Vertex vertices[4]{};
    const uint16_t indices[] = {0, 1, 2, 3};
    for (uint32_t i = 0; i < 4; ++i) {
        vertices[i] = {i & 1u ? 3.5f : -0.5f, i & 2u ? 3.5f : -0.5f,
            0, 1, 0xffffffffu, i & 1u ? 8.0f : 0.0f, i & 2u ? 8.0f : 0.0f};
    }
    uint8_t pixels[56]{};
    const uint16_t colors[] = {0xf800, 0xf800, 0xf800, 0xf800, 0x07e0, 0x001f, 0xffff};
    for (uint32_t block = 0; block < 7; ++block) {
        std::memcpy(pixels + block * 8, &colors[block], 2);
        std::memcpy(pixels + block * 8 + 2, &colors[block], 2);
    }
    RecompD3dPresenterDrawCommand draw{};
    draw.fvf = 0x144;
    draw.vertex_stride = sizeof(Vertex);
    draw.vertex_count = draw.index_count = 4;
    draw.triangle_count = 2;
    draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
    draw.vertex_bytes = vertices;
    draw.index_bytes = indices;
    draw.blend.color_write_mask = 15;
    draw.has_texture = true;
    draw.texture.data = 0x00650000;
    draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_DXT1;
    draw.texture.bits_per_pixel = 4;
    draw.texture.width = draw.texture.height = 8;
    draw.texture.mip_levels = 4;
    draw.texture_bytes = pixels;
    draw.texture_byte_count = sizeof pixels;
    const uint32_t white[] = {0xffffffff,0xffffffff,0xffffffff,0xffffffff};
    if (submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
        !checkPixels(presenter, color, readback, "minification samples guest mip tail", white)) return false;
    --draw.texture_byte_count;
    if (lookupTexture(presenter, draw) != nullptr) return false;
    draw.texture_byte_count = sizeof pixels;
    draw.texture.mip_levels = 1;
    const uint32_t red[] = {0xffff0000,0xffff0000,0xffff0000,0xffff0000};
    return submitDraw(presenter, draw) == RECOMP_D3D_PRESENTER_OK &&
        checkPixels(presenter, color, readback, "mip count changes cache identity", red);
}

static bool testAlphaMask(RecompD3dPresenter *presenter,
    ID3D11Texture2D *color, ID3D11Texture2D *readback)
{
    struct Vertex { float x, y, z, rhw; uint32_t color; float uv[2][2]; };
    static_assert(sizeof(Vertex) == 36u, "two UV sets");
    Vertex vertices[4]{};
    const uint16_t indices[] = {0, 1, 2, 3};
    for (uint32_t i = 0; i < 4; ++i) {
        vertices[i] = {i & 1u ? 3.5f : -0.5f, i & 2u ? 3.5f : -0.5f,
            0, 1, 0x80808080u, {{i & 1u ? 1.0f : 0.0f, i & 2u ? 1.0f : 0.0f},
                {i & 1u ? 0.0f : 1.0f, i & 2u ? 1.0f : 0.0f}}};
    }
    const uint32_t pixels[] = {0x13ff0000, 0x1300ff00, 0x130000ff, 0x13ffff00};
    const uint8_t mask[] = {0, 1, 2, 3};
    uint32_t palette[256] = {0x00123456, 0x80123456, 0xff123456, 0x40123456};
    RecompD3dPresenterDrawCommand draw{};
    draw.fvf = 0x244;
    draw.vertex_stride = sizeof(Vertex);
    draw.vertex_count = draw.index_count = 4;
    draw.triangle_count = 2;
    draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
    draw.vertex_bytes = vertices;
    draw.index_bytes = indices;
    draw.blend.color_write_mask = 15;
    draw.has_texture = draw.has_alpha_mask = true;
    draw.texture = {};
    draw.texture.data = 0x00630000;
    draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
    draw.texture.bits_per_pixel = 32;
    draw.texture.width = 4; draw.texture.height = 1;
    draw.texture_bytes = pixels;
    draw.texture_byte_count = sizeof pixels;
    draw.alpha_mask = draw.texture;
    draw.alpha_mask.data = 0x00640000;
    draw.alpha_mask.format_byte = RECOMP_D3D_TEXTURE_FORMAT_P8;
    draw.alpha_mask.bits_per_pixel = 8;
    draw.alpha_mask_bytes = mask;
    draw.alpha_mask_byte_count = sizeof mask;
    draw.alpha_mask_palette = palette;
    draw.alpha_mask_palette_byte_count = sizeof palette;
    const uint32_t expected[] = {0x00808000, 0x40000080, 0x80008000, 0x20800000};
    return submitDraw(presenter, draw) == RECOMP_D3D_PRESENTER_OK &&
        checkPixels(presenter, color, readback, "independent UV color and palette alpha", expected);
}

static bool testReflection(RecompD3dPresenter *presenter,
    ID3D11Texture2D *color, ID3D11Texture2D *readback)
{
    struct Vertex { float position[3], normal[3], uv[2], unused[2]; };
    Vertex vertices[4]{};
    const uint16_t indices[] = {0, 1, 2, 3};
    for (uint32_t i = 0; i < 4; ++i) {
        vertices[i] = {{i & 1u ? 1.0f : -1.0f, i & 2u ? -1.0f : 1.0f, 0},
            {0, 0, 1}, {0, 0}, {12345, -12345}};
    }
    const uint32_t base[] = {0x800000ff};
    const uint32_t environment[] = {0x80ff0000, 0x8000ff00};
    RecompD3dPresenterDrawCommand draw{};
    draw.fvf = 0x112;
    draw.vertex_stride = sizeof(Vertex); // Padded stream, no third UV read.
    draw.vertex_count = draw.index_count = 4;
    draw.triangle_count = 2;
    draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
    draw.vertex_bytes = vertices;
    draw.index_bytes = indices;
    draw.blend.color_write_mask = 15;
    draw.has_texture = draw.has_transform = draw.has_reflection = true;
    for (uint32_t i = 0; i < 4; ++i) {
        draw.transform[i * 5] = draw.reflection_normal[i * 5] = 1;
        draw.reflection_diffuse[i] = i == 0 || i == 3 ? 0.5f : 1.0f;
    }
    draw.reflection_world_view[14] = draw.reflection_world_view[15] = 1;
    draw.reflection_transform[8] = 0.25f;
    draw.reflection_transform[12] = 0.5f;
    draw.texture.data = 0x00660000;
    draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
    draw.texture.bits_per_pixel = 32;
    draw.texture.width = draw.texture.height = 1;
    draw.texture_bytes = base;
    draw.texture_byte_count = sizeof base;
    draw.reflection_texture = draw.texture;
    draw.reflection_texture.data = 0x00670000;
    draw.reflection_texture.width = 2;
    draw.reflection_bytes = environment;
    draw.reflection_byte_count = sizeof environment;
    const uint32_t red[] = {0x3040007f,0x3040007f,0x3040007f,0x3040007f};
    const uint32_t green[] = {0x3000807f,0x3000807f,0x3000807f,0x3000807f};
    if (submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
        !checkPixels(presenter, color, readback, "reflection alpha blend and diffuse", red)) return false;
    for (Vertex &vertex : vertices) { vertex.normal[0] = 1; vertex.normal[2] = 0; }
    draw.reflection_transform[12] -= 1; // Same texel after wrapping a negative U.
    if (submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
        !checkPixels(presenter, color, readback, "reflection generated coordinates wrap", green)) return false;
    draw.directional.enabled = true;
    for (unsigned i = 0; i < 4; ++i) draw.directional.normal_transforms[0][i * 5] = 1;
    for (unsigned i = 0; i < 3; ++i) draw.directional.ambient_emissive[i] = 0.25f;
    const uint32_t lit[] = {0x30002020,0x30002020,0x30002020,0x30002020};
    if (submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
        !checkPixels(presenter, color, readback, "reflection uses evaluated lighting and preserves alpha", lit)) return false;
    draw.reflection_mesh_uv = true;
    const uint32_t seam[] = {0x30101020,0x30101020,0x30101020,0x30101020};
    if (submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
        !checkPixels(presenter, color, readback, "mesh UV wrap seam blends neighbors", seam)) return false;
    for (Vertex &vertex : vertices) vertex.uv[0] = 0.25f;
    const uint32_t mesh_uv[] = {0x30200020,0x30200020,0x30200020,0x30200020};
    if (submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
        !checkPixels(presenter, color, readback, "environment material uses mesh UV0", mesh_uv)) return false;
    struct DiffuseVertex { float position[3]; uint32_t diffuse; float uv[2], uv1[2]; };
    DiffuseVertex diffuse_vertices[4]{};
    for (uint32_t i = 0; i < 4; ++i) {
        diffuse_vertices[i] = {{i & 1u ? 1.0f : -1.0f, i & 2u ? -1.0f : 1.0f, 0},
            0xffffffffu, {0.25f, 0}, {12345, -12345}};
    }
    auto no_normal = draw;
    no_normal.fvf = 0x242u; // The retail 0x342 stream as the adapter decodes it.
    no_normal.vertex_stride = sizeof(DiffuseVertex);
    no_normal.vertex_bytes = diffuse_vertices;
    no_normal.reflection_mesh_uv = false;
    no_normal.directional.enabled = false;
    // Eye (0,0,1) lands on the green texel; reflecting off a (0,0,1) normal would give red.
    if (submitDraw(presenter, no_normal) != RECOMP_D3D_PRESENTER_OK ||
        !checkPixels(presenter, color, readback, "reflection without a normal uses the eye direction", green)) return false;
    draw.vertex_stride = 24; // Missing UV0 remains invalid.
    return submitDraw(presenter, draw) == RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
}

static bool testFourTapFilter(
    RecompD3dPresenter *presenter,
    ID3D11Texture2D *color,
    ID3D11Texture2D *readback)
{
    struct Vertex { float x, y, z, rhw; uint32_t diffuse; float u, v; };
    struct FilterVertex { float x, y, z, rhw; float uv[4][2]; };
    static_assert(sizeof(FilterVertex) == 48u, "XYZRHW|TEX4 stride");
    Vertex vertices[4]{};
    FilterVertex taps[4]{};
    const uint16_t indices[] = {0u, 1u, 2u, 3u};
    for (uint32_t i = 0u; i < 4u; ++i) {
        vertices[i].x = taps[i].x = i & 1u ? 3.5f : -0.5f;
        vertices[i].y = taps[i].y = i & 2u ? 3.5f : -0.5f;
        vertices[i].rhw = taps[i].rhw = 1.0f;
        vertices[i].diffuse = 0xffffffffu;
    }
    RecompD3dPresenterDrawCommand seed{};
    seed.blend.color_write_mask = 15u;
    seed.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
    seed.index_count = seed.vertex_count = 4u;
    seed.triangle_count = 2u;
    seed.vertex_stride = sizeof(Vertex);
    seed.fvf = 0x144u;
    seed.vertex_bytes = vertices;
    seed.index_bytes = indices;
    seed.has_texture = true;
    seed.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
    seed.texture.bits_per_pixel = 32u;
    seed.texture.width = 4u;
    seed.texture.height = 1u;
    seed.texture_byte_count = 4u * sizeof(uint32_t);
    RecompD3dPresenterTarget source{};
    source.offscreen = source.no_depth = true;
    source.color = seed.texture;
    source.color.format_byte = 0x12u;
    source.color.render_target = source.color.linear = true;
    source.color.height = 4u;
    source.color.pitch = 16u;
    source.color.data = 0x00610000u;
    RecompD3dPresenterTarget filtered = source;
    filtered.color.data = 0x00620000u;
    RecompD3dPresenterDrawCommand draw = seed;
    draw.vertex_stride = sizeof(FilterVertex);
    draw.fvf = 0x404u;
    draw.vertex_bytes = taps;
    draw.four_tap_filter = true;
    struct Case {
        const char *label;
        uint32_t texels[4];
        uint32_t result;
        bool outside_edges;
    };
    const Case cases[] = {
        {"four distinct RGBA taps", {0x10ff0000u, 0x4000ff00u, 0x800000ffu, 0xc0408040u}, 0x64506050u, false},
        {"four-tap 128/255 coefficient", {0x80808080u, 0x80808080u, 0x80808080u, 0x80808080u}, 0x81818181u, false},
        /* Clamping each pair first gives alpha 191 and red 128. Clamping
           only the final sum gives 192 and 129; a plain average also fails
           the preceding coefficient case. */
        {"four-tap pair saturation and partial alpha", {0xffffff00u, 0xffffff00u, 0xfe027c00u, 0u}, 0xbf809f00u, false},
        {"four-tap edge clamp", {0x10ff0000u, 0x4000ff00u, 0x800000ffu, 0xc0408040u}, 0x64506050u, true},
    };
    for (uint32_t c = 0u; c < sizeof cases / sizeof cases[0]; ++c) {
        const Case &test = cases[c];
        const uint32_t expected[4] = {test.result, test.result, test.result, test.result};
        for (uint32_t i = 0u; i < 4u; ++i) {
            vertices[i].u = i & 1u ? 1.0f : 0.0f;
            vertices[i].v = i & 2u ? 1.0f : 0.0f;
        }
        seed.texture.data = 0x00600000u + c * 0x100u;
        seed.texture_bytes = test.texels;
        seed.target = source;
        if (submitDraw(presenter, seed) != RECOMP_D3D_PRESENTER_OK) return false;
        for (uint32_t linear = 0u; linear < 2u; ++linear) {
            draw.target = {};
            draw.texture_is_backbuffer = false;
            draw.texture = linear ? source.color : seed.texture;
            draw.texture_bytes = linear ? nullptr : test.texels;
            draw.texture_byte_count = linear ? 0u : sizeof test.texels;
            for (FilterVertex &vertex : taps) {
                for (uint32_t t = 0u; t < 4u; ++t) {
                    float u = (static_cast<float>(t) + 0.5f) / 4.0f;
                    if (test.outside_edges && t == 0u) u = -1.0f;
                    if (test.outside_edges && t == 3u) u = 2.0f;
                    vertex.uv[t][0] = u * (linear ? 4.0f : 1.0f);
                    vertex.uv[t][1] = linear ? 2.0f : 0.5f;
                }
            }
            char label[112];
            std::snprintf(label, sizeof label, "%s (%s UV)", test.label,
                linear ? "linear pixel" : "swizzled normalized");
            if (submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
                !checkPixels(presenter, color, readback, label, expected)) return false;
        }
        /* Refresh the current back buffer for each case. The filtered output
           must consume that snapshot without overwriting its source. */
        seed.target = {};
        draw.target = filtered;
        draw.texture_is_backbuffer = true;
        draw.texture.data = 0x00630000u;
        if (submitDraw(presenter, seed) != RECOMP_D3D_PRESENTER_OK ||
            submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
            !checkPixels(presenter, color, readback,
                "backbuffer filter preserves source", test.texels)) return false;
        RecompD3dPresenterDrawCommand consume = seed;
        consume.texture = filtered.color;
        consume.texture_bytes = nullptr;
        consume.texture_byte_count = 0u;
        for (Vertex &vertex : vertices) {
            vertex.u *= 4.0f;
            vertex.v *= 4.0f;
        }
        if (submitDraw(presenter, consume) != RECOMP_D3D_PRESENTER_OK ||
            !checkPixels(presenter, color, readback,
                "ordinary glyph consumes filtered offscreen RGBA", expected)) return false;
    }
    return true;
}

static bool testPretransformedGlyphs(
    RecompD3dPresenter *presenter,
    ID3D11Texture2D *color,
    ID3D11Texture2D *readback)
{
    struct Vertex { float x, y, z, rhw; uint32_t diffuse; float u, v; };
    Vertex vertices[] = {
        {-0.5f, -0.5f, 0.25f, 1.0f, 0x80ff8040u, 0.0f, 0.5f},
        { 1.5f, -0.5f, 0.25f, 1.0f, 0x80ff8040u, 1.0f, 0.5f},
        {-0.5f,  3.5f, 0.25f, 1.0f, 0x80ff8040u, 0.0f, 0.5f},
        { 1.5f,  3.5f, 0.25f, 1.0f, 0x80ff8040u, 1.0f, 0.5f},
    };
    const uint16_t indices[] = {0u, 1u, 2u, 3u};
    const uint8_t atlas[] = {0u, 255u};
    RecompD3dPresenterDrawCommand draw{};
    draw.blend.color_write_mask = 15u;
    draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
    draw.index_count = draw.vertex_count = 4u;
    draw.triangle_count = 2u;
    draw.vertex_stride = sizeof(Vertex);
    draw.fvf = 0x144u;
    draw.vertex_bytes = vertices;
    draw.index_bytes = indices;
    draw.has_texture = true;
    draw.texture.data = 0x00200700u;
    draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8;
    draw.texture.bits_per_pixel = 8u;
    draw.texture.width = 2u;
    draw.texture.height = 1u;
    draw.texture_bytes = atlas;
    draw.texture_byte_count = sizeof atlas;
    const RecompD3dPresenterClearCommand clear = {
        true, true, false, 0xff0000ffu, 1.0f, 0u};
    const uint32_t placed[] = {0x00ff8040u, 0x80ff8040u, 0xff0000ffu, 0xff0000ffu};
    if (submitClear(presenter, clear) != RECOMP_D3D_PRESENTER_OK ||
        submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
        !checkPixels(presenter, color, readback, "XYZRHW glyph placement, atlas alpha, diffuse", placed)) {
        return false;
    }
    /* One pixel spans the quad: RHW 1:3 moves its sample from U=.5 to
       U=.75, the opaque texel center. Ignoring RHW gives half the alpha. */
    vertices[1].x = vertices[3].x = 0.5f;
    vertices[1].rhw = vertices[3].rhw = 3.0f;
    const uint32_t perspective[] = {0x80ff8040u, 0xff0000ffu, 0xff0000ffu, 0xff0000ffu};
    if (submitClear(presenter, clear) != RECOMP_D3D_PRESENTER_OK ||
        submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
        !checkPixels(presenter, color, readback, "XYZRHW perspective atlas interpolation", perspective)) {
        return false;
    }

    /* Two identical 4-texel rows, in rectangular Morton order. Treating
       these bytes as linear indices repeats the first two colors. */
    const uint8_t palettized[] = {0u, 1u, 0u, 1u, 2u, 255u, 2u, 255u};
    uint32_t palette[256]{};
    palette[0] = 0x00ff0000u;
    palette[1] = 0x8000ff00u;
    palette[2] = 0xffffff00u;
    palette[255] = 0xff00ffffu;
    vertices[1].x = vertices[3].x = 3.5f;
    for (Vertex &vertex : vertices) {
        vertex.rhw = 1.0f;
        vertex.diffuse = 0xffffffffu;
        vertex.v = 0.25f;
    }
    draw.texture.data = 0x00200800u;
    draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_P8;
    draw.texture.width = 4u;
    draw.texture.height = 2u;
    draw.texture_bytes = palettized;
    draw.texture_byte_count = sizeof palettized;
    draw.palette_bytes = palette;
    draw.palette_byte_count = sizeof palette;
    ID3D11ShaderResourceView *original = lookupTexture(presenter, draw);
    const uint32_t colors[] = {palette[0], palette[1], palette[2], palette[255]};
    if (original == nullptr ||
        submitClear(presenter, clear) != RECOMP_D3D_PRESENTER_OK ||
        submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
        !checkPixels(presenter, color, readback, "P8 rectangular swizzle and ARGB palette", colors)) {
        return false;
    }
    draw.blend.blend_enable = true;
    draw.blend.src_factor = RECOMP_D3D_BLEND_SRC_ALPHA;
    draw.blend.dst_factor = RECOMP_D3D_BLEND_INV_SRC_ALPHA;
    draw.blend.op = RECOMP_D3D_BLEND_OP_ADD;
    const uint32_t blended[] = {0xff0000ffu, 0xbf00807fu, 0xffffff00u, 0xff00ffffu};
    if (submitClear(presenter, clear) != RECOMP_D3D_PRESENTER_OK ||
        submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
        !checkPixels(presenter, color, readback, "P8 transparent and partial alpha blending", blended)) {
        return false;
    }
    draw.blend.blend_enable = false;
    palette[255] = 0xffff00ffu;
    ID3D11ShaderResourceView *changed = lookupTexture(presenter, draw);
    const uint32_t changed_colors[] = {colors[0], colors[1], colors[2], palette[255]};
    if (changed == nullptr || changed == original ||
        submitClear(presenter, clear) != RECOMP_D3D_PRESENTER_OK ||
        submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
        !checkPixels(presenter, color, readback, "P8 same-pointer palette mutation", changed_colors)) {
        return false;
    }
    palette[255] = colors[3];
    if (lookupTexture(presenter, draw) != original ||
        submitClear(presenter, clear) != RECOMP_D3D_PRESENTER_OK ||
        submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
        !checkPixels(presenter, color, readback, "P8 restored palette cache hit", colors)) {
        return false;
    }
    draw.palette_bytes = nullptr;
    if (lookupTexture(presenter, draw) != nullptr) return false;
    draw.palette_bytes = palette;
    draw.palette_byte_count = sizeof palette - 1u;
    return lookupTexture(presenter, draw) == nullptr;
}

static bool testShadowRendering(
    RecompD3dPresenter *presenter,
    ID3D11Texture2D *color,
    ID3D11Texture2D *readback,
    RecompD3dPresenterDrawCommand draw)
{
    const uint32_t white[4] = {0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu};
    const uint32_t shaded[4] = {0xbf7f7f7fu, 0xbf7f7f7fu, 0xbf7f7f7fu, 0xbf7f7f7fu};
    const uint32_t factor[4] = {0x80402010u, 0x80402010u, 0x80402010u, 0x80402010u};
    const RecompD3dPresenterClearCommand clear = {
        true, true, true, white[0], 1.0f, 0xa2u};
    draw.depth = {};
    draw.depth.depth_test_enable = true;
    draw.depth.depth_func = RECOMP_D3D_COMPARE_LESS_EQUAL;
    draw.depth.stencil_enable = true;
    draw.depth.stencil_func = RECOMP_D3D_COMPARE_LESS_EQUAL;
    draw.depth.stencil_ref = 1u;
    draw.depth.stencil_read_mask = draw.depth.stencil_write_mask = 3u;
    draw.depth.stencil_fail = draw.depth.stencil_zfail = RECOMP_D3D_STENCIL_KEEP;
    draw.depth.stencil_pass = RECOMP_D3D_STENCIL_INVERT;
    draw.blend.color_write_mask = 0u;
    draw.use_texture_factor = true;
    draw.texture_factor = 0x80000000u;
    draw.material_alpha_mode = RECOMP_D3D_MATERIAL_ALPHA_MODULATE_TEXTURE;
    draw.material_alpha = 0.65f;

    RecompD3dDepthState cache_depth = draw.depth;
    for (uint32_t i = 0u; i < 18u; ++i) {
        cache_depth.stencil_read_mask = i < 17u ? i : 0u;
        ID3D11DepthStencilState *state = lookupDepthState(presenter, cache_depth);
        if (state == nullptr) {
            std::fprintf(stderr, "FAIL stencil cache tuple=%u unavailable\n", i);
            return false;
        }
        D3D11_DEPTH_STENCIL_DESC desc{};
        state->GetDesc(&desc);
        if (!desc.StencilEnable || desc.StencilReadMask != cache_depth.stencil_read_mask ||
            desc.StencilWriteMask != 3u ||
            desc.FrontFace.StencilFunc != D3D11_COMPARISON_LESS_EQUAL ||
            desc.FrontFace.StencilPassOp != D3D11_STENCIL_OP_INVERT) {
            std::fprintf(stderr, "FAIL stencil cache tuple=%u descriptor\n", i);
            return false;
        }
    }

    /* LEQUAL compares reference 1 against masked stencil 2; INVERT changes
       only the two writable bits, so A2 becomes A1 without coloring pixels. */
    if (submitClear(presenter, clear) != RECOMP_D3D_PRESENTER_OK ||
        submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
        !checkPixels(presenter, color, readback, "shadow stencil mark", white)) {
        return false;
    }
    draw.blend.color_write_mask = 15u;
    draw.depth.stencil_func = RECOMP_D3D_COMPARE_EQUAL;
    draw.depth.stencil_pass = RECOMP_D3D_STENCIL_ZERO;
    draw.depth.alpha_test_enable = true;
    draw.depth.alpha_func = RECOMP_D3D_COMPARE_GREATER;
    draw.depth.alpha_ref = 128u;
    /* Constant alpha 128 must reject every column, including texture alpha
       255, and discard must leave A1 available for the following pass. */
    if (submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
        !checkPixels(presenter, color, readback, "factor alpha rejects texture", white)) {
        return false;
    }
    draw.depth.alpha_func = RECOMP_D3D_COMPARE_EQUAL;
    draw.has_texture = false;
    draw.blend.blend_enable = true;
    draw.blend.src_factor = RECOMP_D3D_BLEND_SRC_ALPHA;
    draw.blend.dst_factor = RECOMP_D3D_BLEND_INV_SRC_ALPHA;
    draw.blend.op = RECOMP_D3D_BLEND_OP_ADD;
    if (submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
        !checkPixels(presenter, color, readback, "black factor overrides normals and blends", shaded)) {
        return false;
    }
    /* ZERO clears the writable low bits while preserving A0. Compare all
       eight bits with a new reference, then expose the exact ARGB factor. */
    draw.depth.stencil_read_mask = 255u;
    draw.depth.stencil_ref = 0xa0u;
    draw.depth.stencil_pass = RECOMP_D3D_STENCIL_KEEP;
    draw.has_texture = true;
    draw.blend.blend_enable = false;
    draw.texture_factor = factor[0];
    draw.material_alpha_mode = RECOMP_D3D_MATERIAL_ALPHA_SELECT_DIFFUSE;
    draw.modulate_texture_factor = true;
    return submitDraw(presenter, draw) == RECOMP_D3D_PRESENTER_OK &&
        checkPixels(presenter, color, readback, "stencil masks, reference, texture factor", factor);
}

static bool testConstantBlend(
    RecompD3dPresenter *presenter,
    ID3D11Texture2D *color,
    ID3D11Texture2D *readback)
{
    struct Vertex { float x, y, z, rhw; uint32_t diffuse; float u, v; };
    const Vertex vertices[] = {
        {-0.5f, -0.5f, 0.25f, 1.0f, 0xffffffffu, 0.0f, 0.0f},
        { 3.5f, -0.5f, 0.25f, 1.0f, 0xffffffffu, 1.0f, 0.0f},
        {-0.5f,  3.5f, 0.25f, 1.0f, 0xffffffffu, 0.0f, 1.0f},
        { 3.5f,  3.5f, 0.25f, 1.0f, 0xffffffffu, 1.0f, 1.0f},
    };
    const uint16_t indices[] = {0u, 1u, 2u, 3u};
    RecompD3dPresenterDrawCommand draw{};
    draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
    draw.index_count = draw.vertex_count = 4u;
    draw.triangle_count = 2u;
    draw.vertex_stride = sizeof(Vertex);
    draw.fvf = 0x144u;
    draw.vertex_bytes = vertices;
    draw.index_bytes = indices;
    draw.use_texture_factor = true;
    draw.texture_factor = 0xffffffffu;
    draw.blend.blend_enable = true;
    draw.blend.color_write_mask = 15u;
    draw.blend.op = RECOMP_D3D_BLEND_OP_ADD;
    const struct {
        const char *label;
        RecompD3dBlendFactor src, dst;
        uint32_t constant, background, pixel;
    } cases[] = {
        {"constant RGBA additive fade", RECOMP_D3D_BLEND_CONSTANT_COLOR,
            RECOMP_D3D_BLEND_ONE, 0x40802010u, 0x10203040u, 0x50a05050u},
        {"zero fade reuses blend state", RECOMP_D3D_BLEND_CONSTANT_COLOR,
            RECOMP_D3D_BLEND_ONE, 0u, 0x10203040u, 0x10203040u},
        {"changed fade reuses blend state", RECOMP_D3D_BLEND_CONSTANT_COLOR,
            RECOMP_D3D_BLEND_ONE, 0x20100804u, 0x10203040u, 0x30303844u},
        {"inverse constant source", RECOMP_D3D_BLEND_INV_CONSTANT_COLOR,
            RECOMP_D3D_BLEND_ZERO, 0x40802010u, 0u, 0xbf7fdfefu},
        {"constant destination", RECOMP_D3D_BLEND_ZERO,
            RECOMP_D3D_BLEND_CONSTANT_COLOR, 0x40802010u, 0xffffffffu, 0x40802010u},
        {"inverse constant destination", RECOMP_D3D_BLEND_ZERO,
            RECOMP_D3D_BLEND_INV_CONSTANT_COLOR, 0x40802010u, 0xffffffffu, 0xbf7fdfefu},
    };
    for (const auto &test : cases) {
        draw.blend.src_factor = test.src;
        draw.blend.dst_factor = test.dst;
        draw.blend.constant_color = test.constant;
        const RecompD3dPresenterClearCommand clear = {
            true, false, false, test.background, 1.0f, 0u};
        const uint32_t expected[] = {test.pixel, test.pixel, test.pixel, test.pixel};
        if (submitClear(presenter, clear) != RECOMP_D3D_PRESENTER_OK ||
            submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
            !checkPixels(presenter, color, readback, test.label, expected)) return false;
    }
    return true;
}

static bool testCullRendering(
    RecompD3dPresenter *presenter, ID3D11Texture2D *color, ID3D11Texture2D *readback)
{
    struct Vertex { float x, y, z; uint32_t color; };
    const Vertex vertices[] = {
        {-1, 1, 0.5f, 0xffffffffu}, {1, 1, 0.5f, 0xffffffffu},
        {-1,-1, 0.5f, 0xffffffffu}, {1,-1, 0.5f, 0xffffffffu},
    };
    const uint16_t clockwise[] = {0,1,2,3}, counter_clockwise[] = {1,0,3,2};
    RecompD3dPresenterDrawCommand draw{};
    draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
    draw.index_count = draw.vertex_count = 4;
    draw.triangle_count = 2;
    draw.vertex_stride = sizeof(Vertex);
    draw.fvf = 0x42;
    draw.has_transform = true;
    draw.transform[0] = draw.transform[5] = draw.transform[10] = draw.transform[15] = 1;
    draw.vertex_bytes = vertices;
    draw.blend.color_write_mask = 15;
    draw.use_texture_factor = true;
    const RecompD3dPresenterClearCommand clear = {true,false,false,0xff000000,1,0};
    for (auto mode : {RECOMP_D3D_CULL_NONE, RECOMP_D3D_CULL_CLOCKWISE,
                      RECOMP_D3D_CULL_COUNTER_CLOCKWISE, RECOMP_D3D_CULL_NONE}) {
        for (bool reverse : {false,true}) {
            if (submitClear(presenter,clear) != RECOMP_D3D_PRESENTER_OK) return false;
            draw.cull_mode = mode;
            for (unsigned i=0; i<2; ++i) {
                const bool cw = (i==0) != reverse;
                draw.index_bytes = cw ? clockwise : counter_clockwise;
                draw.texture_factor = cw ? 0xffff0000 : 0xff00ff00;
                if (submitDraw(presenter,draw) != RECOMP_D3D_PRESENTER_OK) return false;
            }
            const uint32_t expected_color = mode == RECOMP_D3D_CULL_CLOCKWISE ? 0xff00ff00 :
                mode == RECOMP_D3D_CULL_COUNTER_CLOCKWISE ? 0xffff0000 :
                reverse ? 0xffff0000 : 0xff00ff00;
            const uint32_t expected[] = {expected_color,expected_color,expected_color,expected_color};
            if (!checkPixels(presenter,color,readback,"opposite card faces honor cull mode",expected)) return false;
        }
    }
    std::printf("PASS cull mode drops the guest's back faces\n");
    return true;
}

static bool testMaterialAlphaRendering(
    RecompD3dPresenter *presenter,
    ID3D11Texture2D *color,
    ID3D11Texture2D *readback,
    RecompD3dPresenterDrawCommand draw)
{
    const uint32_t red = 0xffff0000u;
    const RecompD3dPresenterClearCommand clear = {
        true, false, false, 0xff0000ffu, 1.0f, 0u};
    draw.depth = {};
    draw.blend.color_write_mask = 15u;
    draw.blend.src_factor = RECOMP_D3D_BLEND_SRC_ALPHA;
    draw.blend.dst_factor = RECOMP_D3D_BLEND_INV_SRC_ALPHA;
    draw.blend.op = RECOMP_D3D_BLEND_OP_ADD;
    draw.use_texture_factor = false;
    draw.material_alpha = 0.65f;
    draw.has_texture = true;
    draw.texture = {};
    draw.texture.data = 0x00200300u;
    draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
    draw.texture.bits_per_pixel = 32u;
    draw.texture.width = draw.texture.height = 1u;
    draw.texture_bytes = &red;
    draw.texture_byte_count = sizeof red;
    const struct {
        const char *label;
        bool material_alpha;
        bool blend;
        bool alpha_test;
        RecompD3dCompareFunc alpha_func;
        uint32_t pixel;
    } cases[] = {
        {"material scales texture alpha", true, false, false, RECOMP_D3D_COMPARE_ALWAYS, 0xa6ff0000u},
        {"material alpha blends over blue", true, true, false, RECOMP_D3D_COMPARE_ALWAYS, 0xc5a60059u},
        {"material alpha equals 166", true, false, true, RECOMP_D3D_COMPARE_EQUAL, 0xa6ff0000u},
        {"material alpha rejects greater 166", true, false, true, RECOMP_D3D_COMPARE_GREATER, 0xff0000ffu},
        {"disabled material restores texture alpha", false, false, false, RECOMP_D3D_COMPARE_ALWAYS, 0xffff0000u},
    };
    for (const auto &test : cases) {
        draw.material_alpha_mode = test.material_alpha
            ? RECOMP_D3D_MATERIAL_ALPHA_MODULATE_TEXTURE : RECOMP_D3D_MATERIAL_ALPHA_NONE;
        draw.blend.blend_enable = test.blend;
        draw.depth.alpha_test_enable = test.alpha_test;
        draw.depth.alpha_func = test.alpha_func;
        draw.depth.alpha_ref = 166u;
        const uint32_t expected[4] = {test.pixel, test.pixel, test.pixel, test.pixel};
        if (submitClear(presenter, clear) != RECOMP_D3D_PRESENTER_OK ||
            submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
            !checkPixels(presenter, color, readback, test.label, expected)) {
            return false;
        }
    }
    const uint32_t textures[] = {0x00ff0000u, 0xffff0000u};
    for (uint32_t i = 0u; i < 4u; ++i) {
        draw.material_alpha_mode = i < 2u ? RECOMP_D3D_MATERIAL_ALPHA_SELECT_DIFFUSE
            : RECOMP_D3D_MATERIAL_ALPHA_MODULATE_TEXTURE;
        draw.material_alpha = i < 2u ? 0.25f : 0.65f;
        draw.texture.data = 0x00200400u + (i & 1u) * 0x100u;
        draw.texture_bytes = &textures[i & 1u];
        const uint32_t pixel = i < 2u ? 0x40ff0000u
            : (i & 1u) ? 0xa6ff0000u : 0x00ff0000u;
        const uint32_t expected[4] = {pixel, pixel, pixel, pixel};
        char label[60];
        std::snprintf(label, sizeof label, "material %s texture alpha=%u",
            i < 2u ? "SELECTDIFFUSE" : "MODULATE", (i & 1u) * 255u);
        if (submitClear(presenter, clear) != RECOMP_D3D_PRESENTER_OK ||
            submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
            !checkPixels(presenter, color, readback, label, expected)) {
            return false;
        }
    }
    const uint32_t texel = 0xff804020u;
    draw.texture.data = 0x00200600u;
    draw.texture_bytes = &texel;
    const struct {
        const char *label;
        bool modulate;
        RecompD3dMaterialAlphaMode material;
        uint32_t factor;
        uint32_t pixel;
    } factor_cases[] = {
        {"stage1 combines material and factor alpha", true,
            RECOMP_D3D_MATERIAL_ALPHA_MODULATE_TEXTURE, 0xa5ffffffu, 0x6b804020u},
        {"disabled stage1 retains material alpha", false,
            RECOMP_D3D_MATERIAL_ALPHA_MODULATE_TEXTURE, 0xa5ffffffu, 0xa6804020u},
        {"stage1 multiplies every RGBA channel", true,
            RECOMP_D3D_MATERIAL_ALPHA_NONE, 0x800080ffu, 0x80002020u},
        {"disabled stage1 restores texture RGBA", false,
            RECOMP_D3D_MATERIAL_ALPHA_NONE, 0x800080ffu, 0xff804020u},
    };
    for (const auto &test : factor_cases) {
        draw.modulate_texture_factor = test.modulate;
        draw.material_alpha_mode = test.material;
        draw.texture_factor = test.factor;
        const uint32_t expected[4] = {test.pixel, test.pixel, test.pixel, test.pixel};
        if (submitClear(presenter, clear) != RECOMP_D3D_PRESENTER_OK ||
            submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
            !checkPixels(presenter, color, readback, test.label, expected)) {
            return false;
        }
    }
    const struct {
        const char *label;
        bool zero_rgb;
        bool replace_factor;
        bool modulate_factor;
        RecompD3dMaterialAlphaMode material;
        uint32_t texel;
        uint32_t factor;
        uint32_t pixel;
    } zero_diffuse_cases[] = {
        {"zero diffuse preserves texture alpha", true, false, false,
            RECOMP_D3D_MATERIAL_ALPHA_NONE, 0x80804020u, 0xa5ffffffu, 0x80000000u},
        {"zero diffuse retains material and factor alpha", true, false, true,
            RECOMP_D3D_MATERIAL_ALPHA_MODULATE_TEXTURE, 0xff804020u, 0xa5ffffffu, 0x6b000000u},
        {"disabled zero diffuse restores RGB", false, false, true,
            RECOMP_D3D_MATERIAL_ALPHA_MODULATE_TEXTURE, 0xff804020u, 0xa5ffffffu, 0x6b804020u},
        {"texture factor replacement overrides zero diffuse", true, true, true,
            RECOMP_D3D_MATERIAL_ALPHA_MODULATE_TEXTURE, 0x80804020u, 0xa5804020u, 0xa5804020u},
    };
    draw.material_alpha = 0.65f;
    for (uint32_t i = 0u; i < sizeof zero_diffuse_cases / sizeof zero_diffuse_cases[0]; ++i) {
        const auto &test = zero_diffuse_cases[i];
        draw.zero_diffuse_rgb = test.zero_rgb;
        draw.use_texture_factor = test.replace_factor;
        draw.modulate_texture_factor = test.modulate_factor;
        draw.material_alpha_mode = test.material;
        draw.texture.data = 0x00200700u + i * 0x100u;
        draw.texture_bytes = &test.texel;
        draw.texture_factor = test.factor;
        const uint32_t expected[4] = {test.pixel, test.pixel, test.pixel, test.pixel};
        if (submitClear(presenter, clear) != RECOMP_D3D_PRESENTER_OK ||
            submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
            !checkPixels(presenter, color, readback, test.label, expected)) {
            return false;
        }
    }
    return true;
}

static bool testAlphaRendering(
    RecompD3dPresenter *presenter,
    ID3D11Texture2D *color,
    ID3D11Texture2D *readback)
{
    /* The oversized triangle samples texel centers across the 4x4 target.
       DXT3 stores alpha 0, 85, 170, 255 in each row; every texel is red. */
    float vertices[3][8] = {
        {-1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f},
        { 3.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 2.0f, 1.0f},
        {-1.0f,  3.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,-1.0f},
    };
    const uint16_t indices[] = {0u, 1u, 2u};
    const uint8_t red[16] = {
        0x50u, 0xfau, 0x50u, 0xfau, 0x50u, 0xfau, 0x50u, 0xfau,
        0x00u, 0xf8u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u,
    };
    const uint32_t blue = 0xff0000ffu;
    const uint32_t green = 0xff00ff00u;
    RecompD3dPresenterDrawCommand draw{};
    draw.blend.color_write_mask = 15u;
    draw.primitive_type = RECOMP_D3D_PT_TRIANGLELIST;
    draw.index_count = 3u;
    draw.triangle_count = 1u;
    draw.vertex_count = 3u;
    draw.vertex_stride = sizeof vertices[0];
    draw.fvf = 0x112u; /* XYZ|NORMAL|TEX1, without diffuse vertex color. */
    draw.vertex_bytes = vertices;
    draw.index_bytes = indices;
    draw.has_transform = true;
    draw.transform[0] = draw.transform[5] =
        draw.transform[10] = draw.transform[15] = 1.0f;
    draw.transform[14] = 0.25f;
    draw.depth.depth_test_enable = true;
    draw.depth.depth_write_enable = true;
    draw.depth.depth_func = RECOMP_D3D_COMPARE_LESS;
    draw.has_texture = true;
    draw.texture.data = 0x00200000u;
    draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_DXT3;
    draw.texture.bits_per_pixel = 8u;
    draw.texture.width = draw.texture.height = 4u;
    draw.texture_bytes = red;
    draw.texture_byte_count = sizeof red;
    const RecompD3dPresenterClearCommand clear = {
        true, true, false, 0xff000000u, 1.0f, 0u};
    const uint32_t preserved_alpha[] = {
        0x00ff0000u, 0x55ff0000u, 0xaaff0000u, 0xffff0000u};
    if (submitClear(presenter, clear) != RECOMP_D3D_PRESENTER_OK ||
        submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK) {
        return false;
    }
    bool passed = checkPixels(
        presenter, color, readback, "texture alpha preserved", preserved_alpha);

    RecompD3dPresenterClearCommand mask_clear = clear;
    mask_clear.color = 0x12345678u;
    const struct { uint8_t mask; uint32_t bytes; } masks[] = {
        {1u, 0x00ff0000u}, {2u, 0x0000ff00u},
        {4u, 0x000000ffu}, {8u, 0xff000000u},
        {15u, 0xffffffffu}, {0u, 0u},
        {1u, 0x00ff0000u}, {15u, 0xffffffffu},
    };
    for (const auto &test : masks) {
        draw.blend.color_write_mask = test.mask;
        uint32_t expected[4];
        for (uint32_t x = 0u; x < 4u; ++x) {
            expected[x] = (preserved_alpha[x] & test.bytes) |
                (mask_clear.color & ~test.bytes);
        }
        char label[40];
        std::snprintf(label, sizeof label, "color write mask=%u", unsigned(test.mask));
        if (submitClear(presenter, mask_clear) != RECOMP_D3D_PRESENTER_OK ||
            submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
            !checkPixels(presenter, color, readback, label, expected)) {
            return false;
        }
    }

    /* Seventeen distinct tuples exceed the sixteen-state cache; the final
       request revisits the first tuple after eviction. */
    RecompD3dBlendState cache_blend = draw.blend;
    cache_blend.src_factor = RECOMP_D3D_BLEND_ONE;
    cache_blend.dst_factor = RECOMP_D3D_BLEND_ZERO;
    cache_blend.op = RECOMP_D3D_BLEND_OP_ADD;
    for (uint32_t i = 0u; i < 18u; ++i) {
        cache_blend.blend_enable = i == 16u;
        cache_blend.color_write_mask = static_cast<uint8_t>(i < 16u ? i : 0u);
        ID3D11BlendState *state = lookupBlendState(presenter, cache_blend);
        if (state == nullptr) {
            std::fprintf(stderr, "FAIL blend cache tuple=%u unavailable\n", i);
            return false;
        }
        D3D11_BLEND_DESC desc{};
        state->GetDesc(&desc);
        if (desc.RenderTarget[0].RenderTargetWriteMask != cache_blend.color_write_mask ||
            (desc.RenderTarget[0].BlendEnable != FALSE) != cache_blend.blend_enable) {
            std::fprintf(stderr, "FAIL blend cache tuple=%u descriptor\n", i);
            return false;
        }
    }

    /* Suppressing every color channel must still write depth at .25. */
    const uint32_t untouched[4] = {
        mask_clear.color, mask_clear.color, mask_clear.color, mask_clear.color};
    draw.blend.color_write_mask = 0u;
    if (submitClear(presenter, mask_clear) != RECOMP_D3D_PRESENTER_OK ||
        submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
        !checkPixels(presenter, color, readback, "zero mask preserves color", untouched)) {
        return false;
    }
    draw.blend.color_write_mask = 15u;
    draw.transform[14] = 0.5f;
    if (submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
        !checkPixels(presenter, color, readback, "zero mask writes depth", untouched)) {
        return false;
    }
    draw.transform[14] = 0.125f;
    if (submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
        !checkPixels(presenter, color, readback, "all color writes restored", preserved_alpha)) {
        return false;
    }
    draw.transform[14] = 0.25f;

    const struct {
        RecompD3dCompareFunc func;
        uint32_t ref;
        uint32_t passing_columns;
        float u_offset;
    } cases[] = {
        {RECOMP_D3D_COMPARE_NEVER,          85u, 0x0u},
        {RECOMP_D3D_COMPARE_LESS,           85u, 0x1u},
        {RECOMP_D3D_COMPARE_EQUAL,          85u, 0x2u},
        {RECOMP_D3D_COMPARE_LESS_EQUAL,     85u, 0x3u},
        {RECOMP_D3D_COMPARE_GREATER,        85u, 0xcu},
        {RECOMP_D3D_COMPARE_NOT_EQUAL,      85u, 0xdu},
        {RECOMP_D3D_COMPARE_GREATER_EQUAL,  85u, 0xeu},
        {RECOMP_D3D_COMPARE_ALWAYS,         85u, 0xfu},
        {RECOMP_D3D_COMPARE_GREATER,         0u, 0xeu},
        {RECOMP_D3D_COMPARE_EQUAL,         255u, 0x8u},
        {RECOMP_D3D_COMPARE_GREATER,       255u, 0x0u},
        /* Linear filtering yields alpha 85.33 in column 1: byte rounding
           must preserve EQ 85, while a raw float comparison rejects it. */
        {RECOMP_D3D_COMPARE_EQUAL,          85u, 0x2u, 1.0f / 1024.0f},
    };
    for (const auto &test : cases) {
        vertices[0][6] = vertices[2][6] = test.u_offset;
        vertices[1][6] = 2.0f + test.u_offset;
        draw.depth.alpha_test_enable = true;
        draw.depth.alpha_func = test.func;
        draw.depth.alpha_ref = test.ref;
        if (submitClear(presenter, clear) != RECOMP_D3D_PRESENTER_OK ||
            submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK) {
            return false;
        }
        /* Rejected red pixels must leave depth untouched so blue can render.
           Farther green must then fail depth behind both blue and red. */
        RecompD3dPresenterDrawCommand background = draw;
        background.depth.alpha_test_enable = false;
        background.transform[14] = 0.5f;
        background.texture.data = 0x00200100u;
        background.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
        background.texture.bits_per_pixel = 32u;
        background.texture.width = background.texture.height = 1u;
        background.texture_bytes = &blue;
        background.texture_byte_count = sizeof blue;
        if (submitDraw(presenter, background) != RECOMP_D3D_PRESENTER_OK) {
            return false;
        }
        background.transform[14] = 0.75f;
        background.texture.data = 0x00200200u;
        background.texture_bytes = &green;
        if (submitDraw(presenter, background) != RECOMP_D3D_PRESENTER_OK) {
            return false;
        }
        uint32_t expected[4];
        for (uint32_t x = 0u; x < 4u; ++x) {
            expected[x] = (test.passing_columns & (1u << x)) != 0u
                ? preserved_alpha[x] : blue;
        }
        char label[80];
        std::snprintf(label, sizeof label, "alpha func=%u ref=%u u=%g depth",
            static_cast<unsigned>(test.func), test.ref, test.u_offset);
        passed &= checkPixels(presenter, color, readback, label, expected);
    }
    return passed && testMaterialAlphaRendering(presenter, color, readback, draw) &&
        testShadowRendering(presenter, color, readback, draw);
}

static bool testLinearTextureUpdates(
    RecompD3dPresenter *presenter,
    ID3D11Texture2D *readback)
{
    uint32_t pixels[4][5]{};
    const uint32_t frames[2][4] = {
        {0xffff0000u, 0xff00ff00u, 0xff0000ffu, 0xffffffffu},
        {0xff112233u, 0x80445566u, 0x00778899u, 0xffaabbccu},
    };
    RecompD3dPresenterDrawCommand draw{};
    draw.has_texture = true;
    draw.texture = {0x12u, 32u, false, false, true, 4u, 4u,
        sizeof pixels[0], 0x00600000u};
    draw.texture_bytes = pixels;
    /* The final row needs its pixels, but not trailing padding. */
    draw.texture_byte_count = sizeof pixels - sizeof pixels[0][4];
    ID3D11ShaderResourceView *original = nullptr;
    for (const auto &frame : frames) {
        for (auto &row : pixels) {
            std::memcpy(row, frame, sizeof frame);
            row[4] = 0xff000000u;
        }
        ID3D11ShaderResourceView *view = lookupTexture(presenter, draw);
        if (view == nullptr || (original != nullptr && view != original)) {
            std::fprintf(stderr, "FAIL linear texture upload/cache reuse\n");
            return false;
        }
        original = view;
        ID3D11Resource *resource = nullptr;
        view->GetResource(&resource);
        const bool passed = checkPixels(presenter,
            static_cast<ID3D11Texture2D *>(resource), readback,
            "linear BGRA same-buffer update with row padding", frame);
        releaseCom(resource);
        if (!passed) return false;
    }
    --draw.texture_byte_count;
    if (lookupTexture(presenter, draw) != nullptr) return false;
    ++draw.texture_byte_count;
    draw.texture.pitch = 15u;
    if (lookupTexture(presenter, draw) != nullptr) return false;
    draw.texture.pitch = sizeof pixels[0];
    draw.texture.bits_per_pixel = 16u;
    if (lookupTexture(presenter, draw) != nullptr) return false;
    draw.texture.bits_per_pixel = 32u;
    draw.texture.linear = false;
    return lookupTexture(presenter, draw) == nullptr;
}

static int testTextureCache(RecompD3dPresenter *presenter, uint32_t &detail)
{
    constexpr uint32_t first_data = 0x00100000u;
    constexpr uint32_t data_stride = 0x00000100u;
    const uint8_t dxt1_block[8] = {
        0xffu, 0xffu, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u};
    RecompD3dPresenterDrawCommand draw{};
    draw.blend.color_write_mask = 15u;
    draw.has_texture = true;
    draw.texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_DXT1;
    draw.texture.bits_per_pixel = 4u;
    draw.texture.width = 4u;
    draw.texture.height = 4u;
    draw.texture_bytes = dxt1_block;
    draw.texture_byte_count = sizeof dxt1_block;
    for (uint32_t i = 0u; i <= kTextureSlots; ++i) {
        draw.texture.data = first_data + i * data_stride;
        detail = i + 1u;
        if (lookupTexture(presenter, draw) == nullptr) {
            return 20;
        }
        if (presenter->texture_count > kTextureSlots) {
            return 30;
        }
    }

    uint32_t evicted_data = 0u;
    uint32_t missing = 0u;
    for (uint32_t i = 0u; i < kTextureSlots; ++i) {
        const uint32_t data = first_data + i * data_stride;
        if (!containsData(*presenter, data)) {
            evicted_data = data;
            ++missing;
        }
    }
    if (missing != 1u) {
        detail = missing;
        return 40;
    }
    draw.texture.data = evicted_data;
    detail = evicted_data;
    if (lookupTexture(presenter, draw) == nullptr ||
        presenter->texture_count != kTextureSlots) {
        return 50;
    }

    // The same buffer refilled with other texels must not return the old texture.
    ID3D11ShaderResourceView *stale = lookupTexture(presenter, draw);
    if (stale == nullptr) return 60;
    stale->AddRef();  // Keep its address from being reused by the rebuilt view.
    const uint8_t refilled[sizeof dxt1_block] = {0x00u, 0xf8u};
    draw.texture_bytes = refilled;
    ID3D11ShaderResourceView *fresh = lookupTexture(presenter, draw);
    stale->Release();
    if (fresh == nullptr || fresh == stale) return 60;
    uint32_t matches = 0u;
    for (uint32_t i = 0u; i < presenter->texture_count; ++i) {
        const TextureEntry &entry = presenter->textures[i];
        if (!entry.used || entry.data != evicted_data) continue;
        ++matches;
        if (entry.fingerprint != textureFingerprint(refilled, sizeof refilled)) return 70;
    }
    if (matches != 1u) return 70;
    return 0;
}

static bool testFog(RecompD3dPresenter *presenter,
    ID3D11Texture2D *color, ID3D11Texture2D *readback)
{
    struct Vertex { float x, y, z, rhw; uint32_t specular; };
    Vertex vertices[] = {{0,0,0.25f,0.5f,0x80000000u}, {4,0,0.25f,0.5f,0x80000000u},
        {0,4,0.25f,0.5f,0x80000000u}, {4,4,0.25f,0.5f,0x80000000u}};
    const uint16_t indices[] = {0,1,2,3};
    RecompD3dPresenterDrawCommand draw{};
    draw.fvf = 0x084u; // XYZRHW | SPECULAR
    draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
    draw.index_count = draw.vertex_count = 4;
    draw.triangle_count = 2;
    draw.vertex_stride = sizeof(Vertex);
    draw.vertex_bytes = vertices;
    draw.index_bytes = indices;
    draw.has_transform = true;
    draw.transform[0] = draw.transform[5] = draw.transform[10] = draw.transform[15] = 1;
    draw.blend.color_write_mask = 15;
    draw.use_texture_factor = true;
    draw.texture_factor = 0x80ffffffu;
    draw.fog.enabled = true;
    draw.fog.color = 0x000000ffu; // Fog alpha must not replace source alpha.
    draw.fog.end = 4;
    const RecompD3dPresenterClearCommand clear = {true, false, false, 0x00ff0000u, 1, 0};
    const struct { uint32_t mode; float density; const char *label; } cases[] = {
        {0,0,"RHW vertex fog uses specular alpha"},
        {1,0.25541281f,"EXP uses reciprocal RHW"},
        {2,0.35736033f,"EXP2 uses reciprocal RHW"},
        {3,0,"linear uses reciprocal RHW, not screen Z"},
    };
    for (const auto &test : cases) {
        draw.fog.mode = test.mode;
        draw.fog.density = test.density;
        // Keep exponential results away from a half-byte quantization boundary.
        const uint32_t pixel = test.mode == 1 || test.mode == 2 ? 0x809999ffu : 0x808080ffu;
        const uint32_t expected[] = {pixel,pixel,pixel,pixel};
        if (submitClear(presenter, clear) != RECOMP_D3D_PRESENTER_OK ||
            submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
            !checkPixels(presenter, color, readback, test.label, expected)) return false;
    }
    draw.fog.enabled = false;
    const uint32_t unfogged[] = {0x80ffffffu,0x80ffffffu,0x80ffffffu,0x80ffffffu};
    if (submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
        !checkPixels(presenter, color, readback, "disabled fog preserves RGB and alpha", unfogged)) return false;
    draw.fog.enabled = true;
    draw.fog.mode = 3;
    draw.fog.density = 0;
    draw.fog.end = 2;
    draw.blend.blend_enable = true;
    draw.blend.src_factor = draw.blend.dst_factor = RECOMP_D3D_BLEND_ONE;
    const uint32_t additive[] = {0x80ff00ffu,0x80ff00ffu,0x80ff00ffu,0x80ff00ffu};
    if (submitClear(presenter, clear) != RECOMP_D3D_PRESENTER_OK ||
        submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
        !checkPixels(presenter, color, readback, "additive draw fogs to fog color before blending", additive)) return false;
    draw.blend.blend_enable = false;
    draw.fog_z = true;
    draw.fog.end = 0.5f;
    const uint32_t zfog[] = {0x808080ffu,0x808080ffu,0x808080ffu,0x808080ffu};
    if (submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
        !checkPixels(presenter, color, readback, "affine fog uses screen Z", zfog)) return false;
    for (unsigned i = 0; i < 4; ++i) {
        vertices[i].x -= 0.5f; vertices[i].y -= 0.5f;
        vertices[i].z = i & 1 ? 0.5f : 0;
        vertices[i].rhw = i & 1 ? 0.25f : 1;
    }
    const uint32_t varying_z[] = {0x80dfdfffu,0x809f9fffu,0x806060ffu,0x802020ffu};
    if (submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
        !checkPixels(presenter, color, readback, "Z fog is affine with varying RHW", varying_z)) return false;
    draw.fog.range = true; // XYZRHW still has only device Z/W, not an eye position.
    if (submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
        !checkPixels(presenter, color, readback, "pretransformed range request retains Z fog", varying_z)) return false;
    const float xyz[][3] = {{-1,1,0},{1,1,0},{-1,-1,0},{1,-1,0}};
    draw.fvf = 0x002u;
    draw.vertex_stride = sizeof xyz[0];
    draw.vertex_bytes = xyz;
    draw.fog_z = false;
    draw.fog.end = 2 * std::sqrt(6.0f);
    for (unsigned i = 0; i < 4; ++i) draw.fog_world_view[0][i*5] = 1;
    draw.fog_world_view[0][14] = 2;
    return submitDraw(presenter, draw) == RECOMP_D3D_PRESENTER_OK &&
        checkPixels(presenter, color, readback, "range fog uses translated eye distance", zfog);
}

static bool testVertexBlending(
    RecompD3dPresenter *presenter, ID3D11Texture2D *color, ID3D11Texture2D *readback)
{
    const uint16_t indices[] = {0u, 1u, 2u, 3u};
    const uint32_t expected[] = {0xff0000ffu, 0xff0000ffu, 0xff00ff00u, 0xff00ff00u};
    for (uint32_t count = 1u; count <= 3u; ++count) {
        float vertices[44]{};
        const uint32_t stride = 8u + count;
        for (uint32_t v = 0u; v < 4u; ++v) {
            float *p = vertices + v * stride;
            p[0] = v & 1u ? 0.0f : -1.0f;
            p[1] = v & 2u ? 1.0f : -1.0f;
            for (uint32_t i = 0u; i < count; ++i) p[3u + i] = 0.25f;
            p[4u + count] = 1.0f; // Normal gives green diagnostic shading.
        }
        RecompD3dPresenterDrawCommand draw{};
        draw.blend.color_write_mask = 15u;
        draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
        draw.index_count = draw.vertex_count = 4u;
        draw.triangle_count = 2u;
        draw.vertex_stride = stride * sizeof(float);
        draw.fvf = 0x114u + 2u * count;
        draw.vertex_bytes = vertices;
        draw.index_bytes = indices;
        draw.has_transform = true;
        draw.blend_weight_count = count;
        for (uint32_t i = 0u; i <= count; ++i) {
            float *m = i ? draw.blend_transforms[i - 1u] : draw.transform;
            m[0] = m[5] = m[10] = m[15] = 1.0f;
            m[12] = i == count ? static_cast<float>(count + 1u) : 2.0f * i - 2.0f;
        }
        /* Different matrices and the implied final weight move the left
           half-screen quad exactly one clip-space unit to the right. */
        RecompD3dPresenterClearCommand clear = {true, true, false, 0xff0000ffu, 1.0f, 0u};
        if (submitClear(presenter, clear) != RECOMP_D3D_PRESENTER_OK ||
            submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
            !checkPixels(presenter, color, readback, "weighted vertex positions", expected)) return false;
        draw.blend_weight_count = count == 1u ? 2u : 1u;
        if (submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND) return false;
        draw.blend_weight_count = 0u;
        draw.transform[12] = 0.0f;
        for (uint32_t v = 0u; v < 4u; ++v) vertices[v * stride + 3u] = NAN;
        const uint32_t unblended[] = {0xff00ff00u, 0xff00ff00u, 0xff0000ffu, 0xff0000ffu};
        if (submitClear(presenter, clear) != RECOMP_D3D_PRESENTER_OK ||
            submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
            !checkPixels(presenter, color, readback, "disabled blending ignores weights", unblended)) return false;
        draw.blend_weight_count = count;
        for (uint32_t v = 0u; v < 4u; ++v) {
            for (uint32_t i = 0u; i < count; ++i) vertices[v * stride + 3u + i] = i ? 0.0f : 1.0f;
        }
        std::memset(draw.blend_transforms, 0, sizeof draw.blend_transforms);
        if (submitClear(presenter, clear) != RECOMP_D3D_PRESENTER_OK ||
            submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
            !checkPixels(presenter, color, readback, "unused zero blend matrices", unblended)) return false;
    }
    return true;
}

static LRESULT CALLBACK closeOnShowWindowProc(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_SHOWWINDOW && wparam) {
        PostMessageW(window, WM_CLOSE, 0u, 0u);
    }
    return presenterWindowProc(window, message, wparam, lparam);
}

/* Regression for the portrait-support pipeline-capacity bug: the real r353
   stream creates eight distinct FVF layouts with 0x244 displacing 0x118. The
   ninth distinct FVF (0x118) used to be rejected permanently; it must evict
   the oldest layout, draw, and the evicted layout must rebuild on return. */
static bool testDrawPipelineEviction(
    RecompD3dPresenter *borrowed)
{
    RecompD3dPresenter presenter{};
    presenter.config = borrowed->config;
    presenter.owner_thread = GetCurrentThreadId();
    presenter.device = borrowed->device;
    presenter.device->AddRef();
    presenter.context = borrowed->context;
    presenter.context->AddRef();
    ID3D11Texture2D *color = nullptr;
    ID3D11Texture2D *readback = nullptr;
    bool passed = createTestTargets(&presenter, &color, &readback);

    const uint16_t indices[] = {0u, 1u, 2u, 3u};
    RecompD3dPresenterDrawCommand draw{};
    draw.blend.color_write_mask = 15u;
    draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
    draw.index_count = draw.vertex_count = 4u;
    draw.triangle_count = 2u;
    draw.index_bytes = indices;
    draw.has_transform = true;
    draw.transform[0] = draw.transform[5] =
        draw.transform[10] = draw.transform[15] = 1.0f;
    const RecompD3dPresenterClearCommand clear = {
        true, true, false, 0xff0000ffu, 1.0f, 0u};
    const uint32_t order[] = {
        0x104u, 0x144u, 0x112u, 0x142u, 0x404u, 0x244u, 0x11au, 0x116u,
        0x118u, 0x104u};
    for (uint32_t i = 0u; i < sizeof order / sizeof order[0] && passed; ++i) {
        RecompD3dVertexLayout layout{};
        passed = recomp_d3d_fvf_layout(order[i], &layout);
        if (!passed) break;
        float vertices[64]{};
        for (uint32_t v = 0u; v < 4u; ++v) {
            float *p = vertices + v * (layout.stride / sizeof(float));
            p[0] = v & 1u ? 1.0f : -1.0f;
            p[1] = v & 2u ? 1.0f : -1.0f;
            if (layout.pretransformed) {
                p[0] = v & 1u ? 3.5f : -0.5f;
                p[1] = v & 2u ? 3.5f : -0.5f;
            }
            p[2] = 0.25f;
            if (layout.pretransformed || layout.blend_weight_count) p[3] = 1.0f;
            if (layout.normal_offset != RECOMP_D3D_FVF_ABSENT) {
                p[layout.normal_offset / sizeof(float) + 1u] = 1.0f;
            }
            if (layout.diffuse_offset != RECOMP_D3D_FVF_ABSENT) {
                const uint32_t white = 0xffffffffu;
                std::memcpy(reinterpret_cast<uint8_t *>(p) + layout.diffuse_offset,
                    &white, sizeof white);
            }
        }
        draw.fvf = order[i];
        draw.vertex_stride = layout.stride;
        draw.vertex_bytes = vertices;
        draw.blend_weight_count = layout.blend_weight_count;
        passed = submitClear(&presenter, clear) == RECOMP_D3D_PRESENTER_OK &&
            submitDraw(&presenter, draw) == RECOMP_D3D_PRESENTER_OK;
        const uint32_t slot = i == 9u ? 0u : i;
        passed = passed && presenter.draw_pipeline_count ==
            (i < 9u ? i + 1u : 9u) &&
            presenter.draw_pipelines[slot].used &&
            presenter.draw_pipelines[slot].fvf == order[i];
        if (!passed) std::fprintf(stderr, "FAIL pipeline cache draw=%u fvf=0x%X\n", i, order[i]);
        if (passed && i >= 8u) {
            const uint32_t pixel = i == 8u ? 0xff00ff00u : 0xffbfbfc7u;
            const uint32_t expected[] = {pixel, pixel, pixel, pixel};
            passed = checkPixels(&presenter, color, readback,
                "nine-layout working set and return", expected);
        }
    }
    releaseCom(readback);
    releaseCom(color);
    releasePresenter(&presenter);
    return passed;
}

static bool testWindowClose(RecompD3dPresenter *warp)
{
    IDXGIDevice *dxgi_device = nullptr;
    IDXGIAdapter *adapter = nullptr;
    IDXGIFactory *factory = nullptr;
    HRESULT result = warp->device->QueryInterface(IID_PPV_ARGS(&dxgi_device));
    if (SUCCEEDED(result)) result = dxgi_device->GetAdapter(&adapter);
    if (SUCCEEDED(result)) result = adapter->GetParent(IID_PPV_ARGS(&factory));
    releaseCom(adapter);
    releaseCom(dxgi_device);
    if (FAILED(result)) return false;

    bool passed = true;
    for (unsigned scenario = 0u; scenario < 5u && passed; ++scenario) {
        RecompD3dPresenter presenter{};
        presenter.config = warp->config;
        presenter.config.width = 320u;
        presenter.config.height = 240u;
        presenter.widescreen = scenario == 1u;
        presenter.owner_thread = GetCurrentThreadId();
        presenter.device = warp->device;
        presenter.device->AddRef();
        presenter.context = warp->context;
        presenter.context->AddRef();
        passed = createWindow(&presenter);
        RECT client{};
        if (passed) passed = GetClientRect(presenter.window, &client) &&
            client.right - client.left ==
                static_cast<LONG>(presentClientWidth(&presenter, presenter.config.height)) &&
            client.bottom - client.top == static_cast<LONG>(presenter.config.height);
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferDesc.Width = presenter.config.width;
        desc.BufferDesc.Height = presenter.config.height;
        desc.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1u;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 1u;
        desc.OutputWindow = presenter.window;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        if (passed) passed = SUCCEEDED(factory->CreateSwapChain(
            presenter.device, &desc, &presenter.swap_chain));
        ID3D11Texture2D *back = nullptr;
        if (passed) passed = SUCCEEDED(presenter.swap_chain->GetBuffer(0u, IID_PPV_ARGS(&back)));
        if (passed) passed = SUCCEEDED(presenter.device->CreateRenderTargetView(
            back, nullptr, &presenter.present_target_view));
        D3D11_TEXTURE2D_DESC game_desc{};
        if (passed) back->GetDesc(&game_desc);
        releaseCom(back);
        game_desc.MiscFlags = 0u;
        if (passed) passed = SUCCEEDED(presenter.device->CreateTexture2D(&game_desc, nullptr, &back));
        if (passed) passed = SUCCEEDED(presenter.device->CreateRenderTargetView(
            back, nullptr, &presenter.render_target_view));
        releaseCom(back);
        if (!passed) std::fprintf(stderr, "FAIL window setup scenario=%u\n", scenario);
        if (passed) {
            active_presenter = &presenter;
            RecompD3dPresenterCommand command{};
            command.type = RECOMP_D3D_PRESENTER_COMMAND_PRESENT;
            command.data.present.effective_flags = 5u;
            command.data.present.swap_counter = 1u;
            if (scenario == 0u || scenario == 4u) {
                passed = PostMessageW(presenter.window, WM_CLOSE, 0u, 0u) != 0;
            } else if (scenario == 1u) {
                SetWindowLongPtrW(presenter.window, GWLP_WNDPROC,
                    reinterpret_cast<LONG_PTR>(closeOnShowWindowProc));
            } else if (scenario == 2u) {
                passed = DestroyWindow(presenter.window) != 0;
            }
            if (scenario == 3u || scenario == 4u) PostQuitMessage(0);
            const RecompD3dPresenterError expected = scenario < 2u
                ? RECOMP_D3D_PRESENTER_CLOSED : RECOMP_D3D_PRESENTER_HOST_FAILURE;
            const RecompD3dPresenterError actual =
                d3d11_backend_submit(&presenter, &command);
            passed = passed && actual == expected &&
                presenter.close_requested == (scenario < 2u || scenario == 4u) &&
                presenter.present_count == (scenario == 1u ? 1u : 0u);
            if (!passed) std::fprintf(stderr,
                "FAIL window close scenario=%u result=%u expected=%u\n",
                scenario, static_cast<unsigned>(actual), static_cast<unsigned>(expected));
            active_presenter = nullptr;
        }
        releasePresenter(&presenter);
    }
    releaseCom(factory);
    if (passed) std::printf("PASS window close before/after present, unexpected loss/quit\n");
    return passed;
}

static bool testWidescreenClientWidth()
{
    RecompD3dPresenter presenter{};
    presenter.config.width = 720u;
    presenter.config.height = 480u;
    presenter.widescreen = true;
    if (presentClientWidth(&presenter, 480u) != 854u) return false;
    presenter.widescreen = false;
    return presentClientWidth(&presenter, 480u) == 640u;
}

static bool testTargetLifetimes(
    RecompD3dPresenter *presenter, ID3D11Texture2D *readback)
{
    presenter->owner_thread = GetCurrentThreadId();
    active_presenter = presenter;
    RecompD3dPresenterClearCommand clear{};
    clear.clear_color = true;
    clear.color = 0xff123456u;
    clear.target.offscreen = clear.target.no_depth = true;
    clear.target.color.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
    clear.target.color.width = clear.target.color.height = 4u;
    clear.target.color.data = 0x02000000u;
    bool passed = submitClear(presenter, clear) == RECOMP_D3D_PRESENTER_OK;
    const RecompD3dTextureDesc retained = clear.target.color;
    clear.target.custom_depth = true;
    clear.target.no_depth = false;
    clear.target.depth = clear.target.color;
    clear.target.depth.depth = true;
    clear.target.depth.format_byte = 0x2eu;
    for (uint32_t i = 0u; passed && i < 40u; ++i) {
        const uint32_t base = 0x01000000u + (i % 20u) * 0x1000u;
        clear.target.color.data = base + 0x100u;
        clear.target.depth.data = base + 0x400u;
        passed = submitClear(presenter, clear) == RECOMP_D3D_PRESENTER_OK;
        passed = passed && d3d11_backend_release_memory(
            presenter, base - 0x100u, 0x100u) == RECOMP_D3D_PRESENTER_OK &&
            presenter->render_targets.size() == 2u && presenter->depth_targets.size() == 1u;
        passed = passed && d3d11_backend_release_memory(
            presenter, base | 0x80000000u, 0x1000u) == RECOMP_D3D_PRESENTER_OK &&
            presenter->render_targets.size() == 1u && presenter->depth_targets.size() == 0u;
    }
    // A live working set can exceed sixteen targets without being a leak.
    for (uint32_t i = 0u; passed && i < 24u; ++i) {
        clear.target.color.data = 0x01000000u + i * 0x1000u;
        clear.target.depth.data = clear.target.color.data + 0x400u;
        passed = submitClear(presenter, clear) == RECOMP_D3D_PRESENTER_OK;
    }
    passed = passed && presenter->render_targets.size() == 25u &&
        presenter->depth_targets.size() == 24u && presenter->target_bytes == 49u * 64u;
    const uint64_t saved_bytes = presenter->target_bytes;
    presenter->target_bytes = kTargetByteLimit;
    clear.target.color.data = 0x03000000u;
    passed = passed && submitClear(presenter, clear) == RECOMP_D3D_PRESENTER_OUT_OF_MEMORY &&
        presenter->render_targets.size() == 25u && presenter->depth_targets.size() == 24u;
    presenter->target_bytes = saved_bytes;
    passed = passed && d3d11_backend_release_memory(
        presenter, 0x81000000u, 24u * 0x1000u) == RECOMP_D3D_PRESENTER_OK &&
        presenter->render_targets.size() == 1u && presenter->depth_targets.empty();
    passed = passed && d3d11_backend_release_memory(
        presenter, 0xfffffff0u, 0x20u) == RECOMP_D3D_PRESENTER_INVALID_ARGUMENT &&
        d3d11_backend_release_memory(presenter, retained.data, 0u) ==
            RECOMP_D3D_PRESENTER_INVALID_ARGUMENT;
    RenderTargetEntry *entry = findRenderTarget(presenter, retained);
    ID3D11Resource *resource = nullptr;
    ID3D11Texture2D *texture = nullptr;
    if (entry) entry->render_view->GetResource(&resource);
    passed = passed && resource && SUCCEEDED(resource->QueryInterface(
        __uuidof(ID3D11Texture2D), reinterpret_cast<void **>(&texture)));
    const uint32_t expected[4] = {0xff123456u, 0xff123456u, 0xff123456u, 0xff123456u};
    if (passed) passed = checkPixels(presenter, texture, readback,
        "live target survives forty storage releases", expected);
    releaseCom(texture);
    releaseCom(resource);
    passed = passed && d3d11_backend_release_memory(
        presenter, retained.data, 0x1000u) == RECOMP_D3D_PRESENTER_OK &&
        presenter->render_targets.size() == 0u;
    active_presenter = nullptr;
    std::fprintf(passed ? stdout : stderr,
        "%s forty lifetimes, 24 simultaneous color/depth targets, byte budget, aliases, retained pixels\n",
        passed ? "PASS" : "FAIL");
    return passed;
}

static bool testGamma(RecompD3dPresenter *presenter,
    ID3D11Texture2D *color, ID3D11Texture2D *readback)
{
    const uint32_t original[4] = {0x804080c0u, 0xff00ff00u, 0xffffffffu, 0u};
    uint32_t pixels[16];
    for (unsigned i = 0u; i < 16u; ++i) pixels[i] = original[i % 4u];
    presenter->context->UpdateSubresource(color, 0u, nullptr, pixels, 16u, 0u);
    D3D11_TEXTURE2D_DESC desc{};
    color->GetDesc(&desc);
    ID3D11Texture2D *output = nullptr;
    ID3D11RenderTargetView *view = nullptr;
    bool passed = SUCCEEDED(presenter->device->CreateTexture2D(&desc, nullptr, &output));
    if (passed) passed = SUCCEEDED(presenter->device->CreateRenderTargetView(output, nullptr, &view));
    if (passed) passed = renderOutput(presenter, view) &&
        checkPixels(presenter, output, readback, "default gamma identity", original);
    uint8_t ramp[3][256];
    for (unsigned i = 0u; i < 256u; ++i) {
        ramp[0][i] = static_cast<uint8_t>(255u-i);
        ramp[1][i] = static_cast<uint8_t>(i/2u);
        ramp[2][i] = 17u;
    }
    uint32_t expected[4];
    for (unsigned i = 0u; i < 4u; ++i) expected[i] = (original[i] & 0xff000000u) |
        ((255u-((original[i]>>16)&255u))<<16) | ((((original[i]>>8)&255u)/2u)<<8) | 17u;
    passed = passed && submitGamma(presenter, ramp) == RECOMP_D3D_PRESENTER_OK;
    for (unsigned repeat = 0u; passed && repeat < 2u; ++repeat) passed =
        renderOutput(presenter, view) &&
        checkPixels(presenter, output, readback, "per-channel gamma", expected) &&
        checkPixels(presenter, color, readback, "gamma preserves guest pixels", original);
    for (auto &channel : ramp) for (unsigned i = 0u; i < 256u; ++i) channel[i] = static_cast<uint8_t>(i);
    passed = passed && submitGamma(presenter, ramp) == RECOMP_D3D_PRESENTER_OK &&
        renderOutput(presenter, view) &&
        checkPixels(presenter, output, readback, "restored gamma identity", original);
    releaseCom(view);
    releaseCom(output);
    return passed;
}

static bool testVertexProgram(RecompD3dPresenter *presenter,
    ID3D11Texture2D *color, ID3D11Texture2D *readback)
{
    float vertices[4][8]{};
    for (unsigned v=0; v<4; ++v) {
        vertices[v][0] = v&1 ? 3.5f : -0.5f;
        vertices[v][1] = v&2 ? 3.5f : -0.5f;
        vertices[v][2] = 0.25f;
        vertices[v][4] = 1.0f;
    }
    const uint16_t indices[] = {0,1,2,3};
    RecompD3dPresenterDrawCommand draw{};
    draw.fvf=0x112u; draw.vertex_stride=32;
    draw.vertex_count=draw.index_count=4; draw.triangle_count=2;
    draw.primitive_type=RECOMP_D3D_PT_TRIANGLESTRIP;
    draw.vertex_bytes=vertices; draw.index_bytes=indices;
    draw.has_transform=true; draw.blend.color_write_mask=15;
    draw.program_count=3;
    // Synthetic MOVs: position from v0, color from c109, UV from v9.
    const unsigned attributes[] = {0,0,9}, outputs[] = {0,3,9};
    for (unsigned i=0;i<3;++i) {
        draw.program[i][1]=(1u<<21) | (attributes[i]<<9) | 0x1bu;
        draw.program[i][2]=(i==1 ? 3u : 2u)<<26;
        draw.program[i][3]=(15u<<12) | (1u<<11) | (outputs[i]<<3) | (i==2 ? 1u:0u);
    }
    draw.program[1][1] |= 109u<<13;
    draw.program_constants[58][0]=2; draw.program_constants[58][1]=-2;
    draw.program_constants[59][0]=draw.program_constants[59][1]=1.5f;
    draw.program_constants[58][2]=1;
    draw.program_constants[109][3]=1;
    const RecompD3dPresenterClearCommand clear={true,true,false,0xff0000ffu,1,0};
    for (unsigned channel=0;channel<2;++channel) {
        draw.program_constants[109][0]=channel==0 ? 1.0f:0.0f;
        draw.program_constants[109][1]=channel==1 ? 1.0f:0.0f;
        const uint32_t pixel=channel==0 ? 0xffff0000u:0xff00ff00u;
        const uint32_t expected[]={pixel,pixel,pixel,pixel};
        if (submitClear(presenter,clear)!=RECOMP_D3D_PRESENTER_OK ||
            submitDraw(presenter,draw)!=RECOMP_D3D_PRESENTER_OK ||
            !checkPixels(presenter,color,readback,"program output and changed constants",expected)) return false;
    }
    draw.fog.enabled = true;
    draw.fog.color = 0x00ff0000u;
    draw.fog.start = 0; draw.fog.end = 1; draw.fog.density = 1;
    const uint32_t no_program_fog[] = {0xff00ff00u,0xff00ff00u,0xff00ff00u,0xff00ff00u};
    for (unsigned mode = 0; mode <= 3; ++mode) {
        draw.fog.mode = mode;
        if (submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_OK ||
            !checkPixels(presenter, color, readback, "vertex programs without oFog remain unchanged", no_program_fog)) return false;
    }
    auto writes_fog = draw;
    writes_fog.program[2][3] &= ~1u;
    std::memcpy(writes_fog.program[3], writes_fog.program[1], sizeof writes_fog.program[3]);
    writes_fog.program[3][3] = (8u<<12) | (1u<<11) | (5u<<3) | 1u;
    writes_fog.program_count = 4;
    for (unsigned mode = 0; mode <= 3; ++mode) {
        writes_fog.fog.mode = mode;
        if (submitDraw(presenter, writes_fog) != RECOMP_D3D_PRESENTER_OK ||
            !checkPixels(presenter, color, readback, "vertex programs writing oFog remain unchanged", no_program_fog)) return false;
    }
    draw.fog.enabled = false;
    // A doubled sample grid must cover the same host pixels after inversion.
    for (auto &vertex : vertices) { vertex[0] = vertex[0]*2+4; vertex[1] = vertex[1]*2+4; }
    draw.program_constants[58][0] *= 2; draw.program_constants[58][1] *= 2;
    draw.program_constants[59][0] = draw.program_constants[59][0]*2+4; draw.program_constants[59][1] = draw.program_constants[59][1]*2+4;
    const uint32_t scaled_green[]={0xff00ff00u,0xff00ff00u,0xff00ff00u,0xff00ff00u};
    if (submitClear(presenter,clear)!=RECOMP_D3D_PRESENTER_OK ||
        submitDraw(presenter,draw)!=RECOMP_D3D_PRESENTER_OK ||
        !checkPixels(presenter,color,readback,"program sample-grid inversion",scaled_green)) return false;
    for (auto &vertex : vertices) { vertex[0] = (vertex[0]-4)/2; vertex[1] = (vertex[1]-4)/2; }
    draw.program_constants[58][0] /= 2; draw.program_constants[58][1] /= 2;
    draw.program_constants[59][0] = (draw.program_constants[59][0]-4)/2; draw.program_constants[59][1] = (draw.program_constants[59][1]-4)/2;
    // Negative UVs distinguish the scene clamp from the water-mask wrap.
    auto water = draw;
    for (auto &vertex : vertices) { vertex[6]=0.25f; vertex[7]=-0.25f; }
    const uint32_t scene[]={0xffff0000u,0xffff0000u,0xff00ff00u,0xff00ff00u};
    const uint32_t mask[]={0x00ffffffu,0x00ffffffu,0xffffffffu,0xffffffffu};
    water.has_texture=true;
    water.texture.format_byte=RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
    water.texture.bits_per_pixel=32;
    water.texture.width=water.texture.height=2;
    water.texture.data=0x006f0000u;
    water.texture_bytes=scene; water.texture_byte_count=sizeof scene;
    water.reflection_texture=water.texture;
    water.reflection_texture.data+=16;
    water.reflection_bytes=mask; water.reflection_byte_count=sizeof mask;
    water.program[2][3]&=~1u;
    std::memcpy(water.program[3],water.program[2],16);
    water.program[3][3]=(15u<<12)|(1u<<11)|(10u<<3)|1u;
    water.program_count=4;
    const uint32_t red[]={0xffff0000u,0xffff0000u,0xffff0000u,0xffff0000u};
    // A mask cache miss must not destroy the scene view before it is bound.
    {
        RecompD3dPresenter cached{};
        if (FAILED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0u,D3D11_SDK_VERSION,
            &cached.device,nullptr,&cached.context))) return false;
        ID3D11Texture2D *cached_color=nullptr, *cached_readback=nullptr;
        bool passed=createTestTargets(&cached,&cached_color,&cached_readback);
        auto cached_water = water;
        cached_water.texture.data = 0x00700000u;
        cached_water.program_alpha_mask = true;
        for (unsigned i=0;i<kTextureSlots && passed;++i) {
            auto upload = cached_water;
            upload.texture.data += i*16;
            passed=lookupTexture(&cached,upload)!=nullptr;
        }
        passed=passed &&
            cached.textures[cached.next_texture_slot].data==cached_water.texture.data &&
            !containsData(cached,cached_water.reflection_texture.data) &&
            submitClear(&cached,clear)==RECOMP_D3D_PRESENTER_OK &&
            submitDraw(&cached,cached_water)==RECOMP_D3D_PRESENTER_OK &&
            checkPixels(&cached,cached_color,cached_readback,"masked program evicts scene texture",red);
        releaseCom(cached_readback);
        releaseCom(cached_color);
        releaseGraphics(&cached);
        if (!passed) return false;
    }
    for (unsigned masked=0;masked<2;++masked) {
        water.program_alpha_mask=masked!=0;
        if (submitClear(presenter,clear)!=RECOMP_D3D_PRESENTER_OK ||
            submitDraw(presenter,water)!=RECOMP_D3D_PRESENTER_OK ||
            !checkPixels(presenter,color,readback,masked ? "water mask wraps negative UV" : "water scene clamps negative UV",red)) return false;
    }
    // Projected scene/mask coordinates carry q=2 through rasterization.
    for (unsigned i=2;i<4;++i) {
        water.program[i][1]=(1u<<21)|((113u+i)<<13)|0x1bu;
        water.program[i][2]=3u<<26;
        water.program_constants[113u+i][0]=0.5f;
        water.program_constants[113u+i][1]=i==2 ? 0.5f : -0.5f;
        water.program_constants[113u+i][3]=2.0f;
    }
    for (unsigned masked=0;masked<2;++masked) {
        water.program_alpha_mask=masked!=0;
        if (submitClear(presenter,clear)!=RECOMP_D3D_PRESENTER_OK ||
            submitDraw(presenter,water)!=RECOMP_D3D_PRESENTER_OK ||
            !checkPixels(presenter,color,readback,masked ? "projected water mask" : "projected water scene",red)) return false;
    }
    // The mask's transparent base level and opaque mip tail distinguish bias.
    uint8_t mip_mask[56]{};
    for (unsigned block=0;block<4;++block)
        std::memset(mip_mask+block*8+4,255,4);
    for (unsigned block=4;block<7;++block)
        std::memset(mip_mask+block*8,255,4);
    water.reflection_texture.format_byte=RECOMP_D3D_TEXTURE_FORMAT_DXT1;
    water.reflection_texture.bits_per_pixel=4;
    water.reflection_texture.width=water.reflection_texture.height=8;
    water.reflection_texture.mip_levels=4;
    water.reflection_texture.data+=16;
    water.reflection_bytes=mip_mask; water.reflection_byte_count=sizeof mip_mask;
    water.program[3][1]=(1u<<21)|(9u<<9)|0x1bu;
    water.program[3][2]=2u<<26;
    for (unsigned i=0;i<4;++i) { vertices[i][6]=i&1 ? 1.0f:0.0f; vertices[i][7]=i&2 ? 1.0f:0.0f; }
    const uint32_t transparent_red[]={0x00ff0000u,0x00ff0000u,0x00ff0000u,0x00ff0000u};
    for (unsigned biased=0;biased<2;++biased) {
        water.program_mask_lod_bias=biased ? -2.0f:0.0f;
        if (submitClear(presenter,clear)!=RECOMP_D3D_PRESENTER_OK ||
            submitDraw(presenter,water)!=RECOMP_D3D_PRESENTER_OK ||
            !checkPixels(presenter,color,readback,"water mask mip bias",biased ? transparent_red:red)) return false;
    }
    const uint32_t green[]={0xff00ff00u,0xff00ff00u,0xff00ff00u,0xff00ff00u};
    auto program = [&](unsigned id) {
        draw.program[1][1]=(1u<<21)|((120u+id)<<13)|0x1bu;
        draw.program_constants[120u+id][1]=1;
        draw.program_constants[120u+id][3]=1;
        return submitDraw(presenter,draw)==RECOMP_D3D_PRESENTER_OK &&
            checkPixels(presenter,color,readback,"cached program pixels",green);
    };
    for (unsigned i=0;i<10;++i) if (!program(i)) return false;
    const auto next=presenter->next_draw_pipeline_slot;
    for (unsigned i=0;i<10;++i) if (!program(i)) return false;
    if (presenter->next_draw_pipeline_slot!=next) {
        std::fprintf(stderr,"FAIL ten-program working set recompiles\n");
        return false;
    }
    for (unsigned i=10;i<=kDrawPipelineSlots;++i) if (!program(i)) return false;
    if (presenter->draw_pipeline_count!=kDrawPipelineSlots || !program(0)) return false;
    // Dual issue must read r2 before the simultaneous MAC overwrites it.
    std::memcpy(draw.program[3],draw.program[2],16);
    std::memcpy(draw.program[2],draw.program[0],16);
    draw.program_count=4;
    draw.program[0][1]=(1u<<21)|(109u<<13)|0x1bu;
    draw.program[0][2]=3u<<26;
    draw.program[0][3]=(15u<<24)|(2u<<20);
    draw.program[1][1]=(1u<<25)|(1u<<21)|(110u<<13)|0x1bu;
    draw.program[1][2]=(3u<<26)|(0x1bu<<2);
    draw.program[1][3]=(2u<<30)|(1u<<28)|(15u<<24)|(2u<<20)|(15u<<12)|(1u<<11)|(3u<<3)|4u;
    draw.program_constants[110][0]=1;
    draw.program_constants[110][3]=1;
    if (submitClear(presenter,clear)!=RECOMP_D3D_PRESENTER_OK ||
        submitDraw(presenter,draw)!=RECOMP_D3D_PRESENTER_OK ||
        !checkPixels(presenter,color,readback,"dual issue reads before writes",green)) return false;
    // Paired MAC writes to r1.x are suppressed, even when ILU only writes r1.y.
    std::memcpy(draw.program[4],draw.program[3],16);
    draw.program_count=5;
    draw.program[0][3]=(15u<<24)|(1u<<20); // Initialize r1 with unpaired MAC.
    draw.program[1][3]=(3u<<28)|(8u<<24)|(1u<<20)|(4u<<16);
    draw.program[3][1]=(1u<<21)|0x1bu;
    draw.program[3][2]=(1u<<28)|(1u<<26);
    draw.program[3][3]=(15u<<12)|(1u<<11)|(3u<<3); // Color from r1.
    const uint32_t black[]={0xff000000u,0xff000000u,0xff000000u,0xff000000u};
    if (submitClear(presenter,clear)!=RECOMP_D3D_PRESENTER_OK ||
        submitDraw(presenter,draw)!=RECOMP_D3D_PRESENTER_OK ||
        !checkPixels(presenter,color,readback,"paired MAC leaves r1.x unchanged",black)) return false;
    draw.program[1][3]|=(15u<<12)|(1u<<11)|(3u<<3);
    draw.program[3][3]=0; // Keep the paired MAC output write as the final color.
    if (submitClear(presenter,clear)!=RECOMP_D3D_PRESENTER_OK ||
        submitDraw(presenter,draw)!=RECOMP_D3D_PRESENTER_OK ||
        !checkPixels(presenter,color,readback,"paired MAC still writes output",red)) return false;
    draw.program[0][3] |= 2u; // Relative constants are deliberately unsupported.
    return submitDraw(presenter,draw)==RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
}

static bool testDirectionalLighting(RecompD3dPresenter *presenter,
    ID3D11Texture2D *color, ID3D11Texture2D *readback)
{
    float vertices[4][8]{};
    for (unsigned i=0;i<4;++i) {
        vertices[i][0]=i&1 ? 1.0f:-1.0f;
        vertices[i][1]=i&2 ? -1.0f:1.0f;
        vertices[i][3]=1.0f;
    }
    const uint16_t indices[]={0,1,2,3};
    const uint32_t texels[]={0xff804020u};
    RecompD3dPresenterDrawCommand draw{};
    draw.fvf=0x112; draw.vertex_stride=32; draw.vertex_count=draw.index_count=4;
    draw.triangle_count=2; draw.primitive_type=RECOMP_D3D_PT_TRIANGLESTRIP;
    draw.vertex_bytes=vertices; draw.index_bytes=indices; draw.blend.color_write_mask=15;
    draw.has_transform=true;
    draw.transform[0]=draw.transform[5]=draw.transform[10]=draw.transform[15]=1;
    draw.has_texture=true; draw.texture.data=0x006f0200;
    draw.texture.format_byte=RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
    draw.texture.width=draw.texture.height=1; draw.texture.bits_per_pixel=32;
    draw.texture_bytes=texels; draw.texture_byte_count=4;
    auto &light=draw.directional;
    light.enabled=light.normalize=true; light.count=1; light.directions[0][0]=1;
    for (unsigned c=0;c<3;++c) {
        light.colors[0][c]=1; light.material_diffuse[c]=0.25f; light.ambient_emissive[c]=0.25f;
    }
    float world[16]{}; world[0]=2; world[5]=world[10]=world[15]=1;
    if (!recomp_d3d_normal_transform(world,light.normal_transforms[0]) ||
        light.normal_transforms[0][0]!=0.5f) return false;
    auto pixels = [&](const char *label, uint32_t value) {
        const uint32_t expected[]={value,value,value,value};
        return submitDraw(presenter,draw)==RECOMP_D3D_PRESENTER_OK &&
            checkPixels(presenter,color,readback,label,expected);
    };
    if (!pixels("directional diffuse plus ambient and material",0xff402010)) return false;
    light.directions[0][0]=-1;
    if (!pixels("back-facing normal gets ambient only",0xff201008)) return false;
    world[0]=-2;
    if (!recomp_d3d_normal_transform(world,light.normal_transforms[0]) ||
        !pixels("world normal transform",0xff402010)) return false;
    float weighted[4][9]{};
    for (unsigned i=0;i<4;++i) {
        std::memcpy(weighted[i],vertices[i],12); weighted[i][3]=0.25f; weighted[i][4]=1;
    }
    draw.fvf=0x116; draw.vertex_stride=36; draw.vertex_bytes=weighted; draw.blend_weight_count=1;
    std::memcpy(draw.blend_transforms[0],draw.transform,64);
    std::memcpy(light.normal_transforms[1],light.normal_transforms[0],64);
    world[0]=2;
    if (!recomp_d3d_normal_transform(world,light.normal_transforms[0]) ||
        !pixels("weighted normals use remainder matrix",0xff402010)) return false;
    light.count=0;
    if (!pixels("no active lights retains ambient",0xff201008)) return false;
    light.enabled=false;
    if (!pixels("unlit texture unchanged",0xff804020)) return false;
    world[0]=0;
    if (recomp_d3d_normal_transform(world,light.normal_transforms[0])) return false;

    draw.fvf=0x112; draw.vertex_stride=32; draw.vertex_bytes=vertices; draw.blend_weight_count=0;
    light={}; light.enabled=light.normalize=true; light.count=1;
    for (unsigned c=0;c<3;++c) light.colors[0][c]=light.material_diffuse[c]=1;
    for (unsigned i=0;i<4;++i) {
        light.world_transforms[0][i*5]=light.normal_transforms[0][i*5]=1;
        vertices[i][3]=0; vertices[i][5]=1;
    }
    light.world_transforms[0][14]=10;
    light.positions[0][2]=30; light.positions[0][3]=1;
    light.attenuation[0][0]=1; light.attenuation[0][1]=0.05f; light.attenuation[0][3]=100;
    // Corner distances are sqrt(402) and sqrt(3602); the resulting UNORM colors round to 1/2 and 1/4.
    if (!pixels("point light near attenuation in world space",0xff402010)) return false;
    light.world_transforms[0][14]=-30;
    if (!pixels("point light far attenuation",0xff201008)) return false;
    for (auto &vertex : vertices) vertex[5]=-1;
    if (!pixels("point light back-facing diffuse is zero",0xff000000)) return false;
    for (auto &vertex : vertices) vertex[5]=1;
    light.attenuation[0][3]=50;
    if (!pixels("point light outside range is zero",0xff000000)) return false;
    light.world_transforms[0][14]=10;
    light.attenuation[0][1]=0; light.attenuation[0][2]=0.0025f;
    if (!pixels("point light quadratic attenuation",0xff402010)) return false;
    light.attenuation[0][0]=2; light.attenuation[0][2]=0;
    if (!pixels("point light constant attenuation",0xff402010)) return false;
    light.attenuation[0][0]=1; light.attenuation[0][1]=0.05f; light.attenuation[0][3]=100;
    light.count=2; light.directions[1][2]=1;
    for (unsigned c=0;c<3;++c) light.colors[1][c]=0.25f;
    if (!pixels("mixed point and directional lights",0xff603018)) return false;
    light.count=1;
    for (unsigned c=0;c<3;++c) { light.colors[0][c]=0; light.ambient[0][c]=1; }
    if (!pixels("point ambient near attenuation",0xff402010)) return false;
    light.world_transforms[0][14]=-30;
    if (!pixels("point ambient far attenuation",0xff201008)) return false;
    for (auto &vertex : vertices) vertex[5]=-1;
    if (!pixels("point ambient ignores normal",0xff201008)) return false;
    light.attenuation[0][3]=50;
    if (!pixels("point ambient outside range is zero",0xff000000)) return false;
    light.ambient_emissive[0]=0.25f;
    if (!pixels("out-of-range point ambient retains global ambient",0xff200000)) return false;
    light.ambient_emissive[0]=0;
    for (unsigned c=0;c<3;++c) { light.colors[0][c]=1; light.ambient[0][c]=0; }
    light.world_transforms[0][14]=10; light.attenuation[0][3]=100;
    for (unsigned i=0;i<4;++i) {
        weighted[i][4]=0; weighted[i][6]=1;
    }
    draw.fvf=0x116; draw.vertex_stride=36; draw.vertex_bytes=weighted; draw.blend_weight_count=1;
    std::memcpy(light.world_transforms[1],light.world_transforms[0],64);
    std::memcpy(light.normal_transforms[1],light.normal_transforms[0],64);
    light.world_transforms[0][14]=-50; light.world_transforms[1][14]=30;
    if (!pixels("point light weighted world position uses remainder matrix",0xff402010)) return false;
    draw.blend_weight_count=0;
    return pixels("point light disabled blending uses world zero",0xff1a0d06);
}

static bool testBackBufferMips(RecompD3dPresenter *presenter)
{
    RecompD3dPresenter scaled{};
    scaled.config = {4u, 3u, RECOMP_D3D_PRESENTER_COLOR_FORMAT_BGRA8_UNORM,
        RECOMP_D3D_PRESENTER_DEPTH_FORMAT_D24S8};
    scaled.scale = 2.0f;
    scaled.device = presenter->device;
    scaled.context = presenter->context;
    scaled.device->AddRef();
    scaled.context->AddRef();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 8u; desc.Height = 6u;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1u;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *source = nullptr, *readback = nullptr;
    bool passed = SUCCEEDED(scaled.device->CreateTexture2D(&desc, nullptr, &source)) &&
        SUCCEEDED(scaled.device->CreateRenderTargetView(source, nullptr, &scaled.render_target_view));
    desc.Width = 4u; desc.Height = 3u;
    desc.BindFlags = 0u;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    passed = passed && SUCCEEDED(scaled.device->CreateTexture2D(&desc, nullptr, &readback));
    RecompD3dPresenterDrawCommand draw{};
    draw.texture_is_backbuffer = true;
    draw.texture.format_byte = 0x12u;
    draw.texture.linear = true;
    draw.texture.width = 8u; draw.texture.height = 3u; // Supersampled guest size.
    const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    const float blue[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    if (passed) {
        scaled.context->ClearRenderTargetView(scaled.render_target_view, red);
        ID3D11ShaderResourceView *view = lookupTexture(&scaled, draw);
        passed = view != nullptr;
        if (passed) scaled.context->GenerateMips(view);
        scaled.context->ClearRenderTargetView(scaled.render_target_view, blue);
    }
    const auto checkMip = [&](const char *label, uint32_t expected) {
        if (lookupTexture(&scaled, draw) == nullptr) {
            std::fprintf(stderr, "FAIL %s lookup\n", label);
            return false;
        }
        scaled.context->CopySubresourceRegion(readback, 0u, 0u, 0u, 0u,
            scaled.back_buffer_copy, 1u, nullptr);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(scaled.context->Map(readback, 0u, D3D11_MAP_READ, 0u, &mapped))) {
            std::fprintf(stderr, "FAIL %s readback\n", label);
            return false;
        }
        bool matched = true;
        for (unsigned y = 0u; y < 3u; ++y) {
            const auto *row = reinterpret_cast<const uint32_t *>(
                static_cast<const uint8_t *>(mapped.pData) + y * mapped.RowPitch);
            for (unsigned x = 0u; x < 4u; ++x) {
                if (row[x] != expected) {
                    std::fprintf(stderr, "FAIL %s pixel=(%u,%u) got=%08x expected=%08x\n",
                        label, x, y, row[x], expected);
                    matched = false;
                }
            }
        }
        scaled.context->Unmap(readback, 0u);
        return matched;
    };
    passed = passed && checkMip("main-target snapshot refresh", 0xff0000ffu);
    if (passed) scaled.context->ClearRenderTargetView(scaled.render_target_view, red);
    draw.target.offscreen = true;
    draw.target.color.width = 8u; draw.target.color.height = 6u;
    passed = passed && checkMip("host-sized snapshot refresh", 0xffff0000u);
    if (passed) {
        uint32_t texels[6][8];
        for (unsigned y = 0u; y < 6u; ++y)
            for (unsigned x = 0u; x < 8u; ++x)
                texels[y][x] = y % 2u ? 0xff0000ffu : 0xffff0000u;
        scaled.context->UpdateSubresource(source, 0u, nullptr, texels, sizeof texels[0], 0u);
    }
    draw.target.color.width = 4u; draw.target.color.height = 3u;
    passed = passed && checkMip("minified snapshot averages red and blue rows", 0xff800080u);
    releaseCom(readback);
    releaseCom(source);
    releaseGraphics(&scaled);
    if (!passed) std::fprintf(stderr, "FAIL backbuffer snapshot mips\n");
    return passed;
}

static bool testSupersampledBackBuffer(RecompD3dPresenter *presenter)
{
    /* The casino transition fills from the supersampled 2x-wide guest back
       buffer; it must resolve to the host snapshot, not an untextured draw. */
    RecompD3dPresenterDrawCommand backbuffer{};
    backbuffer.has_texture = backbuffer.texture_is_backbuffer = true;
    backbuffer.texture.format_byte = 0x12u;
    backbuffer.texture.linear = true;
    backbuffer.texture.bits_per_pixel = 32u;
    backbuffer.texture.width = presenter->config.width * 2u;
    backbuffer.texture.height = presenter->config.height;
    ID3D11ShaderResourceView *view = lookupTexture(presenter, backbuffer);
    if (view == nullptr || view != presenter->back_buffer_sample) {
        std::fprintf(stderr, "FAIL supersampled back buffer snapshot\n");
        return false;
    }
    backbuffer.texture.width = presenter->config.width + 1u;
    if (lookupTexture(presenter, backbuffer) != nullptr) {
        std::fprintf(stderr, "FAIL mismatched back buffer accepted\n");
        return false;
    }
    std::printf("PASS supersampled back buffer resolves to the host snapshot\n");
    return true;
}

static bool testAddressSamplers(RecompD3dPresenter *presenter)
{
    /* Guest D3DTADDRESS: 3 CLAMP, 5 CLAMPTOEDGE, 2 MIRROR, 4 BORDER, 0 unset. */
    const uint32_t cases[][4] = {
        {3u, 3u, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP},
        {5u, 2u, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_MIRROR},
        {0u, 4u, D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_BORDER},
    };
    for (const auto &c : cases) {
        ID3D11SamplerState *sampler = lookupDrawSampler(presenter, c[0], c[1]);
        D3D11_SAMPLER_DESC desc{};
        if (sampler != nullptr) sampler->GetDesc(&desc);
        if (sampler == nullptr || desc.AddressU != c[2] || desc.AddressV != c[3]) {
            std::fprintf(stderr, "FAIL guest address mode %u/%u\n", c[0], c[1]);
            return false;
        }
    }
    std::printf("PASS guest texture address modes select host samplers\n");
    return true;
}

static bool testCombiner(RecompD3dPresenter *presenter,
    ID3D11Texture2D *color, ID3D11Texture2D *readback)
{
    struct Vertex { float x, y, z; uint32_t color; float u, v; };
    Vertex vertices[] = {{-1,1,.5f,0xffff0000u,0,0}, {1,1,.5f,0xffff0000u,0,0},
        {-1,-1,.5f,0xffff0000u,0,0}, {1,-1,.5f,0xffff0000u,0,0}};
    const uint16_t indices[] = {0,1,2,3};
    const uint32_t green = 0xff00ff00u, blue = 0xff0000ffu;
    RecompD3dPresenterDrawCommand draw{};
    draw.fvf = 0x142; draw.vertex_stride = sizeof(Vertex);
    draw.primitive_type = RECOMP_D3D_PT_TRIANGLESTRIP;
    draw.vertex_count = draw.index_count = 4; draw.triangle_count = 2;
    draw.vertex_bytes = vertices; draw.index_bytes = indices;
    draw.has_transform = draw.has_combiner = true;
    draw.transform[0] = draw.transform[5] = draw.transform[10] = draw.transform[15] = 1;
    std::memcpy(draw.reflection_transform, draw.transform, sizeof draw.transform);
    draw.blend.color_write_mask = 15;
    draw.combiner[0][0] = draw.combiner[0][4] = 2;
    draw.combiner[1][0] = 23; draw.combiner[1][1] = 1;
    draw.combiner[1][2] = draw.combiner[1][3] = 2;
    draw.combiner[1][4] = 2; draw.combiner[1][6] = 1;
    draw.reflection_texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8;
    draw.reflection_texture.bits_per_pixel = 32;
    draw.reflection_texture.width = draw.reflection_texture.height = 1;
    draw.reflection_texture.data = 0x00750000u;
    draw.reflection_bytes = &green; draw.reflection_byte_count = sizeof green;
    const RecompD3dPresenterClearCommand clear = {true,false,false,0xff000000u,1,0};
    const auto render = [&](uint32_t pixel, const char *label) {
        const uint32_t expected[] = {pixel,pixel,pixel,pixel};
        return submitClear(presenter, clear) == RECOMP_D3D_PRESENTER_OK &&
            submitDraw(presenter, draw) == RECOMP_D3D_PRESENTER_OK &&
            checkPixels(presenter, color, readback, label, expected);
    };
    if (!render(0xffffff00u, "combiner without stage zero texture")) return false;
    draw.combiner[0][2] = 2;
    if (submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND) return false;
    draw.combiner[0][2] = 0;
    draw.combiner_lod_bias = -2.0f;
    if (!render(0xffffff00u, "combiner stage one LOD bias")) return false;
    ID3D11SamplerState *biased = nullptr;
    presenter->context->PSGetSamplers(1u, 1u, &biased);
    D3D11_SAMPLER_DESC sampler{};
    if (biased) biased->GetDesc(&sampler);
    const bool bias_preserved = biased && sampler.MipLODBias == -2.0f;
    releaseCom(biased);
    if (!bias_preserved) return false;
    draw.combiner_lod_bias = 0.0f;
    draw.reflection_texture.width = 0;
    if (submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND) return false;
    draw.reflection_texture.width = 1;
    draw.reflection_bytes = nullptr; draw.reflection_byte_count = 0;
    draw.combiner[1][0] = 7; draw.combiner[1][2] = 1; draw.combiner[1][3] = 0;
    if (!render(0xffff0000u, "texture-free stage one ADD")) return false;
    draw.combiner[1][0] = 23; draw.combiner[1][2] = draw.combiner[1][3] = 2;
    draw.reflection_bytes = &green; draw.reflection_byte_count = sizeof green;
    draw.has_texture = true; draw.texture = draw.reflection_texture;
    draw.texture.data += 0x100; draw.texture_bytes = &blue; draw.texture_byte_count = sizeof blue;
    if (!render(0xffffff00u, "combiner bypasses legacy texture path")) return false;
    draw.texture.width = 0;
    if (!render(0xffffff00u, "combiner ignores unconsumed stage zero binding")) return false;
    draw.texture.width = 1;
    draw.target.offscreen = draw.target.no_depth = true;
    draw.target.color = draw.reflection_texture;
    if (submitDraw(presenter, draw) != RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND) return false;
    draw.target = {};
    auto snapshot = draw;
    const uint32_t stale[16] = {};
    snapshot.combiner[1][0] = snapshot.combiner[1][4] = 2;
    snapshot.combiner[1][6] = 2;
    snapshot.combiner_is_backbuffer = true;
    snapshot.reflection_texture = {};
    snapshot.reflection_texture.data = 0x00760000u;
    snapshot.reflection_texture.format_byte = 0x12u;
    snapshot.reflection_texture.bits_per_pixel = 32;
    snapshot.reflection_texture.linear = true;
    snapshot.reflection_texture.width = snapshot.reflection_texture.height = 4;
    snapshot.reflection_texture.pitch = 16;
    snapshot.reflection_bytes = stale; snapshot.reflection_byte_count = sizeof stale;
    snapshot.target.offscreen = snapshot.target.no_depth = true;
    snapshot.target.color = snapshot.reflection_texture;
    snapshot.target.color.data += 0x100;
    for (uint32_t pixel : {0xff00ff00u, 0xffff0000u}) {
        auto background = clear;
        background.color = pixel;
        if (submitClear(presenter, background) != RECOMP_D3D_PRESENTER_OK ||
            submitDraw(presenter, snapshot) != RECOMP_D3D_PRESENTER_OK) return false;
        RenderTargetEntry *entry = findRenderTarget(presenter, snapshot.target.color);
        ID3D11Resource *resource = nullptr;
        ID3D11Texture2D *target = nullptr;
        if (entry) entry->render_view->GetResource(&resource);
        const uint32_t expected[] = {pixel,pixel,pixel,pixel};
        const bool passed = resource && SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&target))) &&
            checkPixels(presenter, target, readback, "stage one snapshots current backbuffer", expected);
        releaseCom(target); releaseCom(resource);
        if (!passed) return false;
    }
    const uint8_t alpha = 128;
    draw.reflection_texture.format_byte = RECOMP_D3D_TEXTURE_FORMAT_A8;
    draw.reflection_texture.bits_per_pixel = 8;
    draw.reflection_texture.data += 0x200;
    draw.reflection_bytes = &alpha; draw.reflection_byte_count = 1;
    draw.combiner[1][0] = draw.combiner[1][4] = 2; draw.combiner[1][6] = 2;
    if (!render(0x80ffffffu, "stage one A8 has white RGB")) return false;
    const uint32_t row[] = {0xffff0000u,0xff00ff00u,0xff0000ffu,0xffffffffu};
    draw.reflection_texture.format_byte = 0x12u;
    draw.reflection_texture.bits_per_pixel = 32; draw.reflection_texture.linear = true;
    draw.reflection_texture.width = 4; draw.reflection_texture.pitch = sizeof row;
    draw.reflection_texture.data += 0x100;
    draw.reflection_bytes = row; draw.reflection_byte_count = sizeof row;
    draw.combiner_address_u = draw.combiner_address_v = 3;
    for (auto &vertex : vertices) { vertex.u = 1.5f; vertex.v = 0.5f; }
    if (!render(0xff00ff00u, "stage one linear texel coordinates")) return false;
    struct LitVertex { float x,y,z,nx,ny,nz,u,v; };
    const LitVertex lit[] = {{-1,1,.5f,0,0,1,0,0},{1,1,.5f,0,0,1,0,0},
        {-1,-1,.5f,0,0,1,0,0},{1,-1,.5f,0,0,1,0,0}};
    draw.fvf = 0x112; draw.vertex_bytes = lit; draw.vertex_stride = sizeof(LitVertex);
    draw.directional.enabled = true; draw.directional.ambient_emissive[0] = 1;
    draw.directional.material_diffuse[3] = 0.25f;
    draw.directional.normal_transforms[0][0] = draw.directional.normal_transforms[0][5] =
        draw.directional.normal_transforms[0][10] = draw.directional.normal_transforms[0][15] = 1;
    draw.material_alpha_mode = RECOMP_D3D_MATERIAL_ALPHA_MODULATE_TEXTURE;
    draw.material_alpha = 0.25f;
    draw.combiner[1][2] = draw.combiner[1][6] = 0;
    return render(0x40ff0000u, "combiner uses material diffuse alpha");
}

static bool testHeldFrameTargets()
{
    RecompD3dPresenter presenter{};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &presenter.device, nullptr, &presenter.context))) return false;
    ID3D11Texture2D *color = nullptr, *readback = nullptr;
    bool passed = createTestTargets(&presenter, &color, &readback);
    // A scaled snapshot has mips and follows the display aspect ratio.
    presenter.scale = 2;
    presenter.config.width = presenter.config.height = 2;
    passed &= createBufferCopy(&presenter, presenter.back_buffer_copy,
        presenter.back_buffer_sample, true);
    ID3D11RenderTargetView *snapshot = nullptr;
    passed &= SUCCEEDED(presenter.device->CreateRenderTargetView(
        presenter.back_buffer_copy, nullptr, &snapshot));
    const uint32_t expected[4] = {0xffff0000u,0xffff0000u,0xffff0000u,0xffff0000u};
    const float snapshot_red[] = {1,0,0,1};
    if (snapshot) presenter.context->ClearRenderTargetView(snapshot, snapshot_red);
    D3D11_TEXTURE2D_DESC snapshot_desc{};
    if (presenter.back_buffer_copy) presenter.back_buffer_copy->GetDesc(&snapshot_desc);
    passed &= snapshot_desc.MipLevels > 1u;
    snapshot_desc.MipLevels = 1; snapshot_desc.BindFlags = snapshot_desc.MiscFlags = 0;
    snapshot_desc.Usage = D3D11_USAGE_STAGING;
    snapshot_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    releaseCom(readback);
    passed &= SUCCEEDED(presenter.device->CreateTexture2D(&snapshot_desc,
        nullptr, &readback));
    for (bool gamma : {false, true}) {
        presenter.gamma_enabled = gamma;
        presenter.smaa = !gamma;
        copyFrontBuffer(&presenter);
        if (presenter.front_buffer_copy && readback) passed &= checkPixels(&presenter,
            presenter.front_buffer_copy, readback, "held-frame mip-zero snapshot", expected);
        else passed = false;
    }
    releaseCom(snapshot);
    releaseCom(presenter.front_buffer_sample); releaseCom(presenter.front_buffer_copy);
    releaseCom(presenter.back_buffer_sample); releaseCom(presenter.back_buffer_copy);
    presenter.gamma_enabled = presenter.smaa = false;
    presenter.config.width = presenter.config.height = 4;
    presenter.scale = 3;
    RecompD3dPresenterTarget target{};
    target.offscreen = target.no_depth = true;
    target.color = {RECOMP_D3D_TEXTURE_FORMAT_A8R8G8B8,32,false,false,true,512,512,0,0x00900000u,1};
    ID3D11RenderTargetView *view = nullptr;
    ID3D11DepthStencilView *depth = nullptr;
    const auto bind = [&](bool held) { return bindTarget(&presenter, target, view, depth, held) == RECOMP_D3D_PRESENTER_OK; };
    // A partial held-frame copy must retain an already populated target.
    passed &= bind(false);
    auto *original = view;
    const float red[] = {1,0,0,1};
    if (view) presenter.context->ClearRenderTargetView(view, red);
    passed &= bind(true) && view == original && findRenderTarget(&presenter, target.color)->scale == 1;
    ID3D11Resource *resource = nullptr;
    if (view) view->GetResource(&resource);
    ID3D11Texture2D *staging = nullptr;
    D3D11_TEXTURE2D_DESC desc{};
    if (resource) static_cast<ID3D11Texture2D *>(resource)->GetDesc(&desc);
    desc.BindFlags = 0; desc.Usage = D3D11_USAGE_STAGING; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    passed &= SUCCEEDED(presenter.device->CreateTexture2D(&desc, nullptr, &staging));
    if (staging && resource) {
        presenter.context->CopyResource(staging, resource);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        passed &= SUCCEEDED(presenter.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
        if (mapped.pData) {
            for (uint32_t y : {0u,255u,511u}) {
                    const auto *row = reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(mapped.pData) + y * mapped.RowPitch);
                    for (uint32_t x : {0u,255u,511u}) passed &= row[x] == 0xffff0000u;
                }
            presenter.context->Unmap(staging, 0);
        }
    }
    releaseCom(staging); releaseCom(resource);
    target.color.data += 0x100000;
    passed &= bind(true) && findRenderTarget(&presenter, target.color)->scale == 3;
    if (view) presenter.context->ClearRenderTargetView(view, red);
    target.no_depth = false; target.custom_depth = true; target.depth = target.color;
    target.depth.data += 0x100000; target.depth.format_byte = 0x2a; target.depth.depth = true;
    ID3D11DepthStencilView *shared_depth = nullptr;
    passed &= lookupDepthTarget(&presenter, target, 512, 512, shared_depth) == RECOMP_D3D_PRESENTER_OK;
    // Replacement fits only after crediting the scaled color allocation.
    const uint64_t before_restore = presenter.target_bytes;
    const uint64_t replaced_bytes = findRenderTarget(&presenter, target.color)->bytes;
    presenter.target_bytes = kTargetByteLimit;
    passed &= bind(false) && depth != nullptr && depth == shared_depth;
    passed &= presenter.target_bytes == kTargetByteLimit - replaced_bytes + 512u * 512u * 4u;
    presenter.target_bytes = before_restore - replaced_bytes + 512u * 512u * 4u;
    if (depth) {
        depth->GetResource(&resource);
        static_cast<ID3D11Texture2D *>(resource)->GetDesc(&desc);
        passed &= desc.Width == 512 && desc.Height == 512 && desc.SampleDesc.Count == 1;
        releaseCom(resource);
    }
    if (view) {
        view->GetResource(&resource);
        static_cast<ID3D11Texture2D *>(resource)->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        passed &= SUCCEEDED(presenter.device->CreateTexture2D(&desc, nullptr, &staging));
        if (staging) {
            presenter.context->CopyResource(staging, resource);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            passed &= SUCCEEDED(presenter.context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped));
            if (mapped.pData) {
                for (uint32_t y : {0u,255u,511u}) {
                    const auto *row = reinterpret_cast<const uint32_t *>(static_cast<const uint8_t *>(mapped.pData) + y * mapped.RowPitch);
                    for (uint32_t x : {0u,255u,511u}) passed &= row[x] == 0xffff0000u;
                }
                presenter.context->Unmap(staging, 0);
            }
        }
        releaseCom(staging); releaseCom(resource);
    }
    // The budget rejects supersampling but still admits a guest-size copy.
    const auto allocated = presenter.target_bytes;
    presenter.target_bytes = kTargetByteLimit - 512u * 512u * 4u;
    target.no_depth = true; target.color.data += 0x300000;
    passed &= bind(true) && findRenderTarget(&presenter, target.color)->scale == 1;
    presenter.target_bytes = allocated + 512u * 512u * 4u;
    // A draw with no front-buffer snapshot must decline, rather than paint vertex color.
    const float vertices[][5] = {{-1,1,.5f,0,0},{1,1,.5f,0,0},{-1,-1,.5f,0,0}};
    const uint16_t indices[] = {0,1,2};
    RecompD3dPresenterDrawCommand draw{};
    draw.fvf = 0x102; draw.vertex_stride = sizeof vertices[0];
    draw.vertex_bytes = vertices; draw.index_bytes = indices;
    draw.vertex_count = draw.index_count = 3; draw.triangle_count = 1;
    draw.primitive_type = RECOMP_D3D_PT_TRIANGLELIST;
    draw.has_transform = draw.has_texture = draw.texture_is_frontbuffer = true;
    draw.transform[0] = draw.transform[5] = draw.transform[10] = draw.transform[15] = 1;
    draw.texture.width = draw.texture.height = 4;
    passed &= submitDraw(&presenter, draw) == RECOMP_D3D_PRESENTER_UNSUPPORTED_COMMAND;
    releaseCom(readback); releaseCom(color); releaseGraphics(&presenter);
    std::printf("%s held-frame target retention, depth, budget and absent snapshot\n", passed ? "PASS" : "FAIL");
    return passed;
}

static bool testPacingPolicies()
{
    const UINT rates[][2] = {{60,1}, {119,1}, {120,2}, {121,2}, {144,1},
        {179,1}, {180,1}, {181,1}, {239,1}, {240,4}, {241,4}, {360,1}};
    for (const auto &rate : rates) {
        if (fixedRefreshInterval(rate[0]) != rate[1]) return false;
    }
    RecompD3dPresenter stats_presenter{};
    DXGI_FRAME_STATISTICS stats{};
    stats.PresentCount = 10u; stats.PresentRefreshCount = 100u;
    recordFrameStatistics(&stats_presenter, S_OK, stats);
    for (HRESULT error : {DXGI_ERROR_FRAME_STATISTICS_DISJOINT, E_FAIL}) {
        recordFrameStatistics(&stats_presenter, error, stats);
        if (stats_presenter.last_stat_present != 0u || stats_presenter.last_stat_refresh != 0u) return false;
        ++stats.PresentCount; stats.PresentRefreshCount = 1u;
        recordFrameStatistics(&stats_presenter, S_OK, stats);
        if (stats_presenter.refresh_holds[8] != 0u) return false;
        ++stats.PresentCount; stats.PresentRefreshCount += 2u;
        recordFrameStatistics(&stats_presenter, S_OK, stats);
    }
    if (stats_presenter.refresh_holds[2] != 2u) return false;

    const auto now = std::chrono::steady_clock::now();
    stats_presenter.sync_interval = 4u;
    stats_presenter.next_refresh_check = now + std::chrono::seconds(1);
    if (syncInterval(&stats_presenter) != 4u) return false;
    stats_presenter.present_count = 1u; // Below 60 FPS, a due check still runs.
    stats_presenter.next_refresh_check = now - std::chrono::seconds(1);
    syncInterval(&stats_presenter);
    if (stats_presenter.next_refresh_check <= now) return false;

    const auto worker = [] { recomp_d3d_sleep_until(0); };
    std::thread(worker).join(); // Initialize thread support before counting handles.
    DWORD before = 0u, after = 0u;
    if (!GetProcessHandleCount(GetCurrentProcess(), &before)) return false;
    for (unsigned i = 0u; i < 8u; ++i) std::thread(worker).join();
    if (!GetProcessHandleCount(GetCurrentProcess(), &after) || after != before) return false;

    const bool saved_immediate = immediate_present;
    bool passed = true;
    for (unsigned mode = 0u; mode < 3u; ++mode) {
        RecompD3dPresenter presenter{};
        presenter.config = {320u, 240u, RECOMP_D3D_PRESENTER_COLOR_FORMAT_BGRA8_UNORM,
            RECOMP_D3D_PRESENTER_DEPTH_FORMAT_D24S8};
        immediate_present = mode != 1u;
        presenter.vrr = mode == 2u;
        const bool immediate = mode == 0u;
        IDXGIDevice1 *device = nullptr;
        IDXGISwapChain2 *chain = nullptr;
        UINT latency = 0u;
        DXGI_SWAP_CHAIN_DESC desc{};
        passed = createWindow(&presenter) &&
            SUCCEEDED(createDeviceWithDriver(&presenter, D3D_DRIVER_TYPE_WARP)) &&
            SUCCEEDED(presenter.swap_chain->GetDesc(&desc)) &&
            desc.SwapEffect == (immediate ? DXGI_SWAP_EFFECT_DISCARD : DXGI_SWAP_EFFECT_FLIP_DISCARD) &&
            ((desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0u) == presenter.vrr &&
            ((desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0u) == !immediate;
        if (passed && immediate) {
            passed = SUCCEEDED(presenter.device->QueryInterface(IID_PPV_ARGS(&device))) &&
                SUCCEEDED(device->GetMaximumFrameLatency(&latency)) && latency == 3u;
        } else if (passed) {
            passed = SUCCEEDED(presenter.swap_chain->QueryInterface(IID_PPV_ARGS(&chain))) &&
                SUCCEEDED(chain->GetMaximumFrameLatency(&latency)) && latency == 1u;
        }
        releaseCom(chain);
        releaseCom(device);
        releasePresenter(&presenter);
        if (!passed) break;
    }
    immediate_present = saved_immediate;
    return passed;
}

int main()
{
    _putenv_s("RECOMP_D3D_FOG", "");
    _putenv_s("RECOMP_D3D_FOG_FACTOR", "");
    if (!testHeldFrameTargets()) return 1;
    if (!testPacingPolicies()) {
        std::fprintf(stderr, "FAIL refresh intervals, statistics epochs, timer handles or frame latency\n");
        return 1;
    }
    if (!testWidescreenClientWidth()) {
        std::fprintf(stderr, "FAIL widescreen client width\n");
        return 1;
    }
    FrameRateCounter counter;
    double fps = 0, frame_ms = 0;
    if (sampleFrameRate(counter, 0u, fps, frame_ms)) return 1;
    for (unsigned i = 1; i <= 60; ++i) {
        if (sampleFrameRate(counter, i * 1000u / 60u, fps, frame_ms) != (i == 60u)) return 1;
    }
    if (std::fabs(fps - 60.0) > 0.001 || std::fabs(frame_ms - 1000.0 / 60.0) > 0.001 ||
        !sampleFrameRate(counter, 2000u, fps, frame_ms) || fps != 1.0 || frame_ms != 1000.0) {
        std::fprintf(stderr, "FAIL frame rate interval/stall measurement\n");
        return 1;
    }

    ULONGLONG next_dump = 0u;
    if (!frameDumpDue(100u, 10000u, next_dump) || next_dump != 10100u ||
        frameDumpDue(10099u, 10000u, next_dump) ||
        !frameDumpDue(10100u, 10000u, next_dump) || next_dump != 20100u ||
        !frameDumpDue(100000u, 10000u, next_dump) || next_dump != 110000u ||
        frameDumpDue(100001u, 10000u, next_dump) ||
        !frameDumpDue(100001u, 0u, next_dump)) {
        std::fprintf(stderr, "FAIL capture timing/burst suppression\n");
        return 1;
    }
    RecompD3dPresenter presenter{};
    D3D_FEATURE_LEVEL feature_level{};
    HRESULT result = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0u, D3D11_SDK_VERSION,
        &presenter.device, &feature_level, &presenter.context);
    if (FAILED(result)) {
        return finish(&presenter, 10, static_cast<uint32_t>(result));
    }
    ID3D11Texture2D *color = nullptr;
    ID3D11Texture2D *readback = nullptr;
    uint32_t detail = 0u;
    int status = 0;
    if (!createTestTargets(&presenter, &color, &readback) ||
        !testTargetLifetimes(&presenter, readback) ||
        !testOffscreenRendering(&presenter, color, readback, true)) {
        status = 70;
    } else {
        status = testTextureCache(&presenter, detail);
    }
    if (status == 0 &&
        !testOffscreenRendering(&presenter, color, readback, false)) {
        status = 70;
    }
    if (status == 0 && !testCompressedMips(&presenter, color, readback)) status = 92;
    if (status == 0 && !testAlphaMask(&presenter, color, readback)) status = 91;
    if (status == 0 && !testReflection(&presenter, color, readback)) status = 93;
    if (status == 0 && !testFourTapFilter(&presenter, color, readback)) status = 87;
    if (status == 0 && !testPretransformedGlyphs(&presenter, color, readback)) {
        status = 85;
    }
    if (status == 0 && !testAlphaRendering(&presenter, color, readback)) {
        status = 60;
    }
    if (status == 0 && !testFog(&presenter, color, readback)) status = 82;
    if (status == 0 && !testVertexBlending(&presenter, color, readback)) status = 80;
    if (status == 0 && !testLinearTextureUpdates(&presenter, readback)) status = 88;
    if (status == 0 && !testDrawPipelineEviction(&presenter)) status = 89;
    if (status == 0 && !testGamma(&presenter, color, readback)) status = 94;
    if (status == 0 && !testVertexProgram(&presenter, color, readback)) status = 95;
    if (status == 0 && !testDirectionalLighting(&presenter, color, readback)) status = 96;
    if (status == 0 && !testConstantBlend(&presenter, color, readback)) status = 97;
    if (status == 0 && !testCullRendering(&presenter, color, readback)) status = 98;
    if (status == 0 && !testBackBufferMips(&presenter)) status = 99;
    if (status == 0 && !testSupersampledBackBuffer(&presenter)) status = 84;
    if (status == 0 && !testCombiner(&presenter, color, readback)) status = 99;
    if (status == 0 && !testAddressSamplers(&presenter)) status = 83;
    if (status == 0 && !testWindowClose(&presenter)) status = 86;
    releaseCom(readback);
    releaseCom(color);
    if (status == 0) {
        std::printf("PASS offscreen isolation, retained pixels, viewport restore\n");
        std::printf("PASS offscreen depth occlusion, isolation, shared storage\n");
        std::printf("PASS texture alpha, all alpha comparisons, discarded depth\n");
        std::printf("PASS four-tap RGBA, linear UVs, backbuffer snapshot and filtered output\n");
        std::printf("PASS mutable linear BGRA pixels, row padding, cached source bounds\n");
    }
    return finish(&presenter, status, detail);
}
