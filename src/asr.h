#ifndef VOICE_INPUT_ASR_H
#define VOICE_INPUT_ASR_H

#include <stddef.h>

struct vi_asr;

typedef void (*vi_transcript_callback)(const char *event, const char *text,
                                       void *userdata);

struct vi_asr *vi_asr_create(const char *model_directory, int threads,
                             vi_transcript_callback callback, void *userdata);
void vi_asr_destroy(struct vi_asr *asr);
int vi_asr_accept(struct vi_asr *asr, const float *samples, size_t count);
void vi_asr_finish(struct vi_asr *asr);
const char *vi_asr_state(const struct vi_asr *asr);

#endif
