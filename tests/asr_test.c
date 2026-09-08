#include "asr.h"
#include "audio.h"

#include <sherpa-onnx/c-api/c-api.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool received_text = false;
static char finals[8192];

static void on_transcript(const char *event, const char *text, void *userdata) {
    (void)userdata;
    if (text != NULL && text[0] != '\0') {
        printf("%s: %s\n", event, text);
        received_text = true;
        if (strcmp(event, "final") == 0) {
            size_t used = strlen(finals);
            snprintf(finals + used, sizeof(finals) - used, "%s|", text);
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) return EXIT_FAILURE;
    const bool quiet_test = argc == 3 && strcmp(argv[2], "--quiet") == 0;
    if (argc == 3 && !quiet_test) return EXIT_FAILURE;
    char wav_path[4096];
    int written = snprintf(wav_path, sizeof(wav_path), "%s/test_wavs/2.wav", argv[1]);
    if (written < 0 || (size_t)written >= sizeof(wav_path)) return EXIT_FAILURE;

    const SherpaOnnxWave *wave = SherpaOnnxReadWave(wav_path);
    if (wave == NULL || wave->samples == NULL || wave->num_samples <= 0) {
        fprintf(stderr, "cannot read %s\n", wav_path);
        return EXIT_FAILURE;
    }
    struct vi_asr_config config;
    vi_asr_config_defaults(&config);
    config.model_directory = argv[1];
    struct vi_asr *asr = vi_asr_create(&config, on_transcript, NULL);
    if (asr == NULL) {
        SherpaOnnxFreeWave(wave);
        return EXIT_FAILURE;
    }
    const int chunk_size = 1600;
    float gain = 1.0F;
    for (int offset = 0; offset < wave->num_samples; offset += chunk_size) {
        int count = wave->num_samples - offset;
        if (count > chunk_size) count = chunk_size;
        float chunk[1600];
        for (int i = 0; i < count; ++i) {
            chunk[i] = wave->samples[offset + i] * (quiet_test ? 0.04F : 1.0F);
        }
        if (quiet_test) {
            gain = vi_audio_apply_gain(chunk, (size_t)count, gain, 8.0F, 0.03F);
        }
        if (vi_asr_accept(asr, chunk, (size_t)count) < 0) {
            vi_asr_destroy(asr);
            SherpaOnnxFreeWave(wave);
            return EXIT_FAILURE;
        }
    }
    vi_asr_finish(asr);
    if (!quiet_test) {
        char expected[sizeof(finals)];
        memcpy(expected, finals, sizeof(expected));
        const int sizes[] = {1, 341, 4096};
        for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
            finals[0] = '\0';
            for (int offset = 0; offset < wave->num_samples; offset += sizes[i]) {
                int n = wave->num_samples - offset;
                if (n > sizes[i]) n = sizes[i];
                if (vi_asr_accept(asr, wave->samples + offset, (size_t)n) < 0) return EXIT_FAILURE;
            }
            vi_asr_finish(asr);
            if (strcmp(expected, finals) != 0) {
                fprintf(stderr, "transcript changed with caller block size %d\n", sizes[i]);
                return EXIT_FAILURE;
            }
        }
        /* Cancelling a partial input cannot leak it into the next utterance. */
        vi_asr_accept(asr, wave->samples, 123);
        vi_asr_reset(asr);
        finals[0] = '\0';
        vi_asr_accept(asr, wave->samples, (size_t)wave->num_samples);
        vi_asr_finish(asr);
        if (strcmp(expected, finals) != 0) return EXIT_FAILURE;
    }
    vi_asr_destroy(asr);
    SherpaOnnxFreeWave(wave);
    return received_text ? EXIT_SUCCESS : EXIT_FAILURE;
}
