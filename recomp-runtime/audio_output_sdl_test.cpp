// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio_output.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

extern "C" void recomp_audio_output_test_reset(void);

static void check_fail(const char *expr, const char *file, int line)
{
    std::fprintf(stderr, "Check failed at %s:%d: %s\n", file, line, expr);
    std::abort();
}
#define CHECK(expr) do { if (!(expr)) check_fail(#expr, __FILE__, __LINE__); } while (0)

int main()
{
    // Initially disabled before init
    CHECK(recomp_audio_output_enabled() == 0);

    // Test with valid gain first so real SDL audio initialization is exercised
    setenv("RECOMP_AUDIO_GAIN", "0.5", 1);
    recomp_audio_output_initialize();

    // Test submitting audio
    std::vector<int16_t> pcm_stereo_16(480 * 2, 0);
    for (size_t i = 0; i < pcm_stereo_16.size(); ++i) {
        pcm_stereo_16[i] = static_cast<int16_t>(1000 * std::sin(i * 0.1));
    }

    // Submit stereo 48000 Hz 16-bit
    recomp_audio_output_submit(
        0, reinterpret_cast<const uint8_t *>(pcm_stereo_16.data()),
        static_cast<uint32_t>(pcm_stereo_16.size() * sizeof(int16_t)),
        48000, 2, 16, 0);

    // Submit mono 22050 Hz 8-bit
    std::vector<uint8_t> pcm_mono_8(220, 128);
    recomp_audio_output_submit(
        1, pcm_mono_8.data(), static_cast<uint32_t>(pcm_mono_8.size()),
        22050, 1, 8, -600);

    // Test invalid submissions (should be safely dropped)
    recomp_audio_output_submit(999, pcm_mono_8.data(), 10, 44100, 1, 8, 0); // invalid slot
    recomp_audio_output_submit(0, nullptr, 100, 44100, 1, 8, 0); // null pcm
    recomp_audio_output_submit(0, pcm_mono_8.data(), 3, 44100, 2, 16, 0); // unaligned block

    // Test reset voice
    recomp_audio_output_reset_voice(0);
    recomp_audio_output_reset_voice(1);
    recomp_audio_output_reset_voice(255);

    // Shutdown
    recomp_audio_output_shutdown();
    CHECK(recomp_audio_output_enabled() == 0);

    // Reset attempted flag for muted lifecycle test
    recomp_audio_output_test_reset();

    // Initialize with 0 gain -> must stay disabled / muted
    setenv("RECOMP_AUDIO_GAIN", "0", 1);
    recomp_audio_output_initialize();
    CHECK(recomp_audio_output_enabled() == 0);

    recomp_audio_output_shutdown();

    std::printf("audio output SDL test passed\n");
    return 0;
}
