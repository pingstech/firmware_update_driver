/**
 * @file    fwu_link.h
 * @brief   DEVICE-SIDE LINK LAYER: runs the ring buffer -> parser -> session -> transport loop in one place. MCU
 *          integration thus becomes as simple as "the ISR pushes bytes into the ring buffer, call fwu_link_poll in the main loop".
 *
 * WHAT IT DOES (fwu_link_poll, non-blocking)
 *   1. Calls the transport's poll, if it has one.
 *   2. If a response is waiting to be sent, tries to send it. If the transport is busy (BUFFER_FULL) the response is HELD
 *      and retried with the SAME bytes on the next poll; while a response is pending NO new request is processed (the
 *      stop-and-wait order is not broken).
 *   3. Takes at most FWU_LINK_MAX_FRAMES_PER_POLL frames from the parser; gives each frame to the session and sends the response.
 *   4. If no response is pending, calls fwu_session_poll (verification result, timeouts) and sends what comes out.
 *
 * TX ERROR: if the transport returns an error other than BUFFER_FULL, the response is DROPPED (and counted). A lost
 * response is compensated by the host timeout and retransmission; the session repeats the matching response to repeated
 * requests (fwu_session.h).
 *
 * REBOOT: fwu_link_reboot_ready becomes true if the session requested a reboot AND the last response was DELIVERED to the
 * transport. Waiting for the transport hardware TX to finish (UART drain etc.) is the integration's job: "delivered" =
 * the transport accepted it, not that it has gone out on the wire.
 *
 * EXTENSION (optional): with fwu_link_set_extension, a set of frame types can be routed to another module instead of the
 * session (e.g. fwu_pull: PULL_START / PULL_QUERY). Without a hook (the default) the behavior is unchanged. The response goes
 * through the same path (pending response, BUFFER_FULL retry).
 *
 * RX: this module does NOT read bytes itself. The driver (ISR or main loop) pushes bytes into the ring buffer the parser is
 * attached to (fwu_ringbuf_push); the single-producer / single-consumer rule (fwu_ringbuf.h) applies.
 */

#ifndef FWU_LINK_H_
#define FWU_LINK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fwu_error.h"
#include "fwu_types.h"
#include "fwu_frame.h"
#include "fwu_parser.h"
#include "fwu_session.h"
#include "fwu_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of frames processed in one poll call (bounds the duration of the call). */
#define FWU_LINK_MAX_FRAMES_PER_POLL    4u

/** Diagnostic counters (read-only; 32 bit, wrap around on overflow). */
typedef struct {
    uint32_t frames_in;     /**< Frames taken from the parser and given to the session. */
    uint32_t frames_out;    /**< Responses accepted by the transport. */
    uint32_t tx_busy;       /**< Attempts for which the transport said busy (BUFFER_FULL). */
    uint32_t tx_errors;     /**< Responses dropped because of an error. */
} fwu_link_stats_t;

/** Extension: should this TYPE go to the extension instead of the session? (a pure query) */
typedef bool (*fwu_link_ext_handles_fn)(fwu_byte_t type);

/** Extension frame handler: SAME contract as fwu_session_on_frame (writes the response to out; a protocol error is an ERROR response). */
typedef fwu_return_e (*fwu_link_ext_on_frame_fn)(void *context, const fwu_frame_view_t *view, uint32_t now_ms,
                                                 fwu_byte_t *out, size_t out_capacity, size_t *out_length);

/** Configuration. The caller fills it; init stores a COPY (the objects pointed to must stay valid for the lifetime of the link). */
typedef struct {
    fwu_session_t         *session;     /**< Initialized session. */
    fwu_parser_t          *parser;      /**< Initialized parser (attached to the ring buffer). */
    const fwu_transport_t *transport;   /**< Transport (send is mandatory). */
} fwu_link_config_t;

/** Link state. Do not touch the fields directly (except stats, which is readable). */
typedef struct {
    fwu_link_config_t config;
    fwu_byte_t        pending[FWU_FRAME_MAX_SIZE];  /**< Response frame waiting to be sent. */
    size_t            pending_length;               /**< 0 = nothing pending. */
    fwu_link_stats_t  stats;
    bool              initialized;
    fwu_link_ext_handles_fn  ext_handles;           /**< NULL = no extension (init leaves it so). */
    fwu_link_ext_on_frame_fn ext_on_frame;
    void                    *ext_context;
} fwu_link_t;

/**
 * @brief  Initializes the link.
 * @return FWU_OK; FWU_ERR_INVALID_ARG for a NULL field or transport->send NULL.
 */
fwu_return_e fwu_link_init(fwu_link_t *link, const fwu_link_config_t *config);

/**
 * @brief  Routes a set of frame types to the extension instead of the session (call AFTER init). If BOTH handles and on_frame are NULL
 *         the extension is removed.
 * @return FWU_OK; FWU_ERR_INVALID_ARG for NULL / an uninitialized link or if only one of them is NULL.
 */
fwu_return_e fwu_link_set_extension(fwu_link_t *link, fwu_link_ext_handles_fn handles, fwu_link_ext_on_frame_fn on_frame, void *context);

/**
 * @brief  Does everything that can be done (non-blocking). Called often from the main loop.
 * @param  now_ms  Current time (ms; 32-bit wrap-around is supported).
 * @return FWU_OK; FWU_ERR_INVALID_ARG for NULL or uninitialized; USAGE errors coming from the parser or the session are passed
 *         through unchanged (protocol errors are NOT errors, they are ERROR responses and are sent).
 */
fwu_return_e fwu_link_poll(fwu_link_t *link, uint32_t now_ms);

/** Whether the session requested a reboot and the last response was delivered to the transport (a QUERY). false if NULL. */
bool fwu_link_reboot_ready(const fwu_link_t *link);

/** Whether a response is waiting to be sent (a QUERY). false if NULL. */
bool fwu_link_tx_pending(const fwu_link_t *link);

/** Counters (NULL if NULL). */
const fwu_link_stats_t *fwu_link_get_stats(const fwu_link_t *link);

#ifdef __cplusplus
}
#endif

#endif /* FWU_LINK_H_ */
