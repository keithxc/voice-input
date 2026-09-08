/* Exercise consecutive sessions against a synthetic capture ring. */
#include "../src/audio.c"
#include <assert.h>

int main(void) {
    struct vi_audio audio = {0};
    audio.active = true;
    audio.preroll_ms = 1000;
    audio.max_gain = 1.0F;
    audio.target_rms = 0.03F;
    atomic_store(&audio.write_position, 16000);
    vi_audio_stop(&audio);
    /* A new 100 ms phrase arrives after stop; the other 900 ms belongs to
       the prior session and must never be transcribed a second time. */
    atomic_store(&audio.write_position, 17600);
    assert(vi_audio_start(&audio) == 0);
    assert(audio.read_position == 16000);
    float samples[4096];
    assert(vi_audio_read(&audio, samples, 4096) == 1600);
    assert(vi_audio_read(&audio, samples, 4096) == 0);
    vi_audio_stop(&audio);
    /* After a long idle, retain the full configured pre-roll. */
    atomic_store(&audio.write_position, 49600);
    assert(vi_audio_start(&audio) == 0);
    assert(audio.read_position == 33600);
    /* Quality reporting observes raw clipping, before the gain limiter. */
    audio.metrics = (struct vi_audio_metrics){0};
    audio.samples[33600] = 0.999F;
    float raw[1], processed[1];
    assert(vi_audio_read_with_raw(&audio, processed, raw, 1) == 1);
    assert(raw[0] == 0.999F && processed[0] <= 0.981F);
    struct vi_audio_metrics metrics;
    vi_audio_take_metrics(&audio, &metrics);
    assert(metrics.samples == 1 && metrics.clipped == 1 && metrics.peak == 0.999F);
    vi_audio_take_metrics(&audio, &metrics);
    assert(metrics.samples == 0);
    return 0;
}
