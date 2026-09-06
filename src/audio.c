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
#include <time.h>

#define VI_MAX_SOURCES 16U
#define VI_RING_SAMPLES 65536U
#define VI_SWITCH_MARGIN 6.0F
#define VI_SWITCH_VOTES 8U
#define VI_SWITCH_COOLDOWN_MS 1000L
#define VI_SPEECH_SOURCE_HOLD_MS 1200L

struct vi_audio;

struct vi_source {
    struct vi_audio *audio;
    uint32_t id;
    char name[256];
    char description[256];
    struct pw_stream *stream;
    struct spa_hook listener;
    enum pw_stream_state state;
    float noise_floor;
    float rms;
    float score;
    unsigned chunks;
};

struct vi_audio {
    struct pw_main_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_registry *registry;
    struct spa_hook registry_listener;
    vi_level_callback callback;
    void *userdata;
    bool active;
    struct vi_source sources[VI_MAX_SOURCES];
    struct vi_source *selected;
    struct vi_source *candidate;
    unsigned candidate_votes;
    struct timespec last_switch;
    struct timespec last_selected_speech;
    float samples[VI_RING_SAMPLES];
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

    const float gain = desired < current_gain
                           ? desired
                           : current_gain + 0.35F * (desired - current_gain);
    for (size_t i = 0; i < count; ++i) {
        samples[i] = fmaxf(-0.98F, fminf(samples[i] * gain, 0.98F));
    }
    return gain;
}

float vi_audio_quality_score(float rms, float noise_floor, float clipping_ratio) {
    const float safe_rms = fmaxf(rms, 0.000001F);
    const float safe_noise = fmaxf(noise_floor, 0.000001F);
    const float snr_db = fmaxf(0.0F, 20.0F * log10f(safe_rms / safe_noise));
    const float level_db = 20.0F * log10f(safe_rms);
    const float useful_level = fmaxf(0.0F, fminf(level_db + 60.0F, 40.0F));
    return 2.0F * snr_db + 0.5F * useful_level - 200.0F * clipping_ratio;
}

static long elapsed_ms(const struct timespec *now, const struct timespec *then) {
    return (now->tv_sec - then->tv_sec) * 1000L +
           (now->tv_nsec - then->tv_nsec) / 1000000L;
}

static void select_source(struct vi_audio *audio, struct vi_source *source) {
    if (audio->selected == source) return;
    audio->selected = source;
    audio->candidate = NULL;
    audio->candidate_votes = 0U;
    clock_gettime(CLOCK_MONOTONIC, &audio->last_switch);
    audio->last_selected_speech = audio->last_switch;
    const size_t write_index = atomic_load_explicit(&audio->write_index,
                                                    memory_order_acquire);
    atomic_store_explicit(&audio->read_index, write_index, memory_order_release);
    fprintf(stderr, "voice-inputd: selected audio source: %s\n",
            source != NULL ? source->description : "none");
}

static void consider_source_switch(struct vi_audio *audio,
                                   struct vi_source *updated) {
    struct vi_source *best = NULL;
    for (size_t i = 0; i < VI_MAX_SOURCES; ++i) {
        struct vi_source *source = &audio->sources[i];
        if (source->id == SPA_ID_INVALID || source->stream == NULL ||
            source->state != PW_STREAM_STATE_STREAMING || source->chunks < 4U ||
            source->rms < 0.001F) {
            continue;
        }
        if (best == NULL || source->score > best->score) best = source;
    }
    if (audio->selected == NULL || audio->selected->stream == NULL ||
        audio->selected->state == PW_STREAM_STATE_ERROR ||
        audio->selected->state == PW_STREAM_STATE_UNCONNECTED) {
        if (best != NULL) select_source(audio, best);
        return;
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (elapsed_ms(&now, &audio->last_selected_speech) <
        VI_SPEECH_SOURCE_HOLD_MS) {
        audio->candidate = NULL;
        audio->candidate_votes = 0U;
        return;
    }
    if (best == NULL || best == audio->selected ||
        best->score < audio->selected->score + VI_SWITCH_MARGIN) {
        audio->candidate = NULL;
        audio->candidate_votes = 0U;
        return;
    }
    if (elapsed_ms(&now, &audio->last_switch) < VI_SWITCH_COOLDOWN_MS) return;
    if (updated != best) return;
    if (audio->candidate != best) {
        audio->candidate = best;
        audio->candidate_votes = 1U;
    } else if (++audio->candidate_votes >= VI_SWITCH_VOTES) {
        select_source(audio, best);
    }
}

static void queue_selected_samples(struct vi_source *source, const uint8_t *data,
                                   size_t count, size_t stride) {
    struct vi_audio *audio = source->audio;
    size_t write_index = atomic_load_explicit(&audio->write_index,
                                              memory_order_relaxed);
    const size_t read_index = atomic_load_explicit(&audio->read_index,
                                                   memory_order_acquire);
    for (size_t i = 0; i < count; ++i) {
        int16_t sample = 0;
        memcpy(&sample, data + i * stride, sizeof(sample));
        const size_t next = (write_index + 1U) % VI_RING_SAMPLES;
        if (next == read_index) break;
        audio->samples[write_index] = (float)((double)sample / 32768.0);
        write_index = next;
    }
    atomic_store_explicit(&audio->write_index, write_index, memory_order_release);
}

static void on_stream_state_changed(void *data, enum pw_stream_state old,
                                    enum pw_stream_state state, const char *error) {
    (void)old;
    struct vi_source *source = data;
    source->state = state;
    if (state == PW_STREAM_STATE_ERROR) {
        fprintf(stderr, "voice-inputd: source %s unavailable: %s\n",
                source->description, error != NULL ? error : "unknown error");
        if (source->audio->selected == source) source->audio->selected = NULL;
    }
}

static void on_stream_process(void *data) {
    struct vi_source *source = data;
    struct pw_buffer *pw_buffer = pw_stream_dequeue_buffer(source->stream);
    if (pw_buffer == NULL) return;

    struct spa_buffer *buffer = pw_buffer->buffer;
    if (buffer->n_datas > 0U && buffer->datas[0].data != NULL) {
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
        double squares = 0.0;
        size_t clipped = 0U;
        for (size_t i = 0; i < count; ++i) {
            int16_t sample = 0;
            memcpy(&sample, samples + i * stride, sizeof(sample));
            const float normalized = (float)((double)sample / 32768.0);
            squares += (double)normalized * (double)normalized;
            if (fabsf(normalized) >= 0.98F) ++clipped;
        }
        if (count > 0U) {
            source->rms = (float)sqrt(squares / (double)count);
            if (source->noise_floor <= 0.0F) {
                source->noise_floor = source->rms;
            } else if (source->rms < source->noise_floor) {
                source->noise_floor = 0.8F * source->noise_floor +
                                      0.2F * source->rms;
            } else {
                source->noise_floor = 0.995F * source->noise_floor +
                                      0.005F * source->rms;
            }
            source->score = vi_audio_quality_score(
                source->rms, source->noise_floor,
                (float)clipped / (float)count);
            ++source->chunks;
            if (source->audio->selected == source && source->rms >= 0.003F &&
                source->rms >= 2.0F * source->noise_floor) {
                clock_gettime(CLOCK_MONOTONIC,
                              &source->audio->last_selected_speech);
            }
            consider_source_switch(source->audio, source);
            if (source->audio->selected == source) {
                queue_selected_samples(source, samples, count, stride);
            }
        }
    }
    pw_stream_queue_buffer(source->stream, pw_buffer);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_stream_state_changed,
    .process = on_stream_process,
};

static int start_source(struct vi_source *source) {
    if (source->stream != NULL) return 0;
    char stream_name[64];
    snprintf(stream_name, sizeof(stream_name), "voice-input-capture-%u", source->id);
    source->stream = pw_stream_new(
        source->audio->core, stream_name,
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio",
                          PW_KEY_MEDIA_CATEGORY, "Capture",
                          PW_KEY_MEDIA_ROLE, "Communication",
                          PW_KEY_NODE_NAME, stream_name,
                          PW_KEY_TARGET_OBJECT, source->name, NULL));
    if (source->stream == NULL) return -1;
    pw_stream_add_listener(source->stream, &source->listener,
                           &stream_events, source);

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
        source->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
        params, 1U);
    if (result < 0) {
        pw_stream_destroy(source->stream);
        source->stream = NULL;
        return result;
    }
    source->state = PW_STREAM_STATE_CONNECTING;
    source->noise_floor = 0.0F;
    source->rms = 0.0F;
    source->score = 0.0F;
    source->chunks = 0U;
    fprintf(stderr, "voice-inputd: listening to audio source: %s\n",
            source->description);
    return 0;
}

static void stop_source(struct vi_source *source) {
    if (source->stream != NULL) {
        pw_stream_destroy(source->stream);
        source->stream = NULL;
    }
    source->state = PW_STREAM_STATE_UNCONNECTED;
}

static bool is_audio_source(const char *media_class) {
    return media_class != NULL &&
           (strcmp(media_class, "Audio/Source") == 0 ||
            strncmp(media_class, "Audio/Source/", 13U) == 0);
}

static void on_registry_global(void *data, uint32_t id, uint32_t permissions,
                               const char *type, uint32_t version,
                               const struct spa_dict *props) {
    (void)permissions;
    (void)version;
    struct vi_audio *audio = data;
    if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0 || props == NULL ||
        !is_audio_source(spa_dict_lookup(props, PW_KEY_MEDIA_CLASS))) {
        return;
    }
    const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (name == NULL || strncmp(name, "voice-input", 11U) == 0) return;
    for (size_t i = 0; i < VI_MAX_SOURCES; ++i) {
        if (audio->sources[i].id != SPA_ID_INVALID) continue;
        struct vi_source *source = &audio->sources[i];
        source->audio = audio;
        source->id = id;
        snprintf(source->name, sizeof(source->name), "%s", name);
        const char *description = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
        snprintf(source->description, sizeof(source->description), "%s",
                 description != NULL ? description : name);
        if (audio->active && start_source(source) < 0) {
            fprintf(stderr, "voice-inputd: cannot listen to source: %s\n",
                    source->description);
        }
        return;
    }
    fprintf(stderr, "voice-inputd: ignoring audio source %s (source limit reached)\n",
            name);
}

static void on_registry_global_remove(void *data, uint32_t id) {
    struct vi_audio *audio = data;
    for (size_t i = 0; i < VI_MAX_SOURCES; ++i) {
        struct vi_source *source = &audio->sources[i];
        if (source->id != id) continue;
        if (audio->selected == source) audio->selected = NULL;
        if (audio->candidate == source) audio->candidate = NULL;
        stop_source(source);
        memset(source, 0, sizeof(*source));
        source->id = SPA_ID_INVALID;
        return;
    }
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = on_registry_global,
    .global_remove = on_registry_global_remove,
};

struct vi_audio *vi_audio_create(vi_level_callback callback, void *userdata) {
    pw_init(NULL, NULL);
    struct vi_audio *audio = calloc(1, sizeof(*audio));
    if (audio == NULL) return NULL;
    for (size_t i = 0; i < VI_MAX_SOURCES; ++i) audio->sources[i].id = SPA_ID_INVALID;
    audio->loop = pw_main_loop_new(NULL);
    if (audio->loop == NULL) goto fail;
    audio->context = pw_context_new(pw_main_loop_get_loop(audio->loop), NULL, 0U);
    if (audio->context == NULL) goto fail;
    audio->core = pw_context_connect(audio->context, NULL, 0U);
    if (audio->core == NULL) goto fail;
    audio->registry = pw_core_get_registry(audio->core, PW_VERSION_REGISTRY, 0U);
    if (audio->registry == NULL) goto fail;
    pw_registry_add_listener(audio->registry, &audio->registry_listener,
                             &registry_events, audio);
    audio->callback = callback;
    audio->userdata = userdata;
    audio->max_gain = environment_float("VOICE_INPUT_MAX_GAIN", 6.0F, 1.0F, 16.0F);
    audio->target_rms = environment_float("VOICE_INPUT_TARGET_RMS", 0.08F,
                                          0.01F, 0.30F);
    audio->gain = audio->max_gain;
    return audio;

fail:
    if (audio->core != NULL) pw_core_disconnect(audio->core);
    if (audio->context != NULL) pw_context_destroy(audio->context);
    if (audio->loop != NULL) pw_main_loop_destroy(audio->loop);
    free(audio);
    pw_deinit();
    return NULL;
}

void vi_audio_stop(struct vi_audio *audio) {
    if (audio == NULL) return;
    for (size_t i = 0; i < VI_MAX_SOURCES; ++i) stop_source(&audio->sources[i]);
    audio->active = false;
    audio->selected = NULL;
    audio->candidate = NULL;
    audio->candidate_votes = 0U;
}

void vi_audio_destroy(struct vi_audio *audio) {
    if (audio == NULL) return;
    vi_audio_stop(audio);
    if (audio->registry != NULL) pw_proxy_destroy((struct pw_proxy *)audio->registry);
    if (audio->core != NULL) pw_core_disconnect(audio->core);
    if (audio->context != NULL) pw_context_destroy(audio->context);
    if (audio->loop != NULL) pw_main_loop_destroy(audio->loop);
    free(audio);
    pw_deinit();
}

int vi_audio_start(struct vi_audio *audio) {
    if (audio == NULL) return -1;
    if (audio->active) return 0;
    audio->active = true;
    audio->gain = audio->max_gain;
    atomic_store_explicit(&audio->read_index, 0U, memory_order_relaxed);
    atomic_store_explicit(&audio->write_index, 0U, memory_order_relaxed);
    int started = 0;
    for (size_t i = 0; i < VI_MAX_SOURCES; ++i) {
        if (audio->sources[i].id != SPA_ID_INVALID &&
            start_source(&audio->sources[i]) == 0) ++started;
    }
    fprintf(stderr, "voice-inputd: parallel capture started on %d source(s)\n", started);
    return 0;
}

int vi_audio_iterate(struct vi_audio *audio, int timeout_ms) {
    if (audio == NULL) return -1;
    return pw_loop_iterate(pw_main_loop_get_loop(audio->loop), timeout_ms);
}

size_t vi_audio_read(struct vi_audio *audio, float *samples, size_t capacity) {
    if (audio == NULL || samples == NULL) return 0U;
    size_t read_index = atomic_load_explicit(&audio->read_index, memory_order_relaxed);
    const size_t write_index = atomic_load_explicit(&audio->write_index,
                                                    memory_order_acquire);
    size_t count = 0U;
    while (read_index != write_index && count < capacity) {
        samples[count++] = audio->samples[read_index];
        read_index = (read_index + 1U) % VI_RING_SAMPLES;
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
            audio->callback((float)sqrt(squares / (double)count), audio->userdata);
        }
    }
    return count;
}

const char *vi_audio_state(const struct vi_audio *audio) {
    if (audio == NULL) return "disabled";
    if (!audio->active) return "idle";
    for (size_t i = 0; i < VI_MAX_SOURCES; ++i) {
        if (audio->sources[i].state == PW_STREAM_STATE_STREAMING) return "streaming";
    }
    return "connecting";
}

bool vi_audio_is_active(const struct vi_audio *audio) {
    return audio != NULL && audio->active;
}

const char *vi_audio_selected_source(const struct vi_audio *audio) {
    return audio != NULL && audio->selected != NULL
               ? audio->selected->description
               : "";
}

size_t vi_audio_source_count(const struct vi_audio *audio) {
    if (audio == NULL) return 0U;
    size_t count = 0U;
    for (size_t i = 0; i < VI_MAX_SOURCES; ++i) {
        if (audio->sources[i].id != SPA_ID_INVALID) ++count;
    }
    return count;
}
