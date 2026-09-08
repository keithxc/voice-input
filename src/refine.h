#ifndef VOICE_INPUT_REFINE_H
#define VOICE_INPUT_REFINE_H
#include <stdbool.h>
#include <stddef.h>
#define VI_REFINE_TEXT_SIZE 16384U
#define VI_REFINE_MAX_SAMPLES (60U * 16000U)
struct vi_refiner;
struct vi_refine_result {
    char text[VI_REFINE_TEXT_SIZE];
    char backend[32];
    bool cancelled;
    bool fallback;
    long elapsed_ms;
};
/* Models are loaded once. Only the worker invokes offline recognition. */
struct vi_refiner *vi_refiner_create(const char *paraformer_directory,
                                    const char *sensevoice_directory, int threads);
void vi_refiner_destroy(struct vi_refiner *refiner);
int vi_refiner_submit(struct vi_refiner *refiner, const float *samples,
                      size_t count, const char *draft);
int vi_refiner_poll(struct vi_refiner *refiner, struct vi_refine_result *result);
void vi_refiner_cancel(struct vi_refiner *refiner);
/* Pure policy helpers, also used by the file benchmark. */
bool vi_refine_preserves_words(const char *draft, const char *candidate);
bool vi_refine_english(const char *draft);
float vi_refine_normalize(float *samples, size_t count);
#endif
