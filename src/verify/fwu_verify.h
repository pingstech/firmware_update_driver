/**
 * @file    fwu_verify.h
 * @brief   VERIFICATION PIPELINE: checks, in order, the manifest of the image in staging and the image itself.
 *          The function that plugs into the `verify` hook of activation (fwu_activate.h) lives here. Built on manifest reading
 *          (fwu_manifest.h) and the cryptography interface (fwu_crypto.h).
 *
 * INSIDE STAGING (fwu_layout.h): [manifest page][image pages]. The manifest is in the FIRST page of staging
 * (config.manifest_offset), the image starts at the second page (config.image_offset).
 *
 * CHECK ORDER (CHEAP TO EXPENSIVE; the first mismatch determines the result, later checks are never reached)
 *   1. Read the manifest from flash (138 elements)                   -> flash error is passed on as is (NOT a rejection)
 *   2. Manifest STRUCTURE (fwu_manifest_parse)                       -> FWU_ERR_BAD_MANIFEST
 *   3. Product id                                                    -> FWU_ERR_PRODUCT_MISMATCH
 *   4. Image version >= lowest accepted version (anti-rollback)      -> FWU_ERR_VERSION_REJECTED
 *   5. Image size <= capacity (and the area covered by the commit)   -> FWU_ERR_IMAGE_TOO_LARGE
 *   6. (if signature is required) Key: id exists on the device, key version >= minimum  -> FWU_ERR_KEY_REJECTED
 *   7. (if signature is required) Ed25519 signature (first 74 elements of the manifest) -> FWU_ERR_BAD_SIGNATURE / FWU_ERR_CRYPTO_FAILURE
 *   8. Is the SHA-256 of the image equal to the digest in the manifest (read from flash chunk by chunk) -> FWU_ERR_HASH_MISMATCH
 *   FWU_OK: all passed.
 *
 *   Why this order: the structure and product/version/size/key checks are a few comparisons; the signature is an expensive
 *   computation; the hash of the image is the most expensive (reading hundreds of KB). A forged manifest is rejected at the
 *   signature step without ever touching the image. NOTE: steps 3-5 look at unsigned fields; they are for EARLY REJECTION only,
 *   the authoritative decision is signature + hash (the fields are covered by the signature, if an attacker changes them the signature will not match).
 *
 * "REJECTED" versus "COULD NOT BE PERFORMED" distinction (activation uses this, see fwu_activate.h):
 *   Unacceptable-image codes (activation writes REJECTED, the application is unchanged): BAD_MANIFEST, PRODUCT_MISMATCH,
 *   VERSION_REJECTED, IMAGE_TOO_LARGE, KEY_REJECTED, BAD_SIGNATURE, HASH_MISMATCH.
 *   "Verification could not be performed" codes (REJECTED is NOT written, the error is propagated, a retry is possible): FWU_ERR_CRYPTO_FAILURE,
 *   flash read errors, FWU_ERR_INVALID_ARG (configuration error).
 *
 * SIGNATURE REQUIREMENT (config.require_signature): true in the verifying profiles (INDUSTRIAL, AUTOMOTIVE). false
 * is only for the DEV profile: steps 6-7 are skipped (no key needed), product/version/size/hash are still checked.
 * This is a COMPILE-TIME decision (the integration supplies it from the fwu_config.h profile); it cannot be selected over the
 * link, otherwise an attacker could have the unsigned mode selected (downgrade attack).
 *
 * CLEANUP AFTER AN ERROR: if the SHA-256 computation gets an error midway (flash read, crypto) the pipeline CLOSES the digest; the next
 * verification starts clean.
 */

#ifndef FWU_VERIFY_H_
#define FWU_VERIFY_H_

#include <stdbool.h>
#include <stdint.h>
#include "fwu_error.h"
#include "fwu_types.h"
#include "fwu_flash.h"
#include "fwu_manifest.h"
#include "fwu_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A public key the device trusts, and its id. */
typedef struct {
    fwu_byte_t id;                                              /**< Matches key_id in the manifest. */
    fwu_byte_t public_key[FWU_CRYPTO_ED25519_PUBLIC_KEY_SIZE];  /**< Ed25519 public key (32 elements). */
} fwu_verify_key_t;

/** Verification configuration (filled in by the caller; must remain valid for the duration of verification). */
typedef struct {
    fwu_crypto_t           *crypto;             /**< Cryptography object (the SHA-256 state changes). */
    uint32_t                manifest_offset;    /**< Flash offset of the manifest (fwu_layout: first page of staging). */
    uint32_t                image_offset;       /**< Flash offset of the image (after the manifest page). */
    uint32_t                max_image_size;     /**< Largest accepted image size (fwu_byte_t; staging capacity). */
    uint32_t                product_id;         /**< Product id of this device. */
    uint32_t                min_image_version;  /**< LOWEST accepted image version (anti-rollback counter; equal is accepted). */
    uint16_t                min_key_version;    /**< LOWEST accepted key version. */
    const fwu_verify_key_t *keys;               /**< Trusted public keys (must not be NULL if key_count > 0). */
    uint32_t                key_count;          /**< Number of keys. */
    bool                    require_signature;  /**< true: signature and key check mandatory (everything except DEV). */
    fwu_byte_t             *work;               /**< Read buffer (for SHA-256, chunk by chunk from flash). */
    uint32_t                work_length;        /**< Number of work elements (>= 1). */
} fwu_verify_config_t;

/**
 * @brief  Verifies the image in staging (in the order above).
 * @param  config    Configuration (must not be NULL).
 * @param  flash     Flash from which the manifest and image are read.
 * @param  manifest  On success the parsed manifest is written here (may be NULL); UNCHANGED on error.
 * @return FWU_OK; for an unacceptable image BAD_MANIFEST / PRODUCT_MISMATCH / VERSION_REJECTED / IMAGE_TOO_LARGE /
 *         KEY_REJECTED / BAD_SIGNATURE / HASH_MISMATCH; for could-not-be-performed CRYPTO_FAILURE or a flash error;
 *         INVALID_ARG for a NULL field, work_length 0, keys NULL while key_count > 0, image offset + size overflow.
 */
fwu_return_e fwu_verify_image(const fwu_verify_config_t *config, const fwu_flash_t *flash, fwu_manifest_t *manifest);

/**
 * @brief  Verifies the authenticity of ONLY THE MANIFEST: structure, product id, key and signature (steps 1-3 and 6-7).
 *         Does NOT look at the image version, size or the SHA-256 of the image. Purpose: to answer, without re-reading the
 *         image, "is this manifest signed and genuine for this product?". Used by: the approval hook of the version counters
 *         (fwu_counters.h): the counter can be raised ONLY from a signed manifest, otherwise someone able to write to staging
 *         could set the counter to 0xFFFFFFFF and block all later updates.
 *         In the DEV profile (require_signature false) the signature is skipped.
 * @param  config    Configuration: crypto, manifest_offset, product_id, min_key_version, keys, key_count,
 *                   require_signature are used (work and image fields are NOT used).
 * @param  flash     Flash from which the manifest is read.
 * @param  manifest  On success the parsed manifest is written (may be NULL); UNCHANGED on error.
 * @return FWU_OK; BAD_MANIFEST / PRODUCT_MISMATCH / KEY_REJECTED / BAD_SIGNATURE (unacceptable);
 *         CRYPTO_FAILURE or a flash read error (could not be performed); INVALID_ARG for a NULL field or keys NULL
 *         while key_count > 0.
 */
fwu_return_e fwu_verify_manifest(const fwu_verify_config_t *config, const fwu_flash_t *flash, fwu_manifest_t *manifest);

/**
 * @brief  Evaluates an image OFFER: the manifest bytes are in RAM (not yet written to flash) and the image has not been
 *         downloaded yet. Steps 2-7 (structure, product, version >= minimum, size <= capacity, key, signature); the SHA-256 of the
 *         image is not checked (there is no image). Used by: the session (fwu_session.h): not a SINGLE byte is written to or erased from
 *         staging before the offer passes this function, so an unsigned request cannot wear out the flash.
 * @param  config            Configuration: crypto, product_id, min_image_version, min_key_version, max_image_size, keys,
 *                           key_count, require_signature are used (flash offsets and work are NOT used).
 * @param  manifest_bytes    Manifest bytes (must not be NULL).
 * @param  manifest_length   Number of bytes (>= FWU_MANIFEST_SIZE, otherwise NOT_ENOUGH_DATA).
 * @param  manifest          On success the parsed manifest is written (may be NULL); UNCHANGED on error.
 * @return FWU_OK; NOT_ENOUGH_DATA (short) / BAD_MANIFEST / PRODUCT_MISMATCH / VERSION_REJECTED / IMAGE_TOO_LARGE /
 *         KEY_REJECTED / BAD_SIGNATURE (unacceptable); CRYPTO_FAILURE (could not be performed); INVALID_ARG for a NULL field or
 *         keys NULL while key_count > 0.
 */
fwu_return_e fwu_verify_offer(const fwu_verify_config_t *config, const fwu_byte_t *manifest_bytes,
                              uint32_t manifest_length, fwu_manifest_t *manifest);

/**
 * @brief  The `verify` hook of activation (fwu_activate_verify_fn): `context` is a fwu_verify_config_t.
 *         Runs fwu_verify_image and additionally checks that the area covered by the commit (`length`) satisfies the image
 *         size in the manifest (if the commit supplied too few pages the image would be copied truncated): if it does not,
 *         FWU_ERR_IMAGE_TOO_LARGE.
 * @param  context  `const fwu_verify_config_t *`.
 * @param  flash    Flash used by activation.
 * @param  offset   Image offset of activation: must be the SAME as config.image_offset (otherwise FWU_ERR_INVALID_ARG:
 *                  verification and activation are looking at different images).
 * @param  length   Area covered by the commit (number of pages * page size).
 */
fwu_return_e fwu_verify_activation_hook(void *context, const fwu_flash_t *flash, uint32_t offset, uint32_t length);

#ifdef __cplusplus
}
#endif

#endif /* FWU_VERIFY_H_ */
