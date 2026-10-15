/*------------------------------------------------------------------------------
 * rcvjson.h : shared JSON text helpers for receiver adapters
 *
 * Copyright (C) 2026 H.SHIONO (MRTKLIB Project)
 * SPDX-License-Identifier: BSD-2-Clause
 *-----------------------------------------------------------------------------*/
#ifndef RCVJSON_H
#define RCVJSON_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Validate UTF-8, excluding overlong encodings, surrogates and values above U+10FFFF.
 * @param[in] text Input bytes; may be NULL only when length is zero.
 * @param[in] length Number of input bytes, including any embedded NUL bytes.
 * @return 1 if valid UTF-8, otherwise 0.
 */
int rcvjson_utf8_valid(const char* text, size_t length);

/**
 * @brief Replace each maximal ill-formed UTF-8 subpart with U+FFFD.
 * @param[in] text Input bytes; may be NULL only when length is zero.
 * @param[in] length Number of input bytes, including any embedded NUL bytes.
 * @param[out] out Output buffer; may be NULL when capacity is zero. Must not overlap text.
 * @param[in] capacity Output size including the terminating NUL.
 * @return Required output bytes excluding the terminating NUL, even if truncated.
 * A truncated multibyte character at the input end produces one replacement.
 * When capacity is nonzero, output is NUL-terminated and contains only complete
 * UTF-8 characters. Use capacity zero to determine the allocation size.
 */
size_t rcvjson_sanitize_utf8(const char* text, size_t length, char* out, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif /* RCVJSON_H */
