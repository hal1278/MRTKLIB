/*------------------------------------------------------------------------------
 * unit test : Galileo HAS RS(255,32,224) erasure decoder (mrtk_has_rs.c)
 *
 * Self-contained: depends only on mrtk_has_rs.h and the standard library, so it
 * compiles and runs without the mrtklib library. Build/run standalone:
 *
 *   cc -std=c11 -Wall -Wextra -Iinclude -Isrc/has \
 *      src/has/mrtk_has_rs.c tests/utest/utest_has_rs.c -o /tmp/utest_has_rs \
 *   && /tmp/utest_has_rs
 *
 * Coverage (Galileo HAS SIS ICD Issue 1.0, section 6):
 *   (a) generator-polynomial spot values vs ICD Table 42 (PDF pp.35-36): we
 *       rebuild G internally, so we re-derive g(x) here the same way the
 *       implementation does and compare 32 coefficients against the printed
 *       table. (Independent re-derivation, not a copy of the .c code path.)
 *   (b) round-trip property tests: encode synthetic k-page messages (k=1,10,32)
 *       through G, then decode arbitrary k-PID subsets (systematic-only,
 *       parity-only, mixed) and require bit-exact recovery; plus the systematic
 *       identity (PIDs 1..k reproduce the message verbatim).
 *   (c) ICD Annex C worked example (PDF pp.48-51): 15 received pages decode to
 *       the printed message, checked against the printed w'_1 / m'_1 and the
 *       full first-octet column.
 *   (d) invalid input: duplicate PID, out-of-range PID, bad k -> -1.
 *
 * Explicit CHECK macro (not assert) so it is robust under -DNDEBUG.
 *-----------------------------------------------------------------------------*/
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mrtk_has_rs.h"

static int fails = 0;

#define CHECK(cond, msg)                 \
    do {                                 \
        if (!(cond)) {                   \
            printf("FAIL: %s\n", (msg)); \
            fails++;                     \
        } else {                         \
            printf("ok  : %s\n", (msg)); \
        }                                \
    } while (0)

#define RS_N 255
#define RS_K 32
#define RS_NPAR 223
#define PAGE_OCT 53

/* ------------------------------------------------------------------ *
 * (a) Re-derive g(x) here, independently, and check against Table 42 *
 * ------------------------------------------------------------------ */

static uint8_t exp_t[2 * RS_N];
static uint8_t log_t[256];

static void build_gf(void) {
    int x = 1;
    for (int i = 0; i < RS_N; i++) {
        exp_t[i] = (uint8_t)x;
        log_t[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100) x ^= 0x11D;
    }
    for (int i = RS_N; i < 2 * RS_N; i++) exp_t[i] = exp_t[i - RS_N];
}

static uint8_t mul(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) return 0;
    return exp_t[log_t[a] + log_t[b]];
}

static void build_genpoly(uint8_t g[RS_NPAR + 1]) {
    memset(g, 0, RS_NPAR + 1);
    g[0] = 1;
    int deg = 0;
    for (int i = 1; i <= RS_NPAR; i++) {
        uint8_t root = exp_t[i];
        for (int j = deg + 1; j > 0; j--) g[j] = mul(g[j], root) ^ g[j - 1];
        g[0] = mul(g[0], root);
        deg++;
    }
}

/* Spot values from ICD Table 42 (j, g_j), >= 20 entries spread across the
 * polynomial including both endpoints. */
static void test_genpoly(void) {
    uint8_t g[RS_NPAR + 1];
    build_genpoly(g);

    struct {
        int j;
        uint8_t v;
    } tbl[] = {
        {0, 88},   {1, 216},  {2, 195},  {3, 23},    {4, 111},   {5, 82},   {6, 79},    {7, 81},  {8, 62},
        {9, 120},  {10, 249}, {11, 250}, {12, 11},   {21, 223},  {31, 34},  {32, 118},  {63, 68}, {95, 115},
        {96, 255}, {127, 45}, {128, 39}, {159, 201}, {160, 224}, {191, 99}, {192, 241}, {223, 1},
    };
    int n = (int)(sizeof(tbl) / sizeof(tbl[0]));
    int ok = 1;
    for (int i = 0; i < n; i++) {
        if (g[tbl[i].j] != tbl[i].v) {
            printf("  g[%d] = %d, expected %d\n", tbl[i].j, g[tbl[i].j], tbl[i].v);
            ok = 0;
        }
    }
    char m[64];
    snprintf(m, sizeof(m), "(a) %d generator-poly coeffs vs Table 42", n);
    CHECK(ok, m);
    /* g(x) must be monic of degree 223 (top coeff 1). */
    CHECK(g[RS_NPAR] == 1, "(a) g(x) is monic (g[223]=1)");
}

/* -------------------------------------------------- *
 * (b) round-trip property tests using a local encoder *
 * -------------------------------------------------- */

/* Reference systematic encoder: produce the full 255-row encoded column for a
 * message, mirroring ICD Eq. 11. enc[p-1][oct] is octet `oct` of encoded page
 * with PID p. msg[i][oct] is octet `oct` of message page i+1 (i=0..k-1). */
static void encode_message(const uint8_t msg[][PAGE_OCT], int k, uint8_t enc[RS_N][PAGE_OCT]) {
    uint8_t g[RS_NPAR + 1];
    build_genpoly(g);

    for (int oct = 0; oct < PAGE_OCT; oct++) {
        /* information vector c: c_{31-i} = msg page (i+1); zero-fill i>=k. */
        uint8_t c[RS_K];
        memset(c, 0, sizeof(c));
        for (int i = 0; i < k; i++) c[(RS_K - 1) - i] = msg[i][oct];

        /* dividend c(x)*x^{NPAR}, degrees 0..254. */
        uint8_t rem[RS_N];
        memset(rem, 0, sizeof(rem));
        for (int j = 0; j < RS_K; j++) rem[j + RS_NPAR] = c[j];

        /* long division by g(x) (monic, degree NPAR) -> parity in rem[0..222]. */
        for (int d = RS_N - 1; d >= RS_NPAR; d--) {
            uint8_t lead = rem[d];
            if (lead == 0) continue;
            for (int j = 0; j <= RS_NPAR; j++) rem[d - RS_NPAR + j] ^= mul(lead, g[j]);
        }

        /* code symbol G_r: info part c_{r-NPAR} for r>=NPAR, parity rem[r] else.
         * Row p-1 lists G_{255-p}, i.e. row = 254 - r. */
        for (int r = 0; r < RS_N; r++) {
            uint8_t sym = (r >= RS_NPAR) ? c[r - RS_NPAR] : rem[r];
            enc[RS_N - 1 - r][oct] = sym;
        }
    }
}

/* deterministic pseudo-random byte stream for synthetic messages. */
static uint8_t prng(uint32_t* s) {
    *s = (*s) * 1103515245u + 12345u;
    return (uint8_t)((*s >> 16) & 0xFF);
}

static int run_roundtrip(int k, const uint8_t* pids, const char* label, uint32_t seed) {
    uint8_t msg[RS_K][PAGE_OCT];
    uint32_t s = seed;
    for (int i = 0; i < k; i++)
        for (int j = 0; j < PAGE_OCT; j++) msg[i][j] = prng(&s);

    uint8_t enc[RS_N][PAGE_OCT];
    encode_message(msg, k, enc);

    /* gather the chosen PIDs' encoded pages. */
    uint8_t pages[RS_K][PAGE_OCT];
    for (int i = 0; i < k; i++) memcpy(pages[i], enc[pids[i] - 1], PAGE_OCT);

    uint8_t out[RS_K * PAGE_OCT];
    int rc = has_rs_decode((const uint8_t (*)[PAGE_OCT])pages, pids, k, out);
    int ok = (rc == 0);
    if (ok) {
        for (int i = 0; i < k && ok; i++)
            if (memcmp(out + i * PAGE_OCT, msg[i], PAGE_OCT) != 0) ok = 0;
    }
    CHECK(ok, label);
    return ok;
}

static void test_roundtrip(void) {
    /* k=1: single systematic page. */
    {
        uint8_t pids[] = {1};
        run_roundtrip(1, pids, "(b) k=1 systematic round-trip", 0xABCD0001u);
        uint8_t pids2[] = {99};
        run_roundtrip(1, pids2, "(b) k=1 parity-only round-trip (PID 99)", 0xABCD0002u);
    }
    /* k=10: systematic 1..10, parity-only, and mixed. */
    {
        uint8_t sys[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        run_roundtrip(10, sys, "(b) k=10 systematic identity (PIDs 1..10)", 0x1010A0u);
        uint8_t par[10] = {200, 201, 202, 210, 220, 230, 240, 250, 253, 255};
        run_roundtrip(10, par, "(b) k=10 parity-only round-trip", 0x1010B0u);
        uint8_t mix[10] = {1, 5, 40, 70, 100, 130, 160, 190, 230, 255};
        run_roundtrip(10, mix, "(b) k=10 mixed systematic/parity round-trip", 0x1010C0u);
    }
    /* k=32: full-rank systematic, last-32 parity, and a strided mix. */
    {
        uint8_t sys[RS_K];
        for (int i = 0; i < RS_K; i++) sys[i] = (uint8_t)(i + 1);
        run_roundtrip(RS_K, sys, "(b) k=32 systematic identity (PIDs 1..32)", 0x320001u);

        uint8_t par[RS_K];
        for (int i = 0; i < RS_K; i++) par[i] = (uint8_t)(RS_N - RS_K + 1 + i); /* 224..255 */
        run_roundtrip(RS_K, par, "(b) k=32 parity-only round-trip (PIDs 224..255)", 0x320002u);

        uint8_t mix[RS_K];
        for (int i = 0; i < RS_K; i++) mix[i] = (uint8_t)(7 * i + 1); /* 1,8,15,...,218 */
        run_roundtrip(RS_K, mix, "(b) k=32 strided-mix round-trip", 0x320003u);
    }
}

/* ----------------------------------------------------- *
 * (c) ICD Annex C worked decoding example (k = 15 pages) *
 * ----------------------------------------------------- */

static const uint8_t annexc_pids[15] = {55, 56, 57, 58, 59, 174, 175, 176, 187, 188, 239, 240, 241, 252, 253};

/* 53-octet encoded pages, decimal, in PID order (ICD pp.48-49). */
static const uint8_t annexc_pages[15][PAGE_OCT] = {
    /* PID 55 */
    {132, 123, 199, 73, 235, 125, 113, 116, 36,  71, 136, 251, 69,  70,  145, 140, 0,  39,
     42,  235, 193, 84, 146, 204, 110, 181, 90,  88, 128, 226, 97,  186, 227, 23,  26, 35,
     221, 11,  229, 98, 252, 141, 111, 216, 142, 98, 41,  194, 158, 125, 140, 153, 223},
    /* PID 56 */
    {52, 154, 227, 99,  77,  33,  11,  173, 50, 147, 166, 127, 182, 33,  1,   233, 221, 84,
     48, 123, 198, 121, 237, 105, 155, 213, 12, 174, 174, 197, 100, 133, 243, 248, 22,  84,
     12, 174, 206, 164, 198, 22,  146, 238, 91, 24,  202, 171, 181, 189, 162, 121, 57},
    /* PID 57 */
    {85,  1, 29,  145, 14,  230, 225, 85,  194, 242, 140, 77,  215, 250, 214, 40,  200, 226,
     106, 5, 171, 215, 135, 151, 77,  226, 225, 111, 142, 246, 176, 156, 0,   215, 18,  228,
     41,  8, 34,  151, 24,  174, 236, 105, 28,  5,   39,  243, 194, 63,  128, 181, 19},
    /* PID 58 */
    {44,  163, 27, 35,  21,  83, 238, 106, 156, 122, 59,  255, 250, 132, 43,  45,  12,  243,
     8,   9,   16, 185, 194, 2,  126, 136, 115, 220, 237, 47,  141, 167, 212, 35,  164, 47,
     217, 206, 88, 195, 238, 68, 125, 44,  175, 49,  177, 138, 4,   213, 165, 186, 120},
    /* PID 59 */
    {55,  190, 96,  216, 35,  121, 141, 182, 26,  28,  152, 34,  238, 248, 75,  122, 213, 237,
     99,  213, 34,  61,  152, 173, 145, 204, 133, 143, 64,  117, 119, 92,  224, 76,  187, 36,
     160, 208, 177, 95,  127, 213, 58,  214, 134, 44,  121, 248, 82,  63,  169, 191, 75},
    /* PID 174 */
    {187, 28,  69, 29,  89,  4,   160, 228, 22,  185, 43,  88, 154, 12,  86, 206, 43,  199,
     115, 152, 40, 239, 11,  192, 73,  228, 145, 24,  154, 41, 63,  49,  40, 36,  224, 176,
     100, 94,  31, 100, 152, 109, 111, 135, 185, 118, 207, 58, 18,  247, 59, 144, 33},
    /* PID 175 */
    {117, 25,  72,  154, 251, 194, 111, 69,  202, 191, 253, 159, 120, 178, 246, 68,  171, 41,
     251, 163, 124, 202, 254, 239, 152, 25,  2,   5,   204, 223, 192, 231, 250, 120, 193, 179,
     234, 80,  108, 166, 166, 167, 210, 195, 99,  135, 159, 118, 132, 143, 164, 128, 36},
    /* PID 176 */
    {143, 12, 156, 52,  139, 203, 193, 61, 89, 3,   53,  84,  14,  168, 101, 194, 207, 61,
     113, 59, 188, 39,  200, 99,  26,  41, 88, 222, 211, 134, 178, 117, 71,  15,  136, 150,
     150, 65, 88,  124, 204, 128, 23,  28, 51, 166, 204, 221, 251, 63,  53,  44,  190},
    /* PID 187 */
    {203, 226, 36,  10, 145, 27,  54,  129, 243, 142, 43,  63,  242, 57,  243, 98,  229, 59,
     74,  201, 41,  44, 96,  199, 124, 97,  197, 70,  118, 78,  134, 66,  106, 138, 68,  197,
     64,  140, 187, 91, 201, 10,  138, 135, 16,  254, 109, 113, 144, 220, 128, 204, 93},
    /* PID 188 */
    {29,  55, 158, 167, 195, 223, 144, 158, 158, 116, 87, 219, 101, 36,  71,  28,  189, 52,
     215, 17, 199, 92,  176, 139, 74,  132, 108, 3,   25, 126, 46,  191, 226, 239, 14,  161,
     44,  70, 247, 253, 202, 246, 58,  36,  35,  29,  77, 144, 52,  14,  217, 139, 221},
    /* PID 239 */
    {122, 57,  40,  21,  48,  65,  99,  21,  77,  50,  204, 30,  233, 166, 117, 3,   48,  3,
     115, 250, 224, 78,  143, 108, 245, 144, 255, 199, 147, 114, 161, 38,  145, 41,  107, 172,
     132, 82,  95,  202, 166, 152, 75,  83,  88,  143, 25,  25,  186, 202, 151, 159, 222},
    /* PID 240 */
    {125, 19,  56,  207, 112, 92, 184, 147, 239, 181, 113, 209, 24, 245, 173, 57, 173, 51,
     3,   160, 148, 255, 182, 92, 140, 168, 146, 194, 234, 61,  53, 190, 137, 15, 91,  228,
     231, 9,   111, 222, 52,  62, 205, 189, 90,  185, 129, 222, 74, 19,  154, 94, 29},
    /* PID 241 */
    {161, 204, 117, 222, 253, 61,  201, 66,  207, 106, 21,  166, 117, 149, 224, 164, 249, 50,
     45,  172, 71,  205, 29,  87,  112, 81,  177, 95,  215, 130, 214, 162, 83,  43,  182, 9,
     188, 112, 183, 111, 5,   174, 231, 176, 103, 151, 117, 7,   232, 167, 19,  33,  234},
    /* PID 252 */
    {207, 147, 205, 21,  140, 244, 31,  178, 149, 173, 157, 33,  161, 85,  130, 130, 237, 116,
     136, 51,  54,  137, 106, 123, 126, 234, 208, 57,  145, 34,  116, 229, 209, 226, 26,  86,
     63,  239, 245, 210, 21,  211, 61,  189, 43,  85,  215, 103, 160, 170, 234, 163, 56},
    /* PID 253 */
    {215, 200, 167, 19, 210, 166, 18,  96,  224, 77, 5,  145, 106, 148, 222, 103, 157, 196,
     233, 132, 109, 61, 229, 187, 163, 152, 17,  62, 27, 210, 42,  67,  181, 2,   23,  108,
     68,  206, 189, 76, 58,  39,  164, 43,  254, 9,  87, 41,  18,  228, 135, 212, 165},
};

/* w'_1 (first encoded column) and m'_1 (first decoded column), ICD p.50. */
static const uint8_t annexc_wprime1[15] = {132, 52, 85, 44, 55, 187, 117, 143, 203, 29, 122, 125, 161, 207, 215};
static const uint8_t annexc_mprime1[15] = {0, 255, 71, 67, 79, 240, 64, 221, 31, 174, 0, 128, 4, 0, 32};

static void test_annexc(void) {
    /* sanity: octet 0 of each page must equal the printed w'_1 vector. */
    int ok_w = 1;
    for (int i = 0; i < 15; i++)
        if (annexc_pages[i][0] != annexc_wprime1[i]) ok_w = 0;
    CHECK(ok_w, "(c) Annex C page octet-0 column matches printed w'_1");

    uint8_t out[15 * PAGE_OCT];
    int rc = has_rs_decode(annexc_pages, annexc_pids, 15, out);
    CHECK(rc == 0, "(c) Annex C decode returns 0");

    if (rc == 0) {
        /* first decoded octet of each message page == printed m'_1. */
        int ok_m = 1;
        for (int i = 0; i < 15; i++)
            if (out[i * PAGE_OCT + 0] != annexc_mprime1[i]) {
                printf("  m'_1[%d] = %d, expected %d\n", i, out[i * PAGE_OCT + 0], annexc_mprime1[i]);
                ok_m = 0;
            }
        CHECK(ok_m, "(c) Annex C first decoded column matches printed m'_1");

        /* spot-check decoded message page 1 against the printed message
         * "[ 0 12 192 11 32 255 223 255 255 0 ...]" (ICD p.50). */
        static const uint8_t page1_head[10] = {0, 12, 192, 11, 32, 255, 223, 255, 255, 0};
        int ok_p1 = 1;
        for (int j = 0; j < 10; j++)
            if (out[0 * PAGE_OCT + j] != page1_head[j]) ok_p1 = 0;
        CHECK(ok_p1, "(c) Annex C decoded page 1 head matches printed message");

        /* re-encode the decoded message and confirm it reproduces all 15
         * received pages (full self-consistency of the worked example). */
        uint8_t msg[RS_K][PAGE_OCT];
        for (int i = 0; i < 15; i++) memcpy(msg[i], out + i * PAGE_OCT, PAGE_OCT);
        uint8_t enc[RS_N][PAGE_OCT];
        encode_message(msg, 15, enc);
        int ok_re = 1;
        for (int i = 0; i < 15; i++)
            if (memcmp(enc[annexc_pids[i] - 1], annexc_pages[i], PAGE_OCT) != 0) ok_re = 0;
        CHECK(ok_re, "(c) re-encoding decoded message reproduces all 15 pages");
    }
}

/* --------------------------------- *
 * (d) invalid-input error handling   *
 * --------------------------------- */

static void test_invalid(void) {
    uint8_t pages[RS_K][PAGE_OCT];
    memset(pages, 0, sizeof(pages));

    /* duplicate PID. */
    {
        uint8_t pids[] = {5, 5};
        int rc = has_rs_decode((const uint8_t (*)[PAGE_OCT])pages, pids, 2, NULL);
        /* NULL out -> must reject before touching it. */
        CHECK(rc == -1, "(d) NULL output rejected");
        uint8_t out[2 * PAGE_OCT];
        rc = has_rs_decode((const uint8_t (*)[PAGE_OCT])pages, pids, 2, out);
        CHECK(rc == -1, "(d) duplicate PID rejected");
    }
    /* PID out of range (0 and >255 not representable; test 0). */
    {
        uint8_t pids[] = {0, 7};
        uint8_t out[2 * PAGE_OCT];
        int rc = has_rs_decode((const uint8_t (*)[PAGE_OCT])pages, pids, 2, out);
        CHECK(rc == -1, "(d) PID 0 rejected");
    }
    /* bad k. */
    {
        uint8_t pids[] = {1};
        uint8_t out[PAGE_OCT];
        int rc = has_rs_decode((const uint8_t (*)[PAGE_OCT])pages, pids, 0, out);
        CHECK(rc == -1, "(d) k=0 rejected");
        rc = has_rs_decode((const uint8_t (*)[PAGE_OCT])pages, pids, 33, out);
        CHECK(rc == -1, "(d) k>32 rejected");
    }
}

int main(void) {
    build_gf();
    if (has_rs_init() != 0) {
        printf("FAIL: has_rs_init() != 0\n");
        return 1;
    }

    test_genpoly();   /* (a) */
    test_roundtrip(); /* (b) */
    test_annexc();    /* (c) */
    test_invalid();   /* (d) */

    if (fails) {
        printf("\n%d check(s) FAILED\n", fails);
        return 1;
    }
    printf("\nall HAS RS decoder checks passed\n");
    return 0;
}
