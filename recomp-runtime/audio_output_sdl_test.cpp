#include "audio_output_sdl.cpp"

#include <vector>

static void check(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL audio output: %s (%s)\n", message, SDL_GetError());
        std::abort();
    }
}

static void setGain(const char *gain)
{
#ifdef _WIN32
    _putenv_s("RECOMP_AUDIO_GAIN", gain);
#else
    setenv("RECOMP_AUDIO_GAIN", gain, 1);
#endif
}

static void restart(const char *gain)
{
    recomp_audio_output_shutdown();
    attempted = summary_printed = false;
    setGain(gain);
}

int main()
{
    // Exercise the real mixer without requiring an audio device in CI/WSL.
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    for (const char *gain : {"0", "", "nan", "-1", "1.1", "0.5junk"}) {
        restart(gain);
        recomp_audio_output_initialize();
        check(!recomp_audio_output_enabled(), "invalid/zero gain stays muted");
        setGain("0.5");
        recomp_audio_output_initialize();
        check(!recomp_audio_output_enabled(), "initialization is attempted once");
    }
    restart("0.5");
    recomp_audio_output_initialize();
    check(recomp_audio_output_enabled(), "dummy output opens");
    check(SDL_GetAudioDeviceGain(device) == 0.5f, "master gain");
    check(SDL_PauseAudioDevice(device), "pause consumption for queue assertions");

    uint8_t pcm[] = {1, 0, 2, 0, 3, 0, 4, 0};
    const auto before = dropped_buffers;
    recomp_audio_output_submit(256, pcm, sizeof pcm, 44100, 1, 16, 0);
    recomp_audio_output_submit(0, nullptr, sizeof pcm, 44100, 1, 16, 0);
    recomp_audio_output_submit(0, pcm, 3, 44100, 1, 16, 0);
    recomp_audio_output_submit(0, pcm, sizeof pcm, 999, 1, 16, 0);
    recomp_audio_output_submit(0, pcm, sizeof pcm, 200001, 1, 16, 0);
    recomp_audio_output_submit(0, pcm, sizeof pcm, 44100, 3, 16, 0);
    recomp_audio_output_submit(0, pcm, sizeof pcm, 44100, 1, 24, 0);
    recomp_audio_output_submit(0, pcm, kMaxBufferBytes + 1, 44100, 1, 16, 0);
    check(dropped_buffers == before + 8 && !voices[0].stream, "bad PCM rejected");

    recomp_audio_output_submit(0, pcm, sizeof pcm, 1000, 1, 16, 0);
    auto &voice = voices[0];
    check(voice.queued == 2 && SDL_GetAudioStreamQueued(voice.stream) == 108,
        "50ms preroll precedes copied PCM");
    SDL_UnbindAudioStream(voice.stream);
    SDL_AudioSpec format{SDL_AUDIO_S16LE, 1, 1000};
    check(SDL_SetAudioStreamFormat(voice.stream, &format, &format), "inspect source PCM");
    check(SDL_FlushAudioStream(voice.stream), "flush conversion tail");
    pcm[0] = 99;
    uint8_t copied[108];
    check(SDL_GetAudioStreamData(voice.stream, copied, sizeof copied) == sizeof copied,
        "read copied PCM");
    check(std::all_of(copied, copied + 100, [](uint8_t b) { return b == 0; }) &&
        copied[100] == 1 && copied[106] == 4, "silence and guest memory ownership");
    recomp_audio_output_reset_voice(0);

    std::vector<uint8_t> large(kMaxBufferBytes, 0x80);
    for (unsigned i = 0; i < kQueueSize; ++i)
        recomp_audio_output_submit(1, large.data(), kMaxBufferBytes, 44100, 2, 16, -600);
    check(voices[1].queued == kQueueSize && dropped_buffers == before + 9,
        "full queue drops incoming chunk");
    check(std::abs(SDL_GetAudioStreamGain(voices[1].stream) - std::pow(10.0f, -0.3f)) < 0.00001f,
        "decibel volume");
    check(SDL_GetAudioStreamFrequencyRatio(voices[1].stream) == 1.01f, "rate trim capped");
    recomp_audio_output_submit(1, pcm, sizeof pcm, 22050, 1, 8, -20000);
    check(voices[1].queued == 2 && voices[1].bits_per_sample == 8 &&
        SDL_GetAudioStreamGain(voices[1].stream) == 0, "format replacement and mute clamp");
    SDL_UnbindAudioStream(voices[1].stream);
    format = {SDL_AUDIO_U8, 1, 22050};
    check(SDL_SetAudioStreamFormat(voices[1].stream, &format, &format), "inspect u8 silence");
    check(SDL_SetAudioStreamGain(voices[1].stream, 1) && SDL_FlushAudioStream(voices[1].stream),
        "read unmuted u8 source");
    uint8_t silence[100];
    check(SDL_GetAudioStreamData(voices[1].stream, silence, sizeof silence) == sizeof silence &&
        std::all_of(silence, silence + 100, [](uint8_t b) { return b == 0x80; }), "unsigned silence");
    recomp_audio_output_reset_voice(1);

    recomp_audio_output_submit(2, pcm, sizeof pcm, 44100, 1, 16, 100);
    check(SDL_GetAudioStreamGain(voices[2].stream) == 1, "positive volume clamp");
    check(SDL_ResumeAudioDevice(device), "resume dummy device");
    const auto deadline = SDL_GetTicks() + 2000;
    while (SDL_GetAudioStreamQueued(voices[2].stream) && SDL_GetTicks() < deadline) SDL_Delay(5);
    check(SDL_GetAudioStreamQueued(voices[2].stream) == 0, "dummy mixer consumes buffers");
    check(SDL_PauseAudioDevice(device), "pause after consumption");
    const auto previous_underruns = underruns;
    recomp_audio_output_submit(2, pcm, sizeof pcm, 44100, 1, 16, 0);
    check(underruns == previous_underruns + 1 && voices[2].queued == 2,
        "starved voice retires buffers and restores preroll");
    recomp_audio_output_reset_voice(256);
    check(SDL_InitSubSystem(SDL_INIT_AUDIO), "another SDL audio owner");
    recomp_audio_output_shutdown();
    check(!recomp_audio_output_enabled() && SDL_WasInit(SDL_INIT_AUDIO), "balanced SDL ownership");
    recomp_audio_output_initialize();
    check(!recomp_audio_output_enabled(), "shutdown prevents reopening");
    recomp_audio_output_shutdown();
    SDL_QuitSubSystem(SDL_INIT_AUDIO);

    restart("0.5");
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "doaxbv-no-such-driver");
    recomp_audio_output_initialize();
    check(!recomp_audio_output_enabled(), "unavailable device disables output");
    recomp_audio_output_shutdown();
    std::puts("audio output SDL tests passed");
}
