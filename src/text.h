#ifndef VOICE_INPUT_TEXT_H
#define VOICE_INPUT_TEXT_H

#include <stddef.h>
#include <stdint.h>

/* Minimal UTF-8 handling shared by the parts that have to tell Chinese from
   English inside one transcript: error-rate scoring counts Han characters
   apart from Latin words, and punctuation is rewritten in ASCII when an
   utterance holds no Han at all. */

/* Decodes one codepoint and returns the number of bytes it occupied. An
   invalid sequence yields U+FFFD and consumes one byte, so a scan always
   terminates. */
size_t vi_utf8_decode(const char *text, uint32_t *codepoint);
int vi_utf8_is_han(uint32_t codepoint);
int vi_utf8_has_han(const char *text);

#endif
