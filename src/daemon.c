#define _POSIX_C_SOURCE 200809L

#include "audio.h"
#include "asr.h"
#include "protocol.h"
#include "output.h"

#include <errno.h>
#include <fcntl.h>
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
    bool no_audio;
    bool recording;
    bool running;
    float pending_level;
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

static void send_to_client(struct app *app, size_t index, const char *message) {
    ssize_t sent = send(app->clients[index], message, strlen(message), MSG_NOSIGNAL);
    if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) remove_client(app, index);
}

static void broadcast(struct app *app, const char *message) {
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        if (app->clients[i] >= 0) send_to_client(app, i, message);
    }
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

static int set_recording(struct app *app, bool recording) {
    if (recording == app->recording) {
        broadcast_state(app, "state");
        return 0;
    }
    if (recording && !app->no_audio && vi_audio_start(app->audio) < 0) {
        broadcast(app, "{\"event\":\"error\",\"message\":\"pipewire-start-failed\"}\n");
        return -1;
    }
    if (!recording && !app->no_audio) {
        vi_audio_stop(app->audio);
        vi_asr_finish(app->asr);
    }
    app->recording = recording;
    broadcast_state(app, "state");
    return 0;
}

static void handle_command(struct app *app, size_t index, const char *line) {
    enum vi_command command = vi_parse_command(line);
    switch (command) {
    case VI_COMMAND_STATUS:
        broadcast_state(app, "state");
        break;
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

static void on_transcript(const char *event, const char *text, void *userdata) {
    struct app *app = userdata;
    char message[8192];
    if (vi_json_text(message, sizeof(message), event, text) >= 0) {
        broadcast(app, message);
    }
    if (strcmp(event, "final") == 0 && vi_output_commit(text) < 0) {
        broadcast(app, "{\"event\":\"output-error\",\"backend\":\"fcitx5\"}\n");
    }
}

static void process_audio(struct app *app) {
    if (!app->recording || app->asr == NULL) return;
    float samples[4096];
    size_t count;
    while ((count = vi_audio_read(app->audio, samples, 4096)) > 0) {
        (void)vi_asr_accept(app->asr, samples, count);
    }
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
    fprintf(stream, "Usage: voice-inputd [--socket PATH] [--model DIR] "
                    "[--threads N] [--no-audio] [--version]\n");
}

int main(int argc, char **argv) {
    char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    if (vi_runtime_socket_path(socket_path, sizeof(socket_path)) < 0) {
        perror("voice-inputd: runtime path");
        return EXIT_FAILURE;
    }
    bool no_audio = false;
    const char *model_directory = getenv("VOICE_INPUT_MODEL_DIR");
    int asr_threads = 2;
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
        } else if (strcmp(argv[i], "--version") == 0) {
            puts("voice-inputd " VOICE_INPUT_VERSION);
            return EXIT_SUCCESS;
        } else {
            usage(stderr);
            return EXIT_FAILURE;
        }
    }

    struct app app = { .server_fd = -1, .audio = NULL, .asr = NULL,
                       .no_audio = no_audio,
                       .recording = false, .running = true, .pending_level = -1.0F };
    for (size_t i = 0; i < MAX_CLIENTS; ++i) app.clients[i] = -1;
    if (!no_audio) {
        app.audio = vi_audio_create(on_level, &app);
        if (app.audio == NULL) {
            fputs("voice-inputd: failed to initialize PipeWire\n", stderr);
            return EXIT_FAILURE;
        }
        if (model_directory != NULL && model_directory[0] != '\0') {
            app.asr = vi_asr_create(model_directory, asr_threads,
                                    on_transcript, &app);
            if (app.asr == NULL) {
                fprintf(stderr, "voice-inputd: failed to load ASR model from %s\n",
                        model_directory);
                vi_audio_destroy(app.audio);
                return EXIT_FAILURE;
            }
            fprintf(stderr, "voice-inputd: ASR model loaded from %s\n",
                    model_directory);
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
    return EXIT_SUCCESS;
}
