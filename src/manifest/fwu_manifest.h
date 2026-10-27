/**
 * @file    fwu_manifest.h
 * @brief   Manifest: the image's "identity card". A small, FIXED-size, signed data block that comes with the
 *          image. This module (G7a) only READS the bytes and validates their STRUCTURE; it does NOT CHECK the
 *          signature, product, version or hash (those are G7b-G7d: the cryptography interface and the
 *          verification pipeline).
 *
 * WHY SEPARATE: the structure check is cheap and is done BEFORE the signature. Garbage or corrupt data is
 * rejected immediately without ever entering the expensive signature verification. Also, since the fields
 * are read from big-endian, unaligned bytes, collecting this knowledge in one place preserves portability
 * (the 16-bit byte of the C28x).
 *
 * FORMAT (array of fwu_byte_t, multi-byte fields BIG endian; unit: element = one byte value):
 *
 *   offset size  field            meaning
 *     0      4    magic            magic number 0x46574D46 ("FWMF")
 *     4      1    format_version   manifest format version (now 1)
 *     5      1    key_id           which public key verifies the signature
 *     6      2    reserved         must be zero
 *     8      4    product_id       product identifier (so that another product's image is not installed)
 *    12      4    image_version    image version (compared with the anti-rollback counter)
 *    16      2    key_version      key version (to reject a stolen key)
 *    18      4    image_size       image size (number of fwu_byte_t); cannot be 0
 *    22     32    sha256           SHA-256 digest of the image
 *    54     20    commit_hash      source code version the image was built from (for traceability)
 *    ---------------------------------------------------------------------------------
 *    74     64    signature        signature of the 74 bytes above (Ed25519: 64 bytes)
 *   total 138
 *
 * SIGNED REGION: from offset 0 to 74 (everything EXCEPT the signature). The verification pipeline (G7c)
 * gives the first FWU_MANIFEST_SIGNED_SIZE elements of `buffer` to the cryptography interface as the
 * signed region; that is why the fields come BEFORE the signature and the signature is LAST.
 *
 * RULES
 *   - Reading is a pure operation: `buffer` is not modified, nothing else is written to flash/memory.
 *   - The output (`manifest`) is written ONLY on success; if an error is returned it stays as it was.
 *   - If `buffer_length` is greater than FWU_MANIFEST_SIZE the excess is ignored (the manifest may be at the
 *     start of a stream); if smaller, it is a waiting/missing-data condition (FWU_ERR_NOT_ENOUGH_DATA), NOT corruption.
 *   - Elements are fwu_byte_t: on the C28x the upper bits may hold garbage; every read must be limited
 *     with `& FWU_BYTE_MASK` (the read functions of fwu_bytes.h do this, you do it when copying arrays).
 *   - This module does NOT compute element counts with `sizeof`/`memcpy`/`memset` (the unit of sizeof differs on the C28x).
 *
 * HAND-WRITTEN EXAMPLE (test_manifest.c uses the same bytes):
 *   46 57 4D 46 | 01 | 02 | 00 00 | 00 00 30 39 | 00 00 00 19 | 00 03 | 00 03 60 00 | <32 sha> | <20 commit> | <64 signature>
 *   -> product_id 12345, image_version 25, key_version 3, image_size 221184, key_id 2.
 */

#ifndef FWU_MANIFEST_H_
#define FWU_MANIFEST_H_

#include <stdint.h>
#include "fwu_error.h"
#include "fwu_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ format constants */

#define FWU_MANIFEST_MAGIC              0x46574D46u    /* "FWMF" */
#define FWU_MANIFEST_FORMAT_VERSION     1u

#define FWU_MANIFEST_MAGIC_SIZE         4u
#define FWU_MANIFEST_FORMAT_SIZE        1u
#define FWU_MANIFEST_KEY_ID_SIZE        1u
#define FWU_MANIFEST_RESERVED_SIZE      2u
#define FWU_MANIFEST_PRODUCT_ID_SIZE    4u
#define FWU_MANIFEST_IMAGE_VERSION_SIZE 4u
#define FWU_MANIFEST_KEY_VERSION_SIZE   2u
#define FWU_MANIFEST_IMAGE_SIZE_SIZE    4u
#define FWU_MANIFEST_SHA256_SIZE        32u
#define FWU_MANIFEST_COMMIT_HASH_SIZE   20u
#define FWU_MANIFEST_SIGNATURE_SIZE     64u

/* Offsets are DERIVED from the previous field's offset + size (not written by hand: if a field's size changes
 * the following ones shift automatically, no inconsistency arises). */
#define FWU_MANIFEST_OFFSET_MAGIC           0u
#define FWU_MANIFEST_OFFSET_FORMAT          (FWU_MANIFEST_OFFSET_MAGIC + FWU_MANIFEST_MAGIC_SIZE)
#define FWU_MANIFEST_OFFSET_KEY_ID          (FWU_MANIFEST_OFFSET_FORMAT + FWU_MANIFEST_FORMAT_SIZE)
#define FWU_MANIFEST_OFFSET_RESERVED        (FWU_MANIFEST_OFFSET_KEY_ID + FWU_MANIFEST_KEY_ID_SIZE)
#define FWU_MANIFEST_OFFSET_PRODUCT_ID      (FWU_MANIFEST_OFFSET_RESERVED + FWU_MANIFEST_RESERVED_SIZE)
#define FWU_MANIFEST_OFFSET_IMAGE_VERSION   (FWU_MANIFEST_OFFSET_PRODUCT_ID + FWU_MANIFEST_PRODUCT_ID_SIZE)
#define FWU_MANIFEST_OFFSET_KEY_VERSION     (FWU_MANIFEST_OFFSET_IMAGE_VERSION + FWU_MANIFEST_IMAGE_VERSION_SIZE)
#define FWU_MANIFEST_OFFSET_IMAGE_SIZE      (FWU_MANIFEST_OFFSET_KEY_VERSION + FWU_MANIFEST_KEY_VERSION_SIZE)
#define FWU_MANIFEST_OFFSET_SHA256          (FWU_MANIFEST_OFFSET_IMAGE_SIZE + FWU_MANIFEST_IMAGE_SIZE_SIZE)
#define FWU_MANIFEST_OFFSET_COMMIT_HASH     (FWU_MANIFEST_OFFSET_SHA256 + FWU_MANIFEST_SHA256_SIZE)
#define FWU_MANIFEST_OFFSET_SIGNATURE       (FWU_MANIFEST_OFFSET_COMMIT_HASH + FWU_MANIFEST_COMMIT_HASH_SIZE)

/** Length of the signed region (excluding the signature). */
#define FWU_MANIFEST_SIGNED_SIZE            FWU_MANIFEST_OFFSET_SIGNATURE
/** Total length of the manifest (including the signature). */
#define FWU_MANIFEST_SIZE                   (FWU_MANIFEST_OFFSET_SIGNATURE + FWU_MANIFEST_SIGNATURE_SIZE)

/* ------------------------------------------------------------------ data type */

/** Parsed manifest. Arrays hold one byte value (0..255) per element. */
typedef struct {
    fwu_byte_t format_version;                                  /**< Format version. */
    fwu_byte_t key_id;                                          /**< Identifier of the signing key. */
    uint32_t   product_id;                                      /**< Product identifier. */
    uint32_t   image_version;                                   /**< Image version. */
    uint16_t   key_version;                                     /**< Key version. */
    uint32_t   image_size;                                      /**< Image size (number of fwu_byte_t). */
    fwu_byte_t sha256[FWU_MANIFEST_SHA256_SIZE];                /**< SHA-256 digest of the image. */
    fwu_byte_t commit_hash[FWU_MANIFEST_COMMIT_HASH_SIZE];      /**< Source code version. */
    fwu_byte_t signature[FWU_MANIFEST_SIGNATURE_SIZE];          /**< Signature (only copied here, not verified). */
} fwu_manifest_t;

/* ------------------------------------------------------------------ API */

/**
 * @brief  Reads the manifest at the start of `buffer` and validates its STRUCTURE. Does not verify the
 *         signature, product, version or hash (G7b-G7d).
 *
 * The checks are done in this ORDER (the first mismatch determines the result):
 *   1. `buffer` or `manifest` is NULL                    -> FWU_ERR_INVALID_ARG
 *   2. `buffer_length` < FWU_MANIFEST_SIZE               -> FWU_ERR_NOT_ENOUGH_DATA
 *   3. magic != FWU_MANIFEST_MAGIC                       -> FWU_ERR_BAD_MANIFEST
 *   4. format_version != FWU_MANIFEST_FORMAT_VERSION     -> FWU_ERR_BAD_MANIFEST
 *   5. reserved field (2 elements) is not zero           -> FWU_ERR_BAD_MANIFEST
 *   6. image_size == 0                                   -> FWU_ERR_BAD_MANIFEST
 *
 * @param  buffer         Manifest bytes (at least FWU_MANIFEST_SIZE elements).
 * @param  buffer_length  Number of elements in `buffer`.
 * @param  manifest       Result. Written ONLY on FWU_OK, unchanged on error.
 * @return FWU_OK or one of the error codes above.
 */
fwu_return_e fwu_manifest_parse(const fwu_byte_t *buffer, uint32_t buffer_length, fwu_manifest_t *manifest);

#ifdef __cplusplus
}
#endif

#endif /* FWU_MANIFEST_H_ */
