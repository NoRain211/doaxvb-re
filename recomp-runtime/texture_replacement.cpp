#include "texture_replacement.h"
#include "../xbe/sha.h"
#include <d3dcompiler.h>
#include <wincodec.h>
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace {
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
constexpr uint64_t file_limit = 64u * 1024u * 1024u;
constexpr uint64_t resident_limit = 512u * 1024u * 1024u;

void check(HRESULT result)
{
    if (FAILED(result)) {
        char message[96];
        std::snprintf(message, sizeof message, "image or GPU operation failed hr=0x%08lX",
            static_cast<unsigned long>(result));
        throw std::runtime_error(message);
    }
}

struct Wic {
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> factory;
    ~Wic() { factory.Reset(); if (SUCCEEDED(initialized)) CoUninitialize(); }
    void create() { check(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))); }
};

struct Pixels { UINT width = 0, height = 0; std::vector<uint8_t> bytes; };

void savePng(const fs::path &path, const Pixels &pixels)
{
    Wic wic;
    wic.create();
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    check(wic.factory->CreateStream(&stream));
    check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE));
    check(wic.factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder));
    check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache));
    check(encoder->CreateNewFrame(&frame, nullptr));
    check(frame->Initialize(nullptr));
    check(frame->SetSize(pixels.width, pixels.height));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    check(frame->SetPixelFormat(&format));
    if (format != GUID_WICPixelFormat32bppBGRA) throw std::runtime_error("PNG pixel format");
    auto bgra = pixels.bytes;
    for (size_t i = 0; i < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
    check(frame->WritePixels(pixels.height, pixels.width * 4u,
        static_cast<UINT>(bgra.size()), bgra.data()));
    check(frame->Commit());
    check(encoder->Commit());
}

Pixels loadPng(const fs::path &path)
{
    Wic wic;
    wic.create();
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    check(wic.factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnDemand, &decoder));
    GUID container;
    check(decoder->GetContainerFormat(&container));
    if (container != GUID_ContainerFormatPng) throw std::runtime_error("not a PNG");
    check(decoder->GetFrame(0, &frame));
    Pixels pixels;
    check(frame->GetSize(&pixels.width, &pixels.height));
    uint64_t size = uint64_t(pixels.width) * pixels.height * 4u;
    if (!size || size > file_limit) throw std::runtime_error("PNG exceeds decoded byte budget");
    check(wic.factory->CreateFormatConverter(&converter));
    // Straight alpha, no color-profile or sRGB conversion.
    check(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
    pixels.bytes.resize(static_cast<size_t>(size));
    check(converter->CopyPixels(nullptr, pixels.width * 4u,
        static_cast<UINT>(size), pixels.bytes.data()));
    return pixels;
}

D3D11_TEXTURE2D_DESC describe(ID3D11ShaderResourceView *view)
{
    ComPtr<ID3D11Resource> resource;
    ComPtr<ID3D11Texture2D> texture;
    view->GetResource(&resource);
    check(resource.As(&texture));
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    return desc;
}

// A texel Load lets the GPU decode BC, BGRA and A8 with its own sampling rules.
// All callers run at the frame boundary; no draw pipeline state is retained.
Pixels readPixels(ID3D11Device *device, ID3D11DeviceContext *context,
    ID3D11ShaderResourceView *source, ComPtr<ID3D11VertexShader> &vs,
    ComPtr<ID3D11PixelShader> &ps)
{
    auto desc = describe(source);
    Pixels pixels{desc.Width, desc.Height, {}};
    if (uint64_t(desc.Width) * desc.Height * 4u > file_limit)
        throw std::runtime_error("dump exceeds byte budget");
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    desc.CPUAccessFlags = desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> target, staging;
    ComPtr<ID3D11RenderTargetView> rtv;
    check(device->CreateTexture2D(&desc, nullptr, &target));
    check(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    check(device->CreateTexture2D(&desc, nullptr, &staging));
    static constexpr char shader[] =
        "float4 vs(uint i:SV_VertexID):SV_Position {"
        "return float4(i==2?3:-1,i==1?3:-1,0,1); }"
        "Texture2D image:register(t0);"
        "float4 ps(float4 p:SV_Position):SV_Target {return image.Load(int3(p.xy,0));}";
    static const auto code = [] {
        std::pair<ComPtr<ID3DBlob>, ComPtr<ID3DBlob>> result;
        check(D3DCompile(shader, sizeof shader, nullptr, nullptr, nullptr,
            "vs", "vs_4_0", 0, 0, &result.first, nullptr));
        check(D3DCompile(shader, sizeof shader, nullptr, nullptr, nullptr,
            "ps", "ps_4_0", 0, 0, &result.second, nullptr));
        return result;
    }();
    if (!vs) check(device->CreateVertexShader(code.first->GetBufferPointer(), code.first->GetBufferSize(), nullptr, &vs));
    if (!ps) check(device->CreatePixelShader(code.second->GetBufferPointer(), code.second->GetBufferSize(), nullptr, &ps));
    context->ClearState();
    D3D11_VIEWPORT viewport{0, 0, float(desc.Width), float(desc.Height), 0, 1};
    context->RSSetViewports(1, &viewport);
    ID3D11RenderTargetView *output = rtv.Get();
    context->OMSetRenderTargets(1, &output, nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->PSSetShader(ps.Get(), nullptr, 0);
    context->PSSetShaderResources(0, 1, &source);
    context->Draw(3, 0);
    context->ClearState();
    context->CopyResource(staging.Get(), target.Get());
    pixels.bytes.resize(size_t(desc.Width) * desc.Height * 4u);
    D3D11_MAPPED_SUBRESOURCE mapped;
    check(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
    for (UINT y = 0; y < desc.Height; ++y)
        std::memcpy(pixels.bytes.data() + size_t(y) * desc.Width * 4u,
            static_cast<const uint8_t *>(mapped.pData) + size_t(y) * mapped.RowPitch, desc.Width * 4u);
    context->Unmap(staging.Get(), 0);
    return pixels;
}

void shape(ID3D11Device *device, UINT width, UINT height, const D3D11_TEXTURE2D_DESC &original)
{
    const UINT limit = device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0 ? 16384u : 8192u;
    if (!width || !height || width > limit || height > limit ||
        uint64_t(width) * original.Height != uint64_t(height) * original.Width)
        throw std::runtime_error("dimensions or aspect ratio");
}

uint32_t word(const std::vector<uint8_t> &bytes, size_t offset)
{
    if (offset + 4u > bytes.size()) throw std::runtime_error("truncated DDS");
    uint32_t value;
    std::memcpy(&value, bytes.data() + offset, 4);
    return value;
}

ComPtr<ID3D11ShaderResourceView> loadDds(ID3D11Device *device, const fs::path &path,
    const D3D11_TEXTURE2D_DESC &original, uint64_t &bytes_used, uint64_t budget = resident_limit)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto length = file.tellg();
    if (length < 128 || length > file_limit) throw std::runtime_error("DDS file size");
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char *>(bytes.data()), bytes.size()))
        throw std::runtime_error("DDS read");
    if (word(bytes, 0) != 0x20534444u || word(bytes, 4) != 124 || word(bytes, 76) != 32 ||
        word(bytes, 112) != 0 || word(bytes, 24) > 1)
        throw std::runtime_error("DDS header, array, cube or volume");
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = word(bytes, 16); desc.Height = word(bytes, 12);
    shape(device, desc.Width, desc.Height, original);
    desc.MipLevels = (std::max)(1u, word(bytes, 28));
    UINT max_mips = 1;
    for (UINT dim = (std::max)(desc.Width, desc.Height); dim > 1; dim /= 2) ++max_mips;
    if (desc.MipLevels > max_mips) throw std::runtime_error("DDS mip count");
    size_t offset = 128;
    UINT block = 0;
    if (word(bytes, 80) & 4u) {
        switch (word(bytes, 84)) {
        case 0x31545844: desc.Format = DXGI_FORMAT_BC1_UNORM; block = 8; break;
        case 0x33545844: desc.Format = DXGI_FORMAT_BC2_UNORM; block = 16; break;
        case 0x35545844: desc.Format = DXGI_FORMAT_BC3_UNORM; block = 16; break;
        case 0x30315844:
            if (word(bytes, 132) != 3 || word(bytes, 136) != 0 || word(bytes, 140) != 1 ||
                (word(bytes, 144) != 0 && word(bytes, 144) != 1))
                throw std::runtime_error("DDS DX10 shape or alpha mode");
            desc.Format = static_cast<DXGI_FORMAT>(word(bytes, 128));
            offset = 148;
            switch (desc.Format) {
            case DXGI_FORMAT_BC1_UNORM: block = 8; break;
            case DXGI_FORMAT_BC2_UNORM: case DXGI_FORMAT_BC3_UNORM: block = 16; break;
            case DXGI_FORMAT_BC7_UNORM:
                if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0)
                    throw std::runtime_error("BC7 requires feature level 11");
                block = 16; break;
            case DXGI_FORMAT_R8G8B8A8_UNORM: break;
            default: throw std::runtime_error("DDS DXGI format");
            }
            break;
        default: throw std::runtime_error("DDS FourCC");
        }
    } else {
        if ((word(bytes, 80) & 0x41u) != 0x41u || word(bytes, 88) != 32 ||
            word(bytes, 92) != 0xff || word(bytes, 96) != 0xff00 ||
            word(bytes, 100) != 0xff0000 || word(bytes, 104) != 0xff000000)
            throw std::runtime_error("DDS RGBA masks");
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    }
    if (block && (desc.Width % 4 || desc.Height % 4)) throw std::runtime_error("DDS block dimensions");
    std::vector<D3D11_SUBRESOURCE_DATA> data(desc.MipLevels);
    UINT width = desc.Width, height = desc.Height;
    bytes_used = 0;
    for (auto &mip : data) {
        UINT pitch = block ? ((width + 3u) / 4u) * block : width * 4u;
        size_t size = size_t(pitch) * (block ? (height + 3u) / 4u : height);
        if (offset + size > bytes.size()) throw std::runtime_error("DDS mip payload truncated");
        mip.pSysMem = bytes.data() + offset; mip.SysMemPitch = pitch;
        offset += size; bytes_used += size;
        width = (std::max)(1u, width / 2u); height = (std::max)(1u, height / 2u);
    }
    if (offset != bytes.size()) throw std::runtime_error("DDS trailing payload");
    if (bytes_used > budget) throw std::runtime_error("resident byte budget");
    desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    check(device->CreateTexture2D(&desc, data.data(), &texture));
    check(device->CreateShaderResourceView(texture.Get(), nullptr, &view));
    return view;
}
uint32_t frameNumber(const char *value)
{
    uint32_t number = 0;
    const char *end = value + std::strlen(value);
    const auto parsed = std::from_chars(value, end, number);
    return parsed.ec == std::errc{} && parsed.ptr == end ? number : 0u;
}

fs::path privatePath(const fs::path &path)
{
    const auto root = fs::weakly_canonical(fs::current_path() / "private");
    const auto candidate = fs::weakly_canonical(fs::absolute(path));
    const auto match = std::mismatch(root.begin(), root.end(), candidate.begin(), candidate.end(),
        [](const fs::path &a, const fs::path &b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; });
    if (match.first != root.end()) throw std::runtime_error("texture paths must be under private");
    return candidate;
}
} // namespace

bool TextureIdentity::matches(const RecompD3dPresenterDrawCommand &draw) const
{
    const size_t palette = draw.texture.format_byte == RECOMP_D3D_TEXTURE_FORMAT_P8 ? 1024u : 0u;
    return !key.empty() && mip_levels == draw.texture.mip_levels &&
        bytes.size() == size_t(draw.texture_byte_count) + palette &&
        std::memcmp(bytes.data(), draw.texture_bytes, draw.texture_byte_count) == 0 &&
        (!palette || std::memcmp(bytes.data() + draw.texture_byte_count, draw.palette_bytes, palette) == 0);
}

void TextureIdentity::assign(const RecompD3dPresenterDrawCommand &draw)
{
    key.clear();
    const auto *begin = static_cast<const uint8_t *>(draw.texture_bytes);
    bytes.assign(begin, begin + draw.texture_byte_count);
    if (draw.texture.format_byte == RECOMP_D3D_TEXTURE_FORMAT_P8) {
        const auto *palette = static_cast<const uint8_t *>(draw.palette_bytes);
        bytes.insert(bytes.end(), palette, palette + 1024u);
    }
    mip_levels = draw.texture.mip_levels;
    char prefix[80];
    std::snprintf(prefix, sizeof prefix, "%02x-%ux%u-m%u-", draw.texture.format_byte,
        draw.texture.width, draw.texture.height, (std::max)(1u, mip_levels));
    key = prefix + doaxbv::sha256Hex(bytes);
}

TextureReplacements::TextureReplacements()
{
    const char *dump = std::getenv("RECOMP_TEXTURE_DUMP");
    const char *directory = std::getenv("RECOMP_TEXTURES");
    dump_ = dump && std::strcmp(dump, "1") == 0;
    enabled_ = dump_ || (directory && *directory);
    const char *root = std::getenv("RECOMP_TEXTURE_DUMP_DIR");
    root_ = root && *root ? fs::path(root) : fs::path("private/textures");
    directory_ = directory && *directory && std::strcmp(directory, "1") != 0
        ? fs::path(directory) : fs::path("private/textures/replace");
    const char *at = std::getenv("RECOMP_TEXTURE_FRAME_AT");
    if (at) {
        dump_at_ = frameNumber(at);
        if (!dump_at_) std::fprintf(stderr, "recomp textures: ignoring invalid RECOMP_TEXTURE_FRAME_AT '%s'\n", at);
        else std::fprintf(stderr, "recomp textures: frame dump at %u\n", dump_at_);
    }
    if (enabled_) {
        try { root_ = privatePath(root_); directory_ = privatePath(directory_); }
        catch (const std::exception &e) {
            std::fprintf(stderr, "recomp textures: disabled (%s)\n", e.what());
            enabled_ = dump_ = false;
        }
    }
}

ID3D11ShaderResourceView *TextureReplacements::lookup(const std::string &key,
    ID3D11ShaderResourceView *original)
{
    if (!enabled_ || key.empty()) return original;
    // Retain the original GPU resource, never a pointer into guest memory.
    try { if (frame_.size() < 4096) frame_.emplace(key, original); }
    catch (const std::bad_alloc &) { return original; }
    const auto found = replacements_.find(key);
    return found != replacements_.end() && found->second.view ? found->second.view.Get() : original;
}

void TextureReplacements::scan()
{
    std::unordered_map<std::string, File> files;
    std::error_code error;
    fs::directory_iterator it(directory_, error), end;
    for (; !error && it != end; it.increment(error)) {
        if (files.size() >= 16384) break;
        const auto &entry = *it;
        const auto extension = entry.path().extension();
        if ((extension != ".png" && extension != ".dds") || !entry.is_regular_file(error)) continue;
        auto key = entry.path().stem().string();
        auto time = entry.last_write_time(error);
        if (error) break;
        auto size = entry.file_size(error);
        if (error) break;
        // PNG is the editable master when both names are present.
        if (extension == ".png" || files.find(key) == files.end())
            files[key] = {privatePath(entry.path()), time, size};
    }
    if (error && error != std::errc::no_such_file_or_directory)
        throw std::runtime_error("replacement directory scan");
    for (auto r = replacements_.begin(); r != replacements_.end();) {
        const auto old = files_.find(r->first), next = files.find(r->first);
        if (old == files_.end() || next == files.end() || old->second.path != next->second.path ||
            old->second.time != next->second.time || old->second.size != next->second.size) {
            resident_ -= r->second.bytes;
            r = replacements_.erase(r);
        } else ++r;
    }
    files_ = std::move(files);
    scanned_ = true;
}

void TextureReplacements::finishFrame(ID3D11Device *device, ID3D11DeviceContext *context, uint32_t present)
{
    if (!enabled_) return;
    try {
        if (!scanned_ || reload_) scan();
        if (reload_) std::fprintf(stderr, "recomp textures: rescanned %zu files\n", files_.size());
        reload_ = false;
        const bool frame_dump = frame_dump_ || (dump_at_ && present == dump_at_);
        const auto frame_dir = root_ / ("frame-" + std::to_string(present));
        if (dump_) fs::create_directories(root_ / "dump");
        if (frame_dump) fs::create_directories(frame_dir);
        for (auto &item : frame_) {
            const auto &key = item.first;
            auto original = item.second.Get();
            Pixels pixels;
            auto read = [&]() -> const Pixels & {
                if (pixels.bytes.empty()) pixels = readPixels(device, context, original, readback_vs_, readback_ps_);
                return pixels;
            };
            const bool unique_dump = dump_ && dumped_.size() < 16384 && dumped_.find(key) == dumped_.end();
            if (unique_dump || frame_dump) {
                try {
                    if (unique_dump) {
                        savePng(root_ / "dump" / (key + ".png"), read());
                        dumped_.insert(key);
                    }
                    if (frame_dump) savePng(frame_dir / (key + ".png"), read());
                } catch (const std::exception &e) {
                    std::fprintf(stderr, "recomp textures: dump rejected %s (%s)\n", key.c_str(), e.what());
                }
            }
            if (replacements_.find(key) != replacements_.end()) continue;
            // Missing keys stay in memory until reload. Bound metadata as well as GPU bytes.
            if (replacements_.size() >= 16384) continue;
            auto &replacement = replacements_[key];
            const auto file = files_.find(key);
            if (file == files_.end()) continue;
            try {
                if (file->second.size > file_limit) throw std::runtime_error("file byte budget");
                const auto desc = describe(original);
                ComPtr<ID3D11ShaderResourceView> view;
                uint64_t bytes = 0;
                if (file->second.path.extension() == ".dds") {
                    if (file->second.size > resident_limit - resident_)
                        throw std::runtime_error("resident byte budget");
                    view = loadDds(device, file->second.path, desc, bytes, resident_limit - resident_);
                } else {
                    const auto png = loadPng(file->second.path);
                    shape(device, png.width, png.height, desc);
                    // A round-trip master must preserve authored mips and BC interpolation.
                    if (png.width != desc.Width || png.height != desc.Height || png.bytes != read().bytes) {
                        D3D11_TEXTURE2D_DESC target{};
                        target.Width = png.width; target.Height = png.height;
                        target.ArraySize = target.SampleDesc.Count = 1;
                        target.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                        target.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
                        target.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
                        for (UINT w = png.width, h = png.height;; w = (std::max)(1u, w / 2u), h = (std::max)(1u, h / 2u)) {
                            bytes += uint64_t(w) * h * 4u;
                            if (w == 1 && h == 1) break;
                        }
                        if (resident_ + bytes > resident_limit) throw std::runtime_error("resident byte budget");
                        UINT support = 0;
                        check(device->CheckFormatSupport(target.Format, &support));
                        if (!(support & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN)) throw std::runtime_error("mip generation unsupported");
                        ComPtr<ID3D11Texture2D> texture;
                        check(device->CreateTexture2D(&target, nullptr, &texture));
                        check(device->CreateShaderResourceView(texture.Get(), nullptr, &view));
                        context->UpdateSubresource(texture.Get(), 0, nullptr, png.bytes.data(), png.width * 4u, 0);
                        context->GenerateMips(view.Get());
                    }
                }
                replacement.view = view; replacement.bytes = bytes; resident_ += bytes;
                std::fprintf(stderr, "recomp textures: loaded %s (%s, resident=%llu)\n", key.c_str(),
                    view ? "replacement" : "unchanged original", static_cast<unsigned long long>(resident_));
            } catch (const std::exception &e) {
                std::fprintf(stderr, "recomp textures: rejected %s (%s)\n", key.c_str(), e.what());
            }
        }
        if (frame_dump) std::fprintf(stderr, "recomp textures: frame %u dumped (%zu textures)\n", present, frame_.size());
    } catch (const std::exception &e) {
        std::fprintf(stderr, "recomp textures: frame work failed (%s)\n", e.what());
        // A failed rescan must not leave stale replacements active. Disable
        // continuous dumping after an output-directory error to avoid a log flood.
        replacements_.clear(); files_.clear(); resident_ = 0; dump_ = false;
        // Retry directory scans only on an explicit reload.
        scanned_ = true; reload_ = false;
    }
    frame_.clear();
    frame_dump_ = false;
}

void TextureReplacements::clear()
{
    frame_.clear(); replacements_.clear(); files_.clear(); dumped_.clear(); resident_ = 0;
    readback_vs_.Reset(); readback_ps_.Reset();
    scanned_ = false;
}
