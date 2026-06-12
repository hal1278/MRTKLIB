/*------------------------------------------------------------------------------
 * unit test : Galileo HAS MT1 decoder hardening (mrtk_has.c)
 *
 * Negative / safety tests for the MT1 block parsers, driven entirely through the
 * public has_input_page() API with synthetic recovered messages. No real signal
 * data is needed: the RS(255,32,224) erasure decoder is systematic, so feeding
 * MS pages carrying PIDs 1..MS reproduces the message pages verbatim (rows 0..MS-1
 * of the generator matrix are the identity I32). Each "page" we hand to
 * has_input_page() is a 56-byte buffer: a 24-bit HAS page header followed by the
 * 53-byte (424-bit) message-page payload.
 *
 * Coverage:
 *   (A) a mask block that claims more bits than the recovered buffer holds is
 *       rejected (heap over-read guard, FINDING 1) without crashing or applying
 *       state.
 *   (B) a mask block with a duplicate GNSS ID is rejected (FINDING 2: one mask
 *       per GNSS), without applying state.
 *   (C) a well-formed mask+orbit-only message applies orbit state with iod[0]
 *       set but iod[1] left from a different value, so the satpos_ssr-style gate
 *       iod[0]==iod[1] correctly excludes the satellite until a clock block of
 *       the same IOD set arrives (FINDING 3); a following clock block of the
 *       same IOD set then makes the gate pass.
 *   (D) a TOH >= 3600 and an implausible-latency header are both rejected
 *       (FINDING 1 plausibility gate).
 *
 * Explicit CHECK macro (not assert) so it is robust under -DNDEBUG.
 *-----------------------------------------------------------------------------*/
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mrtklib/mrtk_const.h"
#include "mrtklib/mrtk_foundation.h"
#include "mrtklib/mrtk_has.h"
#include "mrtklib/mrtk_nav.h"
#include "mrtklib/mrtk_time.h"

extern void setbitu(uint8_t* buff, int pos, int len, uint32_t data);
extern int satno(int sys, int prn);

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

#define HAS_GNSS_GPS 0
#define HAS_GNSS_GAL 2
#define PAGE_OCT 53

/* feed a recovered message (ms*53 bytes) through has_input_page() as ms
 * systematic pages (PIDs 1..ms). Returns the last has_input_page() code (10 on a
 * complete, applied MT1; otherwise <10). */
static int feed_message(has_t* has, const uint8_t* msgbits, int ms, gtime_t time) {
    uint8_t page56[56];
    int p, ret = 0;

    for (p = 1; p <= ms; p++) {
        memset(page56, 0, sizeof(page56));
        /* 24-bit HAS page header: HASS(2)|Rsv(2)|MT(2)|MID(5)|MS(5)|PID(8).
         * HASS=1 (operational), MT=1 (the only correction type), MID=0,
         * MS stored minus 1, PID=p. */
        setbitu(page56, 0, 2, 1);       /* HASS = 1 */
        setbitu(page56, 2, 2, 0);       /* Reserved */
        setbitu(page56, 4, 2, 1);       /* MT = 1 */
        setbitu(page56, 6, 5, 0);       /* MID = 0 */
        setbitu(page56, 11, 5, ms - 1); /* MS - 1 */
        setbitu(page56, 16, 8, p);      /* PID */
        memcpy(page56 + 3, msgbits + (p - 1) * PAGE_OCT, PAGE_OCT);
        ret = has_input_page(has, 1, page56, time);
    }
    return ret;
}

/* write the 32-bit MT1 header into a recovered-message buffer. flags is a 6-bit
 * field: mask|orbit|cfull|csub|cb|pb (MSB first). */
static void put_mt1_header(uint8_t* msg, int toh, int flags6, int mid, int iodset) {
    setbitu(msg, 0, 12, toh);
    setbitu(msg, 12, 6, flags6);
    setbitu(msg, 18, 4, 0); /* Reserved */
    setbitu(msg, 22, 5, mid);
    setbitu(msg, 27, 5, iodset);
}

/* count satellites in has->ssr[] that would pass the satpos_ssr gate
 * (orbit and clock both present for the same IOD set). */
static int count_gate_pass(const has_t* has) {
    int i, n = 0;
    for (i = 0; i < MAXSAT; i++) {
        const ssr_t* s = &has->ssr[i];
        if (s->t0[0].time && s->t0[1].time && s->iod[0] == s->iod[1]) {
            n++;
        }
    }
    return n;
}

int main(void) {
    /* reception time: GST sow ~= 1 s past the hour so toh=0 has latency 1 s. */
    gtime_t time = gpst2time(2300, 3601.0);

    /* ---- (A) mask claims more bits than the 1-page (424-bit) buffer ---- */
    {
        has_t* has = has_new();
        CHECK(has != NULL, "A: has_new");
        uint8_t msg[PAGE_OCT]; /* MS=1 -> 424 bits */
        memset(msg, 0, sizeof(msg));
        put_mt1_header(msg, /*toh*/ 0, /*flags mask only*/ 0x20, 0, 0);
        /* Nsys=1, GPS, full SatM (40 sats), full SigM (16 sigs), CMAF=1. The
         * cell mask alone is 40*16=640 bits, far past the 424-bit buffer. */
        int bp = 32;
        setbitu(msg, bp, 4, 1); /* Nsys = 1 */
        bp += 4;
        setbitu(msg, bp, 4, HAS_GNSS_GPS); /* GNSS ID = GPS */
        bp += 4;
        setbitu(msg, bp, 32, 0xFFFFFFFFu); /* SatM bits 0..31 (setbitu caps at 32) */
        setbitu(msg, bp + 32, 8, 0xFF);    /* SatM bits 32..39 */
        bp += 40;
        setbitu(msg, bp, 16, 0xFFFF); /* SigM all 16 set */
        bp += 16;
        setbitu(msg, bp, 1, 1); /* CMAF = 1 (cell mask present) */

        int ret = feed_message(has, msg, 1, time);
        CHECK(ret != 10, "A: over-long mask rejected (no complete MT1)");
        CHECK(count_gate_pass(has) == 0, "A: no satellite state applied");
        has_free(has);
    }

    /* ---- (B) duplicate GNSS ID in the mask block ---- */
    {
        has_t* has = has_new();
        uint8_t msg[2 * PAGE_OCT]; /* MS=2 -> 848 bits, plenty of room */
        memset(msg, 0, sizeof(msg));
        put_mt1_header(msg, 0, 0x20, 0, 0); /* mask flag only */
        int bp = 32;
        setbitu(msg, bp, 4, 2); /* Nsys = 2 */
        bp += 4;
        /* first GNSS: GAL with 1 satellite, 1 signal, CMAF=0 */
        setbitu(msg, bp, 4, HAS_GNSS_GAL);
        bp += 4;
        setbitu(msg, bp, 1, 1); /* SatM bit 0 -> PRN 1 */
        bp += 40;
        setbitu(msg, bp, 1, 1); /* SigM bit 0 */
        bp += 16;
        setbitu(msg, bp, 1, 0); /* CMAF = 0 */
        bp += 1;
        setbitu(msg, bp, 3, 0); /* NM */
        bp += 3;
        /* second GNSS: GAL AGAIN (duplicate) */
        setbitu(msg, bp, 4, HAS_GNSS_GAL);
        bp += 4;
        setbitu(msg, bp, 1, 1);
        bp += 40;
        setbitu(msg, bp, 1, 1);
        bp += 16;
        setbitu(msg, bp, 1, 0);
        bp += 1;
        setbitu(msg, bp, 3, 0);

        int ret = feed_message(has, msg, 2, time);
        CHECK(ret != 10, "B: duplicate GNSS mask rejected");
        CHECK(count_gate_pass(has) == 0, "B: no satellite state applied");
        has_free(has);
    }

    /* ---- (C) orbit-only message: iod[0] set, iod[1] absent -> gate excludes;
     *          a later clock of the same IOD set -> gate passes ---- */
    {
        has_t* has = has_new();
        int gal1 = satno(SYS_GAL, 1);
        CHECK(gal1 > 0, "C: GAL PRN1 satno");

        /* message 1: mask + orbit (flags 110000), IOD set = 5, GAL PRN1 only */
        uint8_t m1[2 * PAGE_OCT];
        memset(m1, 0, sizeof(m1));
        put_mt1_header(m1, 0, 0x30, 0, 5); /* mask + orbit */
        int bp = 32;
        setbitu(m1, bp, 4, 1); /* Nsys = 1 */
        bp += 4;
        setbitu(m1, bp, 4, HAS_GNSS_GAL);
        bp += 4;
        setbitu(m1, bp, 1, 1); /* SatM PRN1 */
        bp += 40;
        setbitu(m1, bp, 1, 1); /* SigM sig0 */
        bp += 16;
        setbitu(m1, bp, 1, 0); /* CMAF=0 */
        bp += 1;
        setbitu(m1, bp, 3, 0); /* NM */
        bp += 3;
        bp += 6; /* mask Reserved */
        /* orbit block: VI(4) | IODref(10 GAL) | DR(13) DIT(12) DCT(12) */
        setbitu(m1, bp, 4, 0); /* VI */
        bp += 4;
        setbitu(m1, bp, 10, 42); /* IODref */
        bp += 10;
        setbitu(m1, bp, 13, 100); /* DR (non-NA) */
        bp += 13;
        setbitu(m1, bp, 12, 50); /* DIT */
        bp += 12;
        setbitu(m1, bp, 12, 25); /* DCT */
        bp += 12;

        int ret = feed_message(has, m1, 2, time);
        CHECK(ret == 10, "C: orbit-only message decoded");
        CHECK(has->ssr[gal1 - 1].t0[0].time != 0, "C: orbit t0[0] set");
        CHECK(has->ssr[gal1 - 1].t0[1].time == 0, "C: clock t0[1] absent");
        CHECK(count_gate_pass(has) == 0, "C: gate excludes sat with no clock");

        /* message 2: clock full-set (flags 001000), same mask + IOD set 5 */
        uint8_t m2[PAGE_OCT];
        memset(m2, 0, sizeof(m2));
        put_mt1_header(m2, 0, 0x08, 0, 5); /* clock full-set, mask flag 0 */
        bp = 32;
        setbitu(m2, bp, 4, 0); /* VI */
        bp += 4;
        setbitu(m2, bp, 2, 0); /* DCM for the single GNSS */
        bp += 2;
        setbitu(m2, bp, 13, 80); /* DCC (non-NA) */
        bp += 13;

        ret = feed_message(has, m2, 1, time);
        CHECK(ret == 10, "C: clock message decoded");
        CHECK(has->ssr[gal1 - 1].t0[1].time != 0, "C: clock t0[1] now set");
        CHECK(has->ssr[gal1 - 1].iod[0] == has->ssr[gal1 - 1].iod[1], "C: iod[0]==iod[1] after both blocks");
        CHECK(count_gate_pass(has) == 1, "C: gate passes once both blocks arrive");
        /* hrclk slot must stay inert: t0[2] never set */
        CHECK(has->ssr[gal1 - 1].t0[2].time == 0, "C: hrclk t0[2] stays 0 (inert)");
        has_free(has);
    }

    /* ---- (D) plausibility gate: TOH>=3600 and implausible latency ---- */
    {
        has_t* has = has_new();
        uint8_t msg[PAGE_OCT];
        memset(msg, 0, sizeof(msg));
        put_mt1_header(msg, /*toh*/ 4000, 0x20, 0, 0); /* TOH out of range */
        int ret = feed_message(has, msg, 1, time);
        CHECK(ret != 10, "D: TOH>=3600 rejected");

        /* latency gate: reception 1200 s past the hour, toh=0 -> latency 1200 s
         * > 600 s -> reject. */
        gtime_t late = gpst2time(2300, 1200.0);
        memset(msg, 0, sizeof(msg));
        put_mt1_header(msg, 0, 0x20, 0, 0);
        ret = feed_message(has, msg, 1, late);
        CHECK(ret != 10, "D: implausible latency rejected");
        has_free(has);
    }

    printf("\n%s: %d failure(s)\n", fails ? "FAILED" : "PASSED", fails);
    return fails ? 1 : 0;
}
