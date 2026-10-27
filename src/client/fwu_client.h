/**
 * @file    fwu_client.h
 * @brief   HOST-SIDE UPDATE CLIENT: talks to fwu_session and uploads an image from start to finish. It is transport
 *          independent; timeouts, retransmission, resuming after a disconnect and restarting after a session loss are handled
 *          here. It is written in C so that fwu_app (Python) can wrap it as a DLL; the update logic is not rewritten in Python.
 *
 * USAGE (pull API, a mirror of fwu_session)
 *   fwu_client_init(&c, &config);  fwu_client_start(&c, now);
 *   loop:   if(fwu_client_poll(&c, now, out, sizeof(out), &n) == FWU_OK && n > 0) transport_send(out, n);   // frame to send
 *           for every received frame (from the parser):  fwu_client_on_frame(&c, &view, now);
 *           when fwu_client_is_finished(&c):  fwu_client_get_result(&c, &result)
 *
 * FLOW (each step is a SINGLE outstanding request; a response whose SEQ and type do not match is ignored)
 *   HELLO -> INFO (version, product, flags, chunk size)
 *   [if the flags say authorization is required] AUTH_REQ -> AUTH_CHALLENGE -> (the auth hook produces the response) AUTH_RESP -> AUTH_OK
 *   REQUEST_DOWNLOAD(manifest) -> DOWNLOAD_ACCEPTED (chunk size, next offset: 0 for a new download, where it left off when resuming)
 *   TRANSFER_DATA(offset, chunk) -> DATA_ACK(next offset)  ... up to the end of the image (the offset reported by the device prevails)
 *   TRANSFER_EXIT(image size) -> EXIT_PENDING, then VERIFY_RESULT(code)   [code 0: valid]
 *   ACTIVATE(image size) -> ACTIVATE_OK   => DONE (the device wrote the commit and will reboot)
 *
 * ERROR TOLERANCE (together with the idempotent repeats of G9)
 *   - Timeout (response_timeout_ms): the SAME frame is resent with the SAME SEQ, at most max_retries times; then
 *     FAILED(TIMEOUT). A lost request and a lost response are compensated the same way (the device recognizes the repeat).
 *   - On a timeout while waiting for VERIFY_RESULT, TRANSFER_EXIT is repeated (answered with EXIT_PENDING / VERIFY_RESULT(0));
 *     the total wait is bounded by verify_timeout_ms.
 *   - If DATA_ACK does not give the expected offset, the client switches to the offset reported by the device (resynchronization).
 *   - ERROR(BAD_BLOCK): REQUEST_DOWNLOAD is repeated, the device reports the offset where it left off, and the transfer continues from there.
 *   - ERROR(BAD_STATE): the device's session was lost (timeout, reboot): restart from the beginning (HELLO), at most
 *     max_restarts times; then FAILED(DEVICE_ERROR, BAD_STATE).
 *   - Other device errors (signature, product, version, size, flash, lock...) are FINAL: FAILED(DEVICE_ERROR, code); not retried.
 *   - If the VERIFY_RESULT code is not 0, FAILED(REJECTED, code): the image did not pass verification.
 *
 * PULL MODE (the DEVICE pulls the image from a server): if pull_target is given in the configuration, TRANSFER_DATA is not sent. After
 *   REQUEST_DOWNLOAD, PULL_START(ip, port, target) -> PULL_STARTED is sent, then every pull_poll_ms PULL_QUERY -> PULL_STATUS (state,
 *   offset written to flash, error); when DONE, TRANSFER_EXIT and ACTIVATE are sent as usual. Progress (acked_bytes) is the offset
 *   reported by the device.
 *   In this mode `image` may be NULL (the host does not have to own the image bytes; the size and SHA-256 come from the manifest).
 *   - If the device stopped pulling (FAILED): FAILED(DEVICE_ERROR, the device's error code) -- e.g. 81 the server did not return 206, 83 wrong file size.
 *   - If the device forgot its session/pull (PULL_STATUS state IDLE or BAD_STATE): restart from the beginning (max_restarts).
 *   - pull_timeout_ms (if not 0): if the total time since PULL_START exceeds it, FAILED(TIMEOUT).
 *
 * CANCEL: fwu_client_cancel sends ABORT on the next poll; it ends with ABORT_OK or on timeout with FAILED(CANCELLED).
 * After the commit is written (ACTIVATE) cancelling is meaningless: BAD_STATE is returned.
 *
 * LIMITS: single session, single request; no windowed streaming. Time is given via now_ms (32-bit wrap-around is supported).
 */

#ifndef FWU_CLIENT_H_
#define FWU_CLIENT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fwu_error.h"
#include "fwu_types.h"
#include "fwu_frame.h"
#include "fwu_decode.h"
#include "fwu_manifest.h"
#include "fwu_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Client states. */
typedef enum {
    FWU_CLIENT_IDLE = 0,        /**< Not started. */
    FWU_CLIENT_HELLO,           /**< Waiting for the HELLO response. */
    FWU_CLIENT_AUTH_REQ,        /**< Waiting for AUTH_CHALLENGE. */
    FWU_CLIENT_AUTH_RESP,       /**< Waiting for AUTH_OK. */
    FWU_CLIENT_REQUEST,         /**< Waiting for DOWNLOAD_ACCEPTED. */
    FWU_CLIENT_TRANSFER,        /**< Sending chunks, waiting for DATA_ACK. */
    FWU_CLIENT_EXIT,            /**< Waiting for the EXIT response. */
    FWU_CLIENT_VERIFY_WAIT,     /**< Waiting for VERIFY_RESULT (the device is verifying). */
    FWU_CLIENT_ACTIVATE,        /**< Waiting for ACTIVATE_OK. */
    FWU_CLIENT_ABORTING,        /**< Waiting for ABORT_OK (cancelled). */
    FWU_CLIENT_DONE,            /**< Finished successfully. */
    FWU_CLIENT_FAILED,          /**< Finished unsuccessfully (see result.failure). */
    FWU_CLIENT_PULL_START,      /**< (pull mode) Waiting for PULL_STARTED. */
    FWU_CLIENT_PULL             /**< (pull mode) The device is pulling the image; progress is read with a PULL_QUERY every pull_poll_ms. */
} fwu_client_state_e;

/** Failure reason. */
typedef enum {
    FWU_CLIENT_FAIL_NONE = 0,
    FWU_CLIENT_FAIL_TIMEOUT,            /**< Retransmissions exhausted or the verification wait was exceeded. */
    FWU_CLIENT_FAIL_DEVICE_ERROR,       /**< The device returned a final ERROR (result.device_error). */
    FWU_CLIENT_FAIL_REJECTED,           /**< The device did not accept the image in verification (result.verify_code). */
    FWU_CLIENT_FAIL_PROTOCOL,           /**< The device's response is invalid (version, product, format, offset). */
    FWU_CLIENT_FAIL_AUTH_UNAVAILABLE,   /**< The device requires authorization but the client has no hook / the hook failed. */
    FWU_CLIENT_FAIL_CANCELLED           /**< The user cancelled. */
} fwu_client_failure_e;

/** Authorization hook: produces the response from the challenge (e.g. a signature with the operator key). Anything other than FWU_OK = failed. */
typedef fwu_return_e (*fwu_client_auth_fn)(void *context, const fwu_byte_t *challenge, uint32_t challenge_length,
                                           fwu_byte_t *response, uint32_t capacity, uint32_t *response_length);

typedef struct {
    const fwu_byte_t  *manifest;            /**< Signed manifest (FWU_MANIFEST_SIZE = 138 bytes). */
    const fwu_byte_t  *image;               /**< Image bytes (image_size elements; the client must keep them valid for the duration of use). May be NULL in pull mode. */
    uint32_t           image_size;          /**< Image size (>= 1; must equal the size in the manifest). */
    /* ---- pull mode: if pull_target != NULL the device pulls the image from the server itself (the following apply and must be kept persistent) ---- */
    const fwu_byte_t  *pull_target;         /**< HTTP request target (path + query), conforming to the rules of fwu_proto_encode_pull_start. NULL = pull mode off. */
    size_t             pull_target_length;  /**< 1..FWU_PROTO_PULL_TARGET_MAX. */
    fwu_byte_t         pull_ip[4];          /**< IPv4 address of the server. */
    uint16_t           pull_port;           /**< TCP port of the server (1..65535). */
    uint32_t           pull_poll_ms;        /**< PULL_QUERY interval (>= 1). */
    uint32_t           pull_timeout_ms;     /**< Limit on the total pull time since PULL_START (0 = no limit). */
    fwu_client_auth_fn auth;                /**< Authorization hook (NULL = none). */
    void              *auth_context;
    uint32_t           expected_product;    /**< If not 0, the product id in INFO must equal this. */
    uint32_t           response_timeout_ms; /**< Time to wait for a response to each request (>= 1). */
    uint32_t           max_retries;         /**< Maximum number of retransmissions for one request. */
    uint32_t           verify_timeout_ms;   /**< Total time to wait for the verification result after EXIT (>= response_timeout_ms). */
    uint32_t           max_restarts;        /**< Maximum number of restarts from the beginning when the device session is lost. */
} fwu_client_config_t;

/** Result and diagnostics. */
typedef struct {
    fwu_client_state_e   state;
    fwu_client_failure_e failure;           /**< The reason if FAILED. */
    fwu_return_e         device_error;      /**< The device's error code if FAIL_DEVICE_ERROR; otherwise FWU_OK. */
    fwu_byte_t           verify_code;       /**< The VERIFY_RESULT code if FAIL_REJECTED. */
    uint32_t             acked_bytes;       /**< Image bytes acknowledged by the device (progress). */
    uint32_t             total_bytes;       /**< Image size. */
    uint32_t             frames_sent;       /**< Frames sent (including retransmissions). */
    uint32_t             retransmits;       /**< Frames resent because of a timeout. */
    uint32_t             restarts;          /**< Number of restarts from the beginning. */
    uint32_t             resyncs;           /**< Number of resynchronizations to the device's offset. */
    uint32_t             ignored;           /**< Responses ignored (SEQ/type did not match). */
    uint16_t             pull_http_status;  /**< (pull mode) Last HTTP status code reported by the device (0 = none). */
} fwu_client_result_t;

/** Client state. Do not touch the fields directly (use get_result). */
typedef struct {
    fwu_client_config_t  config;
    bool                 initialized;
    fwu_client_state_e   state;
    fwu_client_failure_e failure;
    fwu_return_e         device_error;
    fwu_byte_t           verify_code;
    uint32_t             chunk_size;            /**< Chunk size reported by the device. */
    uint32_t             flags;                 /**< INFO flags. */
    uint32_t             acked;                 /**< Offset acknowledged by the device. */
    uint32_t             last_offset;           /**< Offset of the last chunk sent. */
    uint32_t             last_length;           /**< Length of the last chunk sent. */
    bool                 awaiting;              /**< A request is awaiting a response. */
    bool                 send_pending;          /**< req_frame is waiting to be sent. */
    bool                 cancel_requested;
    fwu_byte_t           req_type;
    fwu_byte_t           req_seq;
    fwu_byte_t           next_seq;
    fwu_byte_t           req_frame[FWU_FRAME_MAX_SIZE];
    size_t               req_length;
    uint32_t             sent_ms;               /**< Time of the last send. */
    uint32_t             retries;               /**< Retransmissions made for this request. */
    uint32_t             verify_start_ms;       /**< Time of the first send of EXIT. */
    uint32_t             frames_sent;
    uint32_t             retransmits;
    uint32_t             restarts;
    uint32_t             resyncs;
    uint32_t             ignored;
    uint32_t             next_query_ms;         /**< (pull mode) Time of the next PULL_QUERY. */
    uint32_t             pull_start_ms;         /**< (pull mode) Time PULL_START was first sent (for the total time limit). */
    uint16_t             pull_http_status;
} fwu_client_t;

/**
 * @brief  Prepares the client.
 * @return FWU_OK; FWU_ERR_INVALID_ARG for NULL, manifest NULL, image NULL (while pull mode is off), image_size 0, response_timeout_ms 0, verify_timeout_ms <
 *         response_timeout_ms. In pull mode also FWU_ERR_INVALID_ARG for an invalid target/port (PULL_START cannot be encoded) or
 *         pull_poll_ms 0.
 */
fwu_return_e fwu_client_init(fwu_client_t *client, const fwu_client_config_t *config);

/** Starts the update (HELLO). Only while IDLE; otherwise FWU_ERR_BAD_STATE. */
fwu_return_e fwu_client_start(fwu_client_t *client, uint32_t now_ms);

/** Requests cancellation (the next poll sends ABORT). FWU_ERR_BAD_STATE while IDLE, after finishing, or during the ACTIVATE phase. */
fwu_return_e fwu_client_cancel(fwu_client_t *client);

/**
 * @brief  Gives the frame to send (first send or retransmission after a timeout); out_length is 0 if there is none.
 *         The timeout and the retransmission limit are handled here.
 * @return FWU_OK; FWU_ERR_INVALID_ARG for NULL; FWU_ERR_BUFFER_TOO_SMALL for out_capacity < FWU_FRAME_MAX_SIZE.
 */
fwu_return_e fwu_client_poll(fwu_client_t *client, uint32_t now_ms, fwu_byte_t *out, size_t out_capacity, size_t *out_length);

/**
 * @brief  Processes a frame received from the device. Unexpected/stale responses are silently ignored (and counted). Protocol errors
 *         are NOT a return code, they are the client's FAILED state.
 * @return FWU_OK; FWU_ERR_INVALID_ARG for NULL.
 */
fwu_return_e fwu_client_on_frame(fwu_client_t *client, const fwu_frame_view_t *response, uint32_t now_ms);

/** Whether it has finished (DONE or FAILED). (a QUERY) */
bool fwu_client_is_finished(const fwu_client_t *client);

/** State (a QUERY). IDLE if NULL. */
fwu_client_state_e fwu_client_get_state(const fwu_client_t *client);

/** Fills in the result and the diagnostic counters. @return FWU_OK; FWU_ERR_INVALID_ARG for NULL. */
fwu_return_e fwu_client_get_result(const fwu_client_t *client, fwu_client_result_t *result);

#ifdef __cplusplus
}
#endif

#endif /* FWU_CLIENT_H_ */
