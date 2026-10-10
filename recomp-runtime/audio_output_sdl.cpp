// SPDX-License-Identifier: GPL-3.0-or-later
/* SDL3 host audio backend (macOS/Linux). Mirrors audio_output_xaudio2.cpp:
   one SDL_AudioStream per guest voice, a small silence cushion on start or
   starvation, and an inaudible pitch trim that holds that cushion against the
   device's real consumption rate. */
#include "audio_output.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace {

constexpr uint32_t kVoiceCount = 256;
constexpr uint32_t kMaxBufferBytes = 160000;
constexpr uint32_t kMinSampleRate = 1000;
constexpr uint32_t kMaxSampleRate = 200000;
/* Silence queued ahead of a new or starved voice so producer jitter (pump
   wakeups, game-thread hitches) is absorbed instead of heard as gaps. */
constexpr uint32_t kPrerollMs = 50;
/* The producer follows the host monotonic clock; the device follows its own
   crystal. Each voice trims its pitch by at most this much to stay level. */
constexpr float kRateTrim = 0.01f;

struct SdlVoice {
    SDL_AudioStream *stream;
    uint32_t sample_rate, channels, bits_per_sample;
    float gain, ratio, level_ms;
    uint64_t bytes_submitted;
};

std::mutex audio_mutex;
SDL_AudioDeviceID audio_device;
SdlVoice voices[kVoiceCount];
bool reported_nonzero[kVoiceCount];
bool attempted, summary_printed;
std::vector<uint8_t> silence_buffer;

unsigned long long submitted_buffers, submitted_bytes, nonzero_buffers;
unsigned long long dropped_buffers, underruns, voice_resets;

/* RECOMP_AUDIO_DUMP=<file.wav> records the final device mix for diagnosis. */
FILE *dump_file;
uint64_t dump_bytes;
int dump_channels, dump_rate;

void writeWavHeader(FILE *file, uint32_t data_bytes, int channels, int rate)
{
    auto u32 = [file](uint32_t v) { std::fwrite(&v, 4, 1, file); };
    auto u16 = [file](uint16_t v) { std::fwrite(&v, 2, 1, file); };
    std::fseek(file, 0, SEEK_SET);
    std::fwrite("RIFF", 1, 4, file); u32(36 + data_bytes);
    std::fwrite("WAVEfmt ", 1, 8, file); u32(16);
    u16(3); /* IEEE float */
    u16(static_cast<uint16_t>(channels)); u32(static_cast<uint32_t>(rate));
    u32(static_cast<uint32_t>(rate * channels * 4));
    u16(static_cast<uint16_t>(channels * 4)); u16(32);
    std::fwrite("data", 1, 4, file); u32(data_bytes);
}

void SDLCALL dumpPostmix(void *, const SDL_AudioSpec *spec, float *buffer, int buflen)
{
    if (!dump_file || spec->channels != dump_channels || spec->freq != dump_rate) return;
    std::fwrite(buffer, 1, static_cast<size_t>(buflen), dump_file);
    dump_bytes += static_cast<uint64_t>(buflen);
}

void dropBuffer(uint32_t slot, const char *reason)
{
    ++dropped_buffers;
    if (dropped_buffers <= 4 || (dropped_buffers & (dropped_buffers - 1)) == 0) {
        std::fprintf(stderr, "[audio-output] dropped=%llu slot=%u reason=%s\n",
            dropped_buffers, slot, reason);
    }
}

void destroyVoice(SdlVoice &voice)
{
    if (voice.stream) SDL_DestroyAudioStream(voice.stream);
    voice = {};
}

bool putData(SdlVoice &voice, uint32_t slot, const void *data, uint32_t bytes)
{
    if (!SDL_PutAudioStreamData(voice.stream, data, static_cast<int>(bytes))) {
        dropBuffer(slot, "put-data");
        return false;
    }
    voice.bytes_submitted += bytes;
    return true;
}

} // namespace

extern "C" int recomp_audio_output_enabled(void)
{
    return audio_device != 0 ? 1 : 0;
}

extern "C" void recomp_audio_output_initialize(void)
{
    std::lock_guard<std::mutex> lock(audio_mutex);
    if (attempted) return;
    attempted = true;

    double gain = 0.0;
    const char *setting = std::getenv("RECOMP_AUDIO_GAIN");
    if (setting) {
        char *end;
        gain = std::strtod(setting, &end);
        if (end == setting || *end != '\0' || !std::isfinite(gain) ||
            gain < 0.0 || gain > 1.0) {
            std::fprintf(stderr,
                "[audio-output] muted invalid RECOMP_AUDIO_GAIN (expected 0..1)\n");
            return;
        }
    }
    if (gain == 0.0) {
        std::fprintf(stderr, "[audio-output] muted RECOMP_AUDIO_GAIN=0\n");
        return;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "[audio-output] SDL audio init failed: %s\n", SDL_GetError());
        return;
    }
    /* Float output keeps headroom while SDL sums voices; it is CoreAudio's
       native format, so no extra integer conversion stage is added. */
    SDL_AudioSpec want{SDL_AUDIO_F32, 2, 48000};
    audio_device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want);
    if (audio_device == 0) {
        std::fprintf(stderr, "[audio-output] SDL open audio device failed: %s\n", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return;
    }
    SDL_SetAudioDeviceGain(audio_device, static_cast<float>(gain));

    SDL_AudioSpec spec{};
    int frames = 0;
    SDL_GetAudioDeviceFormat(audio_device, &spec, &frames);
    const char *dump_path = std::getenv("RECOMP_AUDIO_DUMP");
    if (dump_path && *dump_path) {
        dump_file = std::fopen(dump_path, "wb");
        if (dump_file) {
            dump_channels = spec.channels;
            dump_rate = spec.freq;
            dump_bytes = 0;
            writeWavHeader(dump_file, 0, dump_channels, dump_rate);
            SDL_SetAudioPostmixCallback(audio_device, dumpPostmix, nullptr);
        }
    }
    SDL_ResumeAudioDevice(audio_device);
    std::fprintf(stderr,
        "[audio-output] initialized backend=sdl3 master_gain=%.6f driver=%s "
        "freq=%d channels=%d format=0x%04x frames=%d\n",
        gain, SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "unknown",
        spec.freq, spec.channels, static_cast<unsigned>(spec.format), frames);
}

extern "C" void recomp_audio_output_shutdown(void)
{
    std::lock_guard<std::mutex> lock(audio_mutex);
    if (summary_printed) return;
    for (auto &voice : voices) destroyVoice(voice);
    if (audio_device != 0) {
        SDL_CloseAudioDevice(audio_device);
        audio_device = 0;
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    if (dump_file) {
        writeWavHeader(dump_file, static_cast<uint32_t>(dump_bytes), dump_channels, dump_rate);
        std::fclose(dump_file);
        dump_file = nullptr;
    }
    attempted = true;
    summary_printed = true;
    std::fprintf(stderr,
        "[audio-output] summary submitted_buffers=%llu submitted_bytes=%llu "
        "nonzero_buffers=%llu dropped_buffers=%llu underruns=%llu voice_resets=%llu\n",
        submitted_buffers, submitted_bytes, nonzero_buffers, dropped_buffers,
        underruns, voice_resets);
}

extern "C" void recomp_audio_output_test_reset(void)
{
    std::lock_guard<std::mutex> lock(audio_mutex);
    attempted = false;
    summary_printed = false;
}

extern "C" void recomp_audio_output_reset_voice(uint32_t slot)
{
    std::lock_guard<std::mutex> lock(audio_mutex);
    if (slot < kVoiceCount && voices[slot].stream) {
        ++voice_resets;
        destroyVoice(voices[slot]);
    }
}

extern "C" void recomp_audio_output_submit(
    uint32_t slot, const uint8_t *pcm, uint32_t bytes,
    uint32_t sample_rate, uint32_t channels, uint32_t bits_per_sample,
    int32_t volume_hundredth_db)
{
    recomp_audio_output_initialize();
    if (audio_device == 0 || bytes == 0) return;
    if (slot >= kVoiceCount || !pcm || bytes > kMaxBufferBytes ||
        sample_rate < kMinSampleRate || sample_rate > kMaxSampleRate ||
        (channels != 1 && channels != 2) ||
        (bits_per_sample != 8 && bits_per_sample != 16)) {
        dropBuffer(slot, "invalid-pcm");
        return;
    }
    const uint32_t block_align = channels * (bits_per_sample / 8);
    if (bytes % block_align != 0) {
        dropBuffer(slot, "partial-frame");
        return;
    }

    std::lock_guard<std::mutex> lock(audio_mutex);
    SdlVoice &voice = voices[slot];
    if (voice.stream && (voice.sample_rate != sample_rate ||
        voice.channels != channels || voice.bits_per_sample != bits_per_sample)) {
        destroyVoice(voice);
    }
    if (!voice.stream) {
        SDL_AudioSpec src{};
        src.format = bits_per_sample == 8 ? SDL_AUDIO_U8 : SDL_AUDIO_S16LE;
        src.channels = static_cast<int>(channels);
        src.freq = static_cast<int>(sample_rate);
        voice.stream = SDL_CreateAudioStream(&src, nullptr);
        if (!voice.stream) {
            dropBuffer(slot, "create-stream");
            return;
        }
        if (!SDL_BindAudioStream(audio_device, voice.stream)) {
            dropBuffer(slot, "bind-stream");
            destroyVoice(voice);
            return;
        }
        voice.sample_rate = sample_rate;
        voice.channels = channels;
        voice.bits_per_sample = bits_per_sample;
        voice.gain = 1.0f;
        voice.ratio = 1.0f;
        voice.level_ms = kPrerollMs;
    }

    if (volume_hundredth_db < -10000) volume_hundredth_db = -10000;
    if (volume_hundredth_db > 0) volume_hundredth_db = 0;
    const float gain = volume_hundredth_db == -10000 ? 0.0f :
        std::pow(10.0f, volume_hundredth_db / 2000.0f);
    if (gain != voice.gain) {
        voice.gain = gain;
        SDL_SetAudioStreamGain(voice.stream, gain);
    }

    const int queued = SDL_GetAudioStreamQueued(voice.stream);
    if (queued > 0 && static_cast<uint32_t>(queued) >= kMaxBufferBytes) {
        dropBuffer(slot, "queue-full");
        return;
    }
    const bool starved = queued <= 0;
    if (starved && voice.bytes_submitted > 0) ++underruns;
    const uint8_t silence = bits_per_sample == 8 ? 0x80 : 0;
    float queued_ms = queued > 0 ?
        static_cast<float>(queued / block_align) * 1000.0f / sample_rate : 0.0f;
    if (starved) {
        const uint32_t preroll = sample_rate * kPrerollMs / 1000 * block_align;
        if (silence_buffer.size() < preroll) silence_buffer.resize(preroll);
        std::memset(silence_buffer.data(), silence, preroll);
        if (!putData(voice, slot, silence_buffer.data(), preroll)) return;
        queued_ms = static_cast<float>(kPrerollMs);
        voice.level_ms = queued_ms;
    }
    voice.level_ms += (queued_ms - voice.level_ms) / 16.0f; /* smooths pump jitter */
    float ratio = 1.0f + (voice.level_ms - kPrerollMs) / 10000.0f;
    if (ratio < 1.0f - kRateTrim) ratio = 1.0f - kRateTrim;
    if (ratio > 1.0f + kRateTrim) ratio = 1.0f + kRateTrim;
    if (std::fabs(ratio - voice.ratio) >= 0.0005f) {
        voice.ratio = ratio;
        SDL_SetAudioStreamFrequencyRatio(voice.stream, ratio);
    }
    if (!putData(voice, slot, pcm, bytes)) return;

    ++submitted_buffers;
    submitted_bytes += bytes;
    bool nonzero = false;
    for (uint32_t i = 0; i < bytes && !nonzero; ++i) nonzero = pcm[i] != silence;
    if (nonzero) ++nonzero_buffers;
    if (submitted_buffers == 1 || (nonzero && !reported_nonzero[slot])) {
        std::fprintf(stderr,
            "[audio-output] submitted slot=%u bytes=%u rate=%u channels=%u "
            "bits=%u nonzero=%u gain=%.6f\n",
            slot, bytes, sample_rate, channels, bits_per_sample,
            nonzero ? 1u : 0u, static_cast<double>(gain));
    }
    if (nonzero) reported_nonzero[slot] = true;
}
