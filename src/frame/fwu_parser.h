/**
 * @file    fwu_parser.h
 * @brief   Streaming frame parser: extracts frames from bytes arriving piecemeal in a ring buffer.
 *          Discards garbage bytes, waits for a partial frame, recovers from corrupt data.
 *
 * Operating mode (design A): the parser pulls the bytes from the ring buffer into ITS OWN BUFFER (at most
 * one frame, FWU_FRAME_MAX_SIZE) and calls fwu_frame_decode there. Thus
 *   - no insertion into the ring buffer is needed (the SPSC safety between the interrupt and the main loop is not broken),
 *   - the frame always sits in a CONTIGUOUS buffer (decode expects this).
 *
 * The caller regularly calls fwu_parser_poll in the main loop.
 *
 * RESYNCHRONISATION: if decode says BAD_FRAME / BAD_LENGTH / CRC_MISMATCH the parser discards the bytes from the
 * start of the buffer up to the next SOF byte (at least 1 byte) and tries again.
 * Because a SOF may be a "false alarm" (a 0xA5 inside a payload or garbage) and the real frame may start
 * right after it, the whole buffer is NOT discarded.
 *
 * TIMEOUT: the parser is not tied to a hardware clock; on every poll the caller supplies "the current time in ms"
 * (32 bit, wrap-safe). If the bytes at the head of the buffer are waiting for a frame that has not been completed for longer than
 * FWU_PARSER_FRAME_TIMEOUT_MS they are discarded.
 * The time is counted from the moment the head first arrived: an attacker cannot keep the parser busy
 * forever by sending bytes drop by drop.
 *
 * RESOURCE USE: no dynamic memory; the parser structure is the caller's (static) variable. The number of bytes that can be discarded in one poll
 * call is limited by FWU_PARSER_MAX_DROP_PER_POLL (a
 * call does not last forever); when the limit is reached poll returns NOT_ENOUGH_DATA and is called again.
 */

#ifndef FWU_PARSER_H_
#define FWU_PARSER_H_

#include <stddef.h>
#include <stdint.h>
#include "fwu_error.h"
#include "fwu_types.h"
#include "fwu_frame.h"
#include "fwu_decode.h"
#include "fwu_ringbuf.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Time allowed (ms) for a frame to be completed, counted from the moment its head first arrived. */
#define FWU_PARSER_FRAME_TIMEOUT_MS      500u

/** Maximum number of bytes discarded in one poll call (bounds the duration of the call). */
#define FWU_PARSER_MAX_DROP_PER_POLL     1024u

/** Diagnostic counters (read only; 32 bit, wrap around on overflow). */
typedef struct {
    uint32_t frames_ok;        /**< Number of valid frames delivered. */
    uint32_t bytes_dropped;    /**< Number of bytes discarded (garbage, corrupt frame, timeout). */
    uint32_t bad_frame;        /**< Number of times decode said BAD_FRAME. */
    uint32_t bad_length;       /**< Number of times decode said BAD_LENGTH. */
    uint32_t crc_mismatch;     /**< Number of times decode said CRC_MISMATCH. */
    uint32_t timeouts;         /**< Number of partial frames discarded by timeout. */
} fwu_parser_stats_t;

/** Parser state. Do not touch the fields directly (except stats, which may be read). */
typedef struct {
    fwu_ringbuf_t     *source;                         /**< Ring buffer the bytes come from. */
    fwu_byte_t         buffer[FWU_FRAME_MAX_SIZE];     /**< Frame buffer (contiguous). */
    size_t             count;                          /**< Number of bytes held in the buffer. */
    size_t             delivered;                      /**< Size of the frame delivered in the last poll (the next poll removes it). */
    uint32_t           head_time_ms;                   /**< "Start" time of the byte at the head of the buffer. */
    fwu_parser_stats_t stats;                          /**< Diagnostic counters. */
} fwu_parser_t;

/**
 * @brief  Initialises the parser and binds it to a ring buffer.
 * @param  parser  Parser to initialise (must not be NULL).
 * @param  source  Ring buffer the bytes come from (must not be NULL, must remain valid as long as the parser is used).
 * @return FWU_OK; FWU_ERR_INVALID_ARG for a NULL argument.
 */
fwu_return_e fwu_parser_init(fwu_parser_t *parser, fwu_ringbuf_t *source);

/**
 * @brief  Processes the new bytes in the ring buffer and returns the next valid frame (if any).
 *         First the frame delivered in the previous poll is removed, i.e. `view.payload` is valid ONLY until the
 *         next poll call. The caller can obtain all frames by calling poll repeatedly until it returns OK for a frame or until
 *         NOT_ENOUGH_DATA is returned.
 * @param  parser  Parser (must not be NULL).
 * @param  now_ms  Current time (ms), a counter that increases with every call (32-bit wrap is supported).
 * @param  view    Output (must not be NULL); written only if FWU_OK is returned.
 * @return FWU_OK                  frame ready; view filled.
 *         FWU_ERR_NOT_ENOUGH_DATA no frame to deliver right now (normal condition, not an error).
 *         FWU_ERR_INVALID_ARG     NULL argument.
 *         (if the ring buffer pull fails, that error is returned.)
 */
fwu_return_e fwu_parser_poll(fwu_parser_t *parser, uint32_t now_ms, fwu_frame_view_t *view);

#ifdef __cplusplus
}
#endif

#endif /* FWU_PARSER_H_ */
