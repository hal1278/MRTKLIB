/*------------------------------------------------------------------------------
 * mrtk_has_rs.h : Galileo HAS RS(255,32,224) erasure decoder (internal header)
 *
 * High-Parity Vertical Reed-Solomon (HPVRS) outer-layer decoder for Galileo
 * HAS message pages, per Galileo HAS SIS ICD Issue 1.0 (May 2022) section 6.
 *
 * GF(256) is defined by the primitive polynomial p(a) = a^8+a^4+a^3+a^2+1
 * (0x11D). The systematic 255x32 generator matrix G = [I32; P] is built at
 * init by polynomial-remainder encoding of the 32 unit information vectors
 * (ICD section 6.2.2); the decoder recovers a k-page message from any k
 * distinct received encoded pages (ICD section 6.4).
 *
 * Internal to src/has/ — not part of the public mrtklib API.
 *-----------------------------------------------------------------------------*/
#ifndef MRTK_HAS_RS_H
#define MRTK_HAS_RS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Build the GF(256) log/antilog tables and the systematic generator
 *        matrix G. Idempotent: safe to call repeatedly; builds only once.
 * @return 0 on success.
 */
int has_rs_init(void);

/**
 * @brief Decode a k-page HAS message from k distinct received encoded pages
 *        using the RS(255,32,224) erasure decoder (ICD section 6.4).
 *
 * Each received page is a 53-octet encoded payload carrying Page ID @p pids[i]
 * (1..255). The encoded page with PID p corresponds to row p-1 of G; output
 * column j (0-based) corresponds to message page j+1.
 *
 * @param[in]  pages  k encoded pages, pages[i] is the 53-octet payload of PID
 *                    pids[i].
 * @param[in]  pids   k Page IDs (each 1..255), all distinct.
 * @param[in]  k      number of received pages / message pages, 1 <= k <= 32.
 * @param[out] out    recovered message, k*53 octets (message pages
 *                    concatenated, page 1 first).
 * @return 0 on success, -1 on invalid input (bad k, PID out of range,
 *         duplicate PID, or singular decoding matrix).
 */
int has_rs_decode(const uint8_t pages[][53], const uint8_t* pids, int k, uint8_t* out);

#ifdef __cplusplus
}
#endif

#endif /* MRTK_HAS_RS_H */
