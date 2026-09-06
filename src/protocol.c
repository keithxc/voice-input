#define _POSIX_C_SOURCE 200809L

#include "protocol.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

enum vi_command vi_parse_command(const char *line) {
    if (line == NULL) {
        return VI_COMMAND_INVALID;
    }

    char word[32] = {0};
    if (sscanf(line, " %31s", word) != 1) {
        return VI_COMMAND_INVALID;
    }
    if (strcmp(word, "status") == 0) return VI_COMMAND_STATUS;
    if (strcmp(word, "start") == 0) return VI_COMMAND_START;
    if (strcmp(word, "stop") == 0) return VI_COMMAND_STOP;
    if (strcmp(word, "toggle") == 0) return VI_COMMAND_TOGGLE;
    if (strcmp(word, "quit") == 0) return VI_COMMAND_QUIT;
    return VI_COMMAND_INVALID;
}

const char *vi_command_name(enum vi_command command) {
    switch (command) {
    case VI_COMMAND_STATUS: return "status";
    case VI_COMMAND_START: return "start";
    case VI_COMMAND_STOP: return "stop";
    case VI_COMMAND_TOGGLE: return "toggle";
    case VI_COMMAND_QUIT: return "quit";
    default: return "invalid";
    }
}

int vi_runtime_socket_path(char *buffer, size_t size) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    char fallback[64];
    if (runtime == NULL || runtime[0] == '\0') {
        int written = snprintf(fallback, sizeof(fallback), "/tmp/voice-input-%ld",
                               (long)getuid());
        if (written < 0 || (size_t)written >= sizeof(fallback)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        runtime = fallback;
    }
    int written = snprintf(buffer, size, "%s/voice-input/voice-input.sock", runtime);
    if (written < 0 || (size_t)written >= size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

int vi_json_state(char *buffer, size_t size, const char *event, bool recording,
                  const char *audio_state) {
    return snprintf(buffer, size,
                    "{\"event\":\"%s\",\"recording\":%s,\"audio\":\"%s\"}\n",
                    event, recording ? "true" : "false", audio_state);
}
