/**
 * @file    fwu_crypto.h
 * @brief   Cryptography PORT interface: SHA-256 (incremental) and Ed25519 signature VERIFICATION. The core does not
 *          implement cryptography itself; a ready-made library (or hardware) is plugged in behind this interface.
 *          Same pattern as the flash interface (fwu_flash.h): function table + `context` + wrappers.
 *
 * WHY A SEPARATE INTERFACE
 *   - We do not write cryptography ourselves (hard and dangerous); an audited library is used.
 *   - The suitable implementation differs per CPU: a library on STM32, another library or hardware on C2000
 *     (16-bit `char`, no `uint8_t`). The core (verification pipeline) never changes.
 *   - In tests a fake implementation (fwu_crypto_sim.h) is plugged in; the whole pipeline can be exercised without a library.
 *
 * WHAT IT PROVIDES
 *   1. SHA-256, incremental:   sha256_begin -> sha256_update (many times) -> sha256_finish
 *      (the image is read from flash chunk by chunk; it does not all fit in RAM.)
 *   2. Ed25519 verification:   ed25519_verify(message, signature, public key)
 *      (The device only VERIFIES; it does NOT produce signatures, the private key is not present on the device.)
 *
 * SIZES (elements are fwu_byte_t, one byte value 0..255 per element): digest 32, public key 32, signature 64.
 *
 * ERROR CODES (verification result)
 *   FWU_OK                  signature is VALID.
 *   FWU_ERR_BAD_SIGNATURE   signature is INVALID (forged, corrupted, wrong key): the image is REJECTED.
 *   FWU_ERR_CRYPTO_FAILURE  the operation could NOT BE PERFORMED (library/hardware error...): does NOT mean the signature
 *                           is invalid; the image is not rejected, the error is propagated, a retry is possible.
 *   This distinction is for security: counting "could not verify" as "valid", or "invalid" as "could not be performed",
 *   are both wrong. If the port returns any other code, the WRAPPER converts it to FWU_ERR_CRYPTO_FAILURE
 *   (e.g. if a port mistakenly returns FWU_ERR_CRC_MISMATCH, activation must not take it for "image invalid").
 *   Guarantee: the wrapper returns FWU_OK ONLY when the port returns FWU_OK.
 *
 * SHA-256 STATE MACHINE (tracked by the wrapper: fwu_crypto_t::hashing)
 *   begin: hashing must be false (otherwise FWU_ERR_BAD_STATE: the previous digest is not finished).
 *   update, finish: hashing must be true (otherwise FWU_ERR_BAD_STATE: begin was not called).
 *   If the port reports an error, the digest becomes INVALID and is closed (hashing=false): a new begin is required.
 *   finish always closes the digest. Argument errors (NULL...) do not affect the digest (caller error).
 *   ONLY ONE digest can be open at a time.
 *
 * This interface is not reentrant and not thread-safe.
 */

#ifndef FWU_CRYPTO_H_
#define FWU_CRYPTO_H_

#include <stdbool.h>
#include <stdint.h>
#include "fwu_error.h"
#include "fwu_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ sizes */

#define FWU_CRYPTO_SHA256_DIGEST_SIZE           32u
#define FWU_CRYPTO_ED25519_PUBLIC_KEY_SIZE      32u
#define FWU_CRYPTO_ED25519_SIGNATURE_SIZE       64u

/* ------------------------------------------------------------------ port interface */

/**
 * Functions the port must implement (all mandatory). `context` is the port's own data (e.g. the SHA-256 state);
 * the core does not look inside it, it only passes it back to the functions.
 */
typedef struct {
    /** Starts a new SHA-256 computation. @return FWU_OK or an error. */
    fwu_return_e (*sha256_begin)(void *context);

    /** Feeds data into the digest (`length` >= 1, `data` not NULL: guaranteed by the wrapper). */
    fwu_return_e (*sha256_update)(void *context, const fwu_byte_t *data, uint32_t length);

    /** Finishes the digest and writes FWU_CRYPTO_SHA256_DIGEST_SIZE elements to `digest` (0..255 per element). */
    fwu_return_e (*sha256_finish)(void *context, fwu_byte_t *digest);

    /**
     * Verifies an Ed25519 signature. `message` >= 1 element; `signature` 64, `public_key` 32 elements.
     * @return FWU_OK (valid), FWU_ERR_BAD_SIGNATURE (invalid) or FWU_ERR_CRYPTO_FAILURE (could not be performed).
     */
    fwu_return_e (*ed25519_verify)(void *context, const fwu_byte_t *message, uint32_t message_length,
                                   const fwu_byte_t *signature, const fwu_byte_t *public_key);
} fwu_crypto_callback_t;

/** Cryptography object. The caller fills in `context` and `callback` and sets `hashing` to false. */
typedef struct {
    void                        *context;    /**< The port's own data. */
    const fwu_crypto_callback_t *callback;   /**< Port function table. */
    bool                         hashing;    /**< Whether a SHA-256 computation is open (managed by the wrapper, do not touch). */
} fwu_crypto_t;

/* ------------------------------------------------------------------ wrappers (used by the core) */

/**
 * @brief  Checks that the object is usable (object, table and none of the four functions is NULL) and sets
 *         `hashing` to false.
 * @return FWU_OK; FWU_ERR_INVALID_ARG if any of them is NULL.
 */
fwu_return_e fwu_crypto_init(fwu_crypto_t *self, void *context, const fwu_crypto_callback_t *callback);

/** Starts a SHA-256 computation. @return FWU_OK; INVALID_ARG; BAD_STATE if already open; CRYPTO_FAILURE on port error. */
fwu_return_e fwu_crypto_sha256_begin(fwu_crypto_t *self);

/**
 * @brief  Feeds data into the digest. If `length` is 0 it does nothing (FWU_OK, `data` may be NULL, the port is not called).
 * @return FWU_OK; INVALID_ARG for a NULL object or (when length > 0) NULL data; BAD_STATE if no digest is open;
 *         CRYPTO_FAILURE on port error (and the digest is closed).
 */
fwu_return_e fwu_crypto_sha256_update(fwu_crypto_t *self, const fwu_byte_t *data, uint32_t length);

/**
 * @brief  Finishes the digest; writes 32 elements to `digest`. Always closes the digest.
 * @return FWU_OK; INVALID_ARG for NULL; BAD_STATE if no digest is open; CRYPTO_FAILURE on port error
 *         (`digest` is not trustworthy on error).
 */
fwu_return_e fwu_crypto_sha256_finish(fwu_crypto_t *self, fwu_byte_t *digest);

/**
 * @brief  Verifies an Ed25519 signature.
 * @param  message         The signed message (must not be NULL).
 * @param  message_length  >= 1 (cannot be 0: signing an empty message is meaningless, the signed region of the manifest is 74 elements).
 * @param  signature       64 elements.
 * @param  public_key      32 elements.
 * @return FWU_OK (VALID); FWU_ERR_BAD_SIGNATURE (INVALID); FWU_ERR_CRYPTO_FAILURE (could not be performed, port error);
 *         FWU_ERR_INVALID_ARG for NULL or message_length == 0.
 */
fwu_return_e fwu_crypto_ed25519_verify(fwu_crypto_t *self, const fwu_byte_t *message, uint32_t message_length,
                                       const fwu_byte_t *signature, const fwu_byte_t *public_key);

#ifdef __cplusplus
}
#endif

#endif /* FWU_CRYPTO_H_ */
