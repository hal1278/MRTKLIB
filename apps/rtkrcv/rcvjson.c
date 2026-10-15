/*------------------------------------------------------------------------------
 * rcvjson.c : shared JSON text helpers for receiver adapters
 *
 * Copyright (C) 2026 H.SHIONO (MRTKLIB Project)
 * SPDX-License-Identifier: BSD-2-Clause
 *-----------------------------------------------------------------------------*/
#include "rcvjson.h"

#include <string.h>

/* Consume one scalar or maximal ill-formed subpart. Keeping a valid prefix
 * together prevents a character cut by a fixed-size core buffer from becoming
 * several replacement characters. Restrictions on the second byte also rule
 * out overlong encodings, UTF-16 surrogates and values above U+10FFFF. */
static size_t utf8_part(const unsigned char* text, size_t length, int* valid) {
    unsigned char first = text[0], low = 0x80, high = 0xBF;
    size_t width, i;
    *valid = 0;
    if (first < 0x80) {
        *valid = 1;
        return 1;
    }
    if (first >= 0xC2 && first <= 0xDF) {
        width = 2;
    } else if (first >= 0xE0 && first <= 0xEF) {
        width = 3;
        if (first == 0xE0) low = 0xA0;
        if (first == 0xED) high = 0x9F;
    } else if (first >= 0xF0 && first <= 0xF4) {
        width = 4;
        if (first == 0xF0) low = 0x90;
        if (first == 0xF4) high = 0x8F;
    } else {
        return 1;
    }
    for (i = 1; i < width; i++) {
        if (i == length || text[i] < low || text[i] > high) return i;
        low = 0x80;
        high = 0xBF;
    }
    *valid = 1;
    return width;
}

int rcvjson_utf8_valid(const char* text, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        int valid;
        offset += utf8_part((const unsigned char*)text + offset, length - offset, &valid);
        if (!valid) return 0;
    }
    return 1;
}

size_t rcvjson_sanitize_utf8(const char* text, size_t length, char* out, size_t capacity) {
    static const char replacement[] = "\xEF\xBF\xBD";
    size_t offset = 0, required = 0, written = 0;
    int full = 0;
    while (offset < length) {
        int valid;
        size_t part = utf8_part((const unsigned char*)text + offset, length - offset, &valid);
        size_t width = valid ? part : sizeof(replacement) - 1;
        if (!full && capacity > written && width < capacity - written) {
            memcpy(out + written, valid ? text + offset : replacement, width);
            written += width;
        } else {
            full = 1;
        }
        required += width;
        offset += part;
    }
    if (capacity) out[written] = '\0';
    return required;
}
