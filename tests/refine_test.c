#define _POSIX_C_SOURCE 200809L
#include "refine.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sherpa-onnx/c-api/c-api.h>

static struct vi_refine_result wait_result(struct vi_refiner *r) {
    struct vi_refine_result result;
    for (int i = 0; i < 20000; ++i) {
        if (vi_refiner_poll(r, &result)) return result;
        struct timespec delay = {.tv_nsec = 1000000};
        nanosleep(&delay, NULL);
    }
    abort();
}

int main(int argc, char **argv) {
    assert(vi_refine_english("Please check the buffer and return value"));
    assert(vi_refine_english("超过没 THE COMPUTERS DONT UNDERSTAND SOURCE CODE THEY ONLY"));
    assert(!vi_refine_english("检查这个 buffer 的大小然后调用函数"));
    assert(!vi_refine_english("请检查输入设备"));
    assert(!vi_refine_english(""));
    assert(vi_refine_preserves_words("频繁 FREQUENT", "频繁 frequently"));
    assert(!vi_refine_preserves_words("频繁 FREQUENTLY", "频繁 fr"));
    assert(!vi_refine_preserves_words("第二种 ALWAYS", "第二种 os"));
    assert(vi_refine_preserves_words("加了 ES 是现在时", "加了 es 是现在时"));
    float quiet[] = {.01F, -.01F, .02F, -.02F};
    float ratio = quiet[0] / quiet[2];
    assert(vi_refine_normalize(quiet, 4) > 1);
    assert(fabsf(quiet[0] / quiet[2] - ratio) < .0001F);
    float silence[1600] = {0};
    assert(vi_refine_normalize(silence, 1600) == 1);
    float loud[] = {.8F, -.7F};
    assert(vi_refine_normalize(loud, 2) == 1);
    if (argc == 1) return 0;
    assert(argc == 4);
    struct vi_refiner *r = vi_refiner_create(argv[1], argv[2], 2);
    assert(r);
    const SherpaOnnxWave *wave = SherpaOnnxReadWave(argv[3]);
    assert(wave && wave->num_samples > 0);
    assert(vi_refiner_submit(r, wave->samples, (size_t)wave->num_samples, "频繁 frequently") == 0);
    assert(vi_refiner_submit(r, wave->samples, (size_t)wave->num_samples, "duplicate") == -1);
    vi_refiner_cancel(r);
    struct vi_refine_result result = wait_result(r);
    assert(result.cancelled);
    assert(vi_refiner_submit(r, wave->samples, (size_t)wave->num_samples, "频繁 frequently") == 0);
    result = wait_result(r);
    assert(!result.cancelled && result.text[0]);
    assert(strcmp(result.backend, "paraformer-zh-en") == 0);
    assert(vi_refiner_submit(r, silence, 1600, "") == 0);
    result = wait_result(r);
    assert(result.text[0] == '\0');
    assert(vi_refiner_submit(r, wave->samples, VI_REFINE_MAX_SAMPLES + 1, "too long") == -1);
    vi_refiner_destroy(r);
    SherpaOnnxFreeWave(wave);
    return 0;
}
