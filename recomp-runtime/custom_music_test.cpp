#include "custom_music.h"
#include "soundtrack_adapter.h"
extern "C" {
#include "kernel_abi.h"
#include "program_manual.h"
void recomp_test_heap_reset(uint32_t cursor, int fail_after);
}

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#endif

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace {
unsigned originals;
}

/* The adapter wraps or falls through to these generated bodies; the unit test
   has no generated program, so they only retire the call. */
extern "C" void sub_0018145F(void) { ++originals; kernel_return(1u, 0u); }
extern "C" void sub_00188210(void) { kernel_return_caller_cleanup(0u); }
extern "C" void sub_001880D0(void) { kernel_return_caller_cleanup(0u); }
extern "C" void sub_0018D270(void) { ++originals; kernel_return_caller_cleanup(0u); }

#ifdef _WIN32
namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;
namespace {
unsigned service_steps;
std::vector<uint32_t> guest_errors;
constexpr uint32_t kSecondBytes = RECOMP_MUSIC_RATE * RECOMP_MUSIC_FRAME_BYTES;
constexpr uint32_t kPacket = 8192;
using Bytes = std::vector<uint8_t>;
using Decoder = std::unique_ptr<RecompMusicDecoder, decltype(&recomp_music_close)>;

void service_step()
{
    ++service_steps;
    recomp_runtime.registers.ebx = 0xdeadbeef;
    recomp_runtime.fpu_stack[0] = 123;
    kernel_return_caller_cleanup(0);
}
void set_last_error()
{
    guest_errors.push_back(kernel_arg(1u));
    kernel_return(1u, 0u);
}
RecompFunction lookup_service(uint32_t address)
{
    if (address == 0x18cdd0) return service_step;
    if (address == 0x183183) return set_last_error;
    return recomp_lookup_manual(address);
}
void check(bool value, const char *message)
{
    if (!value) throw std::runtime_error(message);
}
bool last_error(uint32_t expected) { return !guest_errors.empty() && guest_errors.back() == expected; }

// Closes every decoder, stops Media Foundation, and removes the fixtures,
// including when a check throws.
struct TempRoot {
    fs::path path = fs::temp_directory_path() / ("doaxbv-music-test-" +
        std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    ~TempRoot()
    {
        recomp_soundtrack_shutdown();
        recomp_music_shutdown();
        std::error_code error;
        fs::remove_all(path, error);
    }
};

struct Guest {
    std::vector<uint8_t> ram = std::vector<uint8_t>(RECOMP_XBOX_RAM_SIZE);
    explicit Guest(int heap_fail_after = -1)
    {
        RecompMemoryRegion region{0, ram.size(), ram.data()};
        recomp_runtime_init(&region, 1, nullptr, 0, nullptr, 0);
        recomp_runtime_set_lookup(lookup_service);
        recomp_test_heap_reset(0x100000, heap_fail_after);
        guest_errors.clear();
        originals = 0;
    }
    ~Guest()
    {
        recomp_soundtrack_shutdown();
        recomp_runtime_init(nullptr, 0, nullptr, 0, nullptr, 0);
    }
};

// One second of a quiet sine as 16-bit stereo PCM.
Bytes tone(double hz)
{
    Bytes pcm(kSecondBytes);
    for (uint32_t i = 0; i < RECOMP_MUSIC_RATE; ++i) {
        const auto sample = static_cast<int16_t>(std::sin(i * hz * 6.283185307179586 /
            RECOMP_MUSIC_RATE) * 8000);
        std::memcpy(pcm.data() + i * 4, &sample, 2);
        std::memcpy(pcm.data() + i * 4 + 2, &sample, 2);
    }
    return pcm;
}

// A canonical RIFF file: 16-bit PCM, or the same tone as 32-bit float.
Bytes wave(const Bytes &pcm, bool floating)
{
    const uint32_t bits = floating ? 32 : 16, size = static_cast<uint32_t>(pcm.size()) * bits / 16;
    Bytes bytes(44 + size);
    auto word = [&](size_t at, uint16_t value) { std::memcpy(bytes.data() + at, &value, 2); };
    auto dword = [&](size_t at, uint32_t value) { std::memcpy(bytes.data() + at, &value, 4); };
    std::memcpy(bytes.data(), "RIFF", 4); dword(4, 36 + size);
    std::memcpy(bytes.data() + 8, "WAVEfmt ", 8); dword(16, 16);
    word(20, floating ? 3 : 1); word(22, 2); dword(24, RECOMP_MUSIC_RATE);
    dword(28, RECOMP_MUSIC_RATE * bits / 4); word(32, static_cast<uint16_t>(bits / 4));
    word(34, static_cast<uint16_t>(bits)); std::memcpy(bytes.data() + 36, "data", 4); dword(40, size);
    for (size_t i = 0; i < pcm.size() / 2; ++i) {
        int16_t sample;
        std::memcpy(&sample, pcm.data() + i * 2, 2);
        if (!floating) std::memcpy(bytes.data() + 44 + i * 2, &sample, 2);
        else { const float value = sample / 32768.0f; std::memcpy(bytes.data() + 44 + i * 4, &value, 4); }
    }
    return bytes;
}

void write(const fs::path &path, const Bytes &bytes)
{
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    check(static_cast<bool>(stream), "write synthetic fixture");
}

// One second of 48 kHz mono 16-bit PCM, which the reader must resample and
// widen to 44.1 kHz stereo.
Bytes wave_48k_mono()
{
    constexpr uint32_t rate = 48000;
    Bytes bytes(44 + rate * 2);
    auto word = [&](size_t at, uint16_t value) { std::memcpy(bytes.data() + at, &value, 2); };
    auto dword = [&](size_t at, uint32_t value) { std::memcpy(bytes.data() + at, &value, 4); };
    std::memcpy(bytes.data(), "RIFF", 4); dword(4, 36 + rate * 2);
    std::memcpy(bytes.data() + 8, "WAVEfmt ", 8); dword(16, 16);
    word(20, 1); word(22, 1); dword(24, rate); dword(28, rate * 2); word(32, 2); word(34, 16);
    std::memcpy(bytes.data() + 36, "data", 4); dword(40, rate * 2);
    for (uint32_t i = 0; i < rate; ++i) {
        const auto sample = static_cast<int16_t>(std::sin(i * 660.0 * 6.283185307179586 / rate) * 8000);
        std::memcpy(bytes.data() + 44 + i * 2, &sample, 2);
    }
    return bytes;
}

// Encodes pcm with a Windows encoder. False when this Windows has none for
// the format; the caller then skips that format and says so.
bool encode(const fs::path &path, const GUID &subtype, const Bytes &pcm)
{
    ComPtr<IMFSinkWriter> writer;
    ComPtr<IMFMediaType> out, in;
    ComPtr<IMFMediaBuffer> buffer;
    ComPtr<IMFSample> sample;
    DWORD stream = 0;
    BYTE *data = nullptr;
    HRESULT hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, nullptr, &writer);
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&out);
    if (SUCCEEDED(hr)) hr = out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(hr)) hr = out->SetGUID(MF_MT_SUBTYPE, subtype);
    if (SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, RECOMP_MUSIC_RATE);
    if (SUCCEEDED(hr)) hr = out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    if (SUCCEEDED(hr) && subtype == MFAudioFormat_MP3)
        hr = out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 128000 / 8);
    if (SUCCEEDED(hr) && subtype == MFAudioFormat_FLAC)
        hr = out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (SUCCEEDED(hr)) hr = writer->AddStream(out.Get(), &stream);
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&in);
    if (SUCCEEDED(hr)) hr = in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(hr)) hr = in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    if (SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, RECOMP_MUSIC_RATE);
    if (SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    if (SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, RECOMP_MUSIC_FRAME_BYTES);
    if (SUCCEEDED(hr)) hr = in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kSecondBytes);
    if (SUCCEEDED(hr)) hr = writer->SetInputMediaType(stream, in.Get(), nullptr);
    if (SUCCEEDED(hr)) hr = writer->BeginWriting();
    if (SUCCEEDED(hr)) hr = MFCreateMemoryBuffer(static_cast<DWORD>(pcm.size()), &buffer);
    if (SUCCEEDED(hr)) hr = buffer->Lock(&data, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
        std::memcpy(data, pcm.data(), pcm.size());
        buffer->Unlock();
        hr = buffer->SetCurrentLength(static_cast<DWORD>(pcm.size()));
    }
    if (SUCCEEDED(hr)) hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer.Get());
    if (SUCCEEDED(hr)) hr = sample->SetSampleTime(0);
    if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(10000000);
    if (SUCCEEDED(hr)) hr = writer->WriteSample(stream, sample.Get());
    if (SUCCEEDED(hr)) hr = writer->Finalize();
    writer.Reset();
    if (FAILED(hr)) {
        std::error_code error;
        fs::remove(path, error);
        std::printf("custom music: no %ls encoder (0x%08lx); format skipped\n",
            path.extension().c_str(), static_cast<unsigned long>(hr));
    }
    return SUCCEEDED(hr);
}

uint32_t call(uint32_t address, std::initializer_list<uint32_t> args, bool caller_cleanup = false)
{
    constexpr uint32_t stack = 0x10000;
    recomp_runtime.registers.esp = stack;
    uint32_t at = stack + 4;
    for (auto value : args) { *recomp_memory_u32(at) = value; at += 4; }
    auto function = recomp_lookup_manual(address);
    check(function && function == recomp_soundtrack_lookup_manual(address), "manual dispatch");
    function();
    check(recomp_runtime.registers.esp == (caller_cleanup ? stack + 4 : at), "guest stack cleanup");
    return recomp_runtime.registers.eax;
}

// Reads the whole song, starting from the decoder's current position.
Bytes read_all(RecompMusicDecoder *decoder)
{
    Bytes pcm, packet(kPacket);
    int count;
    do {
        count = recomp_music_read(decoder, packet.data(), kPacket);
        check(count >= 0, "song decodes");
        pcm.insert(pcm.end(), packet.begin(), packet.begin() + count);
    } while (count == static_cast<int>(kPacket));
    return pcm;
}

// Plays the whole song through guest Process calls, as CRI does.
Bytes play_all(uint32_t object)
{
    uint32_t packet[] = {0x20000, kPacket, 0x5600, 0x5604, 0, 0};
    std::memcpy(recomp_memory(0x5700, sizeof packet), packet, sizeof packet);
    Bytes pcm;
    uint32_t size = kPacket;
    for (uint32_t n = 0; n < 1000 && size == kPacket; ++n) {
        check(call(0x2194b0, {object, 0, 0x5700}) == 0 && *recomp_memory_u32(0x5604) == 0,
            "packet completion");
        size = *recomp_memory_u32(0x5600);
        check(size <= kPacket && size % RECOMP_MUSIC_FRAME_BYTES == 0, "complete PCM frames");
        const uint8_t *data = recomp_memory(0x20000, size);
        pcm.insert(pcm.end(), data, data + size);
    }
    check(size < kPacket, "end of song");
    return pcm;
}

void check_enumeration()
{
    {
        Guest guest;
        const auto handle = call(0x182430, {0x5000});
        check(handle != UINT32_MAX && *recomp_memory_u32(0x5004) == recomp_music_count() &&
            *recomp_memory_u32(0x5008) == recomp_music_duration(), "enumeration");
        check(call(0x182430, {0x5080}) == handle, "enumerations share one token");
        check(call(0x182430, {0}) == UINT32_MAX && last_error(87), "null soundtrack record");
        check(call(0x182411, {handle, 0x5000}) == 0 && last_error(18), "one soundtrack");
        check(call(0x182411, {0x1234, 0x5000}) == 0 && last_error(6), "foreign enumerator");
        check(call(0x18145f, {handle}) == 1 && call(0x18145f, {handle}) == 1 && originals == 0,
            "both enumerations close");
        call(0x18145f, {handle});
        check(originals == 1, "closed token falls through to XFindClose");
        check(call(0x1824df, {1, recomp_music_count(), 0x5100, 0x5104, 0, 0}) == 0 &&
            last_error(87), "invalid song index");
        check(call(0x182708, {0, 1}) == UINT32_MAX && last_error(2), "unknown song");
        call(0x18d270, {0x9000}, true);
        check(originals == 2, "unbound context stop falls through");
    }
    Guest guest(0);
    check(call(0x182430, {0x5000}) == UINT32_MAX && last_error(8), "enumerator allocation failure");
}

void check_track(uint32_t index, const Bytes *exact_pcm)
{
    const RecompMusicTrack track = *recomp_music_track(index);
    const fs::path path = recomp_music_path(track.id);
    // MP3 frames pad the end and resampling filters shift it; both are near 1 s.
    const bool approximate = path.extension() == ".mp3" || path.stem() == "d-48k-mono";
    Guest guest;
    check(call(0x1824df, {1, index, 0x5100, 0x5104, 0x5200, 33}) == 1 &&
        *recomp_memory_u32(0x5100) == track.id &&
        *recomp_memory_u32(0x5104) == track.duration_ms, "song info");
    check(path.stem().wstring() == reinterpret_cast<const wchar_t *>(recomp_memory(0x5200, 64)),
        "song name is the filename stem");
    const auto file = call(0x182708, {track.id, 1});
    check(file != UINT32_MAX, "selected original file opened");
    check(recomp_kernel_close_file(file, 0) == 0, "original file released");

    Decoder reference(recomp_music_open(track.id), recomp_music_close);
    check(reference != nullptr, "reference decoder");
    const Bytes song = read_all(reference.get());
    check(recomp_music_rewind(reference.get()) && read_all(reference.get()) == song,
        "rewind replays the whole song");
    Bytes scratch(RECOMP_MUSIC_MAX_READ + 4);
    check(recomp_music_read(reference.get(), scratch.data(), 3) == -1 &&
        recomp_music_read(reference.get(), scratch.data(), RECOMP_MUSIC_MAX_READ + 4) == -1,
        "invalid read sizes rejected");
    if (approximate) {
        // ponytail: allow a tenth of a second of padding rather than modelling each codec.
        check(std::abs(static_cast<int>(track.duration_ms) - 1000) <= 100 &&
            song.size() >= kSecondBytes && song.size() <= kSecondBytes * 11 / 10,
            "about one second of converted PCM");
    } else {
        check(track.duration_ms == 1000 && song.size() == kSecondBytes,
            "exactly one second of lossless PCM");
    }
    if (exact_pcm) check(song == *exact_pcm, "lossless decode is bit exact");

    *recomp_memory_u32(0x1004) = 0x2000;
    *recomp_memory_u32(0x2004) = 0x3000;
    *recomp_memory_u32(0x30bc) = 0x4000;
    call(0x188210, {0x1000, track.id}, true);
    *recomp_memory_u32(0x4004) = 1;
    *recomp_memory_u32(0x4010) = 0;
    service_steps = 0;
    recomp_runtime.registers.ebx = 7;
    recomp_runtime.fpu_stack[0] = 8;
    call(0x1880d0, {0x3000}, true);
    check(service_steps == 1 && recomp_runtime.registers.ebx == 7 &&
        recomp_runtime.fpu_stack[0] == 8, "cooperative decode preserves caller state");
    call(0x18d270, {0x4000}, true);
    call(0x1880d0, {0x3000}, true);
    check(service_steps == 1 && originals == 0 && *recomp_memory_u32(0x4004) == 0 &&
        *recomp_memory_u32(0x4010) == 1, "stop prevents further decode steps");
    *recomp_memory_u32(0x4004) = 4;
    call(0x1880d0, {0x3000}, true);
    check(service_steps == 1 && *recomp_memory_u32(0x4004) == 3,
        "failed track completes instead of stranding the music channel");

    check(call(0x21972a, {0x18c680, 0x4000, 0, 0x5300, 0x5400}) == 0, "selected decoder creation");
    const auto object = *recomp_memory_u32(0x5400);
    check(*recomp_memory_u16(0x5300) == 1 && *recomp_memory_u16(0x5302) == 2 &&
        *recomp_memory_u32(0x5304) == RECOMP_MUSIC_RATE && *recomp_memory_u16(0x530c) == 4 &&
        *recomp_memory_u16(0x530e) == 16, "PCM format");
    check(call(0x21945d, {object}) == 2 && call(0x21965a, {object}) == 1, "reference lifetime");
    check(call(0x21924c, {object, 0x5500}) == 0 && *recomp_memory_u32(0x5508) == 4, "packet alignment");
    check(play_all(object) == song, "packets carry the selected song");
    check(call(0x218f16, {object}) == 0 && play_all(object) == song, "flush replays the song");
    uint32_t oversized[] = {0x20000, RECOMP_MUSIC_MAX_READ + 4, 0x5600, 0x5604, 0, 0};
    std::memcpy(recomp_memory(0x5700, sizeof oversized), oversized, sizeof oversized);
    check(call(0x2194b0, {object, 0, 0x5700}) != 0, "oversized packet rejected");
    check(call(0x21926c, {object}) == 0 && call(0x21965a, {object}) == 0, "discontinue and release");
    uint32_t packet[] = {0x20000, kPacket, 0x5600, 0x5604, 0, 0};
    std::memcpy(recomp_memory(0x5700, sizeof packet), packet, sizeof packet);
    check(call(0x2194b0, {object, 0, 0x5700}) != 0 && *recomp_memory_u32(0x5600) == 0,
        "released decoder cannot produce packets");
    std::printf("custom music: %ls duration_ms=%u bytes=%zu\n", path.filename().c_str(),
        track.duration_ms, song.size());
}
} // namespace

extern "C" int recomp_custom_music_test(void)
{
    if (!LoadLibraryW(L"mfplat.dll") || !LoadLibraryW(L"mfreadwrite.dll") ||
            FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        std::printf("custom music: Media Foundation unavailable; skipped\n");
        return 77;
    }
    if (FAILED(MFStartup(MF_VERSION))) {
        std::printf("custom music: Media Foundation unavailable; skipped\n");
        CoUninitialize();
        return 77;
    }
    int result = EXIT_FAILURE;
    try {
        TempRoot root;
        const auto folder = root.path / "UserMusic";
        check(fs::create_directories(folder), "new test directory");
        const auto folder_name = folder.string();
        const Bytes pcm = tone(440), pcm_wave = wave(pcm, false);
        write(folder / L"z-\u97f3.wav", pcm_wave);
        write(folder / "broken.mp3", {1, 2, 3});
        recomp_music_initialize(folder_name.c_str());
        check(recomp_music_count() == 1, "malformed file skipped");
        const uint32_t stable_id = recomp_music_track(0)->id;
        recomp_music_shutdown();

        write(folder / "a-float.WAV", wave(pcm, true));
        write(folder / "d-48k-mono.wav", wave_48k_mono());
        const bool flac = encode(folder / "b-flac.flac", MFAudioFormat_FLAC, pcm);
        const bool mp3 = encode(folder / "c-mp3.mp3", MFAudioFormat_MP3, tone(880));
        recomp_music_initialize(folder_name.c_str());
        const uint32_t count = 3u + flac + mp3;
        check(recomp_music_count() == count && recomp_music_track(count - 1)->id == stable_id,
            "sorted by folded name with stable IDs after insertion");
        uint32_t total = 0;
        for (uint32_t i = 0; i < count; ++i) total += recomp_music_track(i)->duration_ms;
        check(recomp_music_duration() == total, "soundtrack duration");
        check_enumeration();
        for (uint32_t index = 0; index < count; ++index) {
            const fs::path name = recomp_music_path(recomp_music_track(index)->id);
            const bool exact = name.extension() == ".flac" || name.filename() == L"z-\u97f3.wav";
            check_track(index, exact ? &pcm : nullptr);
        }
        recomp_music_shutdown();

        std::ifstream original(folder / L"z-\u97f3.wav", std::ios::binary);
        const Bytes after((std::istreambuf_iterator<char>(original)), {});
        original.close();
        check(after == pcm_wave, "original file unchanged");
        for (const auto &entry : fs::directory_iterator(folder)) fs::remove(entry.path());
        recomp_music_initialize(folder_name.c_str());
        check(recomp_music_count() == 0 && !recomp_music_open(stable_id), "removed and empty library");
        Guest guest;
        check(call(0x182430, {0x5000}) == UINT32_MAX && last_error(18), "empty library has no soundtrack");
        result = EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::fprintf(stderr, "custom music test: %s\n", error.what());
    }
    MFShutdown();
    CoUninitialize();
    return result;
}
#else
extern "C" int recomp_custom_music_test(void)
{
    std::printf("custom music: Media Foundation unavailable on non-Windows; skipped\n");
    return 77;
}
#endif
