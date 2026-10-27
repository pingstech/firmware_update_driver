/**
 * @file    fwu_error.h
 * @brief   Common status / error code type shared by all fwu modules.
 *
 * Rules:
 *   - Every function that can fail returns its result with this type.
 *     An error is NEVER swallowed silently.
 *   - There is exactly ONE success value: FWU_OK = 0. Every other value is an error.
 *     Check:  if (status != FWU_OK) { ... }     (do NOT use bool-style "if(status)")
 *   - The numeric values of the error codes are fixed: a number once assigned is never
 *     changed and never reused for another error (they may be carried in records/logs/on the wire).
 *   - Codes are split into groups of ten; errors in the same group are related:
 *       0     success
 *       1x    usage errors
 *       2x    stream / resource
 *       3x    integrity
 *       4x    storage
 *       5x    security (signature, cryptography)
 *       6x    session / protocol (update session, fwu_session)
 *       7x    bootloader contract (fwu_boot)
 *       8x    pull from network (image download over HTTP, fwu_http / fwu_pull)
 *     A new code is added at a free number in the relevant group. 9x and above are reserved
 *     for future use. The session protocol carries the error code in a SINGLE BYTE: codes must not exceed 255.
 *
 * "Transient" error: the same operation may succeed when retried later or again.
 * "Permanent" error: retrying is useless, the upper layer must make a different decision.
 */

#ifndef FWU_ERROR_H_
#define FWU_ERROR_H_

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /** Success. The only success value. */
    FWU_OK                      = 0,

    /* ---- 1x: usage errors ---- */

    /** An invalid parameter was passed to the function (e.g. a NULL pointer).
     *  Permanent: a bug in the calling code. */
    FWU_ERR_INVALID_ARG         = 10,

    /** The supplied buffer is too small to hold the result. Permanent. */
    FWU_ERR_BUFFER_TOO_SMALL    = 11,

    /** The operation is not valid in the current state (e.g. a new commit while an activation is pending,
     *  or a confirmation outside a trial boot). Permanent: a bug in the calling code or the configuration. */
    FWU_ERR_BAD_STATE           = 12,

    /* ---- 2x: stream / resource (20 is unused) ---- */

    /** Buffer is full, the new data does not fit. Transient: may be retried once the buffer drains. */
    FWU_ERR_BUFFER_FULL         = 21,

    /** Buffer is empty, there is no data to read. Transient: may be retried when data arrives. */
    FWU_ERR_BUFFER_EMPTY        = 22,

    /** The requested amount of data is not available (the buffer is not empty but not enough, e.g. 2 bytes present, 3 requested).
     *  Transient: may be retried when data arrives. It is NOT an integrity error (there is no corrupt data,
     *  not enough has arrived yet); the upper layer must not request a retransmission, it must wait. */
    FWU_ERR_NOT_ENOUGH_DATA     = 23,

    /* ---- 3x: integrity ---- */

    /** CRC mismatch. Transient: the chunk may be requested again. */
    FWU_ERR_CRC_MISMATCH        = 30,

    /** The frame length does not match the expected one. Transient: the chunk may be requested again. */
    FWU_ERR_BAD_LENGTH          = 31,

    /** Invalid frame format: SOF, VER or EOF does not match the expected value (byte slip, garbage or an
     *  unsupported version). Permanent: the same bytes give the same result when presented again; that frame
     *  is discarded and the parser searches for the next SOF. */
    FWU_ERR_BAD_FRAME           = 32,

    /** Invalid manifest structure: magic number, format version, reserved field or image size does not match the
     *  expected value (BEFORE the signature, structure check only). Permanent: the same bytes give the same result
     *  when presented again. Because it is in group 3x, the activation verification hook treats it as "image invalid" (REJECTED). */
    FWU_ERR_BAD_MANIFEST        = 33,

    /** The image size exceeds the allocated area: the size in the manifest is larger than the staging image capacity or
     *  larger than the area covered by the page count given at commit. Permanent. Activation treats it as "image invalid". */
    FWU_ERR_IMAGE_TOO_LARGE     = 34,

    /** The SHA-256 digest of the image does not match the digest in the manifest (the image is corrupt or the manifest
     *  belongs to another image). Permanent. Activation treats it as "image invalid". */
    FWU_ERR_HASH_MISMATCH       = 35,

    /* ---- 4x: storage ---- */

    /** Flash write failed. Careful: may be retried once; if it repeats it must be treated
     *  as permanent. */
    FWU_ERR_FLASH_WRITE         = 40,

    /** The requested offset/length runs outside the flash region. Permanent: a bug of the caller,
     *  the same request gives the same result when retried. */
    FWU_ERR_OUT_OF_RANGE        = 41,

    /** The write offset or length is not a multiple of the write granule (write_granule).
     *  Permanent: a bug of the caller. */
    FWU_ERR_WRITE_ALIGNMENT     = 42,

    /** The erase offset or length is not a multiple of the page size (page_length).
     *  Permanent: a bug of the caller. */
    FWU_ERR_ERASE_ALIGNMENT     = 43,

    /** The geometry of the flash object is inconsistent (zero size or the multiples do not match). This means the
     *  port filled in the object incorrectly. Permanent: a software/configuration bug, retrying is useless;
     *  the flash must not be used and no update must be started. */
    FWU_ERR_FLASH_GEOMETRY      = 44,

    /** The area to be written is not erased (blank); it must be erased first. Permanent: a bug of the caller.
     *  Flash can only be written into an erased cell (1 -> 0), otherwise the data is corrupted. The simulated flash and
     *  the STM32G0 port check BEFORE writing and return this code (so that no hardware error flag is raised). */
    FWU_ERR_FLASH_NOT_ERASED    = 45,

    /** Power was lost. Returned ONLY by the simulated flash (tests): on real hardware the operation never
     *  returns, the device restarts. The flash content may hold the partial result of the operation. */
    FWU_ERR_FLASH_POWER_LOSS    = 46,

    /** Flash erase failed (the hardware/HAL erase operation returned an error). Careful: may be retried once;
     *  if it repeats it must be treated as permanent (the page may be worn out). */
    FWU_ERR_FLASH_ERASE         = 47,

    /* ---- 5x: security ---- */

    /** The signature is INVALID: verification was performed and the signature does not match this message/key (forged,
     *  corrupt, wrong key). Permanent: the same input gives the same result. The activation verification hook treats
     *  it as "image invalid" (REJECTED). Note: it is DIFFERENT from FWU_ERR_CRYPTO_FAILURE (there the
     *  verification could not be performed). */
    FWU_ERR_BAD_SIGNATURE       = 50,

    /** The cryptographic operation COULD NOT be performed (library/hardware error, corrupt key format...). It does NOT mean
     *  the signature is invalid; the image is not rejected, the error is reported as an error. May be transient: may be retried. */
    FWU_ERR_CRYPTO_FAILURE      = 51,

    /** The manifest belongs to another product (the product id does not match this device's). Permanent. Activation treats it
     *  as "image invalid". */
    FWU_ERR_PRODUCT_MISMATCH    = 52,

    /** The image version is lower than the minimum accepted version (anti-rollback: a return to an old, vulnerable version).
     *  Permanent. Activation treats it as "image invalid". */
    FWU_ERR_VERSION_REJECTED    = 53,

    /** The key that would verify the signature is not accepted: the key id in the manifest is not present on the device or the key
     *  version is lower than the minimum accepted key version (revoked/old key). Permanent.
     *  Activation treats it as "image invalid". */
    FWU_ERR_KEY_REJECTED        = 54,

    /* ---- 6x: session / protocol (fwu_session) ---- */

    /** Unrecognized message type in the session protocol (unknown TYPE, or a RESPONSE type arriving at the device). The frame
     *  is valid (the CRC matched) but has no meaning. Permanent: the same message gives the same result. The session state does not change. */
    FWU_ERR_UNKNOWN_MESSAGE     = 60,

    /** During download the data chunk is not in the expected order: the offset does not match the next offset the device expects
     *  (lost/duplicate/out-of-order chunk), or the whole image has not been received yet when TRANSFER_EXIT arrives.
     *  The session stays open; the host can continue from the correct offset. */
    FWU_ERR_BAD_BLOCK           = 61,

    /** Authorization failed too many times in a row; the device does not accept a new authorization request for a while
     *  (against brute-force and flash-wear attacks). May be retried once the time has elapsed. */
    FWU_ERR_LOCKED_OUT          = 62,

    /* ---- 7x: bootloader contract (fwu_boot) ---- */

    /** The build contracts of the bootloader and the application (profile, flash layout, product id...) do not match, or the
     *  contract information provided by the bootloader (fwu_boot_info) cannot be read / is corrupt. If the two are not built
     *  with the SAME profile and layout the device may be bricked; this code exists to catch that at startup. Retrying is useless. */
    FWU_ERR_CONTRACT_MISMATCH   = 70,

    /* ---- 8x: pull from network (fwu_http, fwu_pull) ---- */

    /** The HTTP response from the server could not be parsed: malformed status line/header format, header too long, Content-Length
     *  or Content-Range missing or not a number, or inconsistent with the requested range. May be transient (a corrupted
     *  response); fwu_pull retries the same range a limited number of times, then gives up with this code. */
    FWU_ERR_PULL_PROTOCOL       = 80,

    /** The server returned a status other than the expected 206 (Partial Content): 404 (no such file), 401/403 (authorization),
     *  5xx (server error) or 200 (the server ignored Range and tried to send the whole file). Permanent: the same request
     *  gives the same response. The status code is kept in the detail field of fwu_pull. */
    FWU_ERR_PULL_HTTP_STATUS    = 81,

    /** The expected data from the server did not arrive within the set time (no connection, the server does not respond) and the
     *  retry budget is exhausted. The session stays open; if the host restarts with the same manifest it resumes from where it left off. */
    FWU_ERR_PULL_TIMEOUT        = 82,

    /** The total size of the file on the server (the number in Content-Range) differs from the image size in the manifest:
     *  the wrong file (e.g. another version) may be being served. Permanent: no byte is written to flash. */
    FWU_ERR_PULL_SIZE_MISMATCH  = 83
} fwu_return_e;

#ifdef __cplusplus
}
#endif

#endif /* FWU_ERROR_H_ */
