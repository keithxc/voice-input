#include "score.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static struct vi_score compare(const char *reference, const char *hypothesis) {
    struct vi_score score;
    assert(vi_score_compare(reference, hypothesis, &score) == 0);
    return score;
}

static int close_to(double value, double expected) {
    return fabs(value - expected) < 1e-9;
}

/* Spacing, letter case, punctuation and fullwidth forms are not recognition
   errors; a transcript that only differs in those has to score perfectly. */
static void normalisation_is_not_an_error(void) {
    const struct vi_score score =
        compare("Hello, World! 你好。", "hello world 你好");
    assert(score.word.reference == 2 && score.han.reference == 2);
    assert(vi_score_errors(&score.word) == 0);
    assert(vi_score_errors(&score.han) == 0);

    const struct vi_score fullwidth = compare("NixOS ＮｉｘＯＳ", "nixos nixos");
    assert(fullwidth.word.reference == 2);
    assert(vi_score_errors(&fullwidth.word) == 0);
}

static void counts_han_by_character(void) {
    const struct vi_score score = compare("今天天气很好", "今天天气很号");
    assert(score.han.reference == 6);
    assert(score.han.substitutions == 1);
    assert(vi_score_errors(&score.han) == 1);
    assert(close_to(vi_score_rate(&score.han), 1.0 / 6.0));
    assert(score.word.reference == 0);
    assert(vi_score_rate(&score.word) < 0.0);
}

static void counts_latin_by_word(void) {
    const struct vi_score score = compare("please open the terminal",
                                          "please the terminal now");
    assert(score.word.reference == 4);
    assert(score.word.deletions == 1);
    assert(score.word.insertions == 1);
    assert(score.word.substitutions == 0);
    assert(close_to(vi_score_rate(&score.word), 0.5));
}

/* The reason the two classes are kept apart: an error inside a mixed sentence
   has to land on the half that produced it. */
static void mixed_errors_land_on_their_own_class(void) {
    const struct vi_score score = compare("把这个 pull request 合并",
                                          "把这个 pull requests 合并了");
    assert(score.han.reference == 5 && score.word.reference == 2);
    assert(score.word.substitutions == 1);
    assert(vi_score_errors(&score.word) == 1);
    assert(score.han.insertions == 1);
    assert(vi_score_errors(&score.han) == 1);
    assert(close_to(vi_score_rate(&score.word), 0.5));
    assert(close_to(vi_score_rate(&score.han), 1.0 / 5.0));
}

static void silence_deletes_the_whole_reference(void) {
    const struct vi_score score = compare("这是 test", "");
    assert(score.han.deletions == 2 && score.word.deletions == 1);
    assert(close_to(vi_score_rate(&score.han), 1.0));
    assert(close_to(vi_score_rate(&score.word), 1.0));

    const struct vi_score noise = compare("", "什么 noise");
    assert(noise.han.insertions == 2 && noise.word.insertions == 1);
    assert(vi_score_rate(&noise.han) < 0.0);
}

static void totals_accumulate(void) {
    struct vi_score total;
    const struct vi_score first = compare("今天天气很好", "今天天气很号");
    const struct vi_score second = compare("open the file", "open a file");
    vi_score_compare("", "", &total);
    vi_score_accumulate(&total, &first);
    vi_score_accumulate(&total, &second);
    assert(total.han.reference == 6 && total.han.substitutions == 1);
    assert(total.word.reference == 3 && total.word.substitutions == 1);
    assert(close_to(vi_score_rate(&total.word), 1.0 / 3.0));
}

/* A stuck recogniser can repeat one word until the buffer is full; the
   comparison has to refuse it rather than allocate a matrix for it. */
static void oversized_input_is_refused(void) {
    char text[VI_SCORE_MAX_TOKENS * 2 + 4];
    for (size_t i = 0; i < sizeof(text) - 1; i += 2) {
        text[i] = 'a';
        text[i + 1] = ' ';
    }
    text[sizeof(text) - 1] = '\0';
    struct vi_score score;
    assert(vi_score_compare(text, "a", &score) == -1);
    assert(score.word.reference == 0);
}

int main(void) {
    normalisation_is_not_an_error();
    counts_han_by_character();
    counts_latin_by_word();
    mixed_errors_land_on_their_own_class();
    silence_deletes_the_whole_reference();
    totals_accumulate();
    oversized_input_is_refused();
    puts("score tests passed");
    return 0;
}
