/**
 * @file    fwu_decode.h
 * @brief   The READ side of the frame: validates the frame at the start of a byte buffer.
 *
 * The frame format is in fwu_frame.h (fields, sizes, constants). This file only declares the read
 * function and the structure that holds the result. Searching for SOF among garbage bytes in the stream, accumulating partially received
 * data and the timeout are the job of the parser; fwu_frame_decode only answers the question
 * "is the HEAD of the buffer a valid frame?".
 */

#ifndef FWU_DECODE_H_
#define FWU_DECODE_H_

#include <stddef.h>
#include "fwu_error.h"
#include "fwu_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Contents of a valid frame. The payload is NOT COPIED: `payload` points into the buffer the caller supplied
 * and is valid as long as that buffer is not modified/reused.
 */
typedef struct {
    fwu_byte_t        type;             /**< TYPE field (0..255). */
    fwu_byte_t        seq;              /**< SEQ field (0..255). */
    const fwu_byte_t *payload;          /**< Address of the payload in the buffer (no copy). */
    size_t            payload_length;   /**< Number of payload bytes (LEN), 1..FWU_FRAME_MAX_PAYLOAD. */
    size_t            frame_length;     /**< Total number of bytes of the frame: this many bytes are consumed from the stream. */
} fwu_frame_view_t;

/**
 * @brief  Validates the frame at the start of `buffer` and writes its contents to `view`.
 *         Each field is checked as soon as the bytes in hand suffice; invalidity is reported
 *         as soon as it is noticed, the remaining bytes are not awaited (on hostile input the parser cannot be kept waiting forever).
 *         Check order: SOF, VER, the whole header, LEN limit, total length, EOF, CRC.
 *         Bytes after the end of the frame in the buffer (the next frame) are ignored.
 * @param  buffer         Bytes to read (must not be NULL). Not modified; only buffer_length
 *                        bytes are read.
 * @param  buffer_length  Number of bytes available in the buffer (may be 0).
 * @param  view           Output (must not be NULL). Written only if FWU_OK is returned, unchanged on error.
 * @return FWU_OK                  valid frame; view filled.
 *         FWU_ERR_NOT_ENOUGH_DATA the frame may be valid but has not fully arrived yet (including buffer_length == 0).
 *                                 Transient: retry when more data arrives.
 *         FWU_ERR_BAD_FRAME       first byte is not SOF, VER is not supported, or the last byte is not EOF.
 *         FWU_ERR_BAD_LENGTH      LEN == 0 or LEN > FWU_FRAME_MAX_PAYLOAD (returns without waiting).
 *         FWU_ERR_CRC_MISMATCH    the frame arrived completely but its contents are corrupt.
 *         FWU_ERR_INVALID_ARG     buffer or view is NULL.
 */
fwu_return_e fwu_frame_decode(const fwu_byte_t *buffer, size_t buffer_length, fwu_frame_view_t *view);

#ifdef __cplusplus
}
#endif

#endif /* FWU_DECODE_H_ */
