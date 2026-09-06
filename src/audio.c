#define _GNU_SOURCE

#include "audio.h"

#include <math.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/param/format-utils.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct vi_audio {
    struct pw_main_loop *loop;
    struct pw_stream *stream;
    struct spa_hook listener;
    vi_level_callback callback;
    void *userdata;
    bool active;
    const char *state;
    float samples[65536];
    atomic_size_t read_index;
    atomic_size_t write_index;
    float gain;
    float max_gain;
    float target_rms;
};

static float environment_float(const char *name, float fallback,
                               float minimum, float maximum) {
    const char *value = getenv(name);
    if (value == NULL || value[0] == '\0') return fallback;
    char *end = NULL;
    const float parsed = strtof(value, &end);
    return end != value && *end == '\0' && isfinite(parsed) &&
                   parsed >= minimum && parsed <= maximum
               ? parsed
               : fallback;
}

float vi_audio_apply_gain(float *samples, size_t count, float current_gain,
                          float max_gain, float target_rms) {
    if (samples == NULL || count == 0U) return current_gain;
    double squares = 0.0;
    float peak = 0.0F;
    for (size_t i = 0; i < count; ++i) {
        const float magnitude = fabsf(samples[i]);
        if (magnitude > peak) peak = magnitude;
        squares += (double)samples[i] * (double)samples[i];
    }
    const float rms = (float)sqrt(squares / (double)count);
    float desired = rms > 0.0001F ? target_rms / rms : max_gain;
    desired = fmaxf(1.0F, fminf(desired, max_gain));
    if (peak > 0.0F) desired = fminf(desired, 0.98F / peak);

    // Drop gain immediately when speech becomes loud; raise it gradually to
    // avoid pumping while still starting each recording ready for a whisper.
    const float gain = desired < current_gain
                           ? desired
                           : current_gain + 0.35F * (desired - current_gain);
    for (size_t i = 0; i < count; ++i) {
        samples[i] = fmaxf(-0.98F, fminf(samples[i] * gain, 0.98F));
    }
    return gain;
}

static void on_stream_state_changed(void *data, enum pw_stream_state old,
                                    enum pw_stream_state state, const char *error) {
    (void)old;
    struct vi_audio *audio = data;
    audio->state = pw_stream_state_as_string(state);
    if (state == PW_STREAM_STATE_ERROR) {
        fprintf(stderr, "voice-inputd: PipeWire stream error: %s\n",
                error != NULL ? error : "unknown error");
    }
}

static void on_stream_process(void *data) {
    struct vi_audio *audio = data;
    struct pw_buffer *pw_buffer = pw_stream_dequeue_buffer(audio->stream);
    if (pw_buffer == NULL) {
        return;
    }

    struct spa_buffer *buffer = pw_buffer->buffer;
    if (buffer->n_datas > 0 && buffer->datas[0].data != NULL) {
        struct spa_data *spa_data = &buffer->datas[0];
        const uint32_t offset = spa_data->chunk != NULL ? spa_data->chunk->offset : 0U;
        const uint32_t bytes = spa_data->chunk != NULL ? spa_data->chunk->size : 0U;
        const int32_t chunk_stride = spa_data->chunk != NULL
                                         ? spa_data->chunk->stride
                                         : (int32_t)sizeof(int16_t);
        const size_t stride = chunk_stride >= (int32_t)sizeof(int16_t)
                                  ? (size_t)chunk_stride
                                  : sizeof(int16_t);
        const uint8_t *samples = (const uint8_t *)spa_data->data + offset;
        const size_t count = bytes / stride;
        size_t write_index = atomic_load_explicit(&audio->write_index,
                                                  memory_order_relaxed);
        const size_t read_index = atomic_load_explicit(&audio->read_index,
                                                       memory_order_acquire);
        for (size_t i = 0; i < count; ++i) {
            int16_t sample = 0;
            memcpy(&sample, samples + i * stride, sizeof(sample));
            const double normalized = (double)sample / 32768.0;
            const size_t next = (write_index + 1U) % 65536U;
            if (next == read_index) break;
            audio->samples[write_index] = (float)normalized;
            write_index = next;
        }
        atomic_store_explicit(&audio->write_index, write_index, memory_order_release);
    }
    pw_stream_queue_buffer(audio->stream, pw_buffer);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_stream_state_changed,
    .process = on_stream_process,
};

struct vi_audio *vi_audio_create(vi_level_callback callback, void *userdata) {
    pw_init(NULL, NULL);
    struct vi_audio *audio = calloc(1, sizeof(*audio));
    if (audio == NULL) return NULL;
    audio->loop = pw_main_loop_new(NULL);
    if (audio->loop == NULL) {
        free(audio);
        return NULL;
    }
    audio->callback = callback;
    audio->userdata = userdata;
    audio->state = "idle";
    audio->max_gain = environment_float("VOICE_INPUT_MAX_GAIN", 6.0F, 1.0F, 16.0F);
    audio->target_rms = environment_float("VOICE_INPUT_TARGET_RMS", 0.08F,
                                          0.01F, 0.30F);
    audio->gain = audio->max_gain;
    return audio;
}

void vi_audio_stop(struct vi_audio *audio) {
    if (audio == NULL) return;
    if (audio->stream != NULL) {
        pw_stream_destroy(audio->stream);
        audio->stream = NULL;
    }
    audio->active = false;
    audio->state = "idle";
}

void vi_audio_destroy(struct vi_audio *audio) {
    if (audio == NULL) return;
    vi_audio_stop(audio);
    pw_main_loop_destroy(audio->loop);
    free(audio);
    pw_deinit();
}

int vi_audio_start(struct vi_audio *audio) {
    if (audio == NULL) return -1;
    if (audio->active) return 0;

    audio->stream = pw_stream_new_simple(
        pw_main_loop_get_loop(audio->loop), "voice-input-capture",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                          PW_KEY_MEDIA_CATEGORY, "Capture",
                          PW_KEY_MEDIA_ROLE, "Communication",
                          PW_KEY_NODE_NAME, "voice-inputd", NULL),
        &stream_events, audio);
    if (audio->stream == NULL) return -1;

    uint8_t storage[1024];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    const struct spa_audio_info_raw info = {
        .format = SPA_AUDIO_FORMAT_S16_LE,
        .rate = 16000,
        .channels = 1,
        .position = { SPA_AUDIO_CHANNEL_MONO },
    };
    const struct spa_pod *params[] = {
        spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info),
    };
    const int result = pw_stream_connect(
        audio->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
            PW_STREAM_FLAG_RT_PROCESS,
        params, 1);
    if (result < 0) {
        vi_audio_stop(audio);
        return result;
    }
    audio->active = true;
    audio->gain = audio->max_gain;
    audio->state = "connecting";
    return 0;
}

int vi_audio_iterate(struct vi_audio *audio, int timeout_ms) {
    if (audio == NULL) return -1;
    return pw_loop_iterate(pw_main_loop_get_loop(audio->loop), timeout_ms);
}

size_t vi_audio_read(struct vi_audio *audio, float *samples, size_t capacity) {
    if (audio == NULL || samples == NULL) return 0;
    size_t read_index = atomic_load_explicit(&audio->read_index, memory_order_relaxed);
    const size_t write_index = atomic_load_explicit(&audio->write_index,
                                                    memory_order_acquire);
    size_t count = 0;
    while (read_index != write_index && count < capacity) {
        samples[count++] = audio->samples[read_index];
        read_index = (read_index + 1U) % 65536U;
    }
    atomic_store_explicit(&audio->read_index, read_index, memory_order_release);
    if (count > 0U) {
        audio->gain = vi_audio_apply_gain(samples, count, audio->gain,
                                          audio->max_gain, audio->target_rms);
        if (audio->callback != NULL) {
            double squares = 0.0;
            for (size_t i = 0; i < count; ++i) {
                squares += (double)samples[i] * (double)samples[i];
            }
            audio->callback((float)sqrt(squares / (double)count),
                            audio->userdata);
        }
    }
    return count;
}

const char *vi_audio_state(const struct vi_audio *audio) {
    return audio != NULL ? audio->state : "disabled";
}

bool vi_audio_is_active(const struct vi_audio *audio) {
    return audio != NULL && audio->active;
}
