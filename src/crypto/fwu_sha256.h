/**
 * @file    fwu_sha256.h
 * @brief   SHA-256 (FIPS 180-4), incremental (streaming), software implementation. USABLE IN PRODUCTION (unlike the fake port in
 *          fwu_crypto_sim.h, it has no test guard). No dynamic memory, no extra dependencies.
 *
 * Usage:  fwu_sha256_t s;  fwu_sha256_init(&s);  fwu_sha256_update(&s, a, la);  fwu_sha256_update(&s, b, lb);  fwu_sha256_final(&s, digest);
 *
 * C28x: fwu_byte_t is 16 bits; every element is taken as 0..255 and its low 8 bits (FWU_BYTE_MASK) are used; digest elements are 0..255.
 * Size limit: correct for total message lengths up to 2^61 - 1 bytes (bit count held in two 32-bit words); practically unlimited.
 * Speed: simple 64-round loop per 64-byte block (code size is the priority for Cortex-M0+; ~1 KB code, 256-byte constant table).
 * Test: crypto/test_sha256.c (official NIST vectors, hashlib vectors, every split point, cross-check against the fake port).
 */

#ifndef FWU_SHA256_H_
#define FWU_SHA256_H_

#include <stdint.h>
#include "fwu_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FWU_SHA256_DIGEST_SIZE      32u
#define FWU_SHA256_BLOCK_SIZE       64u

/** State. Do not touch the fields directly. */
typedef struct {
    uint32_t   h[8];                            /**< Intermediate digest. */
    uint32_t   bits_hi;                         /**< Total bit count, high word. */
    uint32_t   bits_lo;                         /**< Total bit count, low word. */
    uint32_t   block_length;                    /**< Number of pending bytes in the block (0..63). */
    fwu_byte_t block[FWU_SHA256_BLOCK_SIZE];    /**< Pending (not yet processed) bytes. */
} fwu_sha256_t;

/** Starts a new computation. */
void fwu_sha256_init(fwu_sha256_t *s);

/** Feeds data into the digest. `length` may be 0 (does nothing). */
void fwu_sha256_update(fwu_sha256_t *s, const fwu_byte_t *data, uint32_t length);

/** Finishes the computation and writes FWU_SHA256_DIGEST_SIZE elements to `digest`. Afterwards the state must be re-initialised with init. */
void fwu_sha256_final(fwu_sha256_t *s, fwu_byte_t *digest);

#ifdef __cplusplus
}
#endif

#endif /* FWU_SHA256_H_ */
