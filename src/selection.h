#ifndef VOICE_INPUT_SELECTION_H
#define VOICE_INPUT_SELECTION_H

#include <stdbool.h>
#include <stddef.h>

/* Source arbitration expressed over plain data. Keeping it free of PipeWire
   types lets the switching rules be exercised without an audio server. */

#define VI_SWITCH_MARGIN 6.0F
#define VI_SWITCH_VOTES 8U
#define VI_SWITCH_COOLDOWN_MS 1000L
#define VI_SPEECH_SOURCE_HOLD_MS 1200L
#define VI_SWITCH_MIN_RMS 0.001F
#define VI_SWITCH_MIN_CHUNKS 4U
#define VI_WARMUP_MS 400L
#define VI_WARMUP_MIN_CHUNKS 2U

#define VI_NO_SOURCE (-1)

struct vi_source_stats {
    bool present;    /* slot holds a discovered node */
    bool streaming;  /* its stream is delivering buffers */
    unsigned chunks; /* buffers delivered since capture started */
    float rms;
    float score;
};

struct vi_selection {
    int selected;
    int candidate;
    unsigned candidate_votes;
    long last_switch_ms;
    long last_speech_ms;
    long warmup_until_ms;
};

void vi_selection_reset(struct vi_selection *state);

/* Returns the source index that should feed the recogniser after accounting for
   `updated`, mutating the candidate and vote bookkeeping in `state`. A return
   value different from `state->selected` on entry means the caller must
   re-point capture; `state->selected` is updated either way. */
int vi_selection_update(struct vi_selection *state,
                        const struct vi_source_stats *sources, size_t count,
                        int updated, long now_ms);

#endif
