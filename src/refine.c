#define _POSIX_C_SOURCE 200809L
#include "refine.h"
#include "text.h"
#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <sherpa-onnx/c-api/c-api.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct vi_refiner {
    const SherpaOnnxOfflineRecognizer *paraformer;
    const SherpaOnnxOfflineRecognizer *sensevoice;
    const SherpaOnnxVoiceActivityDetector *vad;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t wake;
    enum { IDLE, QUEUED, RUNNING, DONE } state;
    bool stopping, cancelled;
    float *samples;
    size_t count;
    char draft[VI_REFINE_TEXT_SIZE];
    struct vi_refine_result result;
};

bool vi_refine_english(const char *draft) {
    if (draft == NULL) return false;
    size_t han = 0, letters = 0, words = 0;
    bool in_word = false;
    for (const char *p = draft; *p;) {
        uint32_t cp;
        p += vi_utf8_decode(p, &cp);
        if (vi_utf8_is_han(cp)) ++han;
        if (cp < 128 && isalpha((unsigned char)cp)) {
            ++letters;
            if (!in_word) ++words;
            in_word = true;
        } else in_word = false;
    }
    /* Preserve Chinese/code-switching by default. A long English draft with
       at most a few spurious Han characters can still use the English model. */
    return (han == 0 && letters >= 2) ||
           (words >= 8 && han <= 3 && letters >= 12 * han);
}

bool vi_refine_preserves_words(const char *draft, const char *candidate) {
    if (!draft || !candidate) return false;
    for (const unsigned char *p = (const unsigned char *)draft; *p;) {
        if (*p >= 128 || !isalpha(*p)) { ++p; continue; }
        const unsigned char *start = p;
        bool uppercase = true;
        while (*p < 128 && isalpha(*p)) {
            if (!isupper(*p)) uppercase = false;
            ++p;
        }
        const size_t length = (size_t)(p - start);
        if (length < 4 && !(uppercase && length >= 2)) continue;
        bool found = false;
        for (const unsigned char *q = (const unsigned char *)candidate; *q;) {
            if (*q >= 128 || !isalpha(*q)) { ++q; continue; }
            const unsigned char *word = q;
            while (*q < 128 && isalpha(*q)) ++q;
            const size_t n = (size_t)(q - word);
            const size_t common = n < length ? n : length;
            if (common < 2 || (n != length && common < 4)) continue;
            size_t i = 0;
            while (i < common && tolower(start[i]) == tolower(word[i])) ++i;
            if (i == common) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}

float vi_refine_normalize(float *samples, size_t count) {
    if (samples == NULL || count == 0) return 1.0F;
    double squares = 0;
    float peak = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!isfinite(samples[i])) samples[i] = 0;
        squares += (double)samples[i] * samples[i];
        if (fabsf(samples[i]) > peak) peak = fabsf(samples[i]);
    }
    const float rms = (float)sqrt(squares / (double)count);
    /* One gain per utterance preserves syllable dynamics. Do not boost
       silence, and never invent headroom by clipping an impulse. */
    if (rms < 0.003F || rms >= 0.02F || peak <= 0) return 1.0F;
    const float gain = fmaxf(1.0F, fminf(6.0F, fminf(0.06F / rms, 0.98F / peak)));
    for (size_t i = 0; i < count; ++i) samples[i] *= gain;
    return gain;
}

static long clock_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000L + now.tv_nsec / 1000000L;
}

/* An empty streaming draft is not proof of silence. Only rescue it when a
   separate speech detector finds speech; never run a generative recognizer
   unconditionally on silence/noise. All VAD state belongs to the worker. */
static bool detect_speech(struct vi_refiner *r) {
    if (!r->vad) return false;
    SherpaOnnxVoiceActivityDetectorReset(r->vad);
    for (size_t i = 0; i < r->count; i += 512) {
        float block[512] = {0};
        const size_t n = r->count - i < 512 ? r->count - i : 512;
        memcpy(block, r->samples + i, n * sizeof(float));
        SherpaOnnxVoiceActivityDetectorAcceptWaveform(r->vad, block, 512);
        if (!SherpaOnnxVoiceActivityDetectorEmpty(r->vad)) return true;
    }
    SherpaOnnxVoiceActivityDetectorFlush(r->vad);
    return !SherpaOnnxVoiceActivityDetectorEmpty(r->vad);
}

static const SherpaOnnxOfflineRecognizer *load_model(const char *directory,
                                                     bool sense, int threads) {
    if (directory == NULL || !*directory) return NULL;
    char model[4096], tokens[4096];
    int n = snprintf(model, sizeof(model), "%s/model.int8.onnx", directory);
    if (n < 0 || (size_t)n >= sizeof(model)) return NULL;
    n = snprintf(tokens, sizeof(tokens), "%s/tokens.txt", directory);
    if (n < 0 || (size_t)n >= sizeof(tokens)) return NULL;
    if (access(model, R_OK) != 0 || access(tokens, R_OK) != 0) return NULL;
    SherpaOnnxOfflineRecognizerConfig config = {0};
    config.feat_config.sample_rate = 16000;
    config.feat_config.feature_dim = 80;
    config.model_config.tokens = tokens;
    config.model_config.num_threads = threads;
    config.model_config.provider = "cpu";
    config.decoding_method = "greedy_search";
    if (sense) {
        config.model_config.sense_voice.model = model;
        config.model_config.sense_voice.language = "auto";
        config.model_config.sense_voice.use_itn = 1;
    } else config.model_config.paraformer.model = model;
    return SherpaOnnxCreateOfflineRecognizer(&config);
}

static void *worker(void *userdata) {
    struct vi_refiner *r = userdata;
    pthread_mutex_lock(&r->mutex);
    for (;;) {
        while (!r->stopping && r->state != QUEUED) pthread_cond_wait(&r->wake, &r->mutex);
        if (r->stopping) break;
        r->state = RUNNING;
        pthread_mutex_unlock(&r->mutex);
        struct vi_refine_result result = {0};
        const long before = clock_ms();
        const bool mixed = vi_utf8_has_han(r->draft) &&
            strpbrk(r->draft, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz") != NULL &&
            !vi_refine_english(r->draft);
        const bool english = !mixed;
        const SherpaOnnxOfflineRecognizer *recognizer = english ? r->sensevoice : r->paraformer;
        snprintf(result.backend, sizeof(result.backend), "%s", english ? "sensevoice" : "paraformer-zh-en");
        (void)vi_refine_normalize(r->samples, r->count);
        const bool speech = r->draft[0] || detect_speech(r);
        const SherpaOnnxOfflineStream *stream = speech ? SherpaOnnxCreateOfflineStream(recognizer) : NULL;
        if (stream != NULL) {
            SherpaOnnxAcceptWaveformOffline(stream, 16000, r->samples, (int32_t)r->count);
            SherpaOnnxDecodeOfflineStream(recognizer, stream);
            const SherpaOnnxOfflineRecognizerResult *decoded = SherpaOnnxGetOfflineStreamResult(stream);
            if (decoded && decoded->text && strlen(decoded->text) < sizeof(result.text)) {
                snprintf(result.text, sizeof(result.text), "%s", decoded->text);
            }
            if (decoded) SherpaOnnxDestroyOfflineRecognizerResult(decoded);
            SherpaOnnxDestroyOfflineStream(stream);
        }
        if (result.text[0] == '\0' ||
            (mixed && !vi_refine_preserves_words(r->draft, result.text))) {
            snprintf(result.text, sizeof(result.text), "%s", r->draft);
            result.fallback = true;
        }
        result.elapsed_ms = clock_ms() - before;
        free(r->samples);
        pthread_mutex_lock(&r->mutex);
        r->samples = NULL;
        result.cancelled = r->cancelled;
        r->result = result;
        r->state = DONE;
    }
    pthread_mutex_unlock(&r->mutex);
    return NULL;
}

struct vi_refiner *vi_refiner_create(const char *paraformer, const char *sensevoice, int threads) {
    struct vi_refiner *r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->paraformer = load_model(paraformer, false, threads);
    r->sensevoice = load_model(sensevoice, true, threads);
    if (!r->paraformer || !r->sensevoice) goto fail;
    const char *vad_path = getenv("VOICE_INPUT_VAD_MODEL");
    const char *rescue = getenv("VOICE_INPUT_EMPTY_DRAFT_RESCUE");
    if (rescue && !strcmp(rescue, "1") && vad_path && *vad_path && access(vad_path, R_OK) == 0) {
        SherpaOnnxVadModelConfig c = {0};
        c.silero_vad.model = vad_path;
        c.silero_vad.threshold = 0.5F;
        c.silero_vad.min_speech_duration = 0.25F;
        c.silero_vad.min_silence_duration = 0.1F;
        c.silero_vad.max_speech_duration = 60.0F;
        c.silero_vad.window_size = 512;
        c.sample_rate = 16000;
        c.num_threads = 1;
        c.provider = "cpu";
        r->vad = SherpaOnnxCreateVoiceActivityDetector(&c, 61.0F);
    }
    fprintf(stderr, "voice-inputd: empty-draft speech rescue: %s\n",
            r->vad ? "enabled (experimental)" : "disabled");
    if (pthread_mutex_init(&r->mutex, NULL) != 0) goto fail;
    if (pthread_cond_init(&r->wake, NULL) != 0) { pthread_mutex_destroy(&r->mutex); goto fail; }
    if (pthread_create(&r->thread, NULL, worker, r) != 0) {
        pthread_cond_destroy(&r->wake); pthread_mutex_destroy(&r->mutex); goto fail;
    }
    return r;
fail:
    if (r->vad) SherpaOnnxDestroyVoiceActivityDetector(r->vad);
    if (r->paraformer) SherpaOnnxDestroyOfflineRecognizer(r->paraformer);
    if (r->sensevoice) SherpaOnnxDestroyOfflineRecognizer(r->sensevoice);
    free(r);
    return NULL;
}

int vi_refiner_submit(struct vi_refiner *r, const float *samples, size_t count, const char *draft) {
    if (!r || !samples || count == 0 || count > VI_REFINE_MAX_SAMPLES || !draft || strlen(draft) >= sizeof(r->draft)) return -1;
    pthread_mutex_lock(&r->mutex);
    if (r->state != IDLE) { pthread_mutex_unlock(&r->mutex); return -1; }
    r->samples = malloc(count * sizeof(float));
    if (!r->samples) { pthread_mutex_unlock(&r->mutex); return -1; }
    memcpy(r->samples, samples, count * sizeof(float));
    r->count = count;
    snprintf(r->draft, sizeof(r->draft), "%s", draft);
    r->cancelled = false;
    r->state = QUEUED;
    pthread_cond_signal(&r->wake);
    pthread_mutex_unlock(&r->mutex);
    return 0;
}

int vi_refiner_poll(struct vi_refiner *r, struct vi_refine_result *result) {
    if (!r || !result) return 0;
    pthread_mutex_lock(&r->mutex);
    const bool ready = r->state == DONE;
    if (ready) { *result = r->result; r->state = IDLE; }
    pthread_mutex_unlock(&r->mutex);
    return ready ? 1 : 0;
}

void vi_refiner_cancel(struct vi_refiner *r) {
    if (!r) return;
    pthread_mutex_lock(&r->mutex);
    r->cancelled = true;
    if (r->state == DONE) r->result.cancelled = true;
    pthread_mutex_unlock(&r->mutex);
}

void vi_refiner_destroy(struct vi_refiner *r) {
    if (!r) return;
    pthread_mutex_lock(&r->mutex);
    r->stopping = true;
    pthread_cond_signal(&r->wake);
    pthread_mutex_unlock(&r->mutex);
    pthread_join(r->thread, NULL);
    if (r->vad) SherpaOnnxDestroyVoiceActivityDetector(r->vad);
    free(r->samples);
    pthread_cond_destroy(&r->wake);
    pthread_mutex_destroy(&r->mutex);
    SherpaOnnxDestroyOfflineRecognizer(r->paraformer);
    SherpaOnnxDestroyOfflineRecognizer(r->sensevoice);
    free(r);
}
