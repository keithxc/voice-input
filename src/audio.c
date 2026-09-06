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
};

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
        const int16_t *samples = (const int16_t *)((const uint8_t *)spa_data->data + offset);
        const size_t count = bytes / sizeof(*samples);
        double squares = 0.0;
        size_t write_index = atomic_load_explicit(&audio->write_index,
                                                  memory_order_relaxed);
        const size_t read_index = atomic_load_explicit(&audio->read_index,
                                                       memory_order_acquire);
        for (size_t i = 0; i < count; ++i) {
            const double normalized = (double)samples[i] / 32768.0;
            squares += normalized * normalized;
            const size_t next = (write_index + 1U) % 65536U;
            if (next == read_index) break;
            audio->samples[write_index] = (float)normalized;
            write_index = next;
        }
        atomic_store_explicit(&audio->write_index, write_index, memory_order_release);
        if (count > 0 && audio->callback != NULL) {
            audio->callback((float)sqrt(squares / (double)count), audio->userdata);
        }
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
    return count;
}

const char *vi_audio_state(const struct vi_audio *audio) {
    return audio != NULL ? audio->state : "disabled";
}

bool vi_audio_is_active(const struct vi_audio *audio) {
    return audio != NULL && audio->active;
}
