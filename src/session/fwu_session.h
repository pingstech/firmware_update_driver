/**
 * @file    fwu_session.h
 * @brief   UPDATE SESSION (device-side state machine): processes the frames arriving from the transport (fwu_proto.h messages)
 *          in order, downloads the image into staging, verifies it and takes it up to the commit (fwu_activate).
 *
 * INDEPENDENT OF TRANSPORT AND HARDWARE: the module knows nothing about UART/BLE/CAN/TCP, the MCU or the timer. The caller
 *   (1) passes every frame coming from the parser (fwu_parser_poll) to fwu_session_on_frame; the response frame is WRITTEN
 *       into the caller's buffer (sending it is the caller's job),
 *   (2) calls fwu_session_poll frequently in the main loop (long verification and timeouts run here),
 *   (3) when fwu_session_reboot_requested becomes true, restarts the MCU AFTER SENDING the last response
 *       (the module does not restart by itself; the bootloader applies the commit with fwu_activate_run).
 *   Time is passed as `now_ms` (32-bit wraparound is supported). There is no dynamic memory.
 *
 * STATES AND ALLOWED MESSAGES (cell: resulting state; "-" = BAD_STATE; on an error response the state does not change unless stated otherwise)
 *
 *   message \ state   IDLE           AUTH_PENDING  AUTHENTICATED   DOWNLOADING     VERIFYING   READY       COMMITTED
 *   HELLO             INFO           INFO          INFO            INFO            INFO        INFO        INFO
 *   KEEPALIVE         ok             ok            ok              ok              ok          ok          ok
 *   AUTH_REQ          AUTH_PENDING(1) AUTH_PENDING -               -               -           -           -
 *   AUTH_RESP         -              AUTHENTICATED(2) AUTHENTICATED(2b) -          -           -           -
 *   REQUEST_DOWNLOAD  DOWNLOADING(3) -             DOWNLOADING     DOWNLOADING(4)  -           -           -
 *   TRANSFER_DATA     -              -             -               DOWNLOADING(5)  -           -           -
 *   TRANSFER_EXIT     -              -             -               VERIFYING(6)    VERIFYING(6b) READY(6c) -
 *   ACTIVATE          -              -             -               -               -           COMMITTED(7) COMMITTED(7b)
 *   ABORT             IDLE           IDLE          IDLE            IDLE            IDLE        IDLE        -
 *   unknown/response  UNKNOWN_MESSAGE (in every state; the state does not change)
 *   (1) only if the authorization hook EXISTS; otherwise BAD_STATE in every state. If the hook exists, REQUEST_DOWNLOAD is
 *       rejected from IDLE (authorization first). (2) if it fails, returns to IDLE; the challenge is single-use. (2b) AUTH_RESP in AUTHENTICATED: AUTH_OK was lost, the host repeated the same request;
 *       the response is re-checked against the SAME challenge: if correct, AUTH_OK again (state/counter unchanged), if wrong BAD_STATE (not counted, does not lock out).
 *       A repeat of AUTH_REQ with the SAME SEQ (challenge lost or a delayed copy) returns the SAME challenge again (begin is not called);
 *       a DIFFERENT SEQ is a new attempt and generates a new challenge. The challenge is cleared when the download starts or the session drops to IDLE.
 *   (3) if there is NO hook, accepted from IDLE. Order: is there no pending activation (staging writable) -> the manifest is
 *       verified in RAM (structure, product, version, size, key, signature) -> ONLY after that is the manifest page erased and written.
 *       For an offer that does not pass, NOT A SINGLE BYTE of flash is touched.
 *   (4) a repeat with the SAME manifest = resume (after a disconnect): reports the offset where it left off without touching flash; a different manifest
 *       gives BAD_STATE (ABORT first).
 *   (5) the offset must be exactly the expected offset (BAD_BLOCK, state unchanged); an exact repeat of the previous chunk (lost acknowledgement) is
 *       re-acknowledged without writing. A flash error drops the session to IDLE.
 *   (6) the whole image must have been received (BAD_BLOCK, state unchanged). The response is EXIT_PENDING immediately; verification runs in poll
 *       and the result arrives as a SEPARATE VERIFY_RESULT message (READY if valid, otherwise IDLE).
 *   (7) fwu_activate_commit; if it fails, IDLE. On success fwu_session_reboot_requested becomes true.
 *   (6b) REPEATED EXIT (EXIT_PENDING was lost): EXIT_PENDING again, state unchanged, verification does NOT start again; the result
 *       message carries the SEQ of the LATEST EXIT. (6c) EXIT in READY: verification already passed, VERIFY_RESULT(0) is given
 *       immediately as the response (nothing is recomputed). (7b) ACTIVATE in COMMITTED: the commit is not written a second time, ACTIVATE_OK is given again.
 *       In all three, BAD_BLOCK if the image size claim is wrong. Purpose: when the host loses a response it can make progress by repeating the SAME request.
 *
 * TIMEOUT: in a STARTED session (AUTH_PENDING, AUTHENTICATED, DOWNLOADING, READY) if no frame arrives for `idle_timeout_ms`, poll
 *   drops the session to IDLE (download progress is forgotten; because no commit was written to staging, the running
 *   version is not affected). There is no timeout in VERIFYING (the computation takes time).
 *
 * AUTHORIZATION (hook): fwu_session_auth_t. The REAL implementation of challenge-response (random number, signature) is outside this
 *   module (task G9d); the module only manages the flow, the single-use challenge and the LOCKOUT: after `auth_max_failures`
 *   consecutive failed attempts, AUTH_REQ returns LOCKED_OUT for `auth_lockout_ms`.
 *   If `require_auth` is true the hook cannot be NULL (init returns INVALID_ARG; in profiles with verification the integration passes true).
 *   The purpose of authorization is NOT the authenticity of the image (the signature and hash guarantee that), it is who may update the device:
 *   sending a validly signed image over and over (the same version is accepted) would wear out the flash and keep the device busy.
 *
 * DOWNLOAD: chunk size `chunk_size` (a multiple of the write granule, divides the page size, <= 508). On the first chunk of a page that
 *   page is erased (erase on demand). Every chunk is written and READ BACK and compared; the acknowledgement comes after that
 *   ("written to flash"). The last chunk may be short (written padded to the granule with 0xFF). Verification checks the whole image
 *   with fwu_verify_image (the bootloader verification at activation is the AUTHORITATIVE one, and is done separately).
 *
 * ERROR CODES (ERROR response, code the same as in fwu_error.h): BAD_STATE (message invalid in this state; also a download request
 *   while an activation is pending), BAD_LENGTH (invalid payload length), UNKNOWN_MESSAGE, BAD_BLOCK (order/offset/
 *   incomplete download/size mismatch), LOCKED_OUT, manifest/signature codes (33, 34, 50-54, 51), flash codes
 *   (40, 45-47), the code returned by the hook.
 *
 * USAGE
 *   fwu_session_init(&s, &config);                          // once
 *   // for every frame:   fwu_session_on_frame(&s, &view, now, out, sizeof(out), &n);  if(n > 0) send(out, n);
 *   // in the main loop:  fwu_session_poll(&s, now, out, sizeof(out), &n);             if(n > 0) send(out, n);
 *   // if(fwu_session_reboot_requested(&s)) { /\* send first, then restart *\/ }
 *
 * LIMITS: single session, single client. There is no windowed streaming (without acknowledgements) and no resume across power loss (persistent); resume works only
 * within the same power-up, using the progress held in RAM. Verification is blocking (feeding the watchdog during a long
 * run is the integration's job, see README open issue). On C28x offsets/sizes are counts of fwu_byte_t (image packaging: open issue 10).
 */

#ifndef FWU_SESSION_H_
#define FWU_SESSION_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fwu_error.h"
#include "fwu_types.h"
#include "fwu_frame.h"
#include "fwu_decode.h"
#include "fwu_flash.h"
#include "fwu_manifest.h"
#include "fwu_verify.h"
#include "fwu_activate.h"
#include "fwu_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Internal buffer of the session: the manifest (138) rounded up to the granule must fit here (fits if granule <= 64). */
#define FWU_SESSION_MANIFEST_BUFFER     192u
/** Largest supported write granule (fwu_byte_t). */
#define FWU_SESSION_MAX_GRANULE         64u
/** Minimum size of the output buffer for on_frame/poll (the largest response: the challenge). */
#define FWU_SESSION_MIN_OUT             (FWU_FRAME_OVERHEAD + FWU_PROTO_AUTH_CHALLENGE_MAX)

/** Session states (the values are the SAME as the permanent numbers in fwu_proto.h). */
typedef enum {
    FWU_SESSION_IDLE          = (int)FWU_PROTO_STATE_IDLE,
    FWU_SESSION_AUTH_PENDING  = (int)FWU_PROTO_STATE_AUTH_PENDING,
    FWU_SESSION_AUTHENTICATED = (int)FWU_PROTO_STATE_AUTHENTICATED,
    FWU_SESSION_DOWNLOADING   = (int)FWU_PROTO_STATE_DOWNLOADING,
    FWU_SESSION_VERIFYING     = (int)FWU_PROTO_STATE_VERIFYING,
    FWU_SESSION_READY         = (int)FWU_PROTO_STATE_READY,
    FWU_SESSION_COMMITTED     = (int)FWU_PROTO_STATE_COMMITTED
} fwu_session_state_e;

/**
 * Authorization hook: generates a challenge and verifies the response. The implementation is outside the session (it needs a random
 * number and a signature). The `challenge` must be single-use (every begin a new value).
 */
typedef fwu_return_e (*fwu_session_auth_begin_fn)(void *context, fwu_byte_t *challenge, uint32_t capacity, uint32_t *length);
typedef fwu_return_e (*fwu_session_auth_check_fn)(void *context, const fwu_byte_t *challenge, uint32_t challenge_length,
                                                  const fwu_byte_t *response, uint32_t response_length);

typedef struct {
    fwu_session_auth_begin_fn begin;    /**< Generate a challenge: write 1..capacity elements, give the length; anything other than FWU_OK = error. */
    fwu_session_auth_check_fn check;    /**< Check the response: FWU_OK = authorized; other codes are passed through unchanged in the ERROR response. */
    void                     *context;  /**< Passed to both hooks. */
} fwu_session_auth_t;

/** Configuration. Filled in by the caller; init stores a COPY (the objects pointed to must stay valid for the lifetime of the session). */
typedef struct {
    const fwu_flash_t         *flash;                   /**< The flash that holds staging. */
    fwu_activate_t            *activate;                /**< Initialized activation (the commit is written through it). */
    const fwu_verify_config_t *verify;                  /**< Offer and image verification; manifest_offset, image_offset (page aligned), max_image_size (multiple of a page). */
    const fwu_session_auth_t  *auth;                    /**< Authorization hook; NULL = no authorization. */
    bool                       require_auth;            /**< true: auth cannot be NULL. */
    uint32_t                   running_image_version;   /**< Version of the running image (reported in INFO). */
    uint32_t                   chunk_size;              /**< Data chunk size: multiple of the granule, divides the page size, <= 508. */
    uint32_t                   idle_timeout_ms;         /**< Idle timeout of a started session (>= 1). */
    uint32_t                   auth_max_failures;       /**< Consecutive failed attempts before lockout (>= 1 if auth is present). */
    uint32_t                   auth_lockout_ms;         /**< Lockout duration (>= 1 if auth is present). */
    fwu_byte_t                *work;                    /**< Read-back buffer (>= chunk_size). */
    uint32_t                   work_length;             /**< Number of work elements. */
} fwu_session_config_t;

/** Session state. Do not touch the fields directly (use get_state / reboot_requested). */
typedef struct {
    fwu_session_config_t config;                               /**< Copy of the configuration. */
    bool                 initialized;
    fwu_session_state_e  state;
    uint32_t             page_length;                          /**< From the flash geometry. */
    uint32_t             granule;                              /**< From the flash geometry. */
    uint32_t             last_activity_ms;                     /**< Time of the last frame (for the timeout). */
    uint32_t             image_size;                           /**< Image size in the accepted manifest. */
    uint32_t             next_offset;                          /**< Number of image bytes durably written (= the expected next offset). */
    uint32_t             last_chunk_offset;                    /**< Offset of the last written chunk (duplicate detection). */
    uint32_t             last_chunk_length;                    /**< Length of the last written chunk (0 = none). */
    fwu_manifest_t       manifest;                             /**< The accepted manifest (parsed). */
    fwu_byte_t           manifest_raw[FWU_SESSION_MANIFEST_BUFFER]; /**< Bytes of the accepted manifest (for resume matching, as written to flash). */
    fwu_byte_t           pad[FWU_SESSION_MAX_GRANULE];         /**< The last short chunk padded up to the granule. */
    fwu_byte_t           challenge[FWU_PROTO_AUTH_CHALLENGE_MAX]; /**< The pending challenge. */
    uint32_t             challenge_length;
    fwu_byte_t           auth_seq;                             /**< SEQ of the AUTH_REQ that generated the challenge (same SEQ = repeat). */
    uint32_t             auth_failures;                       /**< Consecutive failed authorizations. */
    bool                 locked;                               /**< Whether the lockout is active. */
    uint32_t             lock_start_ms;                        /**< Time at which the lockout started. */
    fwu_byte_t           pending_seq;                          /**< SEQ of TRANSFER_EXIT (VERIFY_RESULT carries it). */
    bool                 reboot_requested;
} fwu_session_t;

/**
 * @brief  Initializes the session (IDLE).
 * @return FWU_OK; FWU_ERR_INVALID_ARG for a NULL field, auth NULL (or NULL hooks) when require_auth is set, invalid chunk_size (not a multiple of the
 *         granule, does not divide the page, 0 or > 508), work_length < chunk_size, misaligned/overlapping/out-of-flash manifest and image regions,
 *         zero timeout; FWU_ERR_FLASH_GEOMETRY for an invalid flash geometry or granule > 64.
 */
fwu_return_e fwu_session_init(fwu_session_t *session, const fwu_session_config_t *config);

/**
 * @brief  Processes a request and writes the RESPONSE frame into `out`. Protocol errors (wrong state, bad length...) are NOT an
 *         error return, they are an ERROR response: the return is FWU_OK and out_length > 0.
 * @param  request       A valid frame coming from the parser.
 * @param  now_ms        Current time (ms).
 * @param  out           Response buffer (at least FWU_SESSION_MIN_OUT).
 * @param  out_length    Number of bytes of the response (> 0 on every successful call).
 * @return FWU_OK; FWU_ERR_INVALID_ARG for NULL, an invalid frame (payload NULL/0/larger than 512) or an uninitialized session;
 *         FWU_ERR_BUFFER_TOO_SMALL for out_capacity < FWU_SESSION_MIN_OUT (in this case the state does NOT
 *         change and no flash operation is performed).
 */
fwu_return_e fwu_session_on_frame(fwu_session_t *session, const fwu_frame_view_t *request, uint32_t now_ms,
                                  fwu_byte_t *out, size_t out_capacity, size_t *out_length);

/**
 * @brief  Does the time-driven work: runs the pending verification (writes the result into `out` as VERIFY_RESULT) and
 *         applies the timeouts. Called often in the main loop; fast when no work is pending.
 * @return FWU_OK (out_length 0 = nothing to send); FWU_ERR_INVALID_ARG for NULL/uninitialized;
 *         FWU_ERR_BUFFER_TOO_SMALL for out_capacity < FWU_SESSION_MIN_OUT (state unchanged).
 */
fwu_return_e fwu_session_poll(fwu_session_t *session, uint32_t now_ms, fwu_byte_t *out, size_t out_capacity, size_t *out_length);

/**
 * @brief  Writes a data chunk to staging WITHOUT a frame: the same as TRANSFER_DATA, only without the frame carrying the request and
 *         the DATA_ACK response. fwu_pull supplies the chunks from HERE when it pulls the image from the server itself. It shares the
 *         SAME body with the TRANSFER_DATA handler: same rules, same error codes (BAD_STATE: not DOWNLOADING; BAD_LENGTH: chunk size or
 *         the "only the last chunk may be short" rule; BAD_BLOCK: not the expected offset; flash errors: the session drops to IDLE).
 *         An exact repeat of the previous chunk is re-acknowledged without writing (idempotent).
 * @param  offset       Offset of the chunk within the image; must equal the session's next_offset (except for a repeated chunk).
 * @param  data/length  The chunk (1..chunk_size elements).
 * @param  now_ms       Current time: postpones the idle timeout (so the session does not drop during a long pull).
 * @param  next_offset  On success the current next offset (the value DATA_ACK would carry).
 * @return FWU_OK; FWU_ERR_INVALID_ARG for NULL or an uninitialized session; other codes as above.
 */
fwu_return_e fwu_session_push_chunk(fwu_session_t *session, uint32_t offset, const fwu_byte_t *data, size_t length,
                                    uint32_t now_ms, uint32_t *next_offset);

/**
 * @brief  Advances the session's "last activity" time (postpones the idle timeout). May be called without processing a frame: fwu_pull uses it
 *         so that the session does not hit the idle timeout while pulling the image from the server (the host sends no frames).
 *         Does nothing on a NULL or uninitialized session.
 */
void fwu_session_touch(fwu_session_t *session, uint32_t now_ms);

/** Size of the accepted image (a QUERY; 0 when there is no download, 0 if NULL). */
uint32_t fwu_session_get_image_size(const fwu_session_t *session);

/** Number of image bytes written to flash = the expected next offset (a QUERY; 0 if NULL). */
uint32_t fwu_session_get_next_offset(const fwu_session_t *session);

/** Configured data chunk size (a QUERY; 0 if NULL). */
uint32_t fwu_session_get_chunk_size(const fwu_session_t *session);

/** Session state (a QUERY). IDLE if NULL. */
fwu_session_state_e fwu_session_get_state(const fwu_session_t *session);

/** Whether the commit has been written and a restart is awaited (a QUERY). false if NULL. */
bool fwu_session_reboot_requested(const fwu_session_t *session);

#ifdef __cplusplus
}
#endif

#endif /* FWU_SESSION_H_ */
