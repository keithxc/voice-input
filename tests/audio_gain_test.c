#include "audio.h"

#include <math.h>
#include <stddef.h>
#include <stdlib.h>

static float rms(const float *samples, size_t count) {
    double squares = 0.0;
    for (size_t i = 0; i < count; ++i) squares += samples[i] * samples[i];
    return (float)sqrt(squares / (double)count);
}

int main(void) {
    float silence[1600] = { 0.0F };
    const float silent_gain =
        vi_audio_apply_gain(silence, 1600U, 8.0F, 8.0F, 0.10F);
    if (silent_gain > 1.01F || rms(silence, 1600U) != 0.0F) return EXIT_FAILURE;

    float quiet[1600];
    for (size_t i = 0; i < 1600U; ++i) quiet[i] = (i % 2U == 0U) ? 0.005F : -0.005F;
    const float gain = vi_audio_apply_gain(quiet, 1600U, silent_gain, 8.0F, 0.10F);
    if (gain <= 1.0F || gain >= 8.0F || rms(quiet, 1600U) < 0.015F) {
        return EXIT_FAILURE;
    }

    float loud[] = { 0.9F, -0.9F, 0.6F, -0.6F };
    const float reduced = vi_audio_apply_gain(loud, 4U, gain, 8.0F, 0.10F);
    if (reduced >= 2.0F) return EXIT_FAILURE;
    for (size_t i = 0; i < 4U; ++i) {
        if (fabsf(loud[i]) > 0.981F) return EXIT_FAILURE;
    }

    const float clean = vi_audio_quality_score(0.08F, 0.004F, 0.0F);
    const float noisy = vi_audio_quality_score(0.08F, 0.04F, 0.0F);
    const float clipped = vi_audio_quality_score(0.08F, 0.004F, 0.20F);
    if (!(clean > noisy && clean > clipped)) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
