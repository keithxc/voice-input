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
    VI_COMMAND_QUIT,
};

enum vi_command vi_parse_command(const char *line);
const char *vi_command_name(enum vi_command command);
int vi_runtime_socket_path(char *buffer, size_t size);
int vi_json_state(char *buffer, size_t size, const char *event, bool recording,
                  const char *audio_state);

#endif
