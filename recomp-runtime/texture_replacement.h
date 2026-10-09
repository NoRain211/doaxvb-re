#pragma once

#include "d3d_presenter.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// The snapshot owns every mip and the palette; addresses never enter the key.
struct TextureIdentity {
    std::vector<uint8_t> bytes;
    std::string key;
    uint32_t mip_levels = 0;
    bool matches(const RecompD3dPresenterDrawCommand &draw) const;
    void assign(const RecompD3dPresenterDrawCommand &draw);
};

class TextureReplacements {
public:
    TextureReplacements();
    bool enabled() const { return enabled_; }
    void request(bool reload) { if (reload) reload_ = true; else frame_dump_ = true; }
    ID3D11ShaderResourceView *lookup(const std::string &key, ID3D11ShaderResourceView *original);
    // Called after presentation. Only this method accesses files or changes GPU state.
    void finishFrame(ID3D11Device *device, ID3D11DeviceContext *context, uint32_t present);
    void clear();

private:
    using View = Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>;
    struct File {
        std::filesystem::path path;
        std::filesystem::file_time_type time;
        uintmax_t size;
    };
    struct Replacement { View view; uint64_t bytes = 0; };
    void scan();
    bool enabled_ = false, dump_ = false, scanned_ = false;
    bool reload_ = false, frame_dump_ = false;
    uint32_t dump_at_ = 0;
    uint64_t resident_ = 0;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> readback_vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> readback_ps_;
    std::filesystem::path root_, directory_;
    std::unordered_map<std::string, File> files_;
    std::unordered_map<std::string, Replacement> replacements_;
    std::unordered_map<std::string, View> frame_;
    std::unordered_set<std::string> dumped_;
};
