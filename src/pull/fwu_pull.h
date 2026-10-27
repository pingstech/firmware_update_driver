/**
 * @file    fwu_pull.h
 * @brief   PULLING THE IMAGE FROM A SERVER: the host (UI) tells the device "pull the image from this server YOURSELF" (PULL_START); the device
 *          downloads the image piece by piece with HTTP range requests (GET + Range) and writes it to the staging area of fwu_session. It is
 *          independent of the transport (FC41D/TCP) and of the MCU: sending and receiving bytes are hooks supplied by the caller.
 *
 * WHY THIS DESIGN
 *   - There is NO new flash/verification code: every pulled chunk enters the session through fwu_session_push_chunk. The same rules as
 *     TRANSFER_DATA apply (offset order, "only the last chunk may be short", page erase, write + read back); the signature/SHA-256
 *     verification, the commit and the bootloader copy are the same. Pulling only changes the answer to "where do the bytes come from".
 *   - The manifest comes from the host (REQUEST_DOWNLOAD), the image from the server: there is no need to trust the server; if the server gives a wrong/corrupt
 *     file, the SHA-256 and signature verification rejects it. In addition, the total size reported by the server is compared with the size in the manifest
 *     (FWU_ERR_PULL_SIZE_MISMATCH): a wrong file is caught before a single byte is written.
 *   - The device requests EACH RANGE (range_bytes) with a separate GET and writes it chunk by chunk (from the RAM buffer) only after the response has arrived COMPLETELY and its length has been verified: if a byte is lost in the transport layer, the shifted data never enters flash. If the connection drops or the response is corrupt,
 *     the request is always repeated from the last chunk WRITTEN to flash (the session's next_offset); the partial chunk is discarded. This is why the ranges
 *     are a multiple of the chunk size (alignment).
 *
 * FLOW (host)
 *   HELLO -> REQUEST_DOWNLOAD(manifest) -> DOWNLOAD_ACCEPTED -> PULL_START(ip, port, target) -> PULL_STARTED
 *   -> PULL_QUERY ... PULL_STATUS (state, next_offset, error) ... when DONE -> TRANSFER_EXIT -> VERIFY_RESULT -> ACTIVATE
 *   PULL_START can be repeated with the same target (the response was lost: idempotent). After FAILED/DONE, a new PULL_START restarts
 *   the pull from the offset where the session left off.
 *
 * INTEGRATION (device glue code, main loop; see examples/c2000_launchxl_f280049c/application/port/app_pull.c)
 *   1. Each incoming frame: if fwu_pull_handles(type), call fwu_pull_on_frame, otherwise fwu_session_on_frame.
 *   2. Each group of bytes arriving from the TCP socket: fwu_pull_feed (FAST: it only copies into the ring buffer, does not touch flash).
 *   3. When the socket connection is established/dropped: fwu_pull_set_link.
 *   4. fwu_pull_poll: the HEAVY work happens here (HTTP parsing, flash write/erase, timeout, re-request). Flash operations
 *      take milliseconds; if the module sends bytes during that time, the UART FIFO (16 bytes) overflows and bytes are LOST. Therefore the caller calls
 *      poll ONLY while no data is flowing from the module, and before starting each new read it checks with fwu_pull_input_free(...) that the
 *      ring buffer has room (flow control is the caller's job; see app_pull.c).
 *
 * ERRORS (PULL_STATUS.error, fwu_error.h 8x and session codes)
 *   FWU_ERR_PULL_HTTP_STATUS    the server did not return 206 (404, 401, 5xx, or 200 = Range was ignored). PERMANENT: not retried.
 *   FWU_ERR_PULL_SIZE_MISMATCH  the size of the file on the server is not the same as in the manifest. PERMANENT.
 *   FWU_ERR_PULL_PROTOCOL       response corrupt/unexpected range: the same range is retried max_retries times, then given up.
 *   FWU_ERR_PULL_TIMEOUT        no bytes arrived for response_timeout_ms: likewise max_retries times, then given up.
 *   session/flash codes         the code returned by fwu_session_push_chunk (e.g. FLASH_WRITE): PERMANENT; the session may have dropped to IDLE.
 *   As long as the session stays open (except for a flash error), the host can resume from where it left off with the same manifest and PULL_START.
 *
 * TIME: given via now_ms (32-bit wraparound is supported). There is no dynamic memory; the buffers are supplied by the caller (the input ring is inside the
 * structure, its size is FWU_PULL_INPUT_SIZE).
 */

#ifndef FWU_PULL_H_
#define FWU_PULL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fwu_error.h"
#include "fwu_types.h"
#include "fwu_frame.h"
#include "fwu_decode.h"
#include "fwu_proto.h"
#include "fwu_ringbuf.h"
#include "fwu_session.h"
#include "fwu_transport.h"
#include "fwu_http.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Input ring size (elements). fwu_pull_feed puts groups of bytes here, fwu_pull_poll processes them. The caller's flow control
 * (checking for room before starting a read) works according to this size: it is recommended to be twice a single module read (e.g. 512 bytes on the FC41D).
 */
#ifndef FWU_PULL_INPUT_SIZE
#define FWU_PULL_INPUT_SIZE         1024u
#endif

/** Number of bytes poll pulls from the ring at one time (temporary buffer on the stack). */
#define FWU_PULL_SCRATCH_SIZE       64u
/** Minimum size (elements) of the fwu_pull_on_frame output buffer. */
#define FWU_PULL_MIN_OUT            (FWU_FRAME_OVERHEAD + FWU_PROTO_PULL_STATUS_SIZE)

/** Recommended defaults (can be used by the code that fills in the config). */
#define FWU_PULL_DEFAULT_RANGE_BYTES        8192u       /**< Bytes requested by one GET (must be a multiple of the chunk size). */
#define FWU_PULL_DEFAULT_TIMEOUT_MS         8000u       /**< Wait this long if no bytes arrive at all. */
#define FWU_PULL_DEFAULT_MAX_RETRIES        5u          /**< Number of retries for one range. */

FWU_RINGBUF_DEFINE(fwu_pull_rb, FWU_PULL_INPUT_SIZE);

typedef struct {
    fwu_session_t        *session;              /**< The initialized session that has reached the DOWNLOADING state (staging is written here). */
    fwu_transport_send_fn send;                 /**< Sends bytes to the TCP socket (contract: fwu_transport.h; BUFFER_FULL = try later). */
    void                 *send_context;
    fwu_byte_t           *chunk_buffer;         /**< RANGE buffer: the body of a response is held here until it has arrived completely (at least range_bytes elements). */
    uint32_t              chunk_buffer_length;  /**< Number of elements in chunk_buffer (>= range_bytes). */
    uint32_t              range_bytes;          /**< Bytes requested by each GET: >= chunk_size and a MULTIPLE of chunk_size. */
    uint32_t              response_timeout_ms;  /**< If no bytes arrive for this long, the request times out (>= 1). */
    uint32_t              max_retries;          /**< Number of retries for one range (0 = never retries). */
} fwu_pull_config_t;

/** Diagnostic counters (CCS watch window / tests). */
typedef struct {
    uint32_t requests;          /**< Number of GETs sent (including retries). */
    uint32_t responses;         /**< Number of successfully completed responses. */
    uint32_t retries;           /**< Number of requests retried after a timeout/corrupt response. */
    uint32_t chunks_written;    /**< Number of chunks written to staging. */
    uint32_t bytes_received;    /**< Total bytes received from the socket (including headers). */
    uint32_t rx_overflows;      /**< Byte groups that arrived while the input ring was full and were DROPPED (flow-control violation: the response is re-requested). */
    uint32_t link_drops;        /**< Number of connection drops. */
} fwu_pull_stats_t;

/** State of the pull controller. Do not touch the fields directly (use get_status / get_stats). */
typedef struct {
    fwu_pull_config_t config;
    bool              initialized;
    fwu_byte_t        state;                    /**< FWU_PROTO_PULL_*. */
    fwu_return_e      error;                    /**< The reason if FAILED. */
    uint16_t          http_status;              /**< The last HTTP status code reported by the server. */
    fwu_byte_t        ip[4];
    uint16_t          port;
    fwu_byte_t        target[FWU_PROTO_PULL_TARGET_MAX];
    size_t            target_length;
    bool              link_up;
    bool              request_outstanding;      /**< A GET was sent, its response is not complete. */
    bool              send_pending;             /**< A GET is waiting to be sent (first, next range or retry). */
    bool              wait_reconnect;           /**< "Connection: close" was seen: the server will close, wait for the new connection. */
    bool              input_overflowed;         /**< The input ring overflowed: the current response is considered corrupt. */
    uint32_t          range_first;              /**< First byte of the pending/ongoing GET. */
    uint32_t          range_last;               /**< Its last byte (inclusive). */
    uint32_t          retry_count;              /**< Retries made for this range. */
    uint32_t          not_before_ms;            /**< No request is sent before this time (backoff). */
    uint32_t          last_rx_ms;               /**< Time when the last byte arrived or the request was sent. */
    uint32_t          wait_start_ms;            /**< Time when wait_reconnect started. */
    bool              headers_checked;          /**< The headers of the ongoing response have been checked. */
    bool              stale_response;           /**< The ongoing response belongs to another range (stale/duplicate): its body is discarded. */
    uint32_t          chunk_fill;               /**< Number of bytes in chunk_buffer. */
    uint32_t          chunk_offset;             /**< Offset of chunk_buffer[0] within the image. */
    fwu_http_parser_t parser;
    fwu_pull_stats_t  stats;
    fwu_pull_rb_t     input;                    /**< Raw bytes arriving from the socket (feed writes, poll reads). */
} fwu_pull_t;

/**
 * @brief  Prepares the controller (IDLE).
 * @return FWU_OK; FWU_ERR_INVALID_ARG for a NULL field, send NULL, chunk_buffer NULL, response_timeout_ms 0, range_bytes zero or not a
 *         multiple of the session's chunk_size, or chunk_buffer_length < range_bytes; FWU_ERR_BAD_STATE if the session has not been
 *         initialized (chunk_size 0).
 */
fwu_return_e fwu_pull_init(fwu_pull_t *pull, const fwu_pull_config_t *config);

/** Does this frame type belong to fwu_pull (PULL_START, PULL_QUERY)? If not, the frame is given to fwu_session_on_frame. */
bool fwu_pull_handles(fwu_byte_t type);

/**
 * @brief  Handles a PULL_START and PULL_QUERY request; writes the response frame to `out` (protocol errors are ERROR responses: the return is
 *         FWU_OK and out_length > 0, just like fwu_session_on_frame).
 *         PULL_START: BAD_STATE if the session is not DOWNLOADING; BAD_LENGTH / INVALID_ARG if the payload is corrupt; if a pull is already RUNNING, the same target
 *         again = re-confirmation (idempotent), a different target gives BAD_STATE; if IDLE/FAILED/DONE (incomplete), it (re)starts.
 * @return FWU_OK; FWU_ERR_INVALID_ARG for NULL or not initialized; FWU_ERR_BUFFER_TOO_SMALL for out_capacity < FWU_PULL_MIN_OUT
 *         (state does not change). FWU_ERR_UNKNOWN_MESSAGE if the frame type does not belong to fwu_pull (no ERROR response is written).
 */
fwu_return_e fwu_pull_on_frame(fwu_pull_t *pull, const fwu_frame_view_t *request, uint32_t now_ms,
                               fwu_byte_t *out, size_t out_capacity, size_t *out_length);

/** State of the socket connection. When it drops, the ongoing response is discarded (the partial chunk is deleted); when it connects, requests can be sent. */
void fwu_pull_set_link(fwu_pull_t *pull, bool up, uint32_t now_ms);

/**
 * @brief  Puts bytes arriving from the socket into the input ring (FAST: only copies). If no pull is running, the bytes are discarded.
 * @return FWU_OK; FWU_ERR_BUFFER_FULL if there is no room in the ring (the byte group is DROPPED, visible in the counter, the ongoing response is re-requested);
 *         FWU_ERR_INVALID_ARG for NULL.
 */
fwu_return_e fwu_pull_feed(fwu_pull_t *pull, const fwu_byte_t *data, size_t length, uint32_t now_ms);

/**
 * @brief  Heavy work: parses the bytes in the ring, writes chunks to flash (blocking), handles timeout and retry,
 *         sends the next request. May take long (flash erase): the caller must call it while no data is flowing from the module (see "INTEGRATION" above).
 */
void fwu_pull_poll(fwu_pull_t *pull, uint32_t now_ms);

/** Cancels the pull (puts it in IDLE). E.g. when the session is reset by ABORT. */
void fwu_pull_abort(fwu_pull_t *pull);

/** State (a QUERY): state, offset written to flash (from the session), error, last HTTP code. For NULL the fields are zero/IDLE. */
void fwu_pull_get_status(const fwu_pull_t *pull, fwu_proto_pull_status_t *status);

/** Diagnostic counters (a QUERY). */
void fwu_pull_get_stats(const fwu_pull_t *pull, fwu_pull_stats_t *stats);

/** Is a pull running (sending a request/waiting for a response/downloading)? (a QUERY) */
bool fwu_pull_active(const fwu_pull_t *pull);

/** Free space (elements) and pending bytes in the input ring (a QUERY). For flow control. */
uint16_t fwu_pull_input_free(const fwu_pull_t *pull);
uint16_t fwu_pull_input_used(const fwu_pull_t *pull);

#ifdef __cplusplus
}
#endif

#endif /* FWU_PULL_H_ */
