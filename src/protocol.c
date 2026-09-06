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
    if (strcmp(word, "sources") == 0) return VI_COMMAND_SOURCES;
    if (strcmp(word, "quit") == 0) return VI_COMMAND_QUIT;
    return VI_COMMAND_INVALID;
}

const char *vi_command_name(enum vi_command command) {
    switch (command) {
    case VI_COMMAND_STATUS: return "status";
    case VI_COMMAND_START: return "start";
    case VI_COMMAND_STOP: return "stop";
    case VI_COMMAND_TOGGLE: return "toggle";
    case VI_COMMAND_SOURCES: return "sources";
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
                  const char *audio_state, const char *asr_state) {
    return snprintf(buffer, size,
                    "{\"event\":\"%s\",\"recording\":%s,\"audio\":\"%s\","
                    "\"asr\":\"%s\"}\n",
                    event, recording ? "true" : "false", audio_state, asr_state);
}

int vi_json_info(char *buffer, size_t size, const struct vi_status *status) {
    if (buffer == NULL || size == 0 || status == NULL) return -1;
    return snprintf(buffer, size,
                    "{\"event\":\"info\",\"recording\":%s,\"audio\":\"%s\","
                    "\"asr\":\"%s\",\"asr-backend\":\"%s\",\"asr-model\":\"%s\","
                    "\"asr-kind\":\"%s\",\"decoder\":\"%s\",\"threads\":%d,"
                    "\"source-mode\":\"%s\",\"source\":\"%s\","
                    "\"punctuation\":\"%s\",\"punctuation-model\":\"%s\","
                    "\"sample-rate\":%d,\"tail-ms\":%ld}\n",
                    status->recording ? "true" : "false", status->audio,
                    status->asr, status->asr_backend, status->asr_model,
                    status->asr_kind, status->decoder, status->threads,
                    status->source_mode, status->source,
                    status->punctuation, status->punctuation_model,
                    status->sample_rate, status->tail_ms);
}

int vi_json_field(const char *json, const char *key, char *value, size_t size) {
    if (json == NULL || key == NULL || value == NULL || size == 0) return -1;
    char needle[64];
    int written = snprintf(needle, sizeof(needle), "\"%s\":", key);
    if (written < 0 || (size_t)written >= sizeof(needle)) return -1;
    const char *found = strstr(json, needle);
    if (found == NULL) return -1;
    const char *cursor = found + written;
    size_t used = 0;
    if (*cursor == '"') {
        for (++cursor; *cursor != '"'; ++cursor) {
            if (*cursor == '\0') return -1;
            if (*cursor == '\\' && cursor[1] != '\0') ++cursor;
            if (used + 1U >= size) return -1;
            value[used++] = *cursor;
        }
    } else {
        for (; *cursor != ',' && *cursor != '}' && *cursor != '\0'; ++cursor) {
            if (used + 1U >= size) return -1;
            value[used++] = *cursor;
        }
    }
    value[used] = '\0';
    return (int)used;
}

int vi_json_escape(char *buffer, size_t size, const char *value) {
    if (buffer == NULL || size == 0 || value == NULL) return -1;
    size_t used = 0U;
    for (const unsigned char *p = (const unsigned char *)value; *p != '\0'; ++p) {
        const char *escape = NULL;
        if (*p == '"') escape = "\\\"";
        else if (*p == '\\') escape = "\\\\";
        else if (*p == '\n') escape = "\\n";
        else if (*p == '\r') escape = "\\r";
        else if (*p == '\t') escape = "\\t";
        if (escape != NULL) {
            const size_t length = strlen(escape);
            if (used + length + 1U > size) return -1;
            memcpy(buffer + used, escape, length);
            used += length;
        } else if (*p >= 0x20U) {
            if (used + 2U > size) return -1;
            buffer[used++] = (char)*p;
        }
    }
    buffer[used] = '\0';
    return (int)used;
}

int vi_json_text(char *buffer, size_t size, const char *event, const char *value) {
    if (buffer == NULL || size == 0 || event == NULL || value == NULL) return -1;
    int prefix = snprintf(buffer, size, "{\"event\":\"%s\",\"text\":\"", event);
    if (prefix < 0 || (size_t)prefix >= size) return -1;
    const size_t used = (size_t)prefix;
    if (size < used + 4U) return -1;
    const int escaped = vi_json_escape(buffer + used, size - used - 3U, value);
    if (escaped < 0) return -1;
    memcpy(buffer + used + (size_t)escaped, "\"}\n", 4);
    return (int)(used + (size_t)escaped + 3U);
}
