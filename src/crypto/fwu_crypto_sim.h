/**
 * @file    fwu_crypto_sim.h
 * @brief   FAKE CRYPTOGRAPHY PORT (TEST/PC ONLY): implements the fwu_crypto.h interface; intended for exercising the verification
 *          pipeline (task G7c) without a real library. Counterpart of fwu_flash_sim.h on the flash side.
 *
 *  !!! NOT SECURE, MUST NOT BE USED IN PRODUCTION !!!  (raises #error if built with -DFWU_PRODUCTION)
 *
 * WHAT IS REAL, WHAT IS FAKE
 *   - SHA-256: REAL (FIPS 180-4 algorithm; checked against the official test vectors and Python hashlib).
 *     So the SHA-256 in the manifest can be exercised with real digests. It is slow (byte by byte), for tests only.
 *   - Signature: FAKE and INSECURE. "Signature" = SHA-256(public key || message) concatenated with SHA-256(message || public
 *     key) (64 elements). There is NO private key (anyone who knows the public key can produce the signature). Its only purpose
 *     is this: correct key + correct message + correct signature = valid; if any of the three changes = invalid.
 *     We do NOT write a real Ed25519; the real port will be a library.
 *
 * TEST HELPERS
 *   - fwu_crypto_sim_sign(): produces a fake signature (stands in for "the producer's signature" in tests).
 *   - Fault injection: if a code other than FWU_OK is put into the fail_begin / fail_update / fail_finish / fail_verify fields,
 *     that operation returns that code (e.g. FWU_ERR_BAD_SIGNATURE or FWU_ERR_CRYPTO_FAILURE or a nonsensical code).
 *   - Counters: how many times each function was called (e.g. for the test "signature verification must NEVER be called when the structure is corrupt").
 */

#ifndef FWU_CRYPTO_SIM_H_
#define FWU_CRYPTO_SIM_H_

#if defined(FWU_PRODUCTION)
#error "fwu_crypto_sim is NOT SECURE: for tests only, it cannot be built with FWU_PRODUCTION"
#endif

#include <stdint.h>
#include "fwu_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

/** SHA-256 working state (INTERNAL; used inside the fake port and the fake signature). */
typedef struct {
    uint32_t   h[8];            /**< Intermediate digest. */
    fwu_byte_t block[64];       /**< 64-element block being filled. */
    uint32_t   block_length;    /**< Number of elements in the block. */
    uint32_t   total_length;    /**< Total number of elements fed into the digest so far. */
} fwu_crypto_sim_sha_t;

/** Fake port state. Give the `crypto` field to the core/tests. */
typedef struct {
    fwu_crypto_t           crypto;          /**< Interface object (context = this structure). */
    fwu_crypto_callback_t  callback;        /**< Function table. */
    fwu_crypto_sim_sha_t   sha;             /**< Open SHA-256 computation. */
    fwu_return_e           fail_begin;      /**< Other than FWU_OK: begin returns this. */
    fwu_return_e           fail_update;     /**< Other than FWU_OK: update returns this. */
    fwu_return_e           fail_finish;     /**< Other than FWU_OK: finish returns this. */
    fwu_return_e           fail_verify;     /**< Other than FWU_OK: verify returns this. */
    uint32_t               begin_calls;     /**< Number of sha256_begin calls. */
    uint32_t               update_calls;    /**< Number of sha256_update calls. */
    uint32_t               finish_calls;    /**< Number of sha256_finish calls. */
    uint32_t               verify_calls;    /**< Number of ed25519_verify calls. */
    uint32_t               last_verify_message_length;  /**< Length of the last verified message. */
} fwu_crypto_sim_t;

/** Initialises the fake port (fault injection off, counters zero) and fills the `crypto` field. */
fwu_return_e fwu_crypto_sim_init(fwu_crypto_sim_t *sim);

/** One-shot SHA-256 (for independent digest computation in tests). `digest` is 32 elements. */
void fwu_crypto_sim_sha256(const fwu_byte_t *data, uint32_t length, fwu_byte_t *digest);

/**
 * Produces a fake signature (64 elements): SHA-256(public key || message) concatenated with SHA-256(message || public key).
 * Used in tests in place of "the producer signed". NOT SECURE.
 */
void fwu_crypto_sim_sign(const fwu_byte_t *public_key, const fwu_byte_t *message, uint32_t message_length,
                         fwu_byte_t *signature);

#ifdef __cplusplus
}
#endif

#endif /* FWU_CRYPTO_SIM_H_ */
