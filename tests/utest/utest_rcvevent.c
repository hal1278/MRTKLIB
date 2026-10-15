/*------------------------------------------------------------------------------
 * unit test : event ring of the rtk receiver command layer (apps/rtkrcv)
 *
 * Coverage: seq numbering from 1 in publish order, getEvents reads (limit,
 * next, more), the boundary between held and dropped events (lost and the
 * oldest seq), invalid cursors, entry lookup, payload copies, and the newest-N
 * read of one kind used by the telnet `solution` command.
 *
 * Explicit CHECK macro (not assert) so it is robust under -DNDEBUG.
 *----------------------------------------------------------------------------*/
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "rcvevent.h"

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

static rcvevent_t out[16];

static uint64_t publish_log(rcvevent_ring_t* ring, const char* text) {
    rcvevent_t ev;

    memset(&ev, 0, sizeof(ev));
    ev.kind = RCVEVENT_LOG;
    ev.data.log.level = 1;
    ev.data.log.origin = RCVEVENT_LOG_ERRBUF;
    snprintf(ev.data.log.text, sizeof(ev.data.log.text), "%s", text);
    return rcvevent_publish(ring, &ev);
}

static uint64_t publish_solution(rcvevent_ring_t* ring, int week_sec, int stat) {
    rcvevent_t ev;

    memset(&ev, 0, sizeof(ev));
    ev.kind = RCVEVENT_SOLUTION;
    ev.data.solution.sol.time.time = week_sec;
    ev.data.solution.sol.stat = (uint8_t)stat;
    ev.data.solution.rb[0] = 1.0;
    ev.data.solution.has_base = 1;
    return rcvevent_publish(ring, &ev);
}

static void test_empty(void) {
    rcvevent_ring_t ring;
    rcvevent_read_t res;

    printf("--- test_empty\n");
    CHECK(rcvevent_init(&ring, 4), "init");
    CHECK(rcvevent_oldest(&ring) == 0, "empty: oldest 0");
    CHECK(rcvevent_get(&ring, 0) == NULL && rcvevent_get(&ring, 1) == NULL, "empty: no entries");
    CHECK(rcvevent_read(&ring, 0, out, 4, &res) == 0, "empty: read after 0 accepted");
    CHECK(res.n == 0 && res.next == 0 && !res.more && !res.lost, "empty: nothing, next 0");
    CHECK(rcvevent_read(&ring, 1, out, 4, &res) == -1, "empty: after 1 is invalid");
    rcvevent_free(&ring);
}

static void test_read_limit(void) {
    rcvevent_ring_t ring;
    rcvevent_read_t res;

    printf("--- test_read_limit\n");
    rcvevent_init(&ring, 4);
    CHECK(publish_log(&ring, "a") == 1 && publish_solution(&ring, 100, SOLQ_FIX) == 2 && publish_log(&ring, "c") == 3,
          "seq starts at 1 and increases across kinds");
    CHECK(rcvevent_read(&ring, 0, out, 2, &res) == 0 && res.n == 2 && out[0].seq == 1 && out[1].seq == 2,
          "limit 2: seq 1, 2");
    CHECK(res.next == 2 && res.more && !res.lost, "limit 2: next 2, more");
    CHECK(out[0].kind == RCVEVENT_LOG && strcmp(out[0].data.log.text, "a") == 0, "log payload copied");
    CHECK(out[1].kind == RCVEVENT_SOLUTION && out[1].data.solution.sol.time.time == 100 &&
              out[1].data.solution.sol.stat == SOLQ_FIX && out[1].data.solution.rb[0] == 1.0,
          "solution payload copied");
    CHECK(rcvevent_read(&ring, 2, out, 2, &res) == 0 && res.n == 1 && out[0].seq == 3 && res.next == 3 && !res.more,
          "after 2: seq 3, no more");
    CHECK(rcvevent_read(&ring, 3, out, 2, &res) == 0 && res.n == 0 && res.next == 3 && !res.more && !res.lost,
          "after last: nothing, next unchanged");
    CHECK(rcvevent_read(&ring, 4, out, 2, &res) == -1 && res.n == 0, "after > last: invalid");
    rcvevent_free(&ring);
}

static void test_overflow(void) {
    rcvevent_ring_t ring;
    rcvevent_read_t res;
    const rcvevent_t* ev;
    char text[8];
    int i;

    printf("--- test_overflow\n");
    rcvevent_init(&ring, 4);
    for (i = 1; i <= 10; i++) {
        snprintf(text, sizeof(text), "%d", i);
        publish_log(&ring, text);
    }
    CHECK(ring.last == 10 && rcvevent_oldest(&ring) == 7, "10 into 4: oldest 7");
    CHECK(rcvevent_get(&ring, 6) == NULL && rcvevent_get(&ring, 11) == NULL, "get: dropped and future seq absent");
    ev = rcvevent_get(&ring, 7);
    CHECK(ev && ev->seq == 7 && strcmp(ev->data.log.text, "7") == 0, "get: oldest held entry");
    CHECK(rcvevent_read(&ring, 5, out, 8, &res) == 0 && res.lost && res.oldest == 7 && res.n == 0 && res.next == 5,
          "after 5: seq 6 dropped, lost with oldest 7");
    CHECK(rcvevent_read(&ring, 0, out, 8, &res) == 0 && res.lost && res.oldest == 7, "after 0: lost");
    CHECK(rcvevent_read(&ring, 6, out, 8, &res) == 0 && !res.lost && res.n == 4 && out[0].seq == 7 &&
              out[3].seq == 10 && strcmp(out[3].data.log.text, "10") == 0 && res.next == 10 && !res.more,
          "after 6: boundary, 7..10 in order");
    CHECK(rcvevent_read(&ring, 10, out, 8, &res) == 0 && !res.lost && res.n == 0, "after last: not lost");
    rcvevent_free(&ring);
}

static void test_read_newest(void) {
    rcvevent_ring_t ring;
    uint64_t next;
    int i, n;

    printf("--- test_read_newest\n");
    rcvevent_init(&ring, 8);
    n = rcvevent_read_newest(&ring, 0, RCVEVENT_SOLUTION, out, 2, &next);
    CHECK(n == 0 && next == 0, "empty: nothing, next 0");
    /* seq 1..12: odd = solution (time = seq), even = log; 5..12 held */
    for (i = 1; i <= 12; i++) {
        if (i % 2) {
            publish_solution(&ring, i, SOLQ_FLOAT);
        } else {
            publish_log(&ring, "x");
        }
    }
    n = rcvevent_read_newest(&ring, 0, RCVEVENT_SOLUTION, out, 2, &next);
    CHECK(n == 2 && out[0].seq == 9 && out[1].seq == 11 && next == 12, "newest 2 solutions, oldest first");
    n = rcvevent_read_newest(&ring, 0, RCVEVENT_SOLUTION, out, 8, &next);
    CHECK(n == 4 && out[0].seq == 5 && out[3].seq == 11, "dropped events skipped: held solutions 5..11");
    n = rcvevent_read_newest(&ring, 9, RCVEVENT_SOLUTION, out, 8, &next);
    CHECK(n == 1 && out[0].seq == 11 && out[0].data.solution.sol.time.time == 11, "after 9: only 11");
    n = rcvevent_read_newest(&ring, 11, RCVEVENT_SOLUTION, out, 8, &next);
    CHECK(n == 0 && next == 12, "after 11: none, cursor moves to last");
    n = rcvevent_read_newest(&ring, 12, RCVEVENT_SOLUTION, out, 8, &next);
    CHECK(n == 0 && next == 12, "after last: none");
    n = rcvevent_read_newest(&ring, 0, RCVEVENT_STATUS, out, 8, &next);
    CHECK(n == 0, "no event of the kind");
    rcvevent_free(&ring);
}

static void test_solution_finite(void) {
    rcvevent_solution_t s;
    memset(&s, 0, sizeof(s));
    s.leap = 18.0;
    s.sol.rr[0] = -3961904.9;
    CHECK(rcvevent_solution_finite(&s), "finite solution");
    s.sol.rr[3] = NAN;
    s.sol.qv[0] = NAN;
    s.sol.dtr[0] = INFINITY;
    CHECK(rcvevent_solution_finite(&s), "unsent velocity and clock bias are not checked");
    s.sol.time.sec = NAN;
    CHECK(!rcvevent_solution_finite(&s), "non-finite time");
    s.sol.time.sec = 0.5;
    s.sol.qr[5] = INFINITY;
    CHECK(!rcvevent_solution_finite(&s), "non-finite covariance");
    s.sol.qr[5] = 0.0f;
    s.rb[1] = NAN;
    CHECK(rcvevent_solution_finite(&s), "base ignored without has_base");
    s.has_base = 1;
    CHECK(!rcvevent_solution_finite(&s), "non-finite base");
    s.has_base = 0;
    s.separation = NAN;
    s.has_separation = 1;
    CHECK(!rcvevent_solution_finite(&s), "non-finite separation");
}

int main(void) {
    test_empty();
    test_solution_finite();
    test_read_limit();
    test_overflow();
    test_read_newest();
    printf("%s (%d failure%s)\n", fails ? "FAILED" : "PASSED", fails, fails == 1 ? "" : "s");
    return fails ? 1 : 0;
}
