/*------------------------------------------------------------------------------
 * unit test : sigcfg-driven raw-decoder code selection (#189)
 *
 * Guards mrtk_sigcfg_freq_idx(), which lets [positioning].signals (sigcfg) drive
 * which obs code occupies a band's slot during raw decoding — e.g. GLONASS L2C/A
 * as the iono-free 2nd frequency without the legacy -RL2C receiver option.
 * Uses explicit checks (not assert) so it is robust under -DNDEBUG.
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

int main(void) {
    /* full dual-frequency multi-GNSS list; GLONASS 2nd band = L2C/A (R2C) */
    const char* sigs[] = {"G1C", "G2W", "R1C", "R2C", "E1C", "E7Q", "J1C", "J2L"};
    mrtk_sigcfg_t cfg[MRTK_NSYS];
    int nf = 0;

    reset_obsdef();

    CHECK(mrtk_sigcfg_from_signals(sigs, 8, cfg, &nf) == 0, "parse signals list");
    CHECK(nf == 2, "derived nf == 2");

    /* mirror the real flow: obsdef is rearranged from sigcfg before decoding */
    CHECK(mrtk_sigcfg_to_obsdef(cfg) == 0, "apply sigcfg to obsdef");

    /* GLONASS: L1 C/A -> slot 0; L2 C/A -> slot 1 (the #189 fix); L2P -> dropped */
    CHECK(mrtk_sigcfg_freq_idx(SYS_GLO, CODE_L1C, cfg, NEXOBS) == 0, "GLO L1C/A -> main slot 0");
    CHECK(mrtk_sigcfg_freq_idx(SYS_GLO, CODE_L2C, cfg, NEXOBS) == 1, "GLO L2C/A -> main slot 1 (R2C preferred)");
    CHECK(mrtk_sigcfg_freq_idx(SYS_GLO, CODE_L2P, cfg, NEXOBS) == -1, "GLO L2P -> dropped (not the preferred R2 code)");

    /* GPS: L2W selected (G2W) -> slot 1; L2L (2C-band, different code) -> dropped */
    CHECK(mrtk_sigcfg_freq_idx(SYS_GPS, CODE_L2W, cfg, NEXOBS) == 1, "GPS L2W -> main slot 1 (G2W preferred)");
    CHECK(mrtk_sigcfg_freq_idx(SYS_GPS, CODE_L2L, cfg, NEXOBS) == -1, "GPS L2L -> dropped (G2W preferred)");

    /* a constellation absent from the list yields NOOPINION (caller keeps -R/-G defaults) */
    CHECK(mrtk_sigcfg_freq_idx(SYS_CMP, CODE_L2I, cfg, NEXOBS) == MRTK_SIGCFG_NOOPINION,
          "BDS not configured -> NOOPINION (legacy fallback)");

    reset_obsdef();

    if (fails) {
        printf("\n%d check(s) FAILED\n", fails);
        return 1;
    }
    printf("\nall sigcfg-decode #189 checks passed\n");
    return 0;
}
