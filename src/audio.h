#ifndef VOICE_INPUT_AUDIO_H
#define VOICE_INPUT_AUDIO_H

#include <stdbool.h>
#include <stddef.h>

struct vi_audio;

typedef void (*vi_level_callback)(float rms, void *userdata);

struct vi_audio *vi_audio_create(vi_level_callback callback, void *userdata);
void vi_audio_destroy(struct vi_audio *audio);
int vi_audio_start(struct vi_audio *audio);
void vi_audio_stop(struct vi_audio *audio);
int vi_audio_iterate(struct vi_audio *audio, int timeout_ms);
size_t vi_audio_read(struct vi_audio *audio, float *samples, size_t capacity);
float vi_audio_apply_gain(float *samples, size_t count, float current_gain,
                          float max_gain, float target_rms);
const char *vi_audio_state(const struct vi_audio *audio);
bool vi_audio_is_active(const struct vi_audio *audio);

#endif
