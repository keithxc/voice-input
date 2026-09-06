#include "protocol.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    assert(vi_parse_command("start\n") == VI_COMMAND_START);
    assert(vi_parse_command("  toggle \r\n") == VI_COMMAND_TOGGLE);
    assert(vi_parse_command("unknown") == VI_COMMAND_INVALID);
    assert(strcmp(vi_command_name(VI_COMMAND_STOP), "stop") == 0);
    char json[128];
    assert(vi_json_state(json, sizeof(json), "state", true, "streaming", "ready") > 0);
    assert(strstr(json, "\"recording\":true") != NULL);
    assert(vi_json_text(json, sizeof(json), "final", "say \"NixOS\"") > 0);
    assert(strstr(json, "say \\\"NixOS\\\"") != NULL);

    assert(vi_parse_command("sources\n") == VI_COMMAND_SOURCES);
    assert(strcmp(vi_command_name(VI_COMMAND_SOURCES), "sources") == 0);
    char escaped[64];
    assert(vi_json_escape(escaped, sizeof(escaped), "a\"b\\c\nd") == 10);
    assert(strcmp(escaped, "a\\\"b\\\\c\\nd") == 0);
    assert(vi_json_escape(escaped, 4, "abcdefgh") < 0);
    puts("protocol tests passed");
    return 0;
}
