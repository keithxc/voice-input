#include "selection.h"

#include <assert.h>
#include <stdio.h>

static struct vi_source_stats delivering(float rms, float score,
                                         unsigned chunks) {
    return (struct vi_source_stats){ .present = true, .streaming = true,
                                     .chunks = chunks, .rms = rms,
                                     .score = score };
}

/* Bootstrap takes the highest-scoring source, so park selection on the first
   slot by hiding the others until it has been made. */
static void bootstrap_onto_first(struct vi_selection *state,
                                 struct vi_source_stats *sources, size_t count) {
    unsigned saved[8];
    assert(count <= 8U);
    for (size_t i = 1U; i < count; ++i) {
        saved[i] = sources[i].chunks;
        sources[i].chunks = 0U;
    }
    vi_selection_reset(state);
    assert(vi_selection_update(state, sources, count, 0, 0L) == 0);
    for (size_t i = 1U; i < count; ++i) sources[i].chunks = saved[i];
}

/* The regression that made a quiet built-in microphone look completely deaf:
   selection used to require a signal before it would ever pick a source, and
   only the selected source feeds the recogniser. */
static void bootstraps_below_the_switching_threshold(void) {
    struct vi_source_stats sources[2] = {
        delivering(VI_SWITCH_MIN_RMS / 10.0F, 1.0F, 1U),
        delivering(VI_SWITCH_MIN_RMS / 20.0F, 2.0F, 1U),
    };
    struct vi_selection state;
    vi_selection_reset(&state);
    assert(vi_selection_update(&state, sources, 2, 0, 1000L) == 1);
    assert(state.selected == 1);
}

static void waits_for_a_source_that_delivers(void) {
    struct vi_source_stats sources[2] = {
        delivering(0.5F, 100.0F, 0U),
        delivering(0.5F, 100.0F, 4U),
    };
    sources[1].streaming = false;
    struct vi_selection state;
    vi_selection_reset(&state);
    assert(vi_selection_update(&state, sources, 2, 0, 1000L) == VI_NO_SOURCE);
}

static void rebootstraps_when_the_selected_source_drops(void) {
    struct vi_source_stats sources[2] = {
        delivering(0.2F, 50.0F, 8U),
        delivering(0.2F, 10.0F, 8U),
    };
    struct vi_selection state;
    vi_selection_reset(&state);
    assert(vi_selection_update(&state, sources, 2, 0, 1000L) == 0);
    sources[0].streaming = false;
    assert(vi_selection_update(&state, sources, 2, 1, 1200L) == 1);
}

/* Bootstrap takes whichever source delivers first, which is arbitrary; the
   warm-up window must correct that in milliseconds, not after a vote run. */
static void corrects_an_unlucky_first_pick_during_warmup(void) {
    struct vi_source_stats sources[2] = {
        delivering(0.2F, 1.0F, 8U),
        delivering(0.2F, 50.0F, 8U),
    };
    struct vi_selection state;
    bootstrap_onto_first(&state, sources, 2);
    assert(vi_selection_update(&state, sources, 2, 1, VI_WARMUP_MS - 1L) == 1);
    assert(state.candidate_votes == 0U);
}

static void ignores_a_source_without_enough_chunks_during_warmup(void) {
    struct vi_source_stats sources[2] = {
        delivering(0.2F, 1.0F, 8U),
        delivering(0.2F, 50.0F, VI_WARMUP_MIN_CHUNKS - 1U),
    };
    struct vi_selection state;
    bootstrap_onto_first(&state, sources, 2);
    assert(vi_selection_update(&state, sources, 2, 1, VI_WARMUP_MS - 1L) == 0);
}

/* Once warm-up is over the hysteresis must apply again, otherwise the source
   would flap between microphones mid-sentence. The speech hold outlasts warm-up,
   so nothing can even start voting until it has expired too. */
static void stops_taking_sources_outright_after_warmup(void) {
    struct vi_source_stats sources[2] = {
        delivering(0.2F, 1.0F, 8U),
        delivering(0.2F, 50.0F, 8U),
    };
    struct vi_selection state;
    bootstrap_onto_first(&state, sources, 2);
    assert(vi_selection_update(&state, sources, 2, 1, VI_WARMUP_MS) == 0);
    assert(state.candidate_votes == 0U);
    const long settled = VI_SPEECH_SOURCE_HOLD_MS + VI_SWITCH_COOLDOWN_MS;
    assert(vi_selection_update(&state, sources, 2, 1, settled) == 0);
    assert(state.candidate_votes == 1U);
}

static void keeps_the_source_within_the_margin(void) {
    struct vi_source_stats sources[2] = {
        delivering(0.2F, 10.0F, 8U),
        delivering(0.2F, 10.0F + VI_SWITCH_MARGIN - 1.0F, 8U),
    };
    struct vi_selection state;
    bootstrap_onto_first(&state, sources, 2);
    for (int i = 0; i < 40; ++i) {
        assert(vi_selection_update(&state, sources, 2, 1, 5000L + i) == 0);
    }
    assert(state.candidate == VI_NO_SOURCE);
}

static void switches_only_after_a_full_vote_run(void) {
    struct vi_source_stats sources[2] = {
        delivering(0.2F, 10.0F, 8U),
        delivering(0.2F, 10.0F + VI_SWITCH_MARGIN + 1.0F, 8U),
    };
    struct vi_selection state;
    bootstrap_onto_first(&state, sources, 2);
    for (unsigned i = 1U; i < VI_SWITCH_VOTES; ++i) {
        assert(vi_selection_update(&state, sources, 2, 1, 5000L) == 0);
        assert(state.candidate_votes == i);
    }
    assert(vi_selection_update(&state, sources, 2, 1, 5000L) == 1);
}

/* A source that is not the one reporting must not carry an election on stale
   numbers, and a below-threshold source must not become a candidate at all. */
static void only_the_reporting_source_votes(void) {
    struct vi_source_stats sources[3] = {
        delivering(0.2F, 10.0F, 8U),
        delivering(0.2F, 10.0F + VI_SWITCH_MARGIN + 1.0F, 8U),
        delivering(0.2F, 1.0F, 8U),
    };
    struct vi_selection state;
    bootstrap_onto_first(&state, sources, 3);
    for (int i = 0; i < 40; ++i) {
        assert(vi_selection_update(&state, sources, 3, 2, 5000L) == 0);
    }
    assert(state.candidate_votes == 0U);

    sources[1].rms = VI_SWITCH_MIN_RMS / 2.0F;
    for (int i = 0; i < 40; ++i) {
        assert(vi_selection_update(&state, sources, 3, 1, 5000L) == 0);
    }
    assert(state.candidate == VI_NO_SOURCE);
}

static void holds_the_source_through_speech(void) {
    struct vi_source_stats sources[2] = {
        delivering(0.2F, 10.0F, 8U),
        delivering(0.2F, 10.0F + VI_SWITCH_MARGIN + 1.0F, 8U),
    };
    struct vi_selection state;
    bootstrap_onto_first(&state, sources, 2);
    for (unsigned i = 0U; i < VI_SWITCH_VOTES * 4U; ++i) {
        state.last_speech_ms = 5000L;
        assert(vi_selection_update(&state, sources, 2, 1,
                                   5000L + VI_SPEECH_SOURCE_HOLD_MS - 1L) == 0);
        assert(state.candidate_votes == 0U);
    }
}

static void respects_the_switch_cooldown(void) {
    struct vi_source_stats sources[2] = {
        delivering(0.2F, 10.0F, 8U),
        delivering(0.2F, 10.0F + VI_SWITCH_MARGIN + 1.0F, 8U),
    };
    struct vi_selection state;
    bootstrap_onto_first(&state, sources, 2);
    state.last_speech_ms = 0L;
    state.last_switch_ms = 5000L;
    for (unsigned i = 0U; i < VI_SWITCH_VOTES * 4U; ++i) {
        assert(vi_selection_update(&state, sources, 2, 1,
                                   5000L + VI_SWITCH_COOLDOWN_MS - 1L) == 0);
        assert(state.candidate_votes == 0U);
    }
    assert(vi_selection_update(&state, sources, 2, 1,
                               5000L + VI_SWITCH_COOLDOWN_MS) == 0);
    assert(state.candidate_votes == 1U);
}

int main(void) {
    bootstraps_below_the_switching_threshold();
    waits_for_a_source_that_delivers();
    rebootstraps_when_the_selected_source_drops();
    corrects_an_unlucky_first_pick_during_warmup();
    ignores_a_source_without_enough_chunks_during_warmup();
    stops_taking_sources_outright_after_warmup();
    keeps_the_source_within_the_margin();
    switches_only_after_a_full_vote_run();
    only_the_reporting_source_votes();
    holds_the_source_through_speech();
    respects_the_switch_cooldown();
    puts("selection tests passed");
    return 0;
}
