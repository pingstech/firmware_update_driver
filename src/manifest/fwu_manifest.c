/**
 * @file    fwu_manifest.c
 * @brief   Manifest reading and structure validation (see fwu_manifest.h for the format and rules).
 */

#include <stddef.h>
#include "fwu_manifest.h"
#include "fwu_bytes.h"

/** Copies `count` elements from `source` to `destination`; each element is limited to 0..255 (upper bits may be garbage on the C28x). */
static void manifest_copy_bytes(fwu_byte_t *destination, const fwu_byte_t *source, uint32_t count)
{
    for(uint32_t i = 0u; i < count; i++) {
        destination[i] = (fwu_byte_t)(source[i] & FWU_BYTE_MASK);
    }
}

fwu_return_e fwu_manifest_parse(const fwu_byte_t *buffer, uint32_t buffer_length, fwu_manifest_t *manifest)
{
    fwu_manifest_t parsed;      /* the result is collected here first; copied to the output only on success */

    if((buffer == NULL) || (manifest == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }

    /* Missing data is not corruption: the length is checked first, the content is not looked at. */
    if(buffer_length < FWU_MANIFEST_SIZE) {
        return FWU_ERR_NOT_ENOUGH_DATA;
    }

    if(fwu_read_u32_big_endian(&buffer[FWU_MANIFEST_OFFSET_MAGIC]) != FWU_MANIFEST_MAGIC) {
        return FWU_ERR_BAD_MANIFEST;
    }

    parsed.format_version = (fwu_byte_t)(buffer[FWU_MANIFEST_OFFSET_FORMAT] & FWU_BYTE_MASK);
    if(parsed.format_version != FWU_MANIFEST_FORMAT_VERSION) {
        return FWU_ERR_BAD_MANIFEST;
    }

    /* The reserved field must be zero: so that old code does not silently accept a new format if it is given a meaning in the future. */
    for(uint32_t i = 0u; i < FWU_MANIFEST_RESERVED_SIZE; i++) {
        if((buffer[FWU_MANIFEST_OFFSET_RESERVED + i] & FWU_BYTE_MASK) != 0u) {
            return FWU_ERR_BAD_MANIFEST;
        }
    }

    parsed.image_size = fwu_read_u32_big_endian(&buffer[FWU_MANIFEST_OFFSET_IMAGE_SIZE]);
    if(parsed.image_size == 0u) {
        return FWU_ERR_BAD_MANIFEST;
    }

    /* The structure is valid: read the remaining fields. The signature is only copied, not verified (G7b-G7d). */
    parsed.key_id        = (fwu_byte_t)(buffer[FWU_MANIFEST_OFFSET_KEY_ID] & FWU_BYTE_MASK);
    parsed.product_id    = fwu_read_u32_big_endian(&buffer[FWU_MANIFEST_OFFSET_PRODUCT_ID]);
    parsed.image_version = fwu_read_u32_big_endian(&buffer[FWU_MANIFEST_OFFSET_IMAGE_VERSION]);
    parsed.key_version   = fwu_read_u16_big_endian(&buffer[FWU_MANIFEST_OFFSET_KEY_VERSION]);
    manifest_copy_bytes(parsed.sha256, &buffer[FWU_MANIFEST_OFFSET_SHA256], FWU_MANIFEST_SHA256_SIZE);
    manifest_copy_bytes(parsed.commit_hash, &buffer[FWU_MANIFEST_OFFSET_COMMIT_HASH], FWU_MANIFEST_COMMIT_HASH_SIZE);
    manifest_copy_bytes(parsed.signature, &buffer[FWU_MANIFEST_OFFSET_SIGNATURE], FWU_MANIFEST_SIGNATURE_SIZE);

    *manifest = parsed;
    return FWU_OK;
}
