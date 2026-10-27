/**
 * @file    fwu_verify.c
 * @brief   Verification pipeline implementation (see fwu_verify.h for the order, codes and rationale).
 */

#include <stddef.h>
#include "fwu_verify.h"

/** Looks up the key_id from the manifest among the trusted keys; NULL if absent. */
static const fwu_verify_key_t *verify_find_key(const fwu_verify_config_t *config, fwu_byte_t key_id)
{
    for(uint32_t i = 0u; i < config->key_count; i++) {
        if((config->keys[i].id & FWU_BYTE_MASK) == key_id) {
            return &config->keys[i];
        }
    }
    return NULL;
}

/** Steps 1-2: reads the manifest from flash (`raw`, FWU_MANIFEST_SIZE elements) and validates its structure. */
static fwu_return_e verify_read_manifest(const fwu_verify_config_t *config, const fwu_flash_t *flash,
                                         fwu_byte_t *raw, fwu_manifest_t *manifest)
{
    const fwu_return_e status = fwu_flash_read(flash, config->manifest_offset, FWU_MANIFEST_SIZE, raw);

    if(status != FWU_OK) {
        return status;                                  /* could not be read: do not reject, return the error */
    }
    return fwu_manifest_parse(raw, FWU_MANIFEST_SIZE, manifest);
}

/** Steps 6-7: key and signature (skipped in the DEV profile, i.e. when require_signature is false). */
static fwu_return_e verify_signature(const fwu_verify_config_t *config, const fwu_byte_t *raw,
                                     const fwu_manifest_t *manifest)
{
    const fwu_verify_key_t *key;

    if(!config->require_signature) {
        return FWU_OK;
    }
    key = verify_find_key(config, manifest->key_id);
    if((key == NULL) || (manifest->key_version < config->min_key_version)) {
        return FWU_ERR_KEY_REJECTED;
    }
    /* BAD_SIGNATURE (reject) or CRYPTO_FAILURE (could not be performed) is returned as is. */
    return fwu_crypto_ed25519_verify(config->crypto, raw, FWU_MANIFEST_SIGNED_SIZE, manifest->signature,
                                     key->public_key);
}

/**
 * Steps 3-7: product, version, size, key and signature (cheap to expensive). `raw` = manifest bytes (for the signed
 * region), `available_length` = the area the image must fit in (the area covered by the commit; the capacity if there is no limit).
 */
static fwu_return_e verify_policy_and_signature(const fwu_verify_config_t *config, const fwu_byte_t *raw,
                                                const fwu_manifest_t *manifest, uint32_t available_length)
{
    if(manifest->product_id != config->product_id) {
        return FWU_ERR_PRODUCT_MISMATCH;
    }
    if(manifest->image_version < config->min_image_version) {
        return FWU_ERR_VERSION_REJECTED;
    }
    if((manifest->image_size > config->max_image_size) || (manifest->image_size > available_length)) {
        return FWU_ERR_IMAGE_TOO_LARGE;
    }
    return verify_signature(config, raw, manifest);
}

/**
 * Computes the SHA-256 of the image from flash chunk by chunk and compares it with the digest in the manifest.
 * If an error occurs midway the digest is CLOSED (so that the next verification starts clean).
 */
static fwu_return_e verify_image_hash(const fwu_verify_config_t *config, const fwu_flash_t *flash,
                                      const fwu_manifest_t *manifest)
{
    fwu_byte_t digest[FWU_CRYPTO_SHA256_DIGEST_SIZE];
    uint32_t done = 0u;
    fwu_return_e status = fwu_crypto_sha256_begin(config->crypto);

    if(status != FWU_OK) {
        return status;
    }
    while(done < manifest->image_size) {
        uint32_t part = manifest->image_size - done;

        if(part > config->work_length) {
            part = config->work_length;
        }
        status = fwu_flash_read(flash, config->image_offset + done, part, config->work);
        if(status == FWU_OK) {
            status = fwu_crypto_sha256_update(config->crypto, config->work, part);
        }
        if(status != FWU_OK) {
            (void)fwu_crypto_sha256_finish(config->crypto, digest);   /* close the digest (harmless if already closed) */
            return status;
        }
        done += part;
    }
    status = fwu_crypto_sha256_finish(config->crypto, digest);
    if(status != FWU_OK) {
        return status;
    }
    for(uint32_t i = 0u; i < FWU_CRYPTO_SHA256_DIGEST_SIZE; i++) {
        if((digest[i] & FWU_BYTE_MASK) != manifest->sha256[i]) {
            return FWU_ERR_HASH_MISMATCH;
        }
    }
    return FWU_OK;
}

/** Body of the pipeline. `available_length`: the area the image must fit in (the area covered by the commit; the capacity if there is no limit). */
static fwu_return_e verify_core(const fwu_verify_config_t *config, const fwu_flash_t *flash,
                                uint32_t available_length, fwu_manifest_t *manifest_out)
{
    fwu_byte_t raw[FWU_MANIFEST_SIZE];
    fwu_manifest_t manifest;
    fwu_return_e status;

    if((config == NULL) || (config->crypto == NULL) || (flash == NULL) || (config->work == NULL) ||
       (config->work_length == 0u) || ((config->key_count > 0u) && (config->keys == NULL))) {
        return FWU_ERR_INVALID_ARG;
    }
    /* image_offset + image size must not wrap in 32 bits (wrapping would mean reading from the wrong region). */
    if(config->image_offset > (0xFFFFFFFFu - config->max_image_size)) {
        return FWU_ERR_INVALID_ARG;
    }

    /* 1-2. Read the manifest and validate its structure. */
    status = verify_read_manifest(config, flash, raw, &manifest);
    if(status != FWU_OK) {
        return status;
    }

    /* 3-7. Product, version, size (cheap policy checks) and key + signature (skipped in the DEV profile). */
    status = verify_policy_and_signature(config, raw, &manifest, available_length);
    if(status != FWU_OK) {
        return status;
    }

    /* 8. The image itself (most expensive step). */
    status = verify_image_hash(config, flash, &manifest);
    if(status != FWU_OK) {
        return status;
    }

    if(manifest_out != NULL) {
        *manifest_out = manifest;
    }
    return FWU_OK;
}

fwu_return_e fwu_verify_image(const fwu_verify_config_t *config, const fwu_flash_t *flash, fwu_manifest_t *manifest)
{
    if(config == NULL) {
        return FWU_ERR_INVALID_ARG;
    }
    return verify_core(config, flash, config->max_image_size, manifest);
}

fwu_return_e fwu_verify_manifest(const fwu_verify_config_t *config, const fwu_flash_t *flash, fwu_manifest_t *manifest)
{
    fwu_byte_t raw[FWU_MANIFEST_SIZE];
    fwu_manifest_t parsed;
    fwu_return_e status;

    if((config == NULL) || (config->crypto == NULL) || (flash == NULL) ||
       ((config->key_count > 0u) && (config->keys == NULL))) {
        return FWU_ERR_INVALID_ARG;
    }

    status = verify_read_manifest(config, flash, raw, &parsed);
    if(status != FWU_OK) {
        return status;
    }
    if(parsed.product_id != config->product_id) {
        return FWU_ERR_PRODUCT_MISMATCH;
    }
    status = verify_signature(config, raw, &parsed);
    if(status != FWU_OK) {
        return status;
    }

    if(manifest != NULL) {
        *manifest = parsed;
    }
    return FWU_OK;
}

fwu_return_e fwu_verify_offer(const fwu_verify_config_t *config, const fwu_byte_t *manifest_bytes,
                              uint32_t manifest_length, fwu_manifest_t *manifest)
{
    fwu_manifest_t parsed;
    fwu_return_e status;

    if((config == NULL) || (config->crypto == NULL) || (manifest_bytes == NULL) ||
       ((config->key_count > 0u) && (config->keys == NULL))) {
        return FWU_ERR_INVALID_ARG;
    }

    status = fwu_manifest_parse(manifest_bytes, manifest_length, &parsed);
    if(status != FWU_OK) {
        return status;
    }
    status = verify_policy_and_signature(config, manifest_bytes, &parsed, config->max_image_size);
    if(status != FWU_OK) {
        return status;
    }

    if(manifest != NULL) {
        *manifest = parsed;
    }
    return FWU_OK;
}

fwu_return_e fwu_verify_activation_hook(void *context, const fwu_flash_t *flash, uint32_t offset, uint32_t length)
{
    const fwu_verify_config_t *config = (const fwu_verify_config_t *)context;

    if((config == NULL) || (flash == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    if(offset != config->image_offset) {
        return FWU_ERR_INVALID_ARG;                     /* activation and verification are looking at different images */
    }
    return verify_core(config, flash, length, NULL);
}
