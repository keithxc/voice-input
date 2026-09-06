#ifndef VOICE_INPUT_SCORE_H
#define VOICE_INPUT_SCORE_H

#include <stddef.h>

/* Recognition errors are tallied separately for Han characters and for Latin
   words, because a single averaged rate over a mixed sentence hides which of
   the two halves regressed. Han characters are counted one per character and
   Latin runs one per word, so the Han tally is a character error rate and the
   Latin tally a word error rate. Everything else in a transcript -- spaces,
   punctuation, letter case -- is discarded before comparison. */

struct vi_score_class {
    size_t reference;
    size_t substitutions;
    size_t deletions;
    size_t insertions;
};

struct vi_score {
    struct vi_score_class han;
    struct vi_score_class word;
};

/* Compares one hypothesis against one reference. Returns -1 when either side
   holds more than VI_SCORE_MAX_TOKENS scorable tokens. */
#define VI_SCORE_MAX_TOKENS 2048

int vi_score_compare(const char *reference, const char *hypothesis,
                     struct vi_score *out);
void vi_score_accumulate(struct vi_score *total, const struct vi_score *one);
size_t vi_score_errors(const struct vi_score_class *counts);
/* Error rate in [0, inf), or -1.0 when the reference holds no token of that
   class. Insertions can push a rate above 1.0. */
double vi_score_rate(const struct vi_score_class *counts);

#endif
