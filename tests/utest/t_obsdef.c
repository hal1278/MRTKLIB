/*------------------------------------------------------------------------------
 * unit test : obsdef signal-selection idempotency and reset (#186)
 *
 * apply_pppsig()/set_obsdef() rewrite the global obsdef tables in place. This
 * test guards that:
 *   - selection rebuilds from the pristine defaults, so switching the selected
 *     2nd band restores a band a previous selection had trimmed (idempotency);
 *   - reset_obsdef() returns the tables to their full multi-band defaults.
 * Before the #186 fix, a band zeroed by one selection could not be recovered by
 * the next (it stayed 0), which corrupts code2freq_idx() slot mapping after an
 * rtkrcv restart. Uses explicit checks (not assert) so it is robust under
 * -DNDEBUG.
 *-----------------------------------------------------------------------------*/
#include <stdio.h>

#include "mrtklib/rtklib.h"

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

static int gal(int idx) { return freq_idx2freq_num(SYS_GAL, idx); }
static int gps(int idx) { return freq_idx2freq_num(SYS_GPS, idx); }

int main(void) {
    int pppsig[5];

    /* 1. pristine defaults (the first reset_obsdef snapshots them) */
    reset_obsdef();
    CHECK(gal(0) == 1 && gal(1) == 5 && gal(2) == 7 && gal(3) == 6 && gal(4) == 8, "GAL pristine = E1,E5a,E5b,E6,E5ab");
    CHECK(gps(0) == 1 && gps(1) == 2 && gps(2) == 5, "GPS pristine = L1,L2,L5");

    /* 2. select GAL E1-E5b (pppsig[2]=1) and GPS L1-L5 (pppsig[0]=1) */
    pppsig[0] = 1;
    pppsig[1] = 0;
    pppsig[2] = 1;
    pppsig[3] = 0;
    pppsig[4] = 0;
    apply_pppsig(pppsig);
    CHECK(gal(0) == 1 && gal(1) == 7 && gal(2) == 0, "GAL E1-E5b: idx1=E5b(7), idx2 trimmed");
    CHECK(gps(0) == 1 && gps(1) == 5 && gps(2) == 0, "GPS L1-L5: idx1=L5(5), idx2 trimmed");

    /* 3. #186 idempotency: switch GAL to E1-E5a and GPS to L1-L2 from the trimmed
     *    state. The restored band must reappear (not stay 0). */
    pppsig[0] = 0;
    pppsig[2] = 0;
    apply_pppsig(pppsig);
    CHECK(gal(1) == 5, "GAL E1-E5a after E1-E5b: idx1 restored to E5a(5), not 0");
    CHECK(gps(1) == 2, "GPS L1-L2 after L1-L5: idx1 restored to L2(2), not 0");

    /* 4. reset_obsdef() restores the full multi-band defaults from any state */
    pppsig[0] = 1;
    pppsig[2] = 1;
    apply_pppsig(pppsig); /* leave tables trimmed */
    reset_obsdef();
    CHECK(gal(1) == 5 && gal(2) == 7 && gal(3) == 6 && gal(4) == 8, "reset_obsdef restores GAL E5a/E5b/E6/E5ab");
    CHECK(gps(2) == 5, "reset_obsdef restores GPS L5");

    if (fails) {
        printf("\n%d check(s) FAILED\n", fails);
        return 1;
    }
    printf("\nall obsdef #186 checks passed\n");
    return 0;
}
