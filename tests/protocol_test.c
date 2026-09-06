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
    puts("protocol tests passed");
    return 0;
}
