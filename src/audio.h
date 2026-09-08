#ifndef VOICE_INPUT_AUDIO_H
#define VOICE_INPUT_AUDIO_H

#include <stdbool.h>
#include <stddef.h>

struct vi_audio;
struct vi_audio_metrics {
    size_t samples;
    size_t clipped;
    double squares;
    float peak;
};

typedef void (*vi_level_callback)(float rms, void *userdata);

struct vi_audio *vi_audio_create(vi_level_callback callback, void *userdata);
void vi_audio_destroy(struct vi_audio *audio);
int vi_audio_start(struct vi_audio *audio);
void vi_audio_stop(struct vi_audio *audio);
int vi_audio_iterate(struct vi_audio *audio, int timeout_ms);
size_t vi_audio_read(struct vi_audio *audio, float *samples, size_t capacity);
size_t vi_audio_read_with_raw(struct vi_audio *audio, float *samples,
                              float *raw, size_t capacity);
void vi_audio_take_metrics(struct vi_audio *audio, struct vi_audio_metrics *metrics);
float vi_audio_apply_gain(float *samples, size_t count, float current_gain,
                          float max_gain, float target_rms);
float vi_audio_quality_score(float rms, float noise_floor, float clipping_ratio);
const char *vi_audio_state(const struct vi_audio *audio);
bool vi_audio_is_active(const struct vi_audio *audio);
const char *vi_audio_selected_source(const struct vi_audio *audio);
/* "default" (follows the desktop's input setting), "auto" or the pinned name. */
const char *vi_audio_source_mode(const struct vi_audio *audio);
size_t vi_audio_source_count(const struct vi_audio *audio);
int vi_audio_describe_sources(const struct vi_audio *audio, char *buffer,
                              size_t size);

#endif
