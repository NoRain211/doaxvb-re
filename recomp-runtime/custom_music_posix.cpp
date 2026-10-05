#include "custom_music.h"

// miniaudio 0.11.23, f40cf03f80cdb7e741d43e53b7e706e8c1394bcf (MIT-0).
// Only its streaming decoders and PCM conversion are used; SDL owns output.
#define MA_NO_DEVICE_IO
#define MA_NO_THREADING
#define MA_NO_ENGINE
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_GENERATION
#define MA_NO_ENCODING
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include <algorithm>
#include <chrono>
#include <codecvt>
#include <cstdio>
#include <filesystem>
#include <locale>
#include <locale.h>
#include <memory>
#include <string>
#include <vector>
#include <wctype.h>

namespace fs = std::filesystem;

struct RecompMusicDecoder {
    ma_decoder reader{};
    uint32_t duration_ms = 0;
    bool initialized = false, failed = false;
    ~RecompMusicDecoder() { if (initialized) ma_decoder_uninit(&reader); }
};

namespace {
struct Track {
    RecompMusicTrack info{};
    std::string path;
    std::wstring wide_path;
};
std::vector<Track> tracks;
uint32_t total_ms;

void report(const char *what, const char *path)
{
    std::fprintf(stderr, "[custom-music] %s file=\"%s\"\n", what, path);
}

const Track *find(uint32_t id)
{
    for (const auto &track : tracks) if (track.info.id == id) return &track;
    return nullptr;
}

std::unique_ptr<RecompMusicDecoder> open_file(const char *path, uint32_t duration_ms = 0)
{
    auto decoder = std::make_unique<RecompMusicDecoder>();
    auto config = ma_decoder_config_init(ma_format_s16, 2, RECOMP_MUSIC_RATE);
    if (ma_decoder_init_file(path, &config, &decoder->reader) != MA_SUCCESS) return nullptr;
    decoder->initialized = true;
    // MP3 length queries scan the file. Reuse the catalog duration on playback.
    if (duration_ms) {
        decoder->duration_ms = duration_ms;
        return decoder;
    }
    ma_uint64 frames = 0;
    if (ma_decoder_get_length_in_pcm_frames(&decoder->reader, &frames) != MA_SUCCESS ||
        frames < (RECOMP_MUSIC_RATE + 999) / 1000 ||
        frames > uint64_t(UINT32_MAX) * RECOMP_MUSIC_RATE / 1000) return nullptr;
    decoder->duration_ms = static_cast<uint32_t>(frames * 1000 / RECOMP_MUSIC_RATE);
    return decoder;
}

// Hash UTF-16 code units, as on Windows, even though POSIX wchar_t is UTF-32.
uint32_t song_id(const std::u16string &key)
{
    uint32_t hash = 2166136261u;
    for (char16_t c : key) {
        hash = (hash ^ (c & 0xffu)) * 16777619u;
        hash = (hash ^ (c >> 8u)) * 16777619u;
    }
    return hash;
}

std::u16string folded(const std::string &name)
{
    // Do not change the process locale: other runtime threads also use libc.
    struct FoldLocale {
        locale_t value = newlocale(LC_CTYPE_MASK, "C.UTF-8", nullptr);
        ~FoldLocale() { if (value) freelocale(value); }
    };
    static const FoldLocale locale;
    if (!locale.value) return {};
    std::wstring_convert<std::codecvt_utf8<wchar_t>> utf8;
    auto wide = utf8.from_bytes(name);
    for (auto &c : wide) c = static_cast<wchar_t>(towlower_l(c, locale.value));
    return std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>().from_bytes(
        utf8.to_bytes(wide));
}

std::vector<std::pair<std::u16string, fs::path>> list_folder(const fs::path &folder)
{
    std::vector<std::pair<std::u16string, fs::path>> files;
    std::error_code error;
    fs::create_directories(folder, error);
    fs::directory_iterator it(folder, error), end;
    for (; !error && it != end; it.increment(error)) {
        std::error_code entry_error;
        if (!it->is_regular_file(entry_error)) continue;
        const auto &path = it->path();
        try {
            auto key = folded(path.filename().string());
            const auto dot = key.find_last_of(u'.');
            const auto ext = dot == key.npos ? std::u16string() : key.substr(dot);
            if (ext == u".mp3" || ext == u".wav" || ext == u".flac")
                files.emplace_back(std::move(key), path);
        } catch (const std::range_error &) {
            report("skipped invalid UTF-8 name", path.c_str());
        }
    }
    if (error) report("scan stopped", folder.c_str());
    std::sort(files.begin(), files.end());
    return files;
}

void add_track(const std::u16string &key, const fs::path &path)
{
    Track track;
    track.info.id = song_id(key);
    if (track.info.id == 0 || track.info.id == UINT32_MAX || find(track.info.id)) {
        report("skipped duplicate ID", path.c_str());
        return;
    }
    auto decoder = open_file(path.c_str());
    uint8_t probe[RECOMP_MUSIC_FRAME_BYTES];
    if (!decoder || recomp_music_read(decoder.get(), probe, sizeof probe) != sizeof probe ||
        !recomp_music_rewind(decoder.get())) {
        report("skipped", path.c_str());
        return;
    }
    track.info.duration_ms = decoder->duration_ms;
    const auto name = path.stem().u16string();
    size_t length = std::min<size_t>(name.size(), RECOMP_MUSIC_NAME_UNITS - 1);
    if (length && name[length - 1] >= 0xd800 && name[length - 1] <= 0xdbff) --length;
    std::copy_n(name.begin(), length, track.info.name);
    track.path = path.string();
    track.wide_path = std::wstring_convert<std::codecvt_utf8<wchar_t>>().from_bytes(track.path);
    tracks.push_back(std::move(track));
    total_ms = static_cast<uint32_t>(std::min<uint64_t>(
        uint64_t(total_ms) + decoder->duration_ms, UINT32_MAX));
}
} // namespace

extern "C" void recomp_music_initialize(const char *folder_path)
{
    recomp_music_shutdown();
    if (!folder_path) return;
    const auto started = std::chrono::steady_clock::now();
    try {
        for (const auto &[key, path] : list_folder(fs::path(folder_path))) {
            if (tracks.size() == RECOMP_MUSIC_MAX_TRACKS) {
                report("scan stopped at track limit", path.c_str());
                break;
            }
            add_track(key, path);
        }
    } catch (const std::exception &error) {
        std::fprintf(stderr, "[custom-music] scan stopped error=%s\n", error.what());
    }
    const auto scan_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    std::fprintf(stderr, "[custom-music] catalog tracks=%zu scan_ms=%lld\n",
        tracks.size(), static_cast<long long>(scan_ms));
}

extern "C" void recomp_music_shutdown(void) { tracks.clear(); total_ms = 0; }
extern "C" uint32_t recomp_music_count(void) { return static_cast<uint32_t>(tracks.size()); }
extern "C" uint32_t recomp_music_duration(void) { return total_ms; }
extern "C" const RecompMusicTrack *recomp_music_track(uint32_t index)
{
    return index < tracks.size() ? &tracks[index].info : nullptr;
}
extern "C" const wchar_t *recomp_music_path(uint32_t id)
{
    const Track *track = find(id);
    return track ? track->wide_path.c_str() : nullptr;
}
extern "C" RecompMusicDecoder *recomp_music_open(uint32_t id)
{
    const Track *track = find(id);
    if (!track) return nullptr;
    try {
        auto decoder = open_file(track->path.c_str(), track->info.duration_ms);
        if (!decoder) report("open failed", track->path.c_str());
        return decoder.release();
    } catch (const std::bad_alloc &) {
        report("open failed", track->path.c_str());
        return nullptr;
    }
}
extern "C" void recomp_music_close(RecompMusicDecoder *decoder) { delete decoder; }

extern "C" int recomp_music_read(RecompMusicDecoder *decoder, void *pcm, uint32_t bytes)
{
    if (!decoder || !pcm || bytes == 0 || bytes > RECOMP_MUSIC_MAX_READ ||
        bytes % RECOMP_MUSIC_FRAME_BYTES || decoder->failed) return -1;
    ma_uint64 frames = 0;
    const auto result = ma_decoder_read_pcm_frames(&decoder->reader, pcm,
        bytes / RECOMP_MUSIC_FRAME_BYTES, &frames);
    if (result != MA_SUCCESS && result != MA_AT_END) {
        decoder->failed = true;
        return -1;
    }
    return static_cast<int>(frames * RECOMP_MUSIC_FRAME_BYTES);
}

extern "C" int recomp_music_rewind(RecompMusicDecoder *decoder)
{
    if (!decoder || ma_decoder_seek_to_pcm_frame(&decoder->reader, 0) != MA_SUCCESS) return 0;
    decoder->failed = false;
    return 1;
}
