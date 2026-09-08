#ifndef VOICE_INPUT_ASR_H
#define VOICE_INPUT_ASR_H

#include <stdbool.h>
#include <stddef.h>

struct vi_asr;

typedef void (*vi_transcript_callback)(const char *event, const char *text,
                                       void *userdata);

/* Everything that decides what the recogniser is, so a model can be swapped or
   a decoder compared without rebuilding. The daemon fills this from the
   environment and the benchmark tool from its command line; both then hand it
   to the same loader. */
struct vi_asr_config {
    const char *model_directory;
    /* "greedy_search" or "modified_beam_search". Hotwords are ignored by the
       greedy decoder -- sherpa-onnx does not report that, it simply has no
       effect. */
    const char *decoding_method;
    int threads;
    int max_active_paths;
    float blank_penalty;
    const char *hotwords_file;
    float hotwords_score;
    float rule1_min_trailing_silence;
    float rule2_min_trailing_silence;
    float rule3_min_utterance_length;
    /* Quantised weights are the default: they are several times smaller and
       faster, and a float model is only worth loading to check what that
       costs in accuracy. */
    bool prefer_int8;
};

void vi_asr_config_defaults(struct vi_asr_config *config);

struct vi_asr *vi_asr_create(const struct vi_asr_config *config,
                             vi_transcript_callback callback, void *userdata);
void vi_asr_destroy(struct vi_asr *asr);
int vi_asr_accept(struct vi_asr *asr, const float *samples, size_t count);
void vi_asr_finish(struct vi_asr *asr);
void vi_asr_reset(struct vi_asr *asr);
const char *vi_asr_state(const struct vi_asr *asr);

/* For the status reply and the benchmark report. */
const char *vi_asr_model_name(const struct vi_asr *asr);
const char *vi_asr_model_kind(const struct vi_asr *asr);
const char *vi_asr_decoder(const struct vi_asr *asr);
int vi_asr_threads(const struct vi_asr *asr);

#endif
