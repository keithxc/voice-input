#include "text.h"

size_t vi_utf8_decode(const char *text, uint32_t *codepoint) {
    const unsigned char *bytes = (const unsigned char *)text;
    if (bytes[0] < 0x80) {
        *codepoint = bytes[0];
        return 1;
    }
    if ((bytes[0] & 0xE0) == 0xC0 && (bytes[1] & 0xC0) == 0x80) {
        *codepoint = ((uint32_t)(bytes[0] & 0x1F) << 6) |
                     (uint32_t)(bytes[1] & 0x3F);
        return 2;
    }
    if ((bytes[0] & 0xF0) == 0xE0 && (bytes[1] & 0xC0) == 0x80 &&
        (bytes[2] & 0xC0) == 0x80) {
        *codepoint = ((uint32_t)(bytes[0] & 0x0F) << 12) |
                     ((uint32_t)(bytes[1] & 0x3F) << 6) |
                     (uint32_t)(bytes[2] & 0x3F);
        return 3;
    }
    if ((bytes[0] & 0xF8) == 0xF0 && (bytes[1] & 0xC0) == 0x80 &&
        (bytes[2] & 0xC0) == 0x80 && (bytes[3] & 0xC0) == 0x80) {
        *codepoint = ((uint32_t)(bytes[0] & 0x07) << 18) |
                     ((uint32_t)(bytes[1] & 0x3F) << 12) |
                     ((uint32_t)(bytes[2] & 0x3F) << 6) |
                     (uint32_t)(bytes[3] & 0x3F);
        return 4;
    }
    *codepoint = 0xFFFD;
    return 1;
}

int vi_utf8_is_han(uint32_t codepoint) {
    return (codepoint >= 0x3400 && codepoint <= 0x4DBF) ||
           (codepoint >= 0x4E00 && codepoint <= 0x9FFF) ||
           (codepoint >= 0xF900 && codepoint <= 0xFAFF) ||
           (codepoint >= 0x20000 && codepoint <= 0x2A6DF);
}

int vi_utf8_has_han(const char *text) {
    if (text == NULL) return 0;
    for (const char *cursor = text; *cursor != '\0';) {
        uint32_t codepoint = 0;
        cursor += vi_utf8_decode(cursor, &codepoint);
        if (vi_utf8_is_han(codepoint)) return 1;
    }
    return 0;
}
