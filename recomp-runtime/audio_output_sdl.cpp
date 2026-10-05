#include "audio_output.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr uint32_t kVoiceCount = 256;
constexpr uint32_t kQueueSize = 16;
constexpr uint32_t kMaxBufferBytes = 160000;
constexpr uint32_t kPrerollMs = 50;
constexpr float kRateTrim = 0.01f;

struct OutputVoice {
    SDL_AudioStream *stream;
    uint32_t sample_rate, channels, bits_per_sample;
    uint64_t ends[kQueueSize], bytes_submitted;
    uint32_t front, queued;
    float level_ms;
};

SDL_AudioDeviceID device;
OutputVoice voices[kVoiceCount];
bool reported_nonzero[kVoiceCount];
bool attempted, initialized, summary_printed;
unsigned long long submitted_buffers, submitted_bytes, nonzero_buffers;
unsigned long long dropped_buffers, underruns;

void destroyVoice(OutputVoice &voice)
{
    SDL_DestroyAudioStream(voice.stream);
    voice = {};
}

void releaseOutput()
{
    for (auto &voice : voices) destroyVoice(voice);
    if (device) SDL_CloseAudioDevice(device);
    device = 0;
    if (initialized) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    initialized = false;
}

void disableOutput(const char *operation)
{
    std::fprintf(stderr, "[audio-output] disabled operation=%s error=%s\n",
        operation, SDL_GetError());
    releaseOutput();
}

void dropBuffer(uint32_t slot, const char *reason)
{
    ++dropped_buffers;
    if (dropped_buffers <= 4 || (dropped_buffers & (dropped_buffers - 1)) == 0)
        std::fprintf(stderr, "[audio-output] dropped=%llu slot=%u reason=%s\n",
            dropped_buffers, slot, reason);
}

bool queueBuffer(OutputVoice &voice, uint32_t slot, const uint8_t *pcm, uint32_t bytes)
{
    if (voice.queued == kQueueSize) {
        dropBuffer(slot, "queue-full");
        return false;
    }
    // SDL copies the input and owns it until the device consumes it.
    if (!SDL_PutAudioStreamData(voice.stream, pcm, static_cast<int>(bytes))) {
        dropBuffer(slot, "submit");
        disableOutput("SDL_PutAudioStreamData");
        return false;
    }
    voice.bytes_submitted += bytes;
    voice.ends[(voice.front + voice.queued) % kQueueSize] = voice.bytes_submitted;
    ++voice.queued;
    return true;
}
} // namespace

extern "C" int recomp_audio_output_enabled(void) { return device != 0; }

extern "C" void recomp_audio_output_initialize(void)
{
    if (attempted) return;
    attempted = true;
    double gain = 0.0;
    const char *setting = std::getenv("RECOMP_AUDIO_GAIN");
    if (setting) {
        char *end;
        gain = std::strtod(setting, &end);
        if (end == setting || *end != '\0' || !std::isfinite(gain) || gain < 0.0 || gain > 1.0) {
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
        disableOutput("SDL_InitSubSystem");
        return;
    }
    initialized = true;
    device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    if (!device || !SDL_SetAudioDeviceGain(device, static_cast<float>(gain))) {
        disableOutput("SDL_OpenAudioDevice/SDL_SetAudioDeviceGain");
        return;
    }
    std::fprintf(stderr, "[audio-output] initialized backend=sdl3 master_gain=%.6f\n", gain);
}

extern "C" void recomp_audio_output_shutdown(void)
{
    if (summary_printed) return;
    releaseOutput();
    attempted = summary_printed = true;
    // SDL exposes no equivalent of XAudio2's engine glitch counter.
    std::fprintf(stderr,
        "[audio-output] summary submitted_buffers=%llu submitted_bytes=%llu "
        "nonzero_buffers=%llu dropped_buffers=%llu underruns=%llu\n",
        submitted_buffers, submitted_bytes, nonzero_buffers, dropped_buffers, underruns);
}

extern "C" void recomp_audio_output_reset_voice(uint32_t slot)
{
    if (slot < kVoiceCount) destroyVoice(voices[slot]);
}

extern "C" void recomp_audio_output_submit(uint32_t slot, const uint8_t *pcm,
    uint32_t bytes, uint32_t sample_rate, uint32_t channels, uint32_t bits_per_sample,
    int32_t volume_hundredth_db)
{
    recomp_audio_output_initialize();
    if (!device || bytes == 0) return;
    if (slot >= kVoiceCount || !pcm || bytes > kMaxBufferBytes ||
        sample_rate < 1000 || sample_rate > 200000 ||
        (channels != 1 && channels != 2) || (bits_per_sample != 8 && bits_per_sample != 16)) {
        dropBuffer(slot, "invalid-pcm");
        return;
    }
    const uint32_t block_align = channels * (bits_per_sample / 8);
    if (bytes % block_align) { dropBuffer(slot, "partial-frame"); return; }

    OutputVoice &voice = voices[slot];
    if (voice.stream && (voice.sample_rate != sample_rate || voice.channels != channels ||
            voice.bits_per_sample != bits_per_sample)) destroyVoice(voice);
    if (!voice.stream) {
        SDL_AudioSpec format{bits_per_sample == 8 ? SDL_AUDIO_U8 : SDL_AUDIO_S16LE,
            static_cast<int>(channels), static_cast<int>(sample_rate)};
        voice.stream = SDL_CreateAudioStream(&format, nullptr);
        if (!voice.stream || !SDL_BindAudioStream(device, voice.stream)) {
            disableOutput("SDL_CreateAudioStream/SDL_BindAudioStream");
            return;
        }
        voice.sample_rate = sample_rate;
        voice.channels = channels;
        voice.bits_per_sample = bits_per_sample;
        voice.level_ms = kPrerollMs;
    }
    volume_hundredth_db = std::clamp(volume_hundredth_db, -10000, 0);
    const float gain = volume_hundredth_db == -10000 ? 0.0f :
        std::pow(10.0f, volume_hundredth_db / 2000.0f);
    if (!SDL_SetAudioStreamGain(voice.stream, gain)) {
        disableOutput("SDL_SetAudioStreamGain");
        return;
    }
    const int queued_bytes = SDL_GetAudioStreamQueued(voice.stream);
    if (queued_bytes < 0) { disableOutput("SDL_GetAudioStreamQueued"); return; }
    const bool starved = queued_bytes == 0;
    if (starved && voice.queued) ++underruns;
    const uint64_t consumed = voice.bytes_submitted - static_cast<uint32_t>(queued_bytes);
    while (voice.queued && voice.ends[voice.front] <= consumed) {
        voice.front = (voice.front + 1) % kQueueSize;
        --voice.queued;
    }
    const uint8_t silence = bits_per_sample == 8 ? 0x80 : 0;
    uint32_t preroll_bytes = 0;
    if (starved) {
        uint8_t preroll[200000 * kPrerollMs / 1000 * 4];
        preroll_bytes = sample_rate * kPrerollMs / 1000 * block_align;
        std::memset(preroll, silence, preroll_bytes);
        if (!queueBuffer(voice, slot, preroll, preroll_bytes)) return;
    }
    const float queued_ms = (queued_bytes + preroll_bytes) * 1000.0f /
        (sample_rate * block_align);
    voice.level_ms += (queued_ms - voice.level_ms) / 16.0f;
    const float ratio = std::clamp(1.0f + (voice.level_ms - kPrerollMs) / 10000.0f,
        1.0f - kRateTrim, 1.0f + kRateTrim);
    if (!SDL_SetAudioStreamFrequencyRatio(voice.stream, ratio)) {
        disableOutput("SDL_SetAudioStreamFrequencyRatio");
        return;
    }
    if (!queueBuffer(voice, slot, pcm, bytes)) return;
    ++submitted_buffers;
    submitted_bytes += bytes;
    bool nonzero = false;
    for (uint32_t i = 0; i < bytes && !nonzero; ++i) nonzero = pcm[i] != silence;
    if (nonzero) ++nonzero_buffers;
    if (submitted_buffers == 1 || (nonzero && !reported_nonzero[slot]))
        std::fprintf(stderr,
            "[audio-output] submitted slot=%u bytes=%u rate=%u channels=%u "
            "bits=%u nonzero=%u gain=%.6f\n", slot, bytes, sample_rate, channels,
            bits_per_sample, nonzero ? 1u : 0u, static_cast<double>(gain));
    if (nonzero) reported_nonzero[slot] = true;
}
