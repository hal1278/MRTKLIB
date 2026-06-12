/*------------------------------------------------------------------------------
 * mrtk_has_rs.c : Galileo HAS RS(255,32,224) erasure decoder
 *
 * Implements the High-Parity Vertical Reed-Solomon (HPVRS) outer-layer code of
 * the Galileo HAS SIS ICD Issue 1.0 (May 2022) section 6:
 *
 *   - GF(256) arithmetic with primitive polynomial p(a)=a^8+a^4+a^3+a^2+1
 *     (0x11D), via log/antilog tables (ICD section 6.1, Table 41).
 *   - Generator polynomial g(x)=prod_{i=1..223}(x-a^i) (ICD Eq. 8, Table 42).
 *   - Systematic 255x32 generator matrix G=[I32; P], built by polynomial-
 *     remainder encoding of the 32 unit information vectors (ICD section 6.2.2).
 *   - Erasure decoder: from k distinct received pages, build the k x k matrix D
 *     out of the rows pids[i]-1 and the first k columns of G, invert it in
 *     GF(256) by Gauss-Jordan, and recover the message column by column
 *     (ICD section 6.4, Eq. 16/17).
 *
 * Indexing (ICD section 6.2/6.3):
 *   - info vector  c^T = [c31,...,c0], with c31 = message page 1.
 *   - code vector  G^T = [G254,...,G0] = [c31,...,c0, g222,...,g0].
 *   - encoded page PID p  <->  row p-1 of G (G listed as [G254,...,G0], so
 *     row p-1 holds code symbol G_{255-p}).
 *   - column j (0-based)  <->  message page j+1, i.e. info symbol c_{31-j}.
 *   PIDs 1..32 are systematic: G[p-1][j] for p in 1..32 is the identity I32,
 *   so taking PIDs 1..k of an encoded message reproduces the message pages.
 *-----------------------------------------------------------------------------*/
#include "mrtk_has_rs.h"

#include <string.h>

#define GF_PRIM 0x11D         /* primitive polynomial a^8+a^4+a^3+a^2+1 */
#define RS_N 255              /* code vector length  (2^8 - 1)          */
#define RS_K 32               /* information vector length              */
#define RS_NPAR (RS_N - RS_K) /* number of parity symbols = 223 */
#define PAGE_OCT 53           /* octets per page                */

/* GF(256) log/antilog tables. gf_exp is doubled (510 entries) so that
 * gf_exp[gf_log[a] + gf_log[b]] needs no modular reduction. */
static uint8_t gf_exp[2 * RS_N];
static uint8_t gf_log[256];

/* systematic generator matrix G = [I32; P], 255 rows x 32 columns. */
static uint8_t G[RS_N][RS_K];

static int rs_ready = 0;

/* multiply two GF(256) elements via log/antilog tables. */
static uint8_t gf_mul(uint8_t a, uint8_t b) {
    if (a == 0 || b == 0) return 0;
    return gf_exp[gf_log[a] + gf_log[b]];
}

/* multiplicative inverse in GF(256); inv(0) is undefined and returns 0. */
static uint8_t gf_inv(uint8_t a) {
    if (a == 0) return 0;
    return gf_exp[RS_N - gf_log[a]];
}

/* build the GF(256) log/antilog tables from the primitive polynomial. */
static void gf_build_tables(void) {
    int x = 1;
    for (int i = 0; i < RS_N; i++) {
        gf_exp[i] = (uint8_t)x;
        gf_log[x] = (uint8_t)i;
        x <<= 1;
        if (x & 0x100) x ^= GF_PRIM;
    }
    /* mirror the first half so additive indices up to 2*254 are valid. */
    for (int i = RS_N; i < 2 * RS_N; i++) gf_exp[i] = gf_exp[i - RS_N];
    gf_log[0] = 0; /* unused; inv(0)/mul(0) are guarded above. */
}

/* build g(x)=prod_{i=1..223}(x-a^i). Coefficients gpoly[0..223] are stored low
 * order first, with gpoly[223]=1 (the monic x^223 term). Subtraction equals
 * addition (XOR) in GF(256), so (x - a^i) == (x + a^i). */
static void gf_build_genpoly(uint8_t gpoly[RS_NPAR + 1]) {
    memset(gpoly, 0, RS_NPAR + 1);
    gpoly[0] = 1; /* start with g(x) = 1 */
    int deg = 0;
    for (int i = 1; i <= RS_NPAR; i++) {
        uint8_t root = gf_exp[i]; /* a^i */
        /* multiply current g(x) by (x + root). */
        for (int j = deg + 1; j > 0; j--) {
            gpoly[j] = gf_mul(gpoly[j], root) ^ gpoly[j - 1];
        }
        gpoly[0] = gf_mul(gpoly[0], root);
        deg++;
    }
}

/* systematic encoding of a unit information vector to fill one column of G.
 *
 * The information symbol that is 1 corresponds to message page (col+1), i.e.
 * info coefficient c_{31-col} of c(x)=sum_j c_j x^j (ICD Eq. 9). The systematic
 * codeword is G(x)=c(x)*x^{n-k} - R_{g(x)}[c(x)*x^{n-k}] (ICD Eq. 11), whose
 * coefficients in transposed order [G254,...,G0] map to G rows 0..254.
 *
 * For column `col`: c_{31-col}=1, all other c_j=0, so c(x)*x^{n-k} is the single
 * term x^{(31-col)+RS_NPAR}. We compute its remainder modulo g(x) to get the
 * parity, then place the information and parity symbols at their code positions.
 */
static void rs_encode_column(int col, const uint8_t gpoly[RS_NPAR + 1]) {
    /* info exponent in c(x): c_{31-col} -> x^{31-col}.            */
    int info_exp = (RS_K - 1) - col; /* 31 - col */
    /* dividend = x^{info_exp + RS_NPAR}: a single nonzero coefficient. */
    int div_deg = info_exp + RS_NPAR;

    /* polynomial long division of x^div_deg by g(x) (degree RS_NPAR, monic).
     * rem holds the running remainder, indexed by coefficient degree. */
    uint8_t rem[RS_N];
    memset(rem, 0, sizeof(rem));
    rem[div_deg] = 1;
    for (int d = div_deg; d >= RS_NPAR; d--) {
        uint8_t lead = rem[d];
        if (lead == 0) continue;
        /* subtract lead * g(x) shifted so its top term cancels rem[d].
         * g is monic (gpoly[RS_NPAR]=1), so quotient coeff = lead. */
        for (int j = 0; j <= RS_NPAR; j++) {
            rem[d - RS_NPAR + j] ^= gf_mul(lead, gpoly[j]);
        }
    }
    /* now rem[0..RS_NPAR-1] are the parity coefficients g_0..g_222. */

    /* G(x) = info_term*x^{NPAR}=x^{div_deg} XOR parity (the subtracted rem).
     * Code symbol G_r for r=0..254:
     *   r in [RS_NPAR..254] : information part, G_r = c_{r-RS_NPAR}.
     *   r in [0..RS_NPAR-1] : parity part,      G_r = g_r (= rem[r]).
     * Row of G (listing [G254,...,G0]) is row (254 - r). */
    for (int r = 0; r < RS_N; r++) {
        uint8_t sym;
        if (r >= RS_NPAR) {
            sym = (r - RS_NPAR == info_exp) ? 1 : 0; /* c_{r-NPAR} */
        } else {
            sym = rem[r]; /* g_r parity */
        }
        G[RS_N - 1 - r][col] = sym;
    }
}

/* invert a k x k GF(256) matrix in place via Gauss-Jordan elimination.
 * `a` is overwritten; `inv` receives a^{-1}. Returns 0 on success, -1 if
 * singular. */
static int gf_invert(int k, uint8_t a[RS_K][RS_K], uint8_t inv[RS_K][RS_K]) {
    for (int i = 0; i < k; i++) {
        for (int j = 0; j < k; j++) inv[i][j] = (i == j) ? 1 : 0;
    }
    for (int col = 0; col < k; col++) {
        /* find a pivot row with a nonzero entry in this column. */
        int piv = -1;
        for (int r = col; r < k; r++) {
            if (a[r][col] != 0) {
                piv = r;
                break;
            }
        }
        if (piv < 0) return -1; /* singular */
        if (piv != col) {
            for (int j = 0; j < k; j++) {
                uint8_t t = a[col][j];
                a[col][j] = a[piv][j];
                a[piv][j] = t;
                t = inv[col][j];
                inv[col][j] = inv[piv][j];
                inv[piv][j] = t;
            }
        }
        /* scale pivot row so a[col][col] == 1. */
        uint8_t pinv = gf_inv(a[col][col]);
        for (int j = 0; j < k; j++) {
            a[col][j] = gf_mul(a[col][j], pinv);
            inv[col][j] = gf_mul(inv[col][j], pinv);
        }
        /* eliminate this column from every other row. */
        for (int r = 0; r < k; r++) {
            if (r == col) continue;
            uint8_t f = a[r][col];
            if (f == 0) continue;
            for (int j = 0; j < k; j++) {
                a[r][j] ^= gf_mul(f, a[col][j]);
                inv[r][j] ^= gf_mul(f, inv[col][j]);
            }
        }
    }
    return 0;
}

int has_rs_init(void) {
    if (rs_ready) return 0;
    gf_build_tables();
    uint8_t gpoly[RS_NPAR + 1];
    gf_build_genpoly(gpoly);
    for (int col = 0; col < RS_K; col++) rs_encode_column(col, gpoly);
    rs_ready = 1;
    return 0;
}

int has_rs_decode(const uint8_t pages[][PAGE_OCT], const uint8_t* pids, int k, uint8_t* out) {
    if (!pages || !pids || !out) return -1;
    if (k < 1 || k > RS_K) return -1;

    has_rs_init();

    /* validate PIDs: range 1..255 and pairwise distinct. */
    uint8_t seen[256];
    memset(seen, 0, sizeof(seen));
    for (int i = 0; i < k; i++) {
        int p = pids[i];
        if (p < 1 || p > RS_N) return -1;
        if (seen[p]) return -1; /* duplicate */
        seen[p] = 1;
    }

    /* build D (k x k): row i from G row pids[i]-1, first k columns. */
    uint8_t D[RS_K][RS_K], Dinv[RS_K][RS_K];
    for (int i = 0; i < k; i++) {
        int row = pids[i] - 1;
        for (int j = 0; j < k; j++) D[i][j] = G[row][j];
    }
    if (gf_invert(k, D, Dinv) != 0) return -1; /* singular */

    /* for each octet position j, m_j = Dinv * w'_j (ICD Eq. 17). The encoded
     * column w'_j gathers octet j of every received page; the recovered column
     * m_j gives octet j of each message page. */
    for (int oct = 0; oct < PAGE_OCT; oct++) {
        for (int i = 0; i < k; i++) {
            uint8_t acc = 0;
            for (int r = 0; r < k; r++) {
                acc ^= gf_mul(Dinv[i][r], pages[r][oct]);
            }
            out[i * PAGE_OCT + oct] = acc;
        }
    }
    return 0;
}
