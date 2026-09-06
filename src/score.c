#include "score.h"

#include "text.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define VI_TOKEN_TEXT 32

struct vi_token {
    char text[VI_TOKEN_TEXT];
    unsigned char is_han;
};

enum { OP_MATCH = 0, OP_SUBSTITUTE, OP_DELETE, OP_INSERT };

static int push_token(struct vi_token *tokens, size_t *count, const char *text,
                      size_t length, int han) {
    if (*count >= VI_SCORE_MAX_TOKENS) return -1;
    if (length >= VI_TOKEN_TEXT) length = VI_TOKEN_TEXT - 1;
    memcpy(tokens[*count].text, text, length);
    tokens[*count].text[length] = '\0';
    tokens[*count].is_han = (unsigned char)(han ? 1 : 0);
    ++*count;
    return 0;
}

static int flush_word(struct vi_token *tokens, size_t *count, char *word,
                      size_t *length) {
    while (*length > 0 && word[*length - 1] == '\'') --*length;
    if (*length == 0) return 0;
    int status = push_token(tokens, count, word, *length, 0);
    *length = 0;
    return status;
}

static int tokenize(const char *text, struct vi_token *tokens, size_t *count) {
    char word[VI_TOKEN_TEXT];
    size_t word_length = 0;
    *count = 0;
    if (text == NULL) return 0;
    for (const char *cursor = text; *cursor != '\0';) {
        uint32_t codepoint = 0;
        const char *bytes = cursor;
        size_t width = vi_utf8_decode(cursor, &codepoint);
        cursor += width;
        if (codepoint >= 0xFF01 && codepoint <= 0xFF5E) codepoint -= 0xFEE0;
        if (codepoint == 0x2019) codepoint = '\'';
        if (vi_utf8_is_han(codepoint)) {
            if (flush_word(tokens, count, word, &word_length) < 0) return -1;
            if (push_token(tokens, count, bytes, width, 1) < 0) return -1;
            continue;
        }
        char letter = 0;
        if (codepoint < 0x80) {
            letter = (char)codepoint;
            if (letter >= 'A' && letter <= 'Z') letter = (char)(letter + 32);
        }
        const int keep = (letter >= 'a' && letter <= 'z') ||
                         (letter >= '0' && letter <= '9') ||
                         (letter == '\'' && word_length > 0);
        if (!keep) {
            if (flush_word(tokens, count, word, &word_length) < 0) return -1;
            continue;
        }
        if (word_length + 1 < VI_TOKEN_TEXT) word[word_length++] = letter;
    }
    return flush_word(tokens, count, word, &word_length);
}

static struct vi_score_class *class_of(struct vi_score *score,
                                       const struct vi_token *token) {
    return token->is_han ? &score->han : &score->word;
}

int vi_score_compare(const char *reference, const char *hypothesis,
                     struct vi_score *out) {
    if (out == NULL) return -1;
    memset(out, 0, sizeof(*out));

    struct vi_token *reference_tokens =
        calloc(VI_SCORE_MAX_TOKENS, sizeof(*reference_tokens));
    struct vi_token *hypothesis_tokens =
        calloc(VI_SCORE_MAX_TOKENS, sizeof(*hypothesis_tokens));
    size_t reference_count = 0;
    size_t hypothesis_count = 0;
    unsigned char *operations = NULL;
    uint32_t *previous = NULL;
    uint32_t *current = NULL;
    int status = -1;

    if (reference_tokens == NULL || hypothesis_tokens == NULL) goto done;
    if (tokenize(reference, reference_tokens, &reference_count) < 0) goto done;
    if (tokenize(hypothesis, hypothesis_tokens, &hypothesis_count) < 0) goto done;

    const size_t rows = reference_count + 1;
    const size_t columns = hypothesis_count + 1;
    operations = malloc(rows * columns);
    previous = malloc(columns * sizeof(*previous));
    current = malloc(columns * sizeof(*current));
    if (operations == NULL || previous == NULL || current == NULL) goto done;

    for (size_t column = 0; column < columns; ++column) {
        previous[column] = (uint32_t)column;
        operations[column] = OP_INSERT;
    }
    operations[0] = OP_MATCH;
    for (size_t row = 1; row < rows; ++row) {
        current[0] = (uint32_t)row;
        operations[row * columns] = OP_DELETE;
        for (size_t column = 1; column < columns; ++column) {
            const int same = strcmp(reference_tokens[row - 1].text,
                                    hypothesis_tokens[column - 1].text) == 0;
            uint32_t best = previous[column - 1] + (same ? 0U : 1U);
            unsigned char operation = same ? OP_MATCH : OP_SUBSTITUTE;
            if (previous[column] + 1U < best) {
                best = previous[column] + 1U;
                operation = OP_DELETE;
            }
            if (current[column - 1] + 1U < best) {
                best = current[column - 1] + 1U;
                operation = OP_INSERT;
            }
            current[column] = best;
            operations[row * columns + column] = operation;
        }
        uint32_t *swap = previous;
        previous = current;
        current = swap;
    }

    size_t row = reference_count;
    size_t column = hypothesis_count;
    while (row > 0 || column > 0) {
        switch (operations[row * columns + column]) {
        case OP_MATCH:
            --row;
            --column;
            break;
        case OP_SUBSTITUTE:
            ++class_of(out, &reference_tokens[row - 1])->substitutions;
            --row;
            --column;
            break;
        case OP_DELETE:
            ++class_of(out, &reference_tokens[row - 1])->deletions;
            --row;
            break;
        default:
            ++class_of(out, &hypothesis_tokens[column - 1])->insertions;
            --column;
            break;
        }
    }
    for (size_t index = 0; index < reference_count; ++index) {
        ++class_of(out, &reference_tokens[index])->reference;
    }
    status = 0;

done:
    free(current);
    free(previous);
    free(operations);
    free(hypothesis_tokens);
    free(reference_tokens);
    if (status < 0) memset(out, 0, sizeof(*out));
    return status;
}

static void accumulate_class(struct vi_score_class *total,
                             const struct vi_score_class *one) {
    total->reference += one->reference;
    total->substitutions += one->substitutions;
    total->deletions += one->deletions;
    total->insertions += one->insertions;
}

void vi_score_accumulate(struct vi_score *total, const struct vi_score *one) {
    if (total == NULL || one == NULL) return;
    accumulate_class(&total->han, &one->han);
    accumulate_class(&total->word, &one->word);
}

size_t vi_score_errors(const struct vi_score_class *counts) {
    if (counts == NULL) return 0;
    return counts->substitutions + counts->deletions + counts->insertions;
}

double vi_score_rate(const struct vi_score_class *counts) {
    if (counts == NULL || counts->reference == 0) return -1.0;
    return (double)vi_score_errors(counts) / (double)counts->reference;
}
