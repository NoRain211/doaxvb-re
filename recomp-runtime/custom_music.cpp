#include "custom_music.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <new>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;
using Error = HRESULT;
#else
// macOS and Linux have no Media Foundation; miniaudio decodes and resamples.
#define MA_NO_DEVICE_IO
#define MA_NO_THREADING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

using Error = long;
#endif

namespace fs = std::filesystem;

#ifdef _WIN32
constexpr LONGLONG kTicksPerMs = 10000;         // Media Foundation time is in 100 ns units
constexpr uint32_t kMaxEmptySamples = 32;       // consecutive empty reads before giving up
constexpr DWORD kMaxSampleBytes = 1024 * 1024;  // larger decoded samples are treated as corrupt

struct RecompMusicDecoder {
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaBuffer> pending;
    uint32_t offset = 0, duration_ms = 0;
    bool eof = false, failed = false;
};
#else
struct RecompMusicDecoder {
    ma_decoder decoder{};  // miniaudio keeps pointers into it; never moved after init
    bool initialized = false, failed = false;
    uint32_t duration_ms = 0;
    ~RecompMusicDecoder()
    {
        if (initialized) ma_decoder_uninit(&decoder);
    }
};
#endif

namespace {
struct Track {
    RecompMusicTrack info{};
    std::wstring path;
};
std::vector<Track> tracks;
uint32_t total_ms;
bool backend_ready;

#ifdef _WIN32
const Error kFailed = E_FAIL, kInvalid = E_INVALIDARG, kNoMemory = E_OUTOFMEMORY,
    kDuplicate = HRESULT_FROM_WIN32(ERROR_DUP_NAME),
    kTooMany = HRESULT_FROM_WIN32(ERROR_TOO_MANY_NAMES);
DWORD com_thread;
bool com_initialized;

Error from_system(const std::error_code &error) { return HRESULT_FROM_WIN32(error.value()); }
bool failed(Error error) { return FAILED(error); }

void report(const char *what, const fs::path &path, Error error)
{
    std::fprintf(stderr, "[custom-music] %s file=\"%ls\" error=0x%08lx\n",
        what, path.c_str(), static_cast<unsigned long>(error));
}

// Media Foundation reads the duration from headers, so it is always measured.
std::unique_ptr<RecompMusicDecoder> open_file(const fs::path &path, Error &error, bool)
{
    auto decoder = std::make_unique<RecompMusicDecoder>();
    error = MFCreateSourceReaderFromURL(path.c_str(), nullptr, &decoder->reader);
    if (FAILED(error)) return nullptr;
    auto *reader = decoder->reader.Get();
    error = reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    if (SUCCEEDED(error))
        error = reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
    ComPtr<IMFMediaType> type;
    if (SUCCEEDED(error)) error = MFCreateMediaType(&type);
    if (SUCCEEDED(error)) error = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(error)) error = type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    if (SUCCEEDED(error)) error = type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (SUCCEEDED(error)) error = type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    if (SUCCEEDED(error)) error = type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, RECOMP_MUSIC_RATE);
    if (SUCCEEDED(error)) error = reader->SetCurrentMediaType(
        MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, type.Get());
    PROPVARIANT duration{};
    if (SUCCEEDED(error)) error = reader->GetPresentationAttribute(
        MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &duration);
    if (SUCCEEDED(error)) {
        const ULONGLONG ms = duration.uhVal.QuadPart / kTicksPerMs;
        if (duration.vt != VT_UI8 || ms == 0 || ms > UINT32_MAX) error = E_INVALIDARG;
        else decoder->duration_ms = static_cast<uint32_t>(ms);
    }
    PropVariantClear(&duration);
    return SUCCEEDED(error) ? std::move(decoder) : nullptr;
}

// Empty when the name cannot be case-folded; such files are skipped.
std::wstring folded(const std::wstring &name)
{
    if (name.empty() || name.size() > INT_MAX) return {};
    std::wstring result(name.size(), L'\0');
    const int length = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE,
        name.data(), static_cast<int>(name.size()), result.data(),
        static_cast<int>(result.size()), nullptr, nullptr, 0);
    if (length <= 0) return {};
    result.resize(static_cast<size_t>(length));
    return result;
}

// Delay-loaded so Windows N editions without Media Foundation still run.
bool start_backend(const fs::path &folder)
{
    // Loaded once; delay-loaded imports keep them for the process anyway. Search
    // System32 only: the working directory is the imported disc, so a missing
    // system DLL must not fall back to a same-named file shipped on the disc.
    static const DWORD load_error =
        LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) &&
        LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)
            ? ERROR_SUCCESS : GetLastError();
    if (load_error != ERROR_SUCCESS) {
        report("disabled", folder, HRESULT_FROM_WIN32(load_error));
        return false;
    }
    HRESULT error = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(error)) { report("disabled", folder, error); return false; }
    com_initialized = true;
    com_thread = GetCurrentThreadId();
    error = MFStartup(MF_VERSION);
    if (FAILED(error)) { report("disabled", folder, error); return false; }
    return true;
}

void stop_backend()
{
    if (backend_ready) MFShutdown();
    // COM initialization is per thread; only the initializing thread may undo it.
    if (com_initialized && com_thread == GetCurrentThreadId()) CoUninitialize();
    com_initialized = false;
}
#else
const Error kFailed = MA_ERROR, kInvalid = MA_INVALID_ARGS, kNoMemory = MA_OUT_OF_MEMORY,
    kDuplicate = MA_ALREADY_EXISTS, kTooMany = MA_TOO_BIG;

Error from_system(const std::error_code &error) { return error.value(); }
bool failed(Error error) { return error != MA_SUCCESS; }

void report(const char *what, const fs::path &path, Error error)
{
    std::fprintf(stderr, "[custom-music] %s file=\"%s\" error=%ld\n", what, path.c_str(), error);
}

// measure: an MP3's length means reading every frame, so only the catalog scan asks.
std::unique_ptr<RecompMusicDecoder> open_file(const fs::path &path, Error &error, bool measure)
{
    auto decoder = std::make_unique<RecompMusicDecoder>();
    const ma_decoder_config config = ma_decoder_config_init(ma_format_s16, 2, RECOMP_MUSIC_RATE);
    error = ma_decoder_init_file(path.c_str(), &config, &decoder->decoder);
    if (error != MA_SUCCESS) return nullptr;
    decoder->initialized = true;
    if (!measure) return decoder;
    ma_uint64 frames = 0;
    error = ma_decoder_get_length_in_pcm_frames(&decoder->decoder, &frames);
    const uint64_t ms = frames * 1000 / RECOMP_MUSIC_RATE;
    if (error == MA_SUCCESS && (ms == 0 || ms > UINT32_MAX)) error = MA_INVALID_DATA;
    if (error != MA_SUCCESS) return nullptr;
    decoder->duration_ms = static_cast<uint32_t>(ms);
    return decoder;
}

// shortcut: folds ASCII only, so "Ä.mp3" and "ä.mp3" stay distinct; use a
// Unicode case map if players report case-only duplicates.
std::wstring folded(const std::wstring &name)
{
    std::wstring result = name;
    for (auto &c : result)
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    return result;
}

bool start_backend(const fs::path &) { return true; }
void stop_backend() {}
#endif

const Track *find(uint32_t id)
{
    for (const auto &track : tracks)
        if (track.info.id == id) return &track;
    return nullptr;
}

// FNV-1a over the folded filename: stable across scans and insertions.
// Collisions are rejected, never reassigned.
uint32_t song_id(const std::u16string &key)
{
    uint32_t hash = 2166136261u;
    for (char16_t c : key) {
        hash = (hash ^ (c & 0xffu)) * 16777619u;
        hash = (hash ^ (c >> 8u)) * 16777619u;
    }
    return hash;
}

// Supported files sorted by folded name; each entry fails on its own.
// Keys are UTF-16 on every platform, so IDs and order match Windows.
std::vector<std::pair<std::u16string, fs::path>> list_folder(const fs::path &folder)
{
    std::vector<std::pair<std::u16string, fs::path>> files;
    std::error_code error;
    fs::create_directories(folder, error);
    fs::directory_iterator it(folder, error), end;
    for (; !error && it != end; it.increment(error)) {
        std::error_code entry_error;
        const auto &path = it->path();
        if (!it->is_regular_file(entry_error)) continue;
        try {
            const fs::path key = folded(path.filename().wstring());
            const auto ext = key.extension().wstring();
            if (ext == L".mp3" || ext == L".wav" || ext == L".flac")
                files.emplace_back(key.u16string(), path);
        } catch (const std::system_error &) {
            // a POSIX name that is not valid UTF-8 is left out
        }
    }
    if (error) report("scan stopped", folder, from_system(error));
    std::sort(files.begin(), files.end());
    return files;
}

void add_track(const std::u16string &key, const fs::path &path)
{
    Track track;
    track.info.id = song_id(key);
    if (track.info.id == 0 || track.info.id == UINT32_MAX || find(track.info.id)) {
        report("skipped", path, kDuplicate);
        return;
    }
    Error error = kFailed;
    auto decoder = open_file(path, error, true);
    uint8_t probe[RECOMP_MUSIC_FRAME_BYTES];
    if (!decoder || recomp_music_read(decoder.get(), probe, sizeof probe) != sizeof probe ||
            !recomp_music_rewind(decoder.get())) {
        report("skipped", path, failed(error) ? error : kFailed);
        return;
    }
    track.info.duration_ms = decoder->duration_ms;
    const auto name = path.stem().u16string();
    size_t length = std::min<size_t>(name.size(), RECOMP_MUSIC_NAME_UNITS - 1);
    if (length && name[length - 1] >= 0xd800 && name[length - 1] <= 0xdbff) --length;
    std::copy_n(name.begin(), length, track.info.name);
    track.path = path.wstring();
    tracks.push_back(std::move(track));
    total_ms = static_cast<uint32_t>(std::min<uint64_t>(
        uint64_t(total_ms) + track.info.duration_ms, UINT32_MAX));
}
} // namespace

extern "C" void recomp_music_initialize(const char *folder_path)
{
    recomp_music_shutdown();
    if (!folder_path) return;
    const auto started = std::chrono::steady_clock::now();
    const fs::path folder(folder_path);
    if (!start_backend(folder)) return;
    backend_ready = true;
    try {
        for (const auto &[key, path] : list_folder(folder)) {
            if (key.empty()) { report("skipped", path, kInvalid); continue; }
            if (tracks.size() == RECOMP_MUSIC_MAX_TRACKS) {
                report("scan stopped", path, kTooMany);
                break;
            }
            add_track(key, path);
        }
    } catch (const std::bad_alloc &) {
        report("scan stopped", folder, kNoMemory);
    }
    const auto scan_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    std::fprintf(stderr, "[custom-music] catalog tracks=%zu scan_ms=%lld\n",
        tracks.size(), static_cast<long long>(scan_ms));
}

extern "C" void recomp_music_shutdown(void)
{
    tracks.clear();
    total_ms = 0;
    stop_backend();
    backend_ready = false;
}

extern "C" uint32_t recomp_music_count(void) { return static_cast<uint32_t>(tracks.size()); }
extern "C" uint32_t recomp_music_duration(void) { return total_ms; }
extern "C" const RecompMusicTrack *recomp_music_track(uint32_t index)
{
    return index < tracks.size() ? &tracks[index].info : nullptr;
}
extern "C" const wchar_t *recomp_music_path(uint32_t id)
{
    const Track *track = find(id);
    return track ? track->path.c_str() : nullptr;
}
extern "C" RecompMusicDecoder *recomp_music_open(uint32_t id)
{
    const wchar_t *path = recomp_music_path(id);
    if (!path || !backend_ready) return nullptr;
    try {
        Error error = kFailed;
        auto decoder = open_file(path, error, false);
        if (!decoder) report("open failed", path, error);
        return decoder.release();
    } catch (const std::bad_alloc &) {
        report("open failed", path, kNoMemory);
        return nullptr;
    }
}
extern "C" void recomp_music_close(RecompMusicDecoder *decoder) { delete decoder; }

#ifdef _WIN32
extern "C" int recomp_music_read(RecompMusicDecoder *decoder, void *pcm, uint32_t bytes)
{
    if (!decoder || !pcm || bytes == 0 || bytes > RECOMP_MUSIC_MAX_READ ||
        bytes % RECOMP_MUSIC_FRAME_BYTES || decoder->failed) return -1;
    uint32_t written = 0, empty_samples = 0;
    HRESULT error = S_OK;
    while (written < bytes && SUCCEEDED(error)) {
        if (!decoder->pending) {
            if (decoder->eof) break;
            ComPtr<IMFSample> sample;
            DWORD flags = 0;
            error = decoder->reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                0, nullptr, &flags, nullptr, &sample);
            if (SUCCEEDED(error) && (flags & (MF_SOURCE_READERF_ERROR |
                    MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED))) error = E_FAIL;
            if (FAILED(error)) break;
            decoder->eof = (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0;
            if (sample) error = sample->ConvertToContiguousBuffer(&decoder->pending);
            else if (++empty_samples > kMaxEmptySamples) error = E_FAIL;
            decoder->offset = 0;
            continue;
        }
        BYTE *data = nullptr;
        DWORD size = 0;
        error = decoder->pending->Lock(&data, nullptr, &size);
        if (FAILED(error)) break;
        if (size > kMaxSampleBytes || size % RECOMP_MUSIC_FRAME_BYTES) error = E_FAIL;
        else if (size == 0 && ++empty_samples > kMaxEmptySamples) error = E_FAIL;
        else if (decoder->offset < size) {
            const auto count = std::min<uint32_t>(size - decoder->offset, bytes - written);
            std::memcpy(static_cast<uint8_t *>(pcm) + written, data + decoder->offset, count);
            written += count;
            decoder->offset += count;
        }
        decoder->pending->Unlock();
        if (decoder->offset >= size) decoder->pending.Reset();
    }
    if (FAILED(error)) {
        decoder->failed = true;
        return -1;
    }
    return static_cast<int>(written);
}

extern "C" int recomp_music_rewind(RecompMusicDecoder *decoder)
{
    if (!decoder) return 0;
    PROPVARIANT position{};
    position.vt = VT_I8;
    if (FAILED(decoder->reader->SetCurrentPosition(GUID_NULL, position))) return 0;
    decoder->pending.Reset();
    decoder->offset = 0;
    decoder->eof = decoder->failed = false;
    return 1;
}
#else
extern "C" int recomp_music_read(RecompMusicDecoder *decoder, void *pcm, uint32_t bytes)
{
    if (!decoder || !pcm || bytes == 0 || bytes > RECOMP_MUSIC_MAX_READ ||
        bytes % RECOMP_MUSIC_FRAME_BYTES || decoder->failed) return -1;
    // miniaudio reads until the request is full or the song ends.
    ma_uint64 frames = 0;
    const ma_result result = ma_decoder_read_pcm_frames(&decoder->decoder, pcm,
        bytes / RECOMP_MUSIC_FRAME_BYTES, &frames);
    if (result != MA_SUCCESS && result != MA_AT_END) {
        decoder->failed = true;
        return -1;
    }
    return static_cast<int>(frames * RECOMP_MUSIC_FRAME_BYTES);
}

extern "C" int recomp_music_rewind(RecompMusicDecoder *decoder)
{
    if (!decoder || ma_decoder_seek_to_pcm_frame(&decoder->decoder, 0) != MA_SUCCESS) return 0;
    decoder->failed = false;
    return 1;
}
#endif
