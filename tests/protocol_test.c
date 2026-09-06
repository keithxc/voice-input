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

    const struct vi_status status = {
        .recording = false, .audio = "ready", .asr = "ready",
        .asr_backend = "sherpa-cpu", .asr_model = "zipformer-zh-en",
        .asr_kind = "transducer", .decoder = "greedy_search", .threads = 2,
        .source_mode = "default", .source = "Built-in Microphone",
        .punctuation = "enabled", .punctuation_model = "model.int8.onnx",
        .sample_rate = 16000, .tail_ms = 250,
    };
    char info[1024];
    assert(vi_json_info(info, sizeof(info), &status) > 0);
    char value[128];
    assert(vi_json_field(info, "asr-model", value, sizeof(value)) > 0);
    assert(strcmp(value, "zipformer-zh-en") == 0);
    assert(vi_json_field(info, "threads", value, sizeof(value)) > 0);
    assert(strcmp(value, "2") == 0);
    assert(vi_json_field(info, "recording", value, sizeof(value)) > 0);
    assert(strcmp(value, "false") == 0);
    assert(vi_json_field(info, "tail-ms", value, sizeof(value)) > 0);
    assert(strcmp(value, "250") == 0);
    assert(vi_json_field(info, "source-mode", value, sizeof(value)) > 0);
    assert(strcmp(value, "default") == 0);
    /* An absent key and a value that does not fit are both refusals, never a
       truncated answer the caller would act on. */
    assert(vi_json_field(info, "missing", value, sizeof(value)) < 0);
    assert(vi_json_field(info, "asr-model", value, 4) < 0);
    assert(vi_json_field("{\"event\":\"final\",\"text\":\"say \\\"NixOS\\\"\"}",
                         "text", value, sizeof(value)) > 0);
    assert(strcmp(value, "say \"NixOS\"") == 0);

    puts("protocol tests passed");
    return 0;
}
