#include "d3d_presenter_capture.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#define CHECK(condition) do { \
    if (!(condition)) { \
        std::fprintf(stderr, "FAIL capture line %d: %s\n", __LINE__, #condition); \
        return 1; \
    } \
} while (0)

int main()
{
    using Draw = RecompD3dPresenterDrawCommand;
    const void *Draw::*fields[] = {
        &Draw::vertex_bytes, &Draw::index_bytes, &Draw::texture_bytes,
        &Draw::palette_bytes, &Draw::alpha_mask_bytes,
        &Draw::alpha_mask_palette, &Draw::reflection_bytes,
    };
    const size_t sizes[] = {33u, 6u, 65u, 1024u, 17u, 1024u, 31u};
    std::array<std::vector<uint8_t>, 7> sources;
    RecompD3dPresenterCommand input{};
    input.type = RECOMP_D3D_PRESENTER_COMMAND_DRAW;
    auto &draw = input.data.draw;
    draw.vertex_count = 3u;
    draw.vertex_stride = 11u;
    draw.index_count = 3u;
    draw.texture_byte_count = 65u;
    draw.palette_byte_count = 1024u;
    draw.alpha_mask_byte_count = 17u;
    draw.alpha_mask_palette_byte_count = 1024u;
    draw.reflection_byte_count = 31u;
    draw.texture.data = 0x101000u;
    draw.alpha_mask.data = 0x202000u;
    draw.reflection_texture.data = 0x303000u;
    // Linear BGRA is re-uploaded on cache hits, so its bytes must be owned.
    draw.texture.format_byte = draw.alpha_mask.format_byte =
        draw.reflection_texture.format_byte = 0x12u;
    draw.program_count = 1u;
    draw.target.color.data = 0x404000u;
    draw.target.depth.data = 0x505000u;
    draw.program[135][3] = 0x12345678u;
    draw.program_constants[191][3] = 1.25f;
    draw.directional.colors[7][3] = 0.75f;
    for (size_t i = 0u; i < sources.size(); ++i) {
        sources[i].assign(sizes[i], static_cast<uint8_t>(i + 1u));
        draw.*fields[i] = sources[i].data();
    }

    D3dCapturePacket packet;
    CHECK(packet.add(input) == RECOMP_D3D_PRESENTER_OK);
    for (size_t i = 0u; i < sources.size(); ++i) {
        std::fill(sources[i].begin(), sources[i].end(), static_cast<uint8_t>(i + 21u));
    }
    CHECK(packet.add(input) == RECOMP_D3D_PRESENTER_OK);
    CHECK(packet.addRelease(0x123000u, 0x456u) == RECOMP_D3D_PRESENTER_OK);
    for (auto &source : sources) std::fill(source.begin(), source.end(), 0xeeu);
    packet.seal();
    CHECK(packet.count() == 3u);
    CHECK(packet.record(0).kind == D3dCapturePacket::COMMAND);
    CHECK(packet.record(1).kind == D3dCapturePacket::COMMAND);
    CHECK(packet.record(2).kind == D3dCapturePacket::RELEASE);
    CHECK(packet.record(2).base == 0x123000u && packet.record(2).size == 0x456u);
    for (size_t record = 0u; record < 2u; ++record) {
        const auto &captured = packet.command(record).data.draw;
        CHECK(reinterpret_cast<uintptr_t>(&packet.command(record)) % 8u == 0u);
        for (size_t i = 0u; i < sources.size(); ++i) {
            const auto *bytes = static_cast<const uint8_t *>(captured.*fields[i]);
            CHECK(bytes != sources[i].data());
            CHECK(reinterpret_cast<uintptr_t>(bytes) % 8u == 0u);
            CHECK(std::all_of(bytes, bytes + sizes[i], [=](uint8_t byte) {
                return byte == i + 1u + record * 20u;
            }));
        }
        CHECK(captured.texture.data == draw.texture.data);
        CHECK(captured.alpha_mask.data == draw.alpha_mask.data);
        CHECK(captured.reflection_texture.data == draw.reflection_texture.data);
        CHECK(captured.target.color.data == draw.target.color.data);
        CHECK(captured.target.depth.data == draw.target.depth.data);
        CHECK(captured.program[135][3] == 0x12345678u);
        CHECK(captured.program_constants[191][3] == 1.25f);
        CHECK(captured.directional.colors[7][3] == 0.75f);
    }
    const uint64_t sealed_bytes = packet.bytes();
    CHECK(packet.add(input) == RECOMP_D3D_PRESENTER_INVALID_ARGUMENT);
    CHECK(packet.addRelease(0u, 1u) == RECOMP_D3D_PRESENTER_INVALID_ARGUMENT);
    CHECK(packet.addReport() == RECOMP_D3D_PRESENTER_INVALID_ARGUMENT);
    packet.seal();
    CHECK(packet.count() == 3u && packet.bytes() == sealed_bytes);

    D3dCapturePacket moved(std::move(packet));
    const void *moved_vertices = moved.command(0).data.draw.vertex_bytes;
    packet.clear();
    packet = std::move(moved);
    CHECK(packet.command(0).data.draw.vertex_bytes == moved_vertices);
    CHECK(*static_cast<const uint8_t *>(moved_vertices) == 1u);
    packet.clear();
    CHECK(packet.count() == 0u && packet.bytes() == 0u);

    CHECK(packet.add(input) == RECOMP_D3D_PRESENTER_OK);
    const uint64_t first_bytes = packet.bytes();
    CHECK(packet.add(input) == RECOMP_D3D_PRESENTER_OK);
    const uint64_t dedup_delta = packet.bytes() - first_bytes;
    CHECK(dedup_delta == sizeof(RecompD3dPresenterCommand) + sizeof(D3dCapturePacket::Record));
    sources[0][0] = 0xabu;
    const uint64_t before_changed = packet.bytes();
    CHECK(packet.add(input) == RECOMP_D3D_PRESENTER_OK);
    CHECK(packet.bytes() - before_changed > dedup_delta);
    sources[0][0] = 0xeeu;
    const uint64_t before_reverted = packet.bytes();
    CHECK(packet.add(input) == RECOMP_D3D_PRESENTER_OK);
    CHECK(packet.bytes() - before_reverted == dedup_delta);

    const size_t before_count = packet.count();
    const uint64_t before_bytes = packet.bytes();
    RecompD3dPresenterCommand invalid = input;
    invalid.data.draw.vertex_count = UINT32_MAX;
    invalid.data.draw.vertex_stride = UINT32_MAX;
    CHECK(packet.add(invalid) == RECOMP_D3D_PRESENTER_INVALID_ARGUMENT);
    invalid = input;
    invalid.data.draw.index_count = UINT32_MAX;
    CHECK(packet.add(invalid) == RECOMP_D3D_PRESENTER_INVALID_ARGUMENT);
    uint32_t Draw::*counts[] = {
        &Draw::texture_byte_count, &Draw::palette_byte_count,
        &Draw::alpha_mask_byte_count, &Draw::alpha_mask_palette_byte_count,
        &Draw::reflection_byte_count,
    };
    for (auto field : counts) {
        invalid = input;
        invalid.data.draw.*field = UINT32_MAX;
        CHECK(packet.add(invalid) == RECOMP_D3D_PRESENTER_INVALID_ARGUMENT);
    }
    invalid.type = static_cast<RecompD3dPresenterCommandType>(99);
    CHECK(packet.add(invalid) == RECOMP_D3D_PRESENTER_INVALID_ARGUMENT);
    CHECK(packet.count() == before_count && packet.bytes() == before_bytes);
    packet.seal();
    CHECK(packet.command(0).data.draw.vertex_bytes == packet.command(1).data.draw.vertex_bytes);
    CHECK(packet.command(0).data.draw.vertex_bytes == packet.command(3).data.draw.vertex_bytes);
    CHECK(*static_cast<const uint8_t *>(packet.command(2).data.draw.vertex_bytes) == 0xabu);
    CHECK(packet.command(0).data.draw.texture_bytes == packet.command(2).data.draw.texture_bytes);

    packet.clear();
    RecompD3dPresenterCommand value{};
    value.type = RECOMP_D3D_PRESENTER_COMMAND_CLEAR;
    value.data.clear.color = 0xabcdef12u;
    value.data.clear.z = 0.5f;
    CHECK(packet.add(value) == RECOMP_D3D_PRESENTER_OK);
    CHECK(packet.addReport() == RECOMP_D3D_PRESENTER_OK);
    value = {};
    value.type = RECOMP_D3D_PRESENTER_COMMAND_PRESENT;
    value.data.present.swap_counter = 42u;
    CHECK(packet.add(value) == RECOMP_D3D_PRESENTER_OK);
    CHECK(packet.addRelease(7u, 9u) == RECOMP_D3D_PRESENTER_OK);
    value = {};
    value.type = RECOMP_D3D_PRESENTER_COMMAND_GAMMA;
    std::memset(value.data.gamma, 0x7au, sizeof value.data.gamma);
    CHECK(packet.add(value) == RECOMP_D3D_PRESENTER_OK);
    value = {};
    value.type = RECOMP_D3D_PRESENTER_COMMAND_DRAW;
    value.data.draw.texture.format_byte = value.data.draw.alpha_mask.format_byte =
        value.data.draw.reflection_texture.format_byte = 0x12u;
    for (size_t i = 0u; i < sources.size(); ++i) value.data.draw.*fields[i] = sources[i].data();
    value.data.draw.directional.world_transforms[3][14] = 12.0f;
    value.data.draw.directional.positions[7][2] = 30.0f;
    value.data.draw.directional.positions[7][3] = 1.0f;
    value.data.draw.directional.attenuation[7][1] = 0.05f;
    value.data.draw.directional.attenuation[7][3] = 100.0f;
    const auto lighting = value.data.draw.directional;
    CHECK(packet.add(value) == RECOMP_D3D_PRESENTER_OK);
    value.data.draw.directional = {};
    value.data.draw.texture.format_byte = 0u; // swizzled: borrowed from guest RAM
    CHECK(packet.add(value) == RECOMP_D3D_PRESENTER_OK);
    for (auto field : fields) value.data.draw.*field = nullptr;
    value.data.draw.texture_byte_count = 12u;
    CHECK(packet.add(value) == RECOMP_D3D_PRESENTER_OK);
    packet.seal();
    CHECK(packet.count() == 8u);
    CHECK(packet.command(0).data.clear.color == 0xabcdef12u);
    CHECK(packet.command(0).data.clear.z == 0.5f);
    CHECK(packet.record(1).kind == D3dCapturePacket::REPORT);
    CHECK(packet.command(2).data.present.swap_counter == 42u);
    CHECK(packet.record(3).kind == D3dCapturePacket::RELEASE);
    for (const auto &plane : packet.command(4).data.gamma) {
        for (uint8_t byte : plane) CHECK(byte == 0x7au);
    }
    for (size_t i = 0u; i < sources.size(); ++i) {
        CHECK(packet.command(5).data.draw.*fields[i] != nullptr);
        CHECK(packet.command(5).data.draw.*fields[i] != sources[i].data());
        CHECK(packet.command(7).data.draw.*fields[i] == nullptr);
    }
    CHECK(std::memcmp(&packet.command(5).data.draw.directional, &lighting, sizeof lighting) == 0);
    CHECK(packet.command(6).data.draw.texture_bytes == sources[2].data());
    CHECK(packet.command(6).data.draw.vertex_bytes != sources[0].data());
    std::puts("PASS capture ownership, order, deduplication, overflow, alignment, move and reuse");
    return 0;
}
