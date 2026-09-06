#define _POSIX_C_SOURCE 200809L

#include "audio.h"
#include "asr.h"
#include "protocol.h"
#include "output.h"
#include "punctuation.h"

#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS 16

struct app {
    int server_fd;
    int clients[MAX_CLIENTS];
    struct vi_audio *audio;
    struct vi_asr *asr;
    struct vi_punctuation *punctuation;
    bool punctuation_wanted;
    bool no_audio;
    bool recording;
    bool running;
    float pending_level;
    bool first_audio_logged;
    size_t accepted_samples;
    size_t window_samples;
    double window_squares;
    float window_peak;
    long last_throughput_ms;
    long tail_until_ms;
    struct timespec last_level_sent;
    char selected_source[256];
};

static volatile sig_atomic_t stop_requested = 0;

static void on_signal(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int make_parent_directory(const char *socket_path) {
    char directory[sizeof(((struct sockaddr_un *)0)->sun_path)];
    size_t length = strlen(socket_path);
    if (length >= sizeof(directory)) return -1;
    memcpy(directory, socket_path, length + 1);
    char *slash = strrchr(directory, '/');
    if (slash == NULL) return -1;
    *slash = '\0';
    for (char *cursor = directory + 1; *cursor != '\0'; ++cursor) {
        if (*cursor != '/') continue;
        *cursor = '\0';
        if (mkdir(directory, 0700) < 0 && errno != EEXIST) return -1;
        *cursor = '/';
    }
    if (mkdir(directory, 0700) < 0 && errno != EEXIST) return -1;
    return 0;
}

static int create_server(const char *path) {
    if (make_parent_directory(path) < 0) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un address = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(address.sun_path)) {
        close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(address.sun_path, path);
    unlink(path);
    if (bind(fd, (const struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(fd, MAX_CLIENTS) < 0 || set_nonblocking(fd) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void remove_client(struct app *app, size_t index) {
    close(app->clients[index]);
    app->clients[index] = -1;
}

/* VOICE_INPUT_DEBUG_TIMING traces the path from the start command to the first
   committed text with millisecond stamps, so a perceived delay can be attributed
   to capture, decoding or output instead of guessed at. */
static bool debug_timing = false;
static long timing_origin_ms = 0;

static long monotonic_ms(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000L + now.tv_nsec / 1000000L;
}

static void timing_log(const char *format, ...) {
    if (!debug_timing) return;
    fprintf(stderr, "voice-inputd: +%5ld ms  ", monotonic_ms() - timing_origin_ms);
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
}

/* Clients are non-blocking and never written to by the UI, so a stalled overlay
   can only ever cost it events, never stall recognition. */
static void send_to_client(struct app *app, size_t index, const char *message) {
    size_t remaining = strlen(message);
    bool partial = false;
    while (remaining > 0U) {
        const ssize_t sent = send(app->clients[index], message, remaining,
                                  MSG_NOSIGNAL);
        if (sent > 0) {
            message += sent;
            remaining -= (size_t)sent;
            partial = true;
            continue;
        }
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && !partial) {
            return;  /* nothing went out: skip this event rather than block */
        }
        /* Half an event was written, so the client's stream can no longer be
           parsed; drop it and let it reconnect on a clean one. */
        remove_client(app, index);
        return;
    }
}

static void broadcast(struct app *app, const char *message) {
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        if (app->clients[i] >= 0) send_to_client(app, i, message);
    }
}

static const char *punctuation_state(const struct app *app) {
    if (!app->punctuation_wanted) return "disabled";
    return app->punctuation != NULL ? "enabled" : "unavailable";
}

static const char *current_audio_state(const struct app *app) {
    return app->no_audio ? "disabled" : vi_audio_state(app->audio);
}

static void broadcast_state(struct app *app, const char *event) {
    char message[256];
    vi_json_state(message, sizeof(message), event, app->recording,
                  current_audio_state(app), vi_asr_state(app->asr));
    broadcast(app, message);
}

/* Speech trails off, so cutting capture the instant the hotkey is released
   clips the last syllable. Keep recording for a short tail and only then let the
   recogniser finalise. */
static long tail_ms = 250L;

static void finish_recording(struct app *app) {
    if (!app->no_audio) {
        vi_audio_stop(app->audio);
        vi_asr_finish(app->asr);
    }
    app->recording = false;
    app->tail_until_ms = 0L;
    timing_log("recording finished");
    broadcast_state(app, "state");
}

static void maybe_finish_recording(struct app *app) {
    if (app->tail_until_ms == 0L || monotonic_ms() < app->tail_until_ms) return;
    finish_recording(app);
}

static int set_recording(struct app *app, bool recording) {
    if (recording && app->tail_until_ms != 0L) {
        app->tail_until_ms = 0L;  /* speaking again during the tail: carry on */
        broadcast_state(app, "state");
        return 0;
    }
    if (recording == app->recording) {
        broadcast_state(app, "state");
        return 0;
    }
    if (recording) {
        timing_origin_ms = monotonic_ms();
        app->first_audio_logged = false;
        app->accepted_samples = 0U;
        app->window_samples = 0U;
        app->window_squares = 0.0;
        app->window_peak = 0.0F;
        app->last_throughput_ms = 0L;
        timing_log("start command accepted");
        if (!app->no_audio && vi_audio_start(app->audio) < 0) {
            broadcast(app,
                      "{\"event\":\"error\",\"message\":\"pipewire-start-failed\"}\n");
            return -1;
        }
        timing_log("capture streams requested");
        app->recording = true;
        broadcast_state(app, "state");
        return 0;
    }
    if (tail_ms > 0L && !app->no_audio) {
        /* Stay in the recording state until the tail is in, so the panel does
           not announce a result the recogniser has not produced yet. */
        app->tail_until_ms = monotonic_ms() + tail_ms;
        timing_log("stop accepted; capturing a %ld ms tail", tail_ms);
        return 0;
    }
    finish_recording(app);
    return 0;
}

static void handle_command(struct app *app, size_t index, const char *line) {
    enum vi_command command = vi_parse_command(line);
    switch (command) {
    case VI_COMMAND_STATUS: {
        broadcast_state(app, "state");
        struct vi_status status = {
            .recording = app->recording,
            .audio = current_audio_state(app),
            .asr = vi_asr_state(app->asr),
            .asr_backend = app->asr != NULL ? "sherpa-cpu" : "none",
            .asr_model = vi_asr_model_name(app->asr),
            .asr_kind = vi_asr_model_kind(app->asr),
            .decoder = vi_asr_decoder(app->asr),
            .threads = vi_asr_threads(app->asr),
            .punctuation = punctuation_state(app),
            .punctuation_model = vi_punctuation_model(app->punctuation),
            .sample_rate = 16000,
            .tail_ms = tail_ms,
        };
        char message[1024];
        if (vi_json_info(message, sizeof(message), &status) > 0) {
            send_to_client(app, index, message);
        }
        break;
    }
    case VI_COMMAND_START:
        (void)set_recording(app, true);
        break;
    case VI_COMMAND_STOP:
        (void)set_recording(app, false);
        break;
    case VI_COMMAND_TOGGLE:
        (void)set_recording(app, !app->recording);
        break;
    case VI_COMMAND_SOURCES: {
        static char message[16384];
        if (vi_audio_describe_sources(app->no_audio ? NULL : app->audio,
                                      message, sizeof(message)) > 0) {
            send_to_client(app, index, message);
        } else {
            send_to_client(app, index,
                           "{\"event\":\"error\",\"message\":\"sources-failed\"}\n");
        }
        break;
    }
    case VI_COMMAND_QUIT:
        send_to_client(app, index, "{\"event\":\"stopping\"}\n");
        app->running = false;
        break;
    default:
        send_to_client(app, index,
                       "{\"event\":\"error\",\"message\":\"invalid-command\"}\n");
        break;
    }
}

static void accept_clients(struct app *app) {
    for (;;) {
        int client = accept(app->server_fd, NULL, NULL);
        if (client < 0) return;
        if (set_nonblocking(client) < 0 || fcntl(client, F_SETFD, FD_CLOEXEC) < 0) {
            close(client);
            continue;
        }
        size_t slot = MAX_CLIENTS;
        for (size_t i = 0; i < MAX_CLIENTS; ++i) {
            if (app->clients[i] < 0) { slot = i; break; }
        }
        if (slot == MAX_CLIENTS) {
            close(client);
            continue;
        }
        app->clients[slot] = client;
        char hello[256];
        vi_json_state(hello, sizeof(hello), "hello", app->recording,
                      current_audio_state(app), vi_asr_state(app->asr));
        send_to_client(app, slot, hello);
    }
}

static void read_clients(struct app *app) {
    char buffer[256];
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        if (app->clients[i] < 0) continue;
        ssize_t count = recv(app->clients[i], buffer, sizeof(buffer) - 1, MSG_DONTWAIT);
        if (count == 0) {
            remove_client(app, i);
        } else if (count > 0) {
            buffer[count] = '\0';
            char *save = NULL;
            for (char *line = strtok_r(buffer, "\r\n", &save); line != NULL;
                 line = strtok_r(NULL, "\r\n", &save)) {
                handle_command(app, i, line);
            }
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            remove_client(app, i);
        }
    }
}

static void on_level(float rms, void *userdata) {
    struct app *app = userdata;
    app->pending_level = rms;
}

/* Partials are redrawn several times a second and are never committed, so they
   stay on the fast path untouched. Only the final text is punctuated, once,
   immediately before it is handed to Fcitx5. */
static void on_transcript(const char *event, const char *text, void *userdata) {
    struct app *app = userdata;
    timing_log("%s: %s", event, text);
    if (strcmp(event, "final") != 0) {
        char partial[8192];
        if (vi_json_text(partial, sizeof(partial), event, text) >= 0) {
            broadcast(app, partial);
        }
        return;
    }

    static char final_text[8192];
    (void)snprintf(final_text, sizeof(final_text), "%s", text);
    if (app->punctuation != NULL) {
        const long before = monotonic_ms();
        if (vi_punctuation_apply(app->punctuation, text, final_text,
                                 sizeof(final_text)) < 0) {
            timing_log("punctuation failed; committing the raw text");
        } else {
            timing_log("punctuated in %ld ms: %s", monotonic_ms() - before,
                       final_text);
        }
    }
    char message[8192];
    if (vi_json_text(message, sizeof(message), event, final_text) >= 0) {
        broadcast(app, message);
    }
    const long before = monotonic_ms();
    const int committed = vi_output_commit(final_text);
    timing_log("fcitx commit %s in %ld ms",
               committed < 0 ? "failed" : "done", monotonic_ms() - before);
    if (committed < 0) {
        broadcast(app, "{\"event\":\"output-error\",\"backend\":\"fcitx5\"}\n");
    }
}

static void process_audio(struct app *app) {
    if (!app->recording || app->asr == NULL) return;
    float samples[4096];
    size_t count;
    while ((count = vi_audio_read(app->audio, samples, 4096)) > 0) {
        if (!app->first_audio_logged) {
            app->first_audio_logged = true;
            timing_log("first audio reached ASR (%zu samples)", count);
        }
        app->accepted_samples += count;
        if (debug_timing) {
            app->window_samples += count;
            for (size_t i = 0; i < count; ++i) {
                app->window_squares += (double)samples[i] * (double)samples[i];
                const float magnitude = samples[i] < 0.0F ? -samples[i] : samples[i];
                if (magnitude > app->window_peak) app->window_peak = magnitude;
            }
        }
        const long before = monotonic_ms();
        (void)vi_asr_accept(app->asr, samples, count);
        const long spent = monotonic_ms() - before;
        if (spent >= 20L) {
            timing_log("ASR decode of %zu samples took %ld ms", count, spent);
        }
    }
}

/* Audio fed to the recogniser should track wall clock almost exactly; a ratio
   well below 1 means capture is starving it, which looks the same to the user as
   a slow model. */
static void maybe_log_throughput(struct app *app) {
    if (!debug_timing || !app->recording) return;
    const long elapsed = monotonic_ms() - timing_origin_ms;
    if (elapsed - app->last_throughput_ms < 1000L) return;
    app->last_throughput_ms = elapsed;
    const double fed_ms = (double)app->accepted_samples / 16.0;
    /* The level of what actually reaches the recogniser, after source
       selection and gain. Silence here with a moving level meter means the
       wrong source was chosen; silence in both means nothing is being
       captured at all. */
    const double level = app->window_samples > 0
        ? sqrt(app->window_squares / (double)app->window_samples)
        : 0.0;
    timing_log("fed %.0f ms of audio over %ld ms wall (%.2fx realtime), "
               "rms %.4f peak %.3f from %s",
               fed_ms, elapsed, elapsed > 0 ? fed_ms / (double)elapsed : 0.0,
               level, (double)app->window_peak,
               app->no_audio ? "none" : vi_audio_selected_source(app->audio));
    app->window_squares = 0.0;
    app->window_samples = 0U;
    app->window_peak = 0.0F;
}

static void maybe_broadcast_level(struct app *app) {
    if (!app->recording || app->pending_level < 0.0F) return;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long elapsed_ms = (now.tv_sec - app->last_level_sent.tv_sec) * 1000L +
                      (now.tv_nsec - app->last_level_sent.tv_nsec) / 1000000L;
    if (elapsed_ms < 80L) return;
    char message[128];
    snprintf(message, sizeof(message), "{\"event\":\"level\",\"rms\":%.4f}\n",
             (double)app->pending_level);
    broadcast(app, message);
    app->pending_level = -1.0F;
    app->last_level_sent = now;
}

static void maybe_broadcast_source(struct app *app) {
    if (!app->recording || app->no_audio) return;
    const char *source = vi_audio_selected_source(app->audio);
    if (strcmp(source, app->selected_source) == 0) return;
    snprintf(app->selected_source, sizeof(app->selected_source), "%s", source);
    char message[768];
    if (vi_json_text(message, sizeof(message), "source", source) >= 0) {
        broadcast(app, message);
    }
}

static void usage(FILE *stream) {
    fprintf(stream,
            "Usage: voice-inputd [--socket PATH] [--model DIR] [--threads N]\n"
            "                    [--decoder greedy_search|modified_beam_search]\n"
            "                    [--punct-model DIR] [--no-punctuation]\n"
            "                    [--no-audio] [--version]\n");
}

/* Configuration comes from the environment so a model can be swapped between
   two runs of the same build; the command line overrides it for one run. */
static const char *environment_text(const char *name, const char *fallback) {
    const char *value = getenv(name);
    return value != NULL && value[0] != '\0' ? value : fallback;
}

static long environment_long(const char *name, long fallback, long low,
                             long high) {
    const char *value = getenv(name);
    if (value == NULL || value[0] == '\0') return fallback;
    char *end = NULL;
    const long parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < low || parsed > high) {
        fprintf(stderr, "voice-inputd: ignoring %s=%s\n", name, value);
        return fallback;
    }
    return parsed;
}

static float environment_seconds(const char *name, float fallback) {
    const long milliseconds =
        environment_long(name, (long)(fallback * 1000.0F), 0L, 60000L);
    return (float)milliseconds / 1000.0F;
}

int main(int argc, char **argv) {
    char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    if (vi_runtime_socket_path(socket_path, sizeof(socket_path)) < 0) {
        perror("voice-inputd: runtime path");
        return EXIT_FAILURE;
    }
    bool no_audio = false;
    debug_timing = getenv("VOICE_INPUT_DEBUG_TIMING") != NULL;
    const char *tail_setting = getenv("VOICE_INPUT_TAIL_MS");
    if (tail_setting != NULL && tail_setting[0] != '\0') {
        char *end = NULL;
        const long parsed = strtol(tail_setting, &end, 10);
        if (*end == '\0' && parsed >= 0L && parsed <= 2000L) tail_ms = parsed;
    }
    struct vi_asr_config asr_config;
    vi_asr_config_defaults(&asr_config);
    /* VOICE_INPUT_MODEL_DIR is what the Nix wrapper sets; VOICE_INPUT_ASR_MODEL
       is the name the other settings share and wins when both are present. */
    const char *model_directory =
        environment_text("VOICE_INPUT_ASR_MODEL",
                         getenv("VOICE_INPUT_MODEL_DIR"));
    asr_config.decoding_method =
        environment_text("VOICE_INPUT_ASR_DECODER", asr_config.decoding_method);
    asr_config.threads =
        (int)environment_long("VOICE_INPUT_ASR_THREADS", asr_config.threads, 1, 32);
    asr_config.max_active_paths =
        (int)environment_long("VOICE_INPUT_ASR_MAX_ACTIVE_PATHS",
                              asr_config.max_active_paths, 1, 64);
    asr_config.hotwords_file = getenv("VOICE_INPUT_ASR_HOTWORDS");
    asr_config.prefer_int8 = environment_long("VOICE_INPUT_ASR_INT8", 1, 0, 1) != 0;
    asr_config.rule1_min_trailing_silence =
        environment_seconds("VOICE_INPUT_ENDPOINT_RULE1_MS",
                            asr_config.rule1_min_trailing_silence);
    asr_config.rule2_min_trailing_silence =
        environment_seconds("VOICE_INPUT_ENDPOINT_RULE2_MS",
                            asr_config.rule2_min_trailing_silence);
    const char *punctuation_directory =
        environment_text("VOICE_INPUT_PUNCT_MODEL",
                         getenv("VOICE_INPUT_PUNCT_MODEL_DIR"));
    bool punctuation_wanted =
        environment_long("VOICE_INPUT_PUNCTUATION", 1, 0, 1) != 0;
    const int punctuation_threads =
        (int)environment_long("VOICE_INPUT_PUNCT_THREADS", 1, 1, 32);
    int asr_threads = asr_config.threads;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            if (strlen(argv[++i]) >= sizeof(socket_path)) return EXIT_FAILURE;
            strcpy(socket_path, argv[i]);
        } else if (strcmp(argv[i], "--no-audio") == 0) {
            no_audio = true;
        } else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            model_directory = argv[++i];
        } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            asr_threads = atoi(argv[++i]);
            if (asr_threads < 1 || asr_threads > 32) return EXIT_FAILURE;
        } else if (strcmp(argv[i], "--decoder") == 0 && i + 1 < argc) {
            asr_config.decoding_method = argv[++i];
        } else if (strcmp(argv[i], "--punct-model") == 0 && i + 1 < argc) {
            punctuation_directory = argv[++i];
        } else if (strcmp(argv[i], "--no-punctuation") == 0) {
            punctuation_wanted = false;
        } else if (strcmp(argv[i], "--version") == 0) {
            puts("voice-inputd " VOICE_INPUT_VERSION);
            return EXIT_SUCCESS;
        } else {
            usage(stderr);
            return EXIT_FAILURE;
        }
    }

    asr_config.model_directory = model_directory;
    asr_config.threads = asr_threads;

    struct app app = { .server_fd = -1, .audio = NULL, .asr = NULL,
                       .punctuation = NULL,
                       /* --no-audio never loads a recogniser, so there is
                          nothing to punctuate and no model worth loading. */
                       .punctuation_wanted = punctuation_wanted && !no_audio,
                       .no_audio = no_audio,
                       .recording = false, .running = true, .pending_level = -1.0F,
                       .first_audio_logged = false, .tail_until_ms = 0L };
    for (size_t i = 0; i < MAX_CLIENTS; ++i) app.clients[i] = -1;
    if (!no_audio) {
        app.audio = vi_audio_create(on_level, &app);
        if (app.audio == NULL) {
            fputs("voice-inputd: failed to initialize PipeWire\n", stderr);
            return EXIT_FAILURE;
        }
        if (model_directory != NULL && model_directory[0] != '\0') {
            app.asr = vi_asr_create(&asr_config, on_transcript, &app);
            if (app.asr == NULL) {
                fprintf(stderr, "voice-inputd: failed to load ASR model from %s\n",
                        model_directory);
                vi_audio_destroy(app.audio);
                return EXIT_FAILURE;
            }
            fprintf(stderr,
                    "voice-inputd: ASR model loaded from %s (%s, %s, %d threads)\n",
                    model_directory, vi_asr_model_kind(app.asr),
                    vi_asr_decoder(app.asr), vi_asr_threads(app.asr));
        }
        /* Punctuation is a second model on the commit path. It is optional on
           purpose: the daemon has to stay usable when it is missing, so a
           failure here is reported and then ignored. */
        if (punctuation_wanted && punctuation_directory != NULL &&
            punctuation_directory[0] != '\0') {
            const long before = monotonic_ms();
            app.punctuation =
                vi_punctuation_create(punctuation_directory, punctuation_threads);
            if (app.punctuation != NULL) {
                fprintf(stderr,
                        "voice-inputd: punctuation model loaded from %s in %ld ms\n",
                        punctuation_directory, monotonic_ms() - before);
            } else {
                fprintf(stderr,
                        "voice-inputd: punctuation unavailable; committing "
                        "unpunctuated text\n");
            }
        }
    }
    app.server_fd = create_server(socket_path);
    if (app.server_fd < 0) {
        perror("voice-inputd: create socket");
        vi_audio_destroy(app.audio);
        vi_asr_destroy(app.asr);
        return EXIT_FAILURE;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    fprintf(stderr, "voice-inputd: listening on %s\n", socket_path);

    while (app.running && !stop_requested) {
        accept_clients(&app);
        read_clients(&app);
        if (!no_audio) (void)vi_audio_iterate(app.audio, 10);
        else {
            const struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000L };
            nanosleep(&delay, NULL);
        }
        process_audio(&app);
        maybe_finish_recording(&app);
        maybe_log_throughput(&app);
        maybe_broadcast_source(&app);
        maybe_broadcast_level(&app);
    }

    set_recording(&app, false);
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        if (app.clients[i] >= 0) close(app.clients[i]);
    }
    close(app.server_fd);
    unlink(socket_path);
    vi_audio_destroy(app.audio);
    vi_asr_destroy(app.asr);
    vi_punctuation_destroy(app.punctuation);
    return EXIT_SUCCESS;
}
