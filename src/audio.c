#define _GNU_SOURCE

#include "audio.h"

#include "protocol.h"
#include "selection.h"

#include <math.h>
#include <pipewire/extensions/metadata.h>
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

struct vi_audio;

/* Which microphone to listen to. Following the session manager's default is
   what every other application does, so the input picked in the desktop's own
   sound settings is the one that gets used; the parallel scoring of every
   source is kept for the case that needs it and is no longer the default,
   because arbitrating between microphones that are all quiet picks one of them
   before anybody has spoken. */
enum vi_source_mode {
    VI_SOURCE_SYSTEM_DEFAULT,
    VI_SOURCE_AUTO,
    VI_SOURCE_NAMED,
};

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
    struct vi_selection selection;
    enum vi_source_mode mode;
    char wanted[256];
    char default_source[256];
    struct pw_metadata *metadata;
    struct spa_hook metadata_listener;
    /* Lossy ring: the capture thread only ever publishes write_position, and the
       reader owns read_position, so neither blocks the other. Positions are
       monotonic sample counts rather than wrapped indices, which makes an
       overrun and a pre-roll rewind the same clamp. */
    float samples[VI_RING_SAMPLES];
    atomic_uint_least64_t write_position;
    atomic_uint_least64_t boundary_position;
    uint64_t read_position;
    float gain;
    float max_gain;
    float target_rms;
    long preroll_ms;
    bool recording;
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
    /* Do not learn a high gain from pre-roll silence.  Starting a recording
       used to feed amplified room noise to the recognizer and then snap the
       gain down on the first syllable.  Treat sub-noise-floor chunks as
       silence; real quiet speech still clears this deliberately low gate. */
    float desired = rms >= 0.005F ? target_rms / rms : 1.0F;
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

static long now_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000L + now.tv_nsec / 1000000L;
}

static struct vi_source *selected_source(struct vi_audio *audio) {
    return audio->selection.selected != VI_NO_SOURCE
               ? &audio->sources[audio->selection.selected]
               : NULL;
}

static void collect_stats(const struct vi_audio *audio,
                          struct vi_source_stats *stats) {
    for (size_t i = 0; i < VI_MAX_SOURCES; ++i) {
        const struct vi_source *source = &audio->sources[i];
        stats[i].present = source->id != SPA_ID_INVALID && source->stream != NULL;
        stats[i].streaming = source->state == PW_STREAM_STATE_STREAMING;
        stats[i].chunks = source->chunks;
        stats[i].rms = source->rms;
        stats[i].score = source->score;
    }
}

static void consider_source_switch(struct vi_audio *audio,
                                   struct vi_source *updated) {
    struct vi_source_stats stats[VI_MAX_SOURCES];
    collect_stats(audio, stats);
    const int previous = audio->selection.selected;
    const int chosen = vi_selection_update(&audio->selection, stats,
                                           VI_MAX_SOURCES,
                                           (int)(updated - audio->sources),
                                           now_ms());
    if (chosen == previous) return;

    /* Nothing recorded before the switch may be replayed, so one utterance is
       never stitched together from two microphones. */
    atomic_store_explicit(&audio->boundary_position,
                          atomic_load_explicit(&audio->write_position,
                                               memory_order_acquire),
                          memory_order_release);
    fprintf(stderr, "voice-inputd: selected audio source: %s\n",
            chosen != VI_NO_SOURCE ? audio->sources[chosen].description : "none");
}

/* Until the session manager has said which input is the default, behave as the
   parallel mode does rather than capturing nothing. */
static bool arbitrating(const struct vi_audio *audio) {
    return audio->mode == VI_SOURCE_AUTO ||
           (audio->mode == VI_SOURCE_SYSTEM_DEFAULT &&
            audio->default_source[0] == '\0');
}

static bool source_wanted(const struct vi_audio *audio,
                          const struct vi_source *source) {
    if (arbitrating(audio)) return true;
    if (audio->mode == VI_SOURCE_NAMED) {
        return strcasestr(source->name, audio->wanted) != NULL ||
               strcasestr(source->description, audio->wanted) != NULL;
    }
    return strcmp(source->name, audio->default_source) == 0;
}

static void discard_captured_audio(struct vi_audio *audio) {
    /* Nothing captured before a change of microphone may be replayed, so one
       utterance is never stitched together from two of them. */
    atomic_store_explicit(&audio->boundary_position,
                          atomic_load_explicit(&audio->write_position,
                                               memory_order_acquire),
                          memory_order_release);
}

static void pin_selection(struct vi_audio *audio, struct vi_source *source) {
    const int index = (int)(source - audio->sources);
    if (audio->selection.selected == index) return;
    discard_captured_audio(audio);
    audio->selection.selected = index;
    audio->selection.candidate = VI_NO_SOURCE;
    audio->selection.candidate_votes = 0U;
    fprintf(stderr, "voice-inputd: capturing from %s\n", source->description);
}

static void queue_selected_samples(struct vi_source *source, const uint8_t *data,
                                   size_t count, size_t stride, bool silent) {
    struct vi_audio *audio = source->audio;
    uint64_t write_position = atomic_load_explicit(&audio->write_position,
                                                   memory_order_relaxed);
    for (size_t i = 0; i < count; ++i) {
        int16_t sample = 0;
        if (!silent) memcpy(&sample, data + i * stride, sizeof(sample));
        audio->samples[write_position % VI_RING_SAMPLES] =
            (float)((double)sample / 32768.0);
        ++write_position;
    }
    atomic_store_explicit(&audio->write_position, write_position,
                          memory_order_release);
}

static void on_stream_state_changed(void *data, enum pw_stream_state old,
                                    enum pw_stream_state state, const char *error) {
    (void)old;
    struct vi_source *source = data;
    source->state = state;
    if (state == PW_STREAM_STATE_ERROR) {
        fprintf(stderr, "voice-inputd: source %s unavailable: %s\n",
                source->description, error != NULL ? error : "unknown error");
        if (selected_source(source->audio) == source) {
            source->audio->selection.selected = VI_NO_SOURCE;
        }
    }
}

static void on_stream_process(void *data) {
    struct vi_source *source = data;
    struct pw_buffer *pw_buffer = pw_stream_dequeue_buffer(source->stream);
    if (pw_buffer == NULL) return;

    struct spa_buffer *buffer = pw_buffer->buffer;
    if (buffer->n_datas > 0U && buffer->datas[0].data != NULL &&
        buffer->datas[0].chunk != NULL &&
        ((uint32_t)buffer->datas[0].chunk->flags &
         SPA_CHUNK_FLAG_CORRUPTED) == 0U) {
        struct spa_data *spa_data = &buffer->datas[0];
        /* A source with nothing to send flags the chunk EMPTY and may leave the
           mapped memory untouched, so its contents must never be measured or
           forwarded: reading them yields full-scale noise that both drowns the
           recogniser and beats every real microphone on level. */
        const bool silent =
            ((uint32_t)spa_data->chunk->flags & SPA_CHUNK_FLAG_EMPTY) != 0U;
        const uint32_t offset = SPA_MIN(spa_data->chunk->offset, spa_data->maxsize);
        const uint32_t bytes = SPA_MIN(spa_data->chunk->size,
                                       spa_data->maxsize - offset);
        const int32_t chunk_stride = spa_data->chunk->stride;
        const size_t stride = chunk_stride >= (int32_t)sizeof(int16_t)
                                  ? (size_t)chunk_stride
                                  : sizeof(int16_t);
        const uint8_t *samples = (const uint8_t *)spa_data->data + offset;
        const size_t count = bytes / stride;
        double squares = 0.0;
        size_t clipped = 0U;
        for (size_t i = 0; !silent && i < count; ++i) {
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
            if (selected_source(source->audio) == source &&
                source->rms >= 0.003F &&
                source->rms >= 2.0F * source->noise_floor) {
                source->audio->selection.last_speech_ms = now_ms();
            }
            if (arbitrating(source->audio)) {
                consider_source_switch(source->audio, source);
            } else {
                pin_selection(source->audio, source);
            }
            if (selected_source(source->audio) == source) {
                queue_selected_samples(source, samples, count, stride, silent);
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

/* Opens the streams the policy asks for and closes the ones it does not, so a
   change of default input takes effect without restarting the daemon. */
static void apply_source_policy(struct vi_audio *audio) {
    if (!audio->active) return;
    for (size_t i = 0; i < VI_MAX_SOURCES; ++i) {
        struct vi_source *source = &audio->sources[i];
        if (source->id == SPA_ID_INVALID) continue;
        const bool wanted = source_wanted(audio, source);
        if (wanted && source->stream == NULL) {
            if (start_source(source) < 0) {
                fprintf(stderr, "voice-inputd: cannot listen to source: %s\n",
                        source->description);
            }
        } else if (!wanted && source->stream != NULL) {
            if (audio->selection.selected == (int)i) {
                audio->selection.selected = VI_NO_SOURCE;
                discard_captured_audio(audio);
            }
            stop_source(source);
        }
    }
}

static int on_metadata_property(void *data, uint32_t subject, const char *key,
                                const char *type, const char *value) {
    (void)type;
    struct vi_audio *audio = data;
    if (subject != PW_ID_CORE || key == NULL ||
        strcmp(key, "default.audio.source") != 0) {
        return 0;
    }
    char name[sizeof(audio->default_source)];
    /* The value is a JSON object naming the node, e.g. {"name":"alsa_input…"}. */
    if (value == NULL || vi_json_field(value, "name", name, sizeof(name)) <= 0) {
        return 0;
    }
    if (strcmp(name, audio->default_source) == 0) return 0;
    snprintf(audio->default_source, sizeof(audio->default_source), "%s", name);
    fprintf(stderr, "voice-inputd: desktop default input is %s\n",
            audio->default_source);
    apply_source_policy(audio);
    return 0;
}

static const struct pw_metadata_events metadata_events = {
    PW_VERSION_METADATA_EVENTS,
    .property = on_metadata_property,
};

static bool is_audio_source(const char *media_class) {
    /* Audio/Duplex covers combined capture/playback nodes, which is how several
       laptop codecs (including AMD ACP digital microphone arrays) expose their
       only usable input. */
    return media_class != NULL &&
           (strcmp(media_class, "Audio/Source") == 0 ||
            strncmp(media_class, "Audio/Source/", 13U) == 0 ||
            strcmp(media_class, "Audio/Duplex") == 0);
}

static void on_registry_global(void *data, uint32_t id, uint32_t permissions,
                               const char *type, uint32_t version,
                               const struct spa_dict *props) {
    (void)permissions;
    (void)version;
    struct vi_audio *audio = data;
    if (strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
        const char *metadata_name =
            props != NULL ? spa_dict_lookup(props, PW_KEY_METADATA_NAME) : NULL;
        if (audio->metadata != NULL || metadata_name == NULL ||
            strcmp(metadata_name, "default") != 0) {
            return;
        }
        audio->metadata = pw_registry_bind(audio->registry, id, type,
                                           PW_VERSION_METADATA, 0U);
        if (audio->metadata != NULL) {
            pw_metadata_add_listener(audio->metadata, &audio->metadata_listener,
                                     &metadata_events, audio);
        }
        return;
    }
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
        apply_source_policy(audio);
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
        if (audio->selection.selected == (int)i) {
            audio->selection.selected = VI_NO_SOURCE;
        }
        if (audio->selection.candidate == (int)i) {
            audio->selection.candidate = VI_NO_SOURCE;
            audio->selection.candidate_votes = 0U;
        }
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
    vi_selection_reset(&audio->selection);
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
    const char *source_setting = getenv("VOICE_INPUT_SOURCE");
    if (source_setting == NULL || source_setting[0] == '\0' ||
        strcmp(source_setting, "default") == 0) {
        audio->mode = VI_SOURCE_SYSTEM_DEFAULT;
    } else if (strcmp(source_setting, "auto") == 0) {
        audio->mode = VI_SOURCE_AUTO;
        fprintf(stderr, "voice-inputd: scoring every input in parallel\n");
    } else {
        audio->mode = VI_SOURCE_NAMED;
        snprintf(audio->wanted, sizeof(audio->wanted), "%s", source_setting);
        fprintf(stderr, "voice-inputd: pinned to the input matching \"%s\"\n",
                audio->wanted);
    }
    audio->max_gain = environment_float("VOICE_INPUT_MAX_GAIN", 6.0F, 1.0F, 16.0F);
    audio->target_rms = environment_float("VOICE_INPUT_TARGET_RMS", 0.03F,
                                          0.01F, 0.30F);
    audio->gain = 1.0F;
    audio->preroll_ms = (long)environment_float("VOICE_INPUT_PREROLL_MS", 0.0F,
                                                0.0F, 3000.0F);
    if (audio->preroll_ms > 0L) {
        /* Sources discovered from now on start capturing immediately, so the
           ring is always warm. This holds the microphone open for as long as the
           daemon runs, which is why it is opt-in. */
        audio->active = true;
        fprintf(stderr, "voice-inputd: pre-roll enabled (%ld ms); the microphone "
                        "stays open while the daemon runs\n", audio->preroll_ms);
    }
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
    audio->recording = false;
    if (audio->preroll_ms > 0L) return;  /* keep capturing to preserve pre-roll */
    for (size_t i = 0; i < VI_MAX_SOURCES; ++i) stop_source(&audio->sources[i]);
    audio->active = false;
    vi_selection_reset(&audio->selection);
}

void vi_audio_destroy(struct vi_audio *audio) {
    if (audio == NULL) return;
    vi_audio_stop(audio);
    if (audio->metadata != NULL) pw_proxy_destroy((struct pw_proxy *)audio->metadata);
    if (audio->registry != NULL) pw_proxy_destroy((struct pw_proxy *)audio->registry);
    if (audio->core != NULL) pw_core_disconnect(audio->core);
    if (audio->context != NULL) pw_context_destroy(audio->context);
    if (audio->loop != NULL) pw_main_loop_destroy(audio->loop);
    free(audio);
    pw_deinit();
}

static void rewind_to_preroll(struct vi_audio *audio) {
    const uint64_t write_position = atomic_load_explicit(&audio->write_position,
                                                         memory_order_acquire);
    const uint64_t boundary = atomic_load_explicit(&audio->boundary_position,
                                                   memory_order_acquire);
    uint64_t wanted = (uint64_t)audio->preroll_ms * 16U;  /* 16 kHz mono */
    if (wanted > VI_RING_SAMPLES) wanted = VI_RING_SAMPLES;
    const uint64_t earliest = write_position > wanted ? write_position - wanted : 0U;
    audio->read_position = earliest > boundary ? earliest : boundary;
}

int vi_audio_start(struct vi_audio *audio) {
    if (audio == NULL) return -1;
    audio->recording = true;
    audio->gain = 1.0F;
    if (audio->active) {
        /* Capture never stopped, so the ring already holds what was said just
           before the trigger. Rewinding into it hides the whole start-up path -
           hotkey dispatch, stream negotiation - from the recogniser. */
        rewind_to_preroll(audio);
        return 0;
    }
    audio->active = true;
    vi_selection_reset(&audio->selection);
    apply_source_policy(audio);
    int started = 0;
    for (size_t i = 0; i < VI_MAX_SOURCES; ++i) {
        if (audio->sources[i].stream != NULL) ++started;
    }
    if (started == 0) {
        fprintf(stderr, "voice-inputd: no capture source available; check that "
                        "PipeWire exposes an Audio/Source node and that it is "
                        "not muted (voice-inputctl sources)\n");
    } else {
        fprintf(stderr, "voice-inputd: parallel capture started on %d source(s)\n",
                started);
    }
    return 0;
}

int vi_audio_iterate(struct vi_audio *audio, int timeout_ms) {
    if (audio == NULL) return -1;
    return pw_loop_iterate(pw_main_loop_get_loop(audio->loop), timeout_ms);
}

size_t vi_audio_read(struct vi_audio *audio, float *samples, size_t capacity) {
    if (audio == NULL || samples == NULL) return 0U;
    const uint64_t write_position = atomic_load_explicit(&audio->write_position,
                                                         memory_order_acquire);
    const uint64_t boundary = atomic_load_explicit(&audio->boundary_position,
                                                   memory_order_acquire);
    if (audio->read_position < boundary) audio->read_position = boundary;
    if (write_position - audio->read_position > VI_RING_SAMPLES) {
        /* The reader fell further behind than the ring holds; keep the newest
           audio rather than replaying what has already been overwritten. */
        audio->read_position = write_position - VI_RING_SAMPLES;
    }
    size_t count = (size_t)(write_position - audio->read_position);
    if (count > capacity) count = capacity;
    for (size_t i = 0; i < count; ++i) {
        samples[i] = audio->samples[(audio->read_position + i) % VI_RING_SAMPLES];
    }
    audio->read_position += count;
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
    if (!audio->recording) return "idle";
    for (size_t i = 0; i < VI_MAX_SOURCES; ++i) {
        if (audio->sources[i].state == PW_STREAM_STATE_STREAMING) return "streaming";
    }
    return "connecting";
}

bool vi_audio_is_active(const struct vi_audio *audio) {
    return audio != NULL && audio->recording;
}

const char *vi_audio_selected_source(const struct vi_audio *audio) {
    return audio != NULL && audio->selection.selected != VI_NO_SOURCE
               ? audio->sources[audio->selection.selected].description
               : "";
}

const char *vi_audio_source_mode(const struct vi_audio *audio) {
    if (audio == NULL) return "disabled";
    switch (audio->mode) {
    case VI_SOURCE_AUTO: return "auto";
    case VI_SOURCE_NAMED: return audio->wanted;
    default: return "default";
    }
}

size_t vi_audio_source_count(const struct vi_audio *audio) {
    if (audio == NULL) return 0U;
    size_t count = 0U;
    for (size_t i = 0; i < VI_MAX_SOURCES; ++i) {
        if (audio->sources[i].id != SPA_ID_INVALID) ++count;
    }
    return count;
}

static const char *stream_state_name(enum pw_stream_state state) {
    switch (state) {
    case PW_STREAM_STATE_ERROR: return "error";
    case PW_STREAM_STATE_UNCONNECTED: return "unconnected";
    case PW_STREAM_STATE_CONNECTING: return "connecting";
    case PW_STREAM_STATE_PAUSED: return "paused";
    case PW_STREAM_STATE_STREAMING: return "streaming";
    default: return "unknown";
    }
}

int vi_audio_describe_sources(const struct vi_audio *audio, char *buffer,
                              size_t size) {
    if (buffer == NULL || size == 0U) return -1;
    char selected[512];
    if (vi_json_escape(selected, sizeof(selected),
                       vi_audio_selected_source(audio)) < 0) {
        return -1;
    }
    int written = snprintf(buffer, size,
                           "{\"event\":\"sources\",\"active\":%s,"
                           "\"mode\":\"%s\",\"selected\":\"%s\",\"items\":[",
                           vi_audio_is_active(audio) ? "true" : "false",
                           vi_audio_source_mode(audio), selected);
    if (written < 0 || (size_t)written >= size) return -1;
    size_t used = (size_t)written;

    bool first = true;
    for (size_t i = 0; audio != NULL && i < VI_MAX_SOURCES; ++i) {
        const struct vi_source *source = &audio->sources[i];
        if (source->id == SPA_ID_INVALID) continue;
        char name[512];
        char description[512];
        if (vi_json_escape(name, sizeof(name), source->name) < 0 ||
            vi_json_escape(description, sizeof(description),
                           source->description) < 0) {
            return -1;
        }
        written = snprintf(buffer + used, size - used,
                           "%s{\"id\":%u,\"name\":\"%s\",\"description\":\"%s\","
                           "\"state\":\"%s\",\"chunks\":%u,\"rms\":%.6f,"
                           "\"noise\":%.6f,\"score\":%.2f,\"selected\":%s}",
                           first ? "" : ",", source->id, name, description,
                           source->stream != NULL
                               ? stream_state_name(source->state)
                               : "idle",
                           source->chunks, (double)source->rms,
                           (double)source->noise_floor, (double)source->score,
                           audio->selection.selected == (int)i ? "true" : "false");
        if (written < 0 || (size_t)written >= size - used) return -1;
        used += (size_t)written;
        first = false;
    }

    if (used + 3U >= size) return -1;
    memcpy(buffer + used, "]}\n", 4);
    return (int)(used + 3U);
}
