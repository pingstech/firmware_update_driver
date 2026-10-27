/**
 * @file    fwu_frame.h
 * @brief   Frame format: field sizes, offsets, constant values and limits.
 *
 * A frame is used to separate messages from the byte stream arriving from a transport (UART, BLE, CAN, TCP) and to
 * detect corruption. This file is the SINGLE SOURCE OF TRUTH of the format: encode
 * and the parser obtain "at which byte is LEN?" and "how many bytes is the largest frame?" only from here;
 * there are no hard-coded numbers in the code.
 *
 * Layout (all in bytes; multi-byte fields are BIG ENDIAN, i.e. the high byte first):
 *
 *   | SOF | VER | TYPE | SEQ | LEN (2) | PAYLOAD (LEN bytes) | CRC (2) | EOF |
 *      1     1      1      1      2                               2        1
 *           +------------- CRC covers this range -------------+
 *
 *   SOF   Possible start of the frame (for synchronisation).
 *   VER   Frame format version.
 *   TYPE  Message type (its meaning is not the frame layer's business, the upper protocol interprets it).
 *   SEQ   Sequence number (loss/duplicate detection).
 *   LEN   PAYLOAD length (bytes).
 *   CRC   CRC-16/MODBUS (fwu_crc16), initial value 0xFFFF, from VER to the end of PAYLOAD.
 *   EOF   Additional consistency check: the last byte expected immediately after the CRC.
 *
 * "Byte" here is a logical 8-bit value (fwu_byte_t; even though it is 16 bits on C28x only the low
 * 8 bits are meaningful). Sizes and offsets are counts of fwu_byte_t.
 *
 * MISRA C:2012 Dir 4.9 note: only object-like constant macros are present here (no function-like
 * macros); #define was chosen instead of enum/const so that they can be used as array sizes in C99.
 */

#ifndef FWU_FRAME_H_
#define FWU_FRAME_H_

#include <stddef.h>
#include "fwu_error.h"
#include "fwu_types.h"


#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ field sizes (bytes) */

#define FWU_FRAME_SOF_SIZE       1u   /* start byte */
#define FWU_FRAME_VER_SIZE       1u   /* format version */
#define FWU_FRAME_TYPE_SIZE      1u   /* message type */
#define FWU_FRAME_SEQ_SIZE       1u   /* sequence number (0..255, wraps around on overflow) */
#define FWU_FRAME_LEN_SIZE       2u   /* payload length, big endian (can express 0..65535) */
#define FWU_FRAME_CRC_SIZE       2u   /* CRC-16, big endian */
#define FWU_FRAME_EOF_SIZE       1u   /* end byte */

/* ------------------------------------------------------------------ offsets (from the start of the frame) */

/* Each offset is DERIVED from the previous one; if a field is added only the size and this section change. */
#define FWU_FRAME_OFFSET_SOF     0u
#define FWU_FRAME_OFFSET_VER     (FWU_FRAME_OFFSET_SOF + FWU_FRAME_SOF_SIZE)
#define FWU_FRAME_OFFSET_TYPE    (FWU_FRAME_OFFSET_VER + FWU_FRAME_VER_SIZE)
#define FWU_FRAME_OFFSET_SEQ     (FWU_FRAME_OFFSET_TYPE + FWU_FRAME_TYPE_SIZE)
#define FWU_FRAME_OFFSET_LEN     (FWU_FRAME_OFFSET_SEQ + FWU_FRAME_SEQ_SIZE)
#define FWU_FRAME_OFFSET_PAYLOAD (FWU_FRAME_OFFSET_LEN + FWU_FRAME_LEN_SIZE)

/* The CRC covers the byte range starting at VER (after SOF) up to the end of PAYLOAD:
 * the offset of the first byte is the constant below, the length is (HEADER_SIZE - SOF_SIZE) + LEN. */
#define FWU_FRAME_CRC_START_OFFSET  FWU_FRAME_OFFSET_VER

/* The offsets of CRC and EOF depend on the payload length (PAYLOAD offset + LEN), so they are not
 * given here as constants; encode/parser compute them. */

/* ------------------------------------------------------------------ constant values */

#define FWU_FRAME_SOF            0xA5u  /* start byte; DIFFERENT from EOF (bit inverse), so that the end of a shifted
                                           frame does not accidentally pass the EOF check */
#define FWU_FRAME_EOF            0x5Au  /* end byte */
#define FWU_FRAME_VERSION        1u     /* current frame format version */

/* ------------------------------------------------------------------ totals and limits (bytes) */

/** Header: SOF + VER + TYPE + SEQ + LEN. The part before PAYLOAD. */
#define FWU_FRAME_HEADER_SIZE    (FWU_FRAME_SOF_SIZE + FWU_FRAME_VER_SIZE + FWU_FRAME_TYPE_SIZE + \
                                  FWU_FRAME_SEQ_SIZE + FWU_FRAME_LEN_SIZE)

/** Trailer: CRC + EOF. The part after PAYLOAD. */
#define FWU_FRAME_TRAILER_SIZE   (FWU_FRAME_CRC_SIZE + FWU_FRAME_EOF_SIZE)

/** Header + trailer: fixed overhead independent of the payload. */
#define FWU_FRAME_OVERHEAD       (FWU_FRAME_HEADER_SIZE + FWU_FRAME_TRAILER_SIZE)

/**
 * The LARGEST payload (bytes) that can be carried in one frame. If LEN is larger than this the parser rejects the frame
 * without reading anything (overflow protection). Rationale for the choice:
 *   - a 496-byte image chunk (the chunk size of the earlier driver) + the upper protocol's own header
 *     (chunk number/address etc.) must fit,
 *   - a multiple of 8 (divides exactly into 8-byte pieces on CAN): 512 = 64 * 8,
 *   - far smaller than the largest value a 16-bit LEN can express (65535): the buffer size
 *     stays reasonable in terms of memory.
 */
#define FWU_FRAME_MAX_PAYLOAD    512u

/** Largest frame size (bytes): the required size of a frame buffer. */
#define FWU_FRAME_MAX_SIZE       (FWU_FRAME_OVERHEAD + FWU_FRAME_MAX_PAYLOAD)

/**
 * @brief  Produces a frame: writes SOF, VER, TYPE, SEQ, LEN, PAYLOAD, CRC, EOF to `out` in that order.
 *         Nothing is written to `out` until ALL of the validation has passed (the buffer is unchanged on error).
 * @param  type            Value to be written to the TYPE field (low 8 bits).
 * @param  seq             Sequence number to be written to the SEQ field (low 8 bits).
 * @param  payload         Data to carry (must not be NULL). Read only.
 * @param  payload_length  Number of payload bytes; at least 1, at most FWU_FRAME_MAX_PAYLOAD.
 *                         An empty payload (0) is NOT ACCEPTED (design decision: for messages that carry no data
 *                         a status/content field of at least 1 byte is used).
 * @param  out             Buffer the frame is written to (must not be NULL).
 * @param  out_capacity    Number of bytes the `out` buffer can hold. Must be at least
 *                         FWU_FRAME_OVERHEAD + payload_length.
 * @param  out_length      Output: total number of bytes written (= FWU_FRAME_OVERHEAD + payload_length).
 *                         Not modified on error (must not be NULL).
 * @return FWU_OK; FWU_ERR_INVALID_ARG if out, out_length or payload is NULL, payload_length == 0 or
 *         payload_length > FWU_FRAME_MAX_PAYLOAD;
 *         FWU_ERR_BUFFER_TOO_SMALL if out_capacity is not sufficient.
 */
fwu_return_e fwu_frame_encode(fwu_byte_t type, fwu_byte_t seq,
                              const fwu_byte_t *payload, size_t payload_length,
                              fwu_byte_t *out, size_t out_capacity, size_t *out_length);

#ifdef __cplusplus
}
#endif

#endif /* FWU_FRAME_H_ */
