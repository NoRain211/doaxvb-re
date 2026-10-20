// SPDX-License-Identifier: GPL-3.0-or-later
/* libnx audout host audio backend (Nintendo Switch). Mirrors
   audio_output_sdl.cpp: one software voice per guest voice, a small silence
   cushion on start or starvation, and an inaudible pitch trim that holds that
   cushion against the device's real consumption rate. audout plays one
   stereo s16 stream, so this file converts, resamples, scales and mixes the
   voices itself on a dedicated mixer thread. */
#include "audio_output.h"

#include <switch.h>

#include <atomic>
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

constexpr uint32_t kDeviceRate = 48000;
constexpr uint32_t kDeviceChannels = 2;
/* 1024 stereo s16 frames is exactly one 0x1000 page, audout's alignment. */
constexpr uint32_t kMixFrames = 1024;
constexpr size_t kMixBytes = kMixFrames * kDeviceChannels * sizeof(int16_t);
constexpr uint32_t kDeviceBuffers = 3;
/* Above the 0x2C main thread: Horizon does not time-slice equal priorities,
   and the mixer spends nearly all its time blocked in audout. */
constexpr int kMixerPriority = 0x2B;
constexpr u64 kWaitTimeoutNs = 100000000ULL;

struct Voice {
    bool active;
    uint32_t sample_rate, channels, bits_per_sample;
    float gain, ratio, level_ms;
    uint64_t bytes_submitted;
    std::vector<float> samples; /* interleaved at the voice's channel count */
    double position;            /* fractional read frame within samples */
};

std::mutex audio_mutex;
bool audio_device;
float master_gain;
Voice voices[kVoiceCount];
bool reported_nonzero[kVoiceCount];
bool attempted, summary_printed;

Thread mixer_thread;
std::atomic<bool> mixer_running;
AudioOutBuffer device_buffers[kDeviceBuffers];

unsigned long long submitted_buffers, submitted_bytes, nonzero_buffers;
unsigned long long dropped_buffers, underruns, voice_resets;

/* shortcut: RECOMP_AUDIO_DUMP (WAV capture of the mix) is SDL-only; port it
   here if a Switch mix needs offline diagnosis. */

void dropBuffer(uint32_t slot, const char *reason)
{
    ++dropped_buffers;
    if (dropped_buffers <= 4 || (dropped_buffers & (dropped_buffers - 1)) == 0) {
        std::fprintf(stderr, "[audio-output] dropped=%llu slot=%u reason=%s\n",
            dropped_buffers, slot, reason);
    }
}

void destroyVoice(Voice &voice)
{
    voice = {};
}

float volumeGain(int32_t volume_hundredth_db)
{
    if (volume_hundredth_db < -10000) volume_hundredth_db = -10000;
    if (volume_hundredth_db > 0) volume_hundredth_db = 0;
    return volume_hundredth_db == -10000 ? 0.0f :
        std::pow(10.0f, volume_hundredth_db / 2000.0f);
}

/* Converts u8 or s16le PCM to float and appends it to the voice queue. */
void appendPcm(Voice &voice, const uint8_t *pcm, uint32_t bytes)
{
    std::vector<float> &out = voice.samples;
    if (voice.bits_per_sample == 8) {
        for (uint32_t i = 0; i < bytes; ++i) out.push_back((pcm[i] - 128) / 128.0f);
    } else {
        for (uint32_t i = 0; i + 1 < bytes; i += 2) {
            int16_t value;
            std::memcpy(&value, pcm + i, sizeof(value)); /* guest PCM may be unaligned */
            out.push_back(value / 32768.0f);
        }
    }
    voice.bytes_submitted += bytes;
}

void appendSilence(Voice &voice, uint32_t frames)
{
    voice.samples.resize(voice.samples.size() + size_t{frames} * voice.channels, 0.0f);
    voice.bytes_submitted += uint64_t{frames} * voice.channels * (voice.bits_per_sample / 8);
}

/* Queued data in the voice's input format, like SDL_GetAudioStreamQueued. */
uint32_t queuedBytes(const Voice &voice)
{
    const size_t frames = voice.samples.size() / voice.channels;
    const size_t read = static_cast<size_t>(voice.position);
    return read >= frames ? 0 :
        static_cast<uint32_t>((frames - read) * voice.channels * (voice.bits_per_sample / 8));
}

/* Linear-interpolation resample of one voice, added into a stereo float mix. */
void mixVoice(Voice &voice, float *mix, uint32_t out_frames)
{
    const uint32_t channels = voice.channels;
    const float *in = voice.samples.data();
    const size_t frames = voice.samples.size() / channels;
    const double step = static_cast<double>(voice.sample_rate) * voice.ratio / kDeviceRate;
    double position = voice.position;
    for (uint32_t f = 0; f < out_frames; ++f) {
        const size_t index = static_cast<size_t>(position);
        if (index >= frames) break;
        const size_t next = index + 1 < frames ? index + 1 : index; /* hold the last frame */
        const float frac = static_cast<float>(position - index);
        for (uint32_t c = 0; c < kDeviceChannels; ++c) {
            const uint32_t source = channels == 1 ? 0 : c;
            const float a = in[index * channels + source];
            const float b = in[next * channels + source];
            mix[f * kDeviceChannels + c] += (a + (b - a) * frac) * voice.gain;
        }
        position += step;
    }
    const size_t consumed = static_cast<size_t>(position) < frames ?
        static_cast<size_t>(position) : frames;
    voice.samples.erase(voice.samples.begin(),
        voice.samples.begin() + static_cast<std::ptrdiff_t>(consumed * channels));
    voice.position = consumed == frames ? 0.0 : position - consumed;
}

void renderMix(int16_t *out)
{
    static float mix[kMixFrames * kDeviceChannels]; /* mixer thread only */
    std::memset(mix, 0, sizeof(mix));
    {
        std::lock_guard<std::mutex> lock(audio_mutex);
        for (auto &voice : voices) {
            if (voice.active && !voice.samples.empty()) mixVoice(voice, mix, kMixFrames);
        }
    }
    for (uint32_t i = 0; i < kMixFrames * kDeviceChannels; ++i) {
        float value = mix[i] * master_gain * 32768.0f;
        if (value > 32767.0f) value = 32767.0f;
        if (value < -32768.0f) value = -32768.0f;
        out[i] = static_cast<int16_t>(std::lrint(value));
    }
}

void appendDeviceBuffer(AudioOutBuffer *buffer)
{
    renderMix(static_cast<int16_t *>(buffer->buffer));
    audoutAppendAudioOutBuffer(buffer);
}

void mixerMain(void *)
{
    for (auto &buffer : device_buffers) appendDeviceBuffer(&buffer);
    while (mixer_running.load()) {
        AudioOutBuffer *released = nullptr;
        u32 count = 0;
        if (R_FAILED(audoutWaitPlayFinish(&released, &count, kWaitTimeoutNs))) continue;
        /* The wait reports only the first released buffer; drain the rest. */
        while (count > 0 && released) {
            appendDeviceBuffer(released);
            released = nullptr;
            count = 0;
            audoutGetReleasedAudioOutBuffer(&released, &count);
        }
    }
}

void freeDeviceBuffers()
{
    for (auto &buffer : device_buffers) {
        std::free(buffer.buffer);
        buffer = {};
    }
}

} // namespace

extern "C" int recomp_audio_output_enabled(void)
{
    return audio_device ? 1 : 0;
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
    Result rc = audoutInitialize();
    if (R_FAILED(rc)) {
        std::fprintf(stderr, "[audio-output] audout init failed: 0x%x\n", static_cast<unsigned>(rc));
        return;
    }
    const u32 rate = audoutGetSampleRate();
    const u32 channels = audoutGetChannelCount();
    if (rate != kDeviceRate || channels != kDeviceChannels) {
        std::fprintf(stderr, "[audio-output] unsupported audout format freq=%u channels=%u\n",
            static_cast<unsigned>(rate), static_cast<unsigned>(channels));
        audoutExit();
        return;
    }
    for (auto &buffer : device_buffers) {
        buffer = {};
        buffer.buffer = std::aligned_alloc(0x1000, kMixBytes);
        buffer.buffer_size = kMixBytes;
        buffer.data_size = kMixBytes;
        if (!buffer.buffer) rc = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }
    if (R_SUCCEEDED(rc)) rc = audoutStartAudioOut();
    if (R_SUCCEEDED(rc)) {
        master_gain = static_cast<float>(gain);
        mixer_running = true;
        rc = threadCreate(&mixer_thread, mixerMain, nullptr, nullptr, 0x10000, kMixerPriority, -2);
        if (R_SUCCEEDED(rc)) {
            rc = threadStart(&mixer_thread);
            if (R_FAILED(rc)) threadClose(&mixer_thread);
        }
        if (R_FAILED(rc)) {
            mixer_running = false;
            audoutStopAudioOut();
        }
    }
    if (R_FAILED(rc)) {
        std::fprintf(stderr, "[audio-output] audout start failed: 0x%x\n", static_cast<unsigned>(rc));
        freeDeviceBuffers();
        audoutExit();
        return;
    }
    audio_device = true;
    std::fprintf(stderr,
        "[audio-output] initialized backend=libnx-audout master_gain=%.6f "
        "freq=%u channels=%u format=s16 frames=%u buffers=%u\n",
        gain, static_cast<unsigned>(rate), static_cast<unsigned>(channels),
        kMixFrames, kDeviceBuffers);
}

extern "C" void recomp_audio_output_shutdown(void)
{
    /* Join outside the lock: the mixer takes it for every buffer. */
    if (audio_device && mixer_running.exchange(false)) {
        threadWaitForExit(&mixer_thread);
        threadClose(&mixer_thread);
    }
    std::lock_guard<std::mutex> lock(audio_mutex);
    if (summary_printed) return;
    for (auto &voice : voices) destroyVoice(voice);
    if (audio_device) {
        audoutStopAudioOut();
        audoutExit();
        freeDeviceBuffers();
        audio_device = false;
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
    if (slot < kVoiceCount && voices[slot].active) {
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
    if (!audio_device || bytes == 0) return;
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
    Voice &voice = voices[slot];
    if (voice.active && (voice.sample_rate != sample_rate ||
        voice.channels != channels || voice.bits_per_sample != bits_per_sample)) {
        destroyVoice(voice);
    }
    if (!voice.active) {
        voice.active = true;
        voice.sample_rate = sample_rate;
        voice.channels = channels;
        voice.bits_per_sample = bits_per_sample;
        voice.gain = 1.0f;
        voice.ratio = 1.0f;
        voice.level_ms = kPrerollMs;
    }

    const float gain = volumeGain(volume_hundredth_db);
    voice.gain = gain;

    const uint32_t queued = queuedBytes(voice);
    if (queued >= kMaxBufferBytes) {
        dropBuffer(slot, "queue-full");
        return;
    }
    const bool starved = queued == 0;
    if (starved && voice.bytes_submitted > 0) ++underruns;
    const uint8_t silence = bits_per_sample == 8 ? 0x80 : 0;
    float queued_ms = static_cast<float>(queued / block_align) * 1000.0f / sample_rate;
    if (starved) {
        appendSilence(voice, sample_rate * kPrerollMs / 1000);
        queued_ms = static_cast<float>(kPrerollMs);
        voice.level_ms = queued_ms;
    }
    voice.level_ms += (queued_ms - voice.level_ms) / 16.0f; /* smooths pump jitter */
    float ratio = 1.0f + (voice.level_ms - kPrerollMs) / 10000.0f;
    if (ratio < 1.0f - kRateTrim) ratio = 1.0f - kRateTrim;
    if (ratio > 1.0f + kRateTrim) ratio = 1.0f + kRateTrim;
    if (std::fabs(ratio - voice.ratio) >= 0.0005f) voice.ratio = ratio;
    appendPcm(voice, pcm, bytes);

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
