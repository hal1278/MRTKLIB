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

#ifdef RCVJSON_RPC
#include <math.h>
#include <stdlib.h>

#include "mrtklib/mrtk_const.h"
#include "mrtklib/mrtk_coords.h"
#include "mrtklib/mrtk_geoid.h"
#include "mrtklib/mrtk_time.h"

static yyjson_doc* encoded(yyjson_mut_doc* doc, yyjson_mut_val* root) {
    yyjson_doc* copy;
    char* text;
    yyjson_mut_doc_set_root(doc, root);
    text = yyjson_mut_write(doc, 0, NULL);
    CHECK(text != NULL, "conversion encodes finite valid JSON");
    copy = text ? yyjson_read(text, strlen(text), 0) : NULL;
    free(text);
    return copy;
}

static void conversion_checks(void) {
    rcvcmd_lifecycle_t lifecycle = {0};
    rcvcmd_snapshot_t snapshot = {0};
    rcvevent_t event = {0};
    rcvevent_solution_t solution = {0};
    yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);
    yyjson_doc* copy;
    yyjson_val *root, *data;
    int state, reason, error, i;
    char expected[128];
    const opt_t *mode = searchopt("pos1-posmode", sysopts), *geoid = searchopt("out-geoid", sysopts);
    for (i = 0; i <= PMODE_VRS_RTK; i++) {
        snprintf(expected, sizeof(expected), "%d:%s", i, rcvjson_mode_name(i));
        CHECK(mode && strstr(mode->comment, expected), "mode names match core MODOPT");
    }
    for (i = 0; i <= GEOID_GSI2000_M15; i++) {
        snprintf(expected, sizeof(expected), "%d:%s", i, rcvjson_geoid_name(i));
        CHECK(geoid && strstr(geoid->comment, expected), "geoid names match core GEOOPT");
    }
    for (state = RCVCMD_STOPPED; state <= RCVCMD_STOPPING; state++)
        for (reason = RCVCMD_STOP_NONE; reason <= RCVCMD_STOP_ERROR; reason++)
            for (error = RCVCMD_START_ERROR_NONE; error <= RCVCMD_START_ERROR_STDIN_RESERVED; error++) {
                lifecycle.state = state;
                lifecycle.stop_reason = reason;
                lifecycle.last_error = error;
                strcpy(lifecycle.paths, "out.pos");
                copy = encoded(doc, rcvjson_lifecycle(doc, &lifecycle));
                root = yyjson_doc_get_root(copy);
                CHECK((yyjson_obj_get(root, "stopReason") != NULL) ==
                          ((state == RCVCMD_STOPPED || state == RCVCMD_STOPPING) && reason != RCVCMD_STOP_NONE),
                      "stopReason presence follows lifecycle state");
                CHECK((yyjson_obj_get(root, "lastError") != NULL) ==
                          (error != RCVCMD_START_ERROR_NONE &&
                           (state == RCVCMD_STARTING || (state == RCVCMD_STOPPED && reason == RCVCMD_STOP_ERROR))),
                      "lastError presence follows lifecycle state");
                yyjson_doc_free(copy);
            }
    solution.sol.time = gpst2time(2440, 604799.0);
    solution.sol.time.sec = 0.9999999996;
    solution.leap = 18;
    copy = encoded(doc, rcvjson_solution(doc, &solution));
    root = yyjson_doc_get_root(copy);
    data = yyjson_obj_get(root, "time");
    CHECK(yyjson_obj_size(root) == 3 && yyjson_equals_str(yyjson_obj_get(root, "status"), "none"),
          "none has only three members");
    CHECK(yyjson_get_int(yyjson_obj_get(data, "week")) == 2441 && yyjson_get_num(yyjson_obj_get(data, "tow")) == 0.0,
          "nanosecond rounding carries into the GPS week");
    yyjson_doc_free(copy);
    solution.sol.time = gpst2time(2440, 123.1234567891);
    solution.sol.stat = SOLQ_FIX;
    solution.sol.ns = 18;
    solution.sol.age = 0.6f;
    solution.sol.ratio = 12.4f;
    solution.sol.rr[0] = 6378137.0;
    for (i = 0; i < 6; i++) solution.sol.qr[i] = (i + 1) * 0.1f;
    solution.has_base = 1;
    solution.rb[0] = 6378138.0;
    solution.has_separation = 1;
    solution.separation = 36.7;
    copy = encoded(doc, rcvjson_solution(doc, &solution));
    root = yyjson_doc_get_root(copy);
    CHECK(yyjson_obj_size(root) == 10, "fix includes every solution member and base");
    CHECK(yyjson_get_num(yyjson_obj_get(yyjson_obj_get(yyjson_obj_get(root, "position"), "llh"), "height")) == 0.0,
          "ellipsoidal height");
    CHECK(yyjson_get_num(yyjson_obj_get(yyjson_obj_get(yyjson_obj_get(root, "base"), "llh"), "height")) == 1.0,
          "base position");
    data = yyjson_obj_get(yyjson_obj_get(root, "covariance"), "enu");
    CHECK(fabs(yyjson_get_num(yyjson_obj_get(data, "ee")) - solution.sol.qr[1]) < 1e-12 &&
              fabs(yyjson_get_num(yyjson_obj_get(data, "nu")) - solution.sol.qr[5]) < 1e-12,
          "ENU transform uses covariance order");
    {
        char* text = yyjson_mut_write(doc, 0, NULL);
        CHECK(strstr(text, "\"xx\":0.1,") && strstr(text, "\"age\":0.6,"), "float fields use shortest float digits");
        free(text);
    }
    yyjson_doc_free(copy);
    solution.has_base = solution.has_separation = 0;
    copy = encoded(doc, rcvjson_solution(doc, &solution));
    root = yyjson_doc_get_root(copy);
    CHECK(!yyjson_obj_get(root, "base") && yyjson_obj_size(yyjson_obj_get(root, "geoid")) == 1,
          "base and separation omitted when absent");
    yyjson_doc_free(copy);
    for (i = 0; i < 6; i++) {
        double saved = solution.sol.rr[i];
        int position = i < 3;
        solution.sol.rr[i] = i % 2 ? INFINITY : NAN;
        copy = encoded(doc, rcvjson_solution(doc, &solution));
        root = yyjson_doc_get_root(copy);
        CHECK(position ? yyjson_obj_size(root) == 3 && yyjson_equals_str(yyjson_obj_get(root, "status"), "none")
                       : yyjson_obj_get(root, "position") != NULL,
              position ? "non-finite coordinates become none" : "unsent non-finite velocity keeps the solution");
        yyjson_doc_free(copy);
        solution.sol.rr[i] = saved;
    }
    solution.sol.qv[0] = NAN;
    solution.sol.dtr[0] = INFINITY;
    copy = encoded(doc, rcvjson_solution(doc, &solution));
    CHECK(yyjson_obj_get(yyjson_doc_get_root(copy), "position") != NULL,
          "unsent non-finite velocity covariance and clock bias keep the solution");
    yyjson_doc_free(copy);
    solution.sol.qv[0] = 0.0f;
    solution.sol.dtr[0] = 0.0;
    solution.sol.qr[4] = NAN;
    copy = encoded(doc, rcvjson_solution(doc, &solution));
    CHECK(yyjson_obj_size(yyjson_doc_get_root(copy)) == 3, "non-finite covariance becomes none");
    yyjson_doc_free(copy);
    solution.sol.qr[4] = 0.5f;
    solution.has_separation = 1;
    solution.separation = INFINITY;
    copy = encoded(doc, rcvjson_solution(doc, &solution));
    CHECK(yyjson_obj_size(yyjson_doc_get_root(copy)) == 3, "non-finite separation becomes none");
    yyjson_doc_free(copy);
    solution.has_separation = 0;
    for (i = SOLQ_NONE; i <= SOLQ_DR; i++) {
        static const char* names[] = {"none", "fix", "float", "sbas", "dgps", "single", "ppp", "dr"};
        solution.sol.stat = i;
        copy = encoded(doc, rcvjson_solution(doc, &solution));
        CHECK(yyjson_equals_str(yyjson_obj_get(yyjson_doc_get_root(copy), "status"), names[i]), "SOLQ names");
        yyjson_doc_free(copy);
    }
    snapshot.seq = 15;
    snapshot.has_solution = 1;
    snapshot.solution.data.solution = solution;
    for (state = RCVCMD_STOPPED; state <= RCVCMD_STOPPING; state++) {
        snapshot.lifecycle.state = state;
        copy = encoded(doc, rcvjson_snapshot(doc, &snapshot));
        root = yyjson_doc_get_root(copy);
        data = yyjson_obj_get(root, "status");
        CHECK(yyjson_obj_size(data) == (state == RCVCMD_RUNNING ? 3 : 1),
              "snapshot includes mode and solution only while running");
        yyjson_doc_free(copy);
    }
    event.seq = 16;
    event.kind = RCVEVENT_LOG;
    event.data.log.level = 1;
    strcpy(event.data.log.text, "bad\xFF");
    copy = encoded(doc, rcvjson_event(doc, &event));
    root = yyjson_doc_get_root(copy);
    CHECK(yyjson_obj_size(root) == 3 && yyjson_equals_str(yyjson_obj_get(root, "kind"), "logMessage") &&
              yyjson_equals_str(yyjson_obj_get(yyjson_obj_get(root, "data"), "text"), "bad" REPLACEMENT),
          "log event sanitizes UTF-8");
    yyjson_doc_free(copy);
    /* A whole sink message of invalid bytes triples in length when replaced. */
    memset(event.data.log.text, 0xFF, RCVEVENT_TEXT - 1);
    event.data.log.text[RCVEVENT_TEXT - 1] = '\0';
    copy = encoded(doc, rcvjson_event(doc, &event));
    root = yyjson_doc_get_root(copy);
    {
        yyjson_val* text = yyjson_obj_get(yyjson_obj_get(root, "data"), "text");
        CHECK(yyjson_get_len(text) == 3 * (RCVEVENT_TEXT - 1) &&
                  rcvjson_utf8_valid(yyjson_get_str(text), yyjson_get_len(text)),
              "a full-length invalid log text is replaced completely");
    }
    yyjson_doc_free(copy);
    event.kind = RCVEVENT_STATUS;
    memset(&event.data, 0, sizeof(event.data));
    event.data.status.last_error = RCVCMD_START_ERROR_OVERWRITE;
    event.data.status.stop_reason = RCVCMD_STOP_ERROR;
    memcpy(event.data.status.paths, "first\0second\0", 14);
    copy = encoded(doc, rcvjson_event(doc, &event));
    root = yyjson_doc_get_root(copy);
    data = yyjson_obj_get(yyjson_obj_get(root, "data"), "lastError");
    CHECK(yyjson_arr_size(yyjson_obj_get(data, "paths")) == 2, "status event unpacks refused paths");
    yyjson_doc_free(copy);
    yyjson_mut_doc_free(doc);
}
#endif

int main(void) {
    char output[16];
#ifdef RCVJSON_RPC
    conversion_checks();
#endif
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
