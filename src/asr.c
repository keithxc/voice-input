#define _POSIX_C_SOURCE 200809L

#include "asr.h"

#include <dirent.h>
#include <sherpa-onnx/c-api/c-api.h>
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
    char model_name[128];
    char decoder[32];
    const char *kind;
    int threads;
};

/* A model directory is whatever upstream shipped: file names carry epoch and
   averaging counts that differ per release, and the same architecture appears
   with and without quantised weights. Rather than hard-coding one release's
   names, each role is filled by the best matching file present. */
struct pick {
    char path[4096];
    int score;
};

static bool ends_with(const char *name, const char *suffix) {
    const size_t name_length = strlen(name);
    const size_t suffix_length = strlen(suffix);
    return name_length >= suffix_length &&
           strcmp(name + name_length - suffix_length, suffix) == 0;
}

static void consider(struct pick *best, const char *directory, const char *name,
                     const char *prefix, bool prefer_int8) {
    if (!ends_with(name, ".onnx")) return;
    if (strncmp(name, prefix, strlen(prefix)) != 0) return;
    const bool int8 = strstr(name, ".int8.") != NULL;
    const int score = (int8 == prefer_int8) ? 2 : 1;
    if (score < best->score) return;
    char path[sizeof(best->path)];
    int written = snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (written < 0 || (size_t)written >= sizeof(path)) return;
    /* Same preference: keep the lexicographically first so a directory holding
       several checkpoints always loads the same one. */
    if (score == best->score && best->score > 0 && strcmp(path, best->path) >= 0) {
        return;
    }
    memcpy(best->path, path, (size_t)written + 1U);
    best->score = score;
}

static int scan_directory(const char *directory, bool prefer_int8,
                          struct pick *encoder, struct pick *decoder,
                          struct pick *joiner, struct pick *single) {
    DIR *handle = opendir(directory);
    if (handle == NULL) return -1;
    const struct dirent *entry = NULL;
    while ((entry = readdir(handle)) != NULL) {
        consider(encoder, directory, entry->d_name, "encoder", prefer_int8);
        consider(decoder, directory, entry->d_name, "decoder", prefer_int8);
        consider(joiner, directory, entry->d_name, "joiner", prefer_int8);
        consider(single, directory, entry->d_name, "model", prefer_int8);
        consider(single, directory, entry->d_name, "ctc", prefer_int8);
    }
    closedir(handle);
    return 0;
}

static void remember_name(char *buffer, size_t size, const char *directory) {
    const char *name = strrchr(directory, '/');
    name = name != NULL ? name + 1 : directory;
    if (name[0] == '\0') name = directory;
    size_t length = strlen(name);
    if (length >= size) length = size - 1;
    memcpy(buffer, name, length);
    buffer[length] = '\0';
}

void vi_asr_config_defaults(struct vi_asr_config *config) {
    if (config == NULL) return;
    memset(config, 0, sizeof(*config));
    config->decoding_method = "greedy_search";
    config->threads = 2;
    config->max_active_paths = 4;
    config->hotwords_score = 1.5F;
    config->rule1_min_trailing_silence = 2.4F;
    config->rule2_min_trailing_silence = 1.2F;
    config->rule3_min_utterance_length = 20.0F;
    config->prefer_int8 = true;
}

struct vi_asr *vi_asr_create(const struct vi_asr_config *config,
                             vi_transcript_callback callback, void *userdata) {
    if (config == NULL || config->model_directory == NULL ||
        config->model_directory[0] == '\0') {
        return NULL;
    }
    char tokens[4096];
    int written = snprintf(tokens, sizeof(tokens), "%s/tokens.txt",
                           config->model_directory);
    if (written < 0 || (size_t)written >= sizeof(tokens)) return NULL;

    struct pick encoder = {0};
    struct pick decoder = {0};
    struct pick joiner = {0};
    struct pick single = {0};
    if (scan_directory(config->model_directory, config->prefer_int8, &encoder,
                       &decoder, &joiner, &single) < 0) {
        fprintf(stderr, "voice-inputd: cannot read model directory %s\n",
                config->model_directory);
        return NULL;
    }

    struct vi_asr *asr = calloc(1, sizeof(*asr));
    if (asr == NULL) return NULL;

    SherpaOnnxOnlineRecognizerConfig recognizer;
    memset(&recognizer, 0, sizeof(recognizer));
    recognizer.feat_config.sample_rate = 16000;
    recognizer.feat_config.feature_dim = 80;
    if (encoder.score > 0 && decoder.score > 0 && joiner.score > 0) {
        recognizer.model_config.transducer.encoder = encoder.path;
        recognizer.model_config.transducer.decoder = decoder.path;
        recognizer.model_config.transducer.joiner = joiner.path;
        asr->kind = "transducer";
    } else if (encoder.score > 0 && decoder.score > 0) {
        recognizer.model_config.paraformer.encoder = encoder.path;
        recognizer.model_config.paraformer.decoder = decoder.path;
        asr->kind = "paraformer";
    } else if (single.score > 0) {
        recognizer.model_config.zipformer2_ctc.model = single.path;
        asr->kind = "zipformer2-ctc";
    } else {
        fprintf(stderr, "voice-inputd: no usable model files in %s\n",
                config->model_directory);
        free(asr);
        return NULL;
    }
    recognizer.model_config.tokens = tokens;
    recognizer.model_config.provider = "cpu";
    recognizer.model_config.num_threads = config->threads > 0 ? config->threads : 2;
    recognizer.decoding_method = config->decoding_method != NULL
                                     ? config->decoding_method
                                     : "greedy_search";
    recognizer.max_active_paths = config->max_active_paths;
    recognizer.blank_penalty = config->blank_penalty;
    recognizer.hotwords_file = config->hotwords_file;
    recognizer.hotwords_score = config->hotwords_score;
    recognizer.enable_endpoint = 1;
    recognizer.rule1_min_trailing_silence = config->rule1_min_trailing_silence;
    recognizer.rule2_min_trailing_silence = config->rule2_min_trailing_silence;
    recognizer.rule3_min_utterance_length = config->rule3_min_utterance_length;

    asr->recognizer = SherpaOnnxCreateOnlineRecognizer(&recognizer);
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
    asr->threads = recognizer.model_config.num_threads;
    remember_name(asr->model_name, sizeof(asr->model_name),
                  config->model_directory);
    snprintf(asr->decoder, sizeof(asr->decoder), "%s", recognizer.decoding_method);
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

const char *vi_asr_model_name(const struct vi_asr *asr) {
    return asr != NULL ? asr->model_name : "";
}

const char *vi_asr_model_kind(const struct vi_asr *asr) {
    return asr != NULL && asr->kind != NULL ? asr->kind : "";
}

const char *vi_asr_decoder(const struct vi_asr *asr) {
    return asr != NULL ? asr->decoder : "";
}

int vi_asr_threads(const struct vi_asr *asr) {
    return asr != NULL ? asr->threads : 0;
}
