#include "asr.h"

#include <sherpa-onnx/c-api/c-api.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct vi_asr {
    const SherpaOnnxOnlineRecognizer *recognizer;
    const SherpaOnnxOnlineStream *stream;
    vi_transcript_callback callback;
    void *userdata;
    char previous[4096];
};

static int model_path(char *buffer, size_t size, const char *directory,
                      const char *filename) {
    int written = snprintf(buffer, size, "%s/%s", directory, filename);
    return written >= 0 && (size_t)written < size ? 0 : -1;
}

struct vi_asr *vi_asr_create(const char *model_directory, int threads,
                             vi_transcript_callback callback, void *userdata) {
    if (model_directory == NULL || model_directory[0] == '\0') return NULL;
    struct vi_asr *asr = calloc(1, sizeof(*asr));
    if (asr == NULL) return NULL;

    char encoder[4096];
    char decoder[4096];
    char joiner[4096];
    char tokens[4096];
    if (model_path(encoder, sizeof(encoder), model_directory,
                   "encoder-epoch-99-avg-1.int8.onnx") < 0 ||
        model_path(decoder, sizeof(decoder), model_directory,
                   "decoder-epoch-99-avg-1.onnx") < 0 ||
        model_path(joiner, sizeof(joiner), model_directory,
                   "joiner-epoch-99-avg-1.int8.onnx") < 0 ||
        model_path(tokens, sizeof(tokens), model_directory, "tokens.txt") < 0) {
        free(asr);
        return NULL;
    }

    SherpaOnnxOnlineRecognizerConfig config;
    memset(&config, 0, sizeof(config));
    config.feat_config.sample_rate = 16000;
    config.feat_config.feature_dim = 80;
    config.model_config.transducer.encoder = encoder;
    config.model_config.transducer.decoder = decoder;
    config.model_config.transducer.joiner = joiner;
    config.model_config.tokens = tokens;
    config.model_config.provider = "cpu";
    config.model_config.num_threads = threads > 0 ? threads : 2;
    config.decoding_method = "greedy_search";
    config.enable_endpoint = 1;
    config.rule1_min_trailing_silence = 2.4F;
    config.rule2_min_trailing_silence = 1.2F;
    config.rule3_min_utterance_length = 20.0F;

    asr->recognizer = SherpaOnnxCreateOnlineRecognizer(&config);
    if (asr->recognizer == NULL) {
        free(asr);
        return NULL;
    }
    asr->stream = SherpaOnnxCreateOnlineStream(asr->recognizer);
    if (asr->stream == NULL) {
        SherpaOnnxDestroyOnlineRecognizer(asr->recognizer);
        free(asr);
        return NULL;
    }
    asr->callback = callback;
    asr->userdata = userdata;
    return asr;
}

void vi_asr_destroy(struct vi_asr *asr) {
    if (asr == NULL) return;
    if (asr->stream != NULL) SherpaOnnxDestroyOnlineStream(asr->stream);
    SherpaOnnxDestroyOnlineRecognizer(asr->recognizer);
    free(asr);
}

static void emit_result(struct vi_asr *asr, bool endpoint) {
    const SherpaOnnxOnlineRecognizerResult *result =
        SherpaOnnxGetOnlineStreamResult(asr->recognizer, asr->stream);
    if (result == NULL) return;
    const char *text = result->text != NULL ? result->text : "";
    if (text[0] != '\0' && (endpoint || strcmp(text, asr->previous) != 0)) {
        if (asr->callback != NULL) {
            asr->callback(endpoint ? "final" : "partial", text, asr->userdata);
        }
        snprintf(asr->previous, sizeof(asr->previous), "%s", text);
    }
    SherpaOnnxDestroyOnlineRecognizerResult(result);
}

static void decode_ready(struct vi_asr *asr) {
    while (SherpaOnnxIsOnlineStreamReady(asr->recognizer, asr->stream)) {
        SherpaOnnxDecodeOnlineStream(asr->recognizer, asr->stream);
    }
    const bool endpoint =
        SherpaOnnxOnlineStreamIsEndpoint(asr->recognizer, asr->stream) != 0;
    emit_result(asr, endpoint);
    if (endpoint) {
        SherpaOnnxOnlineStreamReset(asr->recognizer, asr->stream);
        asr->previous[0] = '\0';
    }
}

int vi_asr_accept(struct vi_asr *asr, const float *samples, size_t count) {
    if (asr == NULL || samples == NULL || count == 0) return -1;
    while (count > 0) {
        int32_t chunk = count > (size_t)INT32_MAX ? INT32_MAX : (int32_t)count;
        SherpaOnnxOnlineStreamAcceptWaveform(asr->stream, 16000, samples, chunk);
        samples += chunk;
        count -= (size_t)chunk;
    }
    decode_ready(asr);
    return 0;
}

void vi_asr_finish(struct vi_asr *asr) {
    if (asr == NULL) return;
    float padding[4800] = {0};
    SherpaOnnxOnlineStreamAcceptWaveform(asr->stream, 16000, padding, 4800);
    SherpaOnnxOnlineStreamInputFinished(asr->stream);
    decode_ready(asr);
    emit_result(asr, true);
    SherpaOnnxDestroyOnlineStream(asr->stream);
    asr->stream = SherpaOnnxCreateOnlineStream(asr->recognizer);
    asr->previous[0] = '\0';
}

const char *vi_asr_state(const struct vi_asr *asr) {
    return asr != NULL ? "ready" : "disabled";
}
