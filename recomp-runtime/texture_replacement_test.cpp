// Like the presenter tests, exercise the private codecs without exposing a test API.
#include "texture_replacement.cpp"
#include "d3d_presenter_capture.h"

#define REQUIRE(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "failed line %d: %s\n", __LINE__, #condition); return 1; } } while (0)

int main()
{
    const fs::path root = (fs::current_path() / "private") /
        ("recomp-textures-test-" + std::to_string(GetCurrentProcessId()));
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code ec; fs::remove_all(path, ec); } } cleanup{root};
    fs::create_directories(root / "replace");
    _putenv_s("RECOMP_TEXTURES", (root / "replace").string().c_str());
    _putenv_s("RECOMP_TEXTURE_DUMP_DIR", root.string().c_str());
    _putenv_s("RECOMP_TEXTURE_DUMP", "1");
    try {
        for (const char *invalid : {"", "no", "1x", "-1", "0", "4294967296"}) REQUIRE(frameNumber(invalid) == 0);
        REQUIRE(frameNumber("123") == 123 && frameNumber("4294967295") == UINT32_MAX);
        bool rejected_path = false;
        try { privatePath(fs::current_path() / "outside"); } catch (const std::exception &) { rejected_path = true; }
        REQUIRE(rejected_path && privatePath(root) == fs::weakly_canonical(root));
        rejected_path = false;
        try { privatePath(fs::current_path() / "private" / ".." / "escape"); } catch (const std::exception &) { rejected_path = true; }
        REQUIRE(rejected_path);
        bool diagnosed = false;
        try { check(E_INVALIDARG); } catch (const std::exception &error) {
            diagnosed = std::strstr(error.what(), "hr=0x80070057") != nullptr;
        }
        REQUIRE(diagnosed);
        ComPtr<ID3D11VertexShader> readback_vs;
        ComPtr<ID3D11PixelShader> readback_ps;
        RecompD3dPresenterDrawCommand draw{};
        draw.texture = {RECOMP_D3D_TEXTURE_FORMAT_DXT1, 4, false, false, false, 64, 64, 0, 4096, 7};
        std::vector<uint8_t> payload(recomp_d3d_texture_mip_span(&draw.texture), 0);
        draw.texture_bytes = payload.data(); draw.texture_byte_count = static_cast<uint32_t>(payload.size());
        TextureIdentity first, second;
        first.assign(draw);
        draw.texture.data += 4096;
        second.assign(draw);
        REQUIRE(first.key == second.key);
        // Not one of the presenter's 128 sampled words.
        payload[11] = 17;
        REQUIRE(!first.matches(draw));
        second.assign(draw);
        REQUIRE(first.key != second.key);
        payload.back() = 31;
        first.assign(draw);
        REQUIRE(first.key != second.key);
        draw.texture = {RECOMP_D3D_TEXTURE_FORMAT_P8, 8, false, false, false, 4, 4, 0, 4096, 3};
        REQUIRE(recomp_d3d_texture_mip_span(&draw.texture) == 21);
        std::vector<uint8_t> palette(1024, 0);
        payload.resize(21);
        draw.texture_bytes = payload.data(); draw.texture_byte_count = 21;
        draw.palette_bytes = palette.data(); draw.palette_byte_count = 1024;
        first.assign(draw); palette.back() = 1; second.assign(draw);
        REQUIRE(first.key != second.key);

        RecompD3dPresenterCommand command{};
        command.type = RECOMP_D3D_PRESENTER_COMMAND_DRAW;
        command.data.draw = draw;
        D3dCapturePacket packet;
        REQUIRE(packet.add(command) == RECOMP_D3D_PRESENTER_OK);
        packet.seal();
        payload[0] = 123; palette[0] = 123;
        REQUIRE(static_cast<const uint8_t *>(packet.command(0).data.draw.texture_bytes)[0] != 123);
        REQUIRE(static_cast<const uint8_t *>(packet.command(0).data.draw.palette_bytes)[0] != 123);

        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL requested = D3D_FEATURE_LEVEL_10_0, obtained;
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &requested, 1,
            D3D11_SDK_VERSION, &device, &obtained, &context));
        REQUIRE(obtained == D3D_FEATURE_LEVEL_10_0);
        Pixels pixels{4, 4, std::vector<uint8_t>(64, 0)};
        for (size_t i = 0; i < 16; ++i) {
            pixels.bytes[i * 4] = static_cast<uint8_t>(i * 13);
            pixels.bytes[i * 4 + 1] = 90;
            pixels.bytes[i * 4 + 2] = 155;
            pixels.bytes[i * 4 + 3] = static_cast<uint8_t>(i * 17);
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = 4; desc.ArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{pixels.bytes.data(), 16, 0};
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> original;
        check(device->CreateTexture2D(&desc, &data, &texture));
        check(device->CreateShaderResourceView(texture.Get(), nullptr, &original));
        REQUIRE(readPixels(device.Get(), context.Get(), original.Get(), readback_vs, readback_ps).bytes == pixels.bytes);
        auto *cached_vs = readback_vs.Get(); auto *cached_ps = readback_ps.Get();
        REQUIRE(readPixels(device.Get(), context.Get(), original.Get(), readback_vs, readback_ps).bytes == pixels.bytes);
        REQUIRE(cached_vs == readback_vs.Get() && cached_ps == readback_ps.Get());
        TextureReplacements replacements;
        REQUIRE(replacements.lookup(first.key, original.Get()) == original.Get());
        replacements.finishFrame(device.Get(), context.Get(), 1);
        REQUIRE(loadPng(root / "dump" / (first.key + ".png")).bytes == pixels.bytes);
        auto replacement_path = root / "replace" / (first.key + ".png");
        fs::copy_file(root / "dump" / (first.key + ".png"), replacement_path);
        // A cached miss survives creation until explicitly reloaded.
        replacements.lookup(first.key, original.Get());
        replacements.finishFrame(device.Get(), context.Get(), 2);
        REQUIRE(replacements.lookup(first.key, original.Get()) == original.Get());
        replacements.request(true);
        replacements.finishFrame(device.Get(), context.Get(), 3);
        REQUIRE(replacements.lookup(first.key, original.Get()) == original.Get());
        pixels.bytes[0] = 250;
        savePng(replacement_path, pixels);
        fs::last_write_time(replacement_path, fs::file_time_type::clock::now() + std::chrono::seconds(2));
        replacements.request(true);
        replacements.finishFrame(device.Get(), context.Get(), 4);
        auto changed = replacements.lookup(first.key, original.Get());
        REQUIRE(changed != original.Get());
        REQUIRE(describe(changed).MipLevels == 3);
        REQUIRE(readPixels(device.Get(), context.Get(), changed, readback_vs, readback_ps).bytes == pixels.bytes);
        // Frame dumps always contain originals, including on a replacement cache hit.
        replacements.request(false);
        replacements.finishFrame(device.Get(), context.Get(), 5);
        REQUIRE(loadPng(root / "frame-5" / (first.key + ".png")).bytes != pixels.bytes);
        { std::ofstream bad(replacement_path, std::ios::binary); bad << "invalid"; }
        replacements.request(true);
        replacements.lookup(first.key, original.Get());
        replacements.finishFrame(device.Get(), context.Get(), 6);
        REQUIRE(replacements.lookup(first.key, original.Get()) == original.Get());
        Pixels wrong{2, 4, std::vector<uint8_t>(32, 127)};
        savePng(replacement_path, wrong);
        replacements.request(true);
        replacements.finishFrame(device.Get(), context.Get(), 7);
        REQUIRE(replacements.lookup(first.key, original.Get()) == original.Get());
        fs::remove(replacement_path);
        replacements.request(true);
        replacements.finishFrame(device.Get(), context.Get(), 8);
        REQUIRE(replacements.lookup(first.key, original.Get()) == original.Get());
        savePng(replacement_path, pixels);
        replacements.request(true);
        replacements.finishFrame(device.Get(), context.Get(), 9);
        REQUIRE(replacements.lookup(first.key, original.Get()) != original.Get());
        replacements.clear();
        replacements.lookup(first.key, original.Get());
        replacements.finishFrame(device.Get(), context.Get(), 10);
        REQUIRE(replacements.lookup(first.key, original.Get()) != original.Get());
        // A locked output retries on the next continuous-dump frame.
        const auto retry_path = root / "dump" / "retry.png";
        { std::ofstream placeholder(retry_path); placeholder << "locked"; }
        HANDLE locked = CreateFileW(retry_path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        REQUIRE(locked != INVALID_HANDLE_VALUE);
        replacements.lookup("retry", original.Get());
        replacements.finishFrame(device.Get(), context.Get(), 11);
        CloseHandle(locked);
        replacements.lookup("retry", original.Get());
        replacements.finishFrame(device.Get(), context.Get(), 12);
        REQUIRE(loadPng(retry_path).bytes.size() == 64);
        fs::rename(root / "replace", root / "old-replace");
        { std::ofstream blocked(root / "replace"); blocked << "not a directory"; }
        replacements.request(true);
        replacements.finishFrame(device.Get(), context.Get(), 13);
        REQUIRE(replacements.lookup(first.key, original.Get()) == original.Get());

        // Synthetic DDS chains: legacy BC1/2/3 and straight RGBA8.
        for (UINT format : {0x31545844u, 0x33545844u, 0x35545844u, 0u}) {
            UINT block = format == 0x31545844u ? 8 : 16;
            std::vector<uint8_t> dds(128 + (format ? 3 * block : 84), 0);
            auto set = [&](size_t offset, uint32_t value) { std::memcpy(dds.data() + offset, &value, 4); };
            set(0, 0x20534444); set(4, 124); set(12, 4); set(16, 4); set(28, 3); set(76, 32);
            set(80, format ? 4 : 0x41); set(84, format);
            if (!format) { set(88, 32); set(92, 0xff); set(96, 0xff00); set(100, 0xff0000); set(104, 0xff000000); }
            const auto path = root / "test.dds";
            { std::ofstream out(path, std::ios::binary); out.write(reinterpret_cast<char *>(dds.data()), dds.size()); }
            uint64_t used = 0;
            auto view = loadDds(device.Get(), path, desc, used);
            REQUIRE(view && describe(view.Get()).MipLevels == 3 && used == dds.size() - 128);
            // Small files fit above 448 MiB resident, but payload allocation still respects the remaining budget.
            REQUIRE(loadDds(device.Get(), path, desc, used, resident_limit - 449u * 1024u * 1024u));
            bool over_budget = false;
            try { loadDds(device.Get(), path, desc, used, 1); } catch (const std::exception &) { over_budget = true; }
            REQUIRE(over_budget);
            dds.pop_back();
            { std::ofstream out(path, std::ios::binary); out.write(reinterpret_cast<char *>(dds.data()), dds.size()); }
            bool rejected = false;
            try { loadDds(device.Get(), path, desc, used); } catch (const std::exception &) { rejected = true; }
            REQUIRE(rejected);
        }
        // DX10 BC7 is deliberately refused even if a downlevel driver exposes it.
        std::vector<uint8_t> bc7(164, 0);
        auto set = [&](size_t offset, uint32_t value) { std::memcpy(bc7.data() + offset, &value, 4); };
        set(0, 0x20534444); set(4, 124); set(12, 4); set(16, 4); set(76, 32);
        set(80, 4); set(84, 0x30315844); set(128, DXGI_FORMAT_BC7_UNORM);
        set(132, 3); set(140, 1); set(144, 1);
        const auto bc7_path = root / "bc7.dds";
        { std::ofstream out(bc7_path, std::ios::binary); out.write(reinterpret_cast<char *>(bc7.data()), bc7.size()); }
        uint64_t used = 0;
        bool rejected = false;
        try { loadDds(device.Get(), bc7_path, desc, used); } catch (const std::exception &) { rejected = true; }
        REQUIRE(rejected);
        device.Reset(); context.Reset();
        requested = D3D_FEATURE_LEVEL_11_0;
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &requested, 1,
            D3D11_SDK_VERSION, &device, &obtained, &context));
        REQUIRE(loadDds(device.Get(), bc7_path, desc, used) && used == 16);
        std::puts("PASS texture identity, capture ownership, PNG round trip, reload, fallback, DDS and FL10 mips");
    } catch (const std::exception &e) {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
    return 0;
}
