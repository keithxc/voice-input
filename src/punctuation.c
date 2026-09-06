#define _POSIX_C_SOURCE 200809L

#include "punctuation.h"

#include "text.h"

#include <sherpa-onnx/c-api/c-api.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct vi_punctuation {
    const SherpaOnnxOfflinePunctuation *engine;
    char model[128];
};

/* The int8 model is a quarter of the size of the float one and loads faster;
   prefer it, but accept either so a hand-placed model directory also works. */
static int find_model(const char *directory, char *buffer, size_t size) {
    static const char *const candidates[] = { "model.int8.onnx", "model.onnx" };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        int written = snprintf(buffer, size, "%s/%s", directory, candidates[i]);
        if (written < 0 || (size_t)written >= size) return -1;
        if (access(buffer, R_OK) == 0) return 0;
    }
    return -1;
}

static void remember_model(struct vi_punctuation *punctuation, const char *path) {
    const char *name = strrchr(path, '/');
    name = name != NULL ? name + 1 : path;
    size_t length = strlen(name);
    if (length >= sizeof(punctuation->model)) {
        length = sizeof(punctuation->model) - 1;
    }
    memcpy(punctuation->model, name, length);
    punctuation->model[length] = '\0';
}

struct vi_punctuation *vi_punctuation_create(const char *model_directory,
                                             int threads) {
    if (model_directory == NULL || model_directory[0] == '\0') return NULL;
    char model[4096];
    if (find_model(model_directory, model, sizeof(model)) < 0) {
        fprintf(stderr, "voice-inputd: no punctuation model in %s\n",
                model_directory);
        return NULL;
    }
    struct vi_punctuation *punctuation = calloc(1, sizeof(*punctuation));
    if (punctuation == NULL) return NULL;

    SherpaOnnxOfflinePunctuationConfig config;
    memset(&config, 0, sizeof(config));
    config.model.ct_transformer = model;
    config.model.num_threads = threads > 0 ? threads : 1;
    config.model.provider = "cpu";
    punctuation->engine = SherpaOnnxCreateOfflinePunctuation(&config);
    if (punctuation->engine == NULL) {
        free(punctuation);
        return NULL;
    }
    remember_model(punctuation, model);
    return punctuation;
}

void vi_punctuation_destroy(struct vi_punctuation *punctuation) {
    if (punctuation == NULL) return;
    if (punctuation->engine != NULL) {
        SherpaOnnxDestroyOfflinePunctuation(punctuation->engine);
    }
    free(punctuation);
}

/* The ct-transformer was trained on a mixed corpus and punctuates everything in
   Chinese, so a dictated English sentence comes back ending in a fullwidth
   stop. When the utterance holds no Han character the marks it chose are simply
   written in the wrong script: rewrite them, without adding or removing any. */
void vi_punctuation_localise(char *text, size_t size) {
    static const struct { const char *from; const char *to; } marks[] = {
        { "\xef\xbc\x8c", ", " },  /* U+FF0C fullwidth comma */
        { "\xe3\x80\x81", ", " },  /* U+3001 ideographic comma */
        { "\xe3\x80\x82", ". " },  /* U+3002 ideographic full stop */
        { "\xef\xbc\x9f", "? " },  /* U+FF1F fullwidth question mark */
        { "\xef\xbc\x81", "! " },  /* U+FF01 fullwidth exclamation mark */
    };
    if (text == NULL || size == 0 || vi_utf8_has_han(text)) return;

    char output[8192];
    size_t used = 0;
    for (const char *cursor = text; *cursor != '\0';) {
        const char *piece = cursor;
        uint32_t codepoint = 0;
        size_t width = vi_utf8_decode(cursor, &codepoint);
        size_t length = width;
        for (size_t i = 0; i < sizeof(marks) / sizeof(marks[0]); ++i) {
            if (strncmp(cursor, marks[i].from, strlen(marks[i].from)) != 0) {
                continue;
            }
            piece = marks[i].to;
            width = strlen(marks[i].from);
            length = strlen(marks[i].to);
            break;
        }
        if (used + length >= sizeof(output)) return;
        memcpy(output + used, piece, length);
        used += length;
        cursor += width;
    }
    while (used > 0 && output[used - 1] == ' ') --used;
    if (used >= size) return;
    memcpy(text, output, used);
    text[used] = '\0';
}

int vi_punctuation_apply(struct vi_punctuation *punctuation, const char *text,
                         char *buffer, size_t size) {
    if (text == NULL || buffer == NULL || size == 0) return -1;
    (void)snprintf(buffer, size, "%s", text);
    if (punctuation == NULL || punctuation->engine == NULL || text[0] == '\0') {
        return -1;
    }
    const char *punctuated =
        SherpaOfflinePunctuationAddPunct(punctuation->engine, text);
    if (punctuated == NULL) return -1;
    const int written = snprintf(buffer, size, "%s", punctuated);
    SherpaOfflinePunctuationFreeText(punctuated);
    /* A truncated result would commit half a sentence; keep the raw text. */
    if (written < 0 || (size_t)written >= size) {
        (void)snprintf(buffer, size, "%s", text);
        return -1;
    }
    vi_punctuation_localise(buffer, size);
    return 0;
}

const char *vi_punctuation_model(const struct vi_punctuation *punctuation) {
    return punctuation != NULL ? punctuation->model : "";
}
