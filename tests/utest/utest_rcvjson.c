/*------------------------------------------------------------------------------
 * unit test : receiver JSON UTF-8 validation and sanitization
 *-----------------------------------------------------------------------------*/
#include <stdio.h>
#include <string.h>

#include "rcvjson.h"

static int fails;

#define CHECK(condition, message)                   \
    do {                                            \
        if (!(condition)) {                         \
            fprintf(stderr, "FAIL: %s\n", message); \
            fails++;                                \
        }                                           \
    } while (0)

#define REPLACEMENT "\xEF\xBF\xBD"

static void test_case(const char* input, size_t length, int valid, const char* expected, const char* name) {
    char output[256];
    size_t required = strlen(expected);
    CHECK(rcvjson_utf8_valid(input, length) == valid, name);
    CHECK(rcvjson_sanitize_utf8(input, length, NULL, 0) == required, "sanitizer sizing pass");
    memset(output, 0x7F, sizeof(output));
    CHECK(rcvjson_sanitize_utf8(input, length, output, sizeof(output)) == required, "sanitizer reports size");
    CHECK(!memcmp(output, expected, required + 1), name);
    CHECK(rcvjson_utf8_valid(output, required), "sanitizer produces valid UTF-8");
}

int main(void) {
    char output[16];
    static const char boundaries[] =
        "\x7F\xC2\x80\xDF\xBF\xE0\xA0\x80\xED\x9F\xBF"
        "\xEE\x80\x80\xEF\xBF\xBF\xF0\x90\x80\x80\xF4\x8F\xBF\xBF";
    static const char embedded[] = {'a', '\0', 'b'};
    test_case(NULL, 0, 1, "", "empty input");
    test_case("ASCII\t\n", 7, 1, "ASCII\t\n", "ASCII controls");
    test_case(boundaries, sizeof(boundaries) - 1, 1, boundaries, "UTF-8 scalar boundaries");
    test_case("\xC0\xAF", 2, 0, REPLACEMENT REPLACEMENT, "two-byte overlong encoding");
    test_case("\xE0\x80\xAF", 3, 0, REPLACEMENT REPLACEMENT REPLACEMENT, "three-byte overlong encoding");
    test_case("\xF0\x80\x80\xAF", 4, 0, REPLACEMENT REPLACEMENT REPLACEMENT REPLACEMENT, "four-byte overlong encoding");
    test_case("\xED\xA0\x80", 3, 0, REPLACEMENT REPLACEMENT REPLACEMENT, "surrogate");
    test_case("\xF4\x90\x80\x80", 4, 0, REPLACEMENT REPLACEMENT REPLACEMENT REPLACEMENT, "above Unicode maximum");
    test_case("\xF5\x80\x80\x80", 4, 0, REPLACEMENT REPLACEMENT REPLACEMENT REPLACEMENT, "invalid leading byte");
    test_case("\x80\xBF\xFF", 3, 0, REPLACEMENT REPLACEMENT REPLACEMENT, "stray continuation and invalid byte");
    test_case("\xC2", 1, 0, REPLACEMENT, "truncated two-byte character");
    test_case("\xE2\x82", 2, 0, REPLACEMENT, "truncated three-byte character");
    test_case("\xF0\x90\x80", 3, 0, REPLACEMENT, "truncated four-byte character");
    test_case(
        "a\xE2\x82"
        "z",
        4, 0, "a" REPLACEMENT "z", "valid prefix before missing continuation");
    test_case("\xE2\xC2\xA2", 3, 0, REPLACEMENT "\xC2\xA2", "next valid character is preserved");
    CHECK(rcvjson_utf8_valid(embedded, sizeof(embedded)), "embedded NUL is valid UTF-8");
    CHECK(rcvjson_sanitize_utf8(embedded, sizeof(embedded), output, sizeof(output)) == sizeof(embedded) &&
              !memcmp(embedded, output, sizeof(embedded)) && output[sizeof(embedded)] == '\0',
          "sanitizer preserves embedded NUL");
    CHECK(rcvjson_sanitize_utf8("a\xE2\x82\xAC"
                                "b",
                                5, output, 4) == 5 &&
              !strcmp(output, "a"),
          "output capacity never splits a UTF-8 character or skips to later text");
    CHECK(rcvjson_sanitize_utf8("\xFF", 1, output, 3) == 3 && !strcmp(output, ""),
          "output capacity never splits a replacement");
    CHECK(rcvjson_sanitize_utf8("\xFF", 1, output, 4) == 3 && !strcmp(output, REPLACEMENT),
          "replacement plus terminator fits exactly");
    CHECK(rcvjson_sanitize_utf8("a", 1, output, 1) == 1 && output[0] == '\0', "one-byte output is terminated");
    printf("%s (%d failures)\n", fails ? "FAILED" : "PASSED", fails);
    return fails ? 1 : 0;
}
