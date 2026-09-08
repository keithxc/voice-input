#ifndef VOICE_INPUT_PROTOCOL_H
#define VOICE_INPUT_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>

enum vi_command {
    VI_COMMAND_INVALID = 0,
    VI_COMMAND_STATUS,
    VI_COMMAND_START,
    VI_COMMAND_STOP,
    VI_COMMAND_TOGGLE,
    VI_COMMAND_SOURCES,
    VI_COMMAND_QUIT,
    VI_COMMAND_CANCEL,
};

/* What `voice-inputctl status` reports: enough to tell which model is loaded
   and whether punctuation came up, without reading the daemon's log. */
struct vi_status {
    bool recording;
    bool processing;
    const char *final_mode;
    const char *audio;
    const char *asr;
    const char *asr_backend;
    const char *asr_model;
    const char *asr_kind;
    const char *decoder;
    int threads;
    const char *source_mode;
    const char *source;
    const char *punctuation;
    const char *punctuation_model;
    int sample_rate;
    long tail_ms;
};

enum vi_command vi_parse_command(const char *line);
const char *vi_command_name(enum vi_command command);
int vi_runtime_socket_path(char *buffer, size_t size);
int vi_json_state(char *buffer, size_t size, const char *event, bool recording,
                  const char *audio_state, const char *asr_state);
int vi_json_text(char *buffer, size_t size, const char *event, const char *value);
int vi_json_info(char *buffer, size_t size, const struct vi_status *status);
/* Reads one top-level field out of an event line. Strings come back without
   their quotes, numbers and booleans as they were written. Returns -1 when the
   key is absent or the value does not fit. */
int vi_json_field(const char *json, const char *key, char *value, size_t size);
int vi_json_escape(char *buffer, size_t size, const char *value);

#endif
