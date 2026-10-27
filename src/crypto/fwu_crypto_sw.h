/**
 * @file    fwu_crypto_sw.h
 * @brief   SOFTWARE CRYPTOGRAPHY PORT: implements the SHA-256 part of the fwu_crypto.h interface for REAL (fwu_sha256) and the
 *          Ed25519 part as a SAFE PLACEHOLDER. Usable in production (no test guard); but Ed25519 is NOT PRESENT.
 *
 * NO ED25519: `ed25519_verify` always returns FWU_ERR_CRYPTO_FAILURE ("verification COULD NOT BE PERFORMED"): it never says "valid", so
 *   if a signed profile (INDUSTRIAL/AUTOMOTIVE) is enabled with this port, NO image is accepted (fail-closed; the safe direction). The DEV
 *   profile (no signature, SHA-256 digest only: FWU_BOOT_VERIFY_HASH) works with this port. When a real Ed25519 arrives only this function changes.
 *   This is the resolution of README open issue 37 for DEV; it is NOT a solution for the signed profiles.
 *
 * Usage:
 *   static fwu_crypto_sw_t sw;  static fwu_crypto_t crypto;
 *   fwu_crypto_sw_init(&sw, &crypto);     // fills the crypto object and validates it with fwu_crypto_init
 */

#ifndef FWU_CRYPTO_SW_H_
#define FWU_CRYPTO_SW_H_

#include "fwu_error.h"
#include "fwu_crypto.h"
#include "fwu_sha256.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Port state (SHA-256 context + function table). Must stay FIXED in place: `crypto.context` points to this structure. */
typedef struct {
    fwu_sha256_t          sha;
    fwu_crypto_callback_t callback;
} fwu_crypto_sw_t;

/**
 * @brief  Initialises the port and binds the `crypto` object (via fwu_crypto_init) to this port.
 * @return FWU_OK; FWU_ERR_INVALID_ARG for NULL.
 */
fwu_return_e fwu_crypto_sw_init(fwu_crypto_sw_t *sw, fwu_crypto_t *crypto);

#ifdef __cplusplus
}
#endif

#endif /* FWU_CRYPTO_SW_H_ */
