#include "selection.h"

static bool delivers(const struct vi_source_stats *source) {
    return source->present && source->streaming && source->chunks > 0U;
}

static bool carries_signal(const struct vi_source_stats *source) {
    return delivers(source) && source->chunks >= VI_SWITCH_MIN_CHUNKS &&
           source->rms >= VI_SWITCH_MIN_RMS;
}

static int best_source(const struct vi_source_stats *sources, size_t count,
                       bool require_signal) {
    int best = VI_NO_SOURCE;
    for (size_t i = 0; i < count; ++i) {
        if (require_signal ? !carries_signal(&sources[i])
                           : !delivers(&sources[i])) {
            continue;
        }
        if (best == VI_NO_SOURCE || sources[i].score > sources[best].score) {
            best = (int)i;
        }
    }
    return best;
}

static void commit(struct vi_selection *state, int index, long now_ms) {
    /* Bootstrap has to take whichever source happens to deliver first, which is
       arbitrary. Allow a short window afterwards in which a better source can be
       taken outright, so an unlucky first pick is corrected in milliseconds
       rather than after a full margin-and-vote run. */
    if (state->selected == VI_NO_SOURCE) {
        state->warmup_until_ms = now_ms + VI_WARMUP_MS;
    }
    state->selected = index;
    state->candidate = VI_NO_SOURCE;
    state->candidate_votes = 0U;
    state->last_switch_ms = now_ms;
    state->last_speech_ms = now_ms;
}

static void clear_votes(struct vi_selection *state) {
    state->candidate = VI_NO_SOURCE;
    state->candidate_votes = 0U;
}

void vi_selection_reset(struct vi_selection *state) {
    if (state == NULL) return;
    state->selected = VI_NO_SOURCE;
    state->candidate = VI_NO_SOURCE;
    state->candidate_votes = 0U;
    state->last_switch_ms = 0L;
    state->last_speech_ms = 0L;
    state->warmup_until_ms = 0L;
}

int vi_selection_update(struct vi_selection *state,
                        const struct vi_source_stats *sources, size_t count,
                        int updated, long now_ms) {
    if (state == NULL || sources == NULL) return VI_NO_SOURCE;

    const bool holds_source = state->selected != VI_NO_SOURCE &&
                              (size_t)state->selected < count &&
                              delivers(&sources[state->selected]);
    if (!holds_source) {
        /* Bootstrap on anything that delivers buffers, however quiet. Demanding
           a signal here would deadlock capture on low-output built-in
           microphones: nothing would be selected, so no samples would ever be
           queued and the recogniser would stay silent. */
        const int bootstrap = best_source(sources, count, false);
        if (bootstrap != VI_NO_SOURCE) commit(state, bootstrap, now_ms);
        return state->selected;
    }

    if (now_ms < state->warmup_until_ms) {
        const int best = best_source(sources, count, false);
        if (best != VI_NO_SOURCE && best != state->selected &&
            sources[best].chunks >= VI_WARMUP_MIN_CHUNKS &&
            sources[best].score > sources[state->selected].score) {
            commit(state, best, now_ms);
        }
        clear_votes(state);
        return state->selected;
    }

    if (now_ms - state->last_speech_ms < VI_SPEECH_SOURCE_HOLD_MS) {
        clear_votes(state);
        return state->selected;
    }

    const int best = best_source(sources, count, true);
    if (best == VI_NO_SOURCE || best == state->selected ||
        sources[best].score <
            sources[state->selected].score + VI_SWITCH_MARGIN) {
        clear_votes(state);
        return state->selected;
    }
    if (now_ms - state->last_switch_ms < VI_SWITCH_COOLDOWN_MS) {
        return state->selected;
    }
    /* Only the source that just reported may cast a vote, so a stale reading
       cannot carry an election on its own. */
    if (updated != best) return state->selected;

    if (state->candidate != best) {
        state->candidate = best;
        state->candidate_votes = 1U;
    } else if (++state->candidate_votes >= VI_SWITCH_VOTES) {
        commit(state, best, now_ms);
    }
    return state->selected;
}
