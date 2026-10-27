/**
 * @file    fwu_proto.h
 * @brief   UPDATE SESSION PROTOCOL: message types, payload formats and encode/parse functions.
 *          It is the SINGLE SOURCE OF TRUTH of the protocol (just as fwu_frame.h is for the frame format): the device
 *          (fwu_session) and a host tool take the same constants from here. It keeps no state; it only builds/decodes bytes.
 *
 * CARRIER: a message is the PAYLOAD of a fwu_frame.h frame; the message TYPE is the frame's TYPE byte. A response carries the
 * request's SEQ unchanged. Because the frame does not accept an empty payload, messages that carry no data carry a 1-byte
 * placeholder field. Multi-byte numbers are BIG ENDIAN. Offsets and sizes are counts of fwu_byte_t (bytes on STM32).
 *
 * ALIGNMENT WITH UDS (ISO 14229) [UNVERIFIED]: the flow follows UDS concepts (authorization, download request, data transfer,
 * transfer exit, "busy", TesterPresent), but the NUMBERS ARE OUR OWN and have not been verified against the official UDS
 * service numbers. Response type = request type | 0x80 (a deliberate deviation from UDS's +0x40 rule: it is separated by a bit mask).
 *
 * MESSAGES (host -> device)                                  RESPONSE (device -> host)
 *   0x01 HELLO             [version(1)]                       0x81 INFO             13 bytes, below
 *   0x02 AUTH_REQ          [0]                                0x82 AUTH_CHALLENGE   [challenge(1..64)]
 *   0x03 AUTH_RESP         [response(1..128)]                 0x83 AUTH_OK          [0]
 *   0x10 REQUEST_DOWNLOAD  [manifest(138)]                    0x90 DOWNLOAD_ACCEPTED 9 bytes, below
 *   0x11 TRANSFER_DATA     [offset(4)][data(1..508)]          0x91 DATA_ACK         [next_offset(4)]
 *   0x12 TRANSFER_EXIT     [image_size(4)]                    0x92 EXIT_PENDING     [1]  (verification in progress)
 *                                                             0x94 VERIFY_RESULT    [code(1)]  (0 = valid; separate, LATER message)
 *   0x13 ACTIVATE          [image_size(4)]                    0x93 ACTIVATE_OK      [0]
 *   0x15 PULL_START        [ip(4)][port(2)][length(1)]        0x95 PULL_STARTED     8 bytes, PULL_STATUS format
 *                          [target(1..96)]
 *   0x16 PULL_QUERY        [0]                                0x96 PULL_STATUS      8 bytes, below
 *   0x1F ABORT             [reason(1)]                        0x9F ABORT_OK         [0]
 *   0x3E KEEPALIVE         [0]                                0xBE KEEPALIVE_OK     [state(1)]
 *                          (every request may fail:)          0x7F ERROR            [code(1)][request_type(1)]
 *
 *   `code` = the PERMANENT error number from fwu_error.h (in a single byte; no code exceeds 255). The host tells
 *   "BAD_SIGNATURE or CRYPTO_FAILURE?" apart from this number.
 *
 * INFO (13 bytes): version(1) flags(1) product_id(4) running_image_version(4) max_chunk(2) state(1)
 *   flags: bit0 = authorization required, bit1 = signature required. state: FWU_PROTO_STATE_*.
 * DOWNLOAD_ACCEPTED (9 bytes): chunk_size(2) mode(1) timeout_s(2) next_offset(4). The device reports its flow/performance
 *   parameters in THIS response (decision D20); the protection profile is not reported, it is a build-time constant. mode 0 =
 *   acknowledgement per chunk (stop-and-wait), other modes later (windowed streaming).
 *
 * PULL_STATUS (8 bytes): state(1) next_offset(4) error(1) http_status(2). state: FWU_PROTO_PULL_*. next_offset: number of image bytes
 *   pulled from the server and WRITTEN to flash (the basis for resuming). error: the fwu_error.h number while FWU_PROTO_PULL_FAILED, otherwise 0.
 *   http_status: the last HTTP status code the server reported (for debugging; 0 if no response ever arrived).
 *   PULL_START asks the device to pull the image FROM THE SERVER ITSELF (the host does not send TRANSFER_DATA): target is the HTTP
 *   request-target string (path and query, e.g. "/api/com/tcp-config/getFwUpdateFile?deviceType=fan&version=12"); the device builds the
 *   Host header from ip:port. It is valid only while the manifest is ACCEPTED (DOWNLOADING) (REQUEST_DOWNLOAD first). When the pull
 *   finishes the host sends TRANSFER_EXIT and ACTIVATE as usual. PULL_QUERY reads the progress (it changes nothing).
 *
 * FLOW (summary; state transitions are in fwu_session.h):
 *   HELLO -> (AUTH_REQ -> AUTH_RESP) -> REQUEST_DOWNLOAD -> TRANSFER_DATA... -> TRANSFER_EXIT -> VERIFY_RESULT
 *   -> ACTIVATE. The acknowledgement of TRANSFER_DATA means "the chunk was WRITTEN TO FLASH and read back and verified".
 */

#ifndef FWU_PROTO_H_
#define FWU_PROTO_H_

#include <stddef.h>
#include <stdint.h>
#include "fwu_error.h"
#include "fwu_types.h"
#include "fwu_frame.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ version and constants */

#define FWU_PROTO_VERSION               1u      /**< Protocol version (reported in INFO). */

#define FWU_PROTO_RESPONSE_FLAG         0x80u   /**< Response type = request type | this flag. */

/* Request types (host -> device). */
#define FWU_MSG_HELLO                   0x01u
#define FWU_MSG_AUTH_REQ                0x02u
#define FWU_MSG_AUTH_RESP               0x03u
#define FWU_MSG_REQUEST_DOWNLOAD        0x10u
#define FWU_MSG_TRANSFER_DATA           0x11u
#define FWU_MSG_TRANSFER_EXIT           0x12u
#define FWU_MSG_ACTIVATE                0x13u
#define FWU_MSG_PULL_START              0x15u
#define FWU_MSG_PULL_QUERY              0x16u
#define FWU_MSG_ABORT                   0x1Fu
#define FWU_MSG_KEEPALIVE               0x3Eu

/* Response types (device -> host). */
#define FWU_MSG_ERROR                   0x7Fu
#define FWU_MSG_INFO                    0x81u
#define FWU_MSG_AUTH_CHALLENGE          0x82u
#define FWU_MSG_AUTH_OK                 0x83u
#define FWU_MSG_DOWNLOAD_ACCEPTED       0x90u
#define FWU_MSG_DATA_ACK                0x91u
#define FWU_MSG_EXIT_PENDING            0x92u
#define FWU_MSG_ACTIVATE_OK             0x93u
#define FWU_MSG_PULL_STARTED            0x95u
#define FWU_MSG_PULL_STATUS             0x96u
#define FWU_MSG_VERIFY_RESULT           0x94u
#define FWU_MSG_ABORT_OK                0x9Fu
#define FWU_MSG_KEEPALIVE_OK            0xBEu

/* Session states (PERMANENT numbers: INFO and KEEPALIVE_OK carry them). */
#define FWU_PROTO_STATE_IDLE            0u      /**< Idle. */
#define FWU_PROTO_STATE_AUTH_PENDING    1u      /**< Challenge sent, waiting for the response. */
#define FWU_PROTO_STATE_AUTHENTICATED   2u      /**< Authorization complete. */
#define FWU_PROTO_STATE_DOWNLOADING     3u      /**< Manifest accepted, image being downloaded. */
#define FWU_PROTO_STATE_VERIFYING       4u      /**< Download finished, verification in progress. */
#define FWU_PROTO_STATE_READY           5u      /**< Verified, waiting for ACTIVATE. */
#define FWU_PROTO_STATE_COMMITTED       6u      /**< Commit written, waiting for restart. */

/* Pull states (in PULL_STATUS; PERMANENT numbers). */
#define FWU_PROTO_PULL_IDLE             0u      /**< Pull has not started (or the session was reset). */
#define FWU_PROTO_PULL_REQUESTING       1u      /**< Request sent to the server / waiting for the header / retrying. */
#define FWU_PROTO_PULL_DOWNLOADING      2u      /**< Body streaming, chunks being written to flash. */
#define FWU_PROTO_PULL_DONE             3u      /**< The whole image has been pulled and written. */
#define FWU_PROTO_PULL_FAILED           4u      /**< Failed (see the error field). */

/* INFO flags. */
#define FWU_PROTO_FLAG_AUTH_REQUIRED    0x01u
#define FWU_PROTO_FLAG_SIGNED           0x02u

/* Flow modes (DOWNLOAD_ACCEPTED). */
#define FWU_PROTO_MODE_HANDSHAKE        0u      /**< Acknowledgement per chunk (stop-and-wait). */

/* Sizes (fwu_byte_t). */
#define FWU_PROTO_INFO_SIZE             13u
#define FWU_PROTO_ACCEPTED_SIZE         9u
#define FWU_PROTO_U32_SIZE              4u
#define FWU_PROTO_DATA_HEADER_SIZE      4u      /**< Offset field in TRANSFER_DATA. */
#define FWU_PROTO_ERROR_SIZE            2u
#define FWU_PROTO_MAX_CHUNK             (FWU_FRAME_MAX_PAYLOAD - FWU_PROTO_DATA_HEADER_SIZE)   /**< 508 */
#define FWU_PROTO_PULL_START_FIXED      7u      /**< Fields before the target in PULL_START: ip(4) port(2) length(1). */
#define FWU_PROTO_PULL_TARGET_MAX       96u     /**< Maximum length of the HTTP request target. */
#define FWU_PROTO_PULL_STATUS_SIZE      8u
#define FWU_PROTO_AUTH_CHALLENGE_MAX    64u
#define FWU_PROTO_AUTH_RESPONSE_MAX     128u
#define FWU_PROTO_PLACEHOLDER           0u      /**< 1-byte placeholder field of messages that carry no data. */

/* ------------------------------------------------------------------ structures */

/** Content of the INFO response. */
typedef struct {
    fwu_byte_t proto_version;           /**< Protocol version of the device. */
    fwu_byte_t flags;                   /**< FWU_PROTO_FLAG_*. */
    uint32_t   product_id;              /**< Product id of the device. */
    uint32_t   running_image_version;   /**< Version of the running image (informational). */
    uint16_t   max_chunk;               /**< Data chunk size the device accepts. */
    fwu_byte_t state;                   /**< FWU_PROTO_STATE_*. */
} fwu_proto_info_t;

/** Content of the DOWNLOAD_ACCEPTED response. */
typedef struct {
    uint16_t   chunk_size;              /**< Data length in each TRANSFER_DATA (except the last chunk). */
    fwu_byte_t mode;                    /**< FWU_PROTO_MODE_*. */
    uint16_t   timeout_s;               /**< Idle timeout of the device (seconds). */
    uint32_t   next_offset;             /**< First offset the device expects (0 for a new download, where it left off when resuming). */
} fwu_proto_accepted_t;

/** Content of the TRANSFER_DATA request (`data` points inside the payload, no copy). */
typedef struct {
    uint32_t          offset;           /**< Offset of the chunk within the image. */
    const fwu_byte_t *data;             /**< Chunk data. */
    size_t            data_length;      /**< 1..FWU_PROTO_MAX_CHUNK. */
} fwu_proto_data_t;

/** Content of the PULL_START request (`target` points inside the payload, no copy; not NUL-terminated). */
typedef struct {
    fwu_byte_t        ip[4];            /**< IPv4 address of the server (in a.b.c.d order). */
    uint16_t          port;             /**< TCP port of the server (1..65535). */
    const fwu_byte_t *target;           /**< HTTP request target (path + query), printable ASCII. */
    size_t            target_length;    /**< 1..FWU_PROTO_PULL_TARGET_MAX. */
} fwu_proto_pull_start_t;

/** PULL_STATUS content. */
typedef struct {
    fwu_byte_t state;                   /**< FWU_PROTO_PULL_*. */
    uint32_t   next_offset;             /**< Image bytes written to flash. */
    fwu_byte_t error;                   /**< fwu_error.h code if FAILED, otherwise 0. */
    uint16_t   http_status;             /**< Last HTTP status code (0 = no response at all). */
} fwu_proto_pull_status_t;

/* ------------------------------------------------------------------ encode / parse
 * Contract common to all of them: encode_*: `out` is written only if ALL validation passes; FWU_ERR_BUFFER_TOO_SMALL if the capacity
 * is not enough, FWU_ERR_INVALID_ARG for NULL; `out_length` is the payload length on output.
 * parse_*: FWU_ERR_BAD_LENGTH if the length does not match the expected one, FWU_ERR_INVALID_ARG for NULL; the output is written only
 * on FWU_OK. */

fwu_return_e fwu_proto_encode_info(const fwu_proto_info_t *info, fwu_byte_t *out, size_t out_capacity, size_t *out_length);
fwu_return_e fwu_proto_parse_info(const fwu_byte_t *payload, size_t payload_length, fwu_proto_info_t *info);

fwu_return_e fwu_proto_encode_accepted(const fwu_proto_accepted_t *accepted, fwu_byte_t *out, size_t out_capacity, size_t *out_length);
fwu_return_e fwu_proto_parse_accepted(const fwu_byte_t *payload, size_t payload_length, fwu_proto_accepted_t *accepted);

/** A single 32-bit value (DATA_ACK next offset, TRANSFER_EXIT and ACTIVATE image size). */
fwu_return_e fwu_proto_encode_u32(uint32_t value, fwu_byte_t *out, size_t out_capacity, size_t *out_length);
fwu_return_e fwu_proto_parse_u32(const fwu_byte_t *payload, size_t payload_length, uint32_t *value);

/** TRANSFER_DATA: encode is the host side. parse: requires at least 1 byte of data; BAD_LENGTH if the data exceeds 508. */
fwu_return_e fwu_proto_encode_data(uint32_t offset, const fwu_byte_t *data, size_t data_length,
                                   fwu_byte_t *out, size_t out_capacity, size_t *out_length);
fwu_return_e fwu_proto_parse_data(const fwu_byte_t *payload, size_t payload_length, fwu_proto_data_t *data);

/** VERIFY_RESULT: a single-byte error code (0 = valid). FWU_ERR_INVALID_ARG if code exceeds 255. */
fwu_return_e fwu_proto_encode_result(fwu_return_e code, fwu_byte_t *out, size_t out_capacity, size_t *out_length);
fwu_return_e fwu_proto_parse_result(const fwu_byte_t *payload, size_t payload_length, fwu_byte_t *code);

/** ERROR: [code(1)][request_type(1)]. code must be 1..255 (0 = success, cannot be an error response), otherwise INVALID_ARG. */
fwu_return_e fwu_proto_encode_error(fwu_return_e code, fwu_byte_t request_type,
                                    fwu_byte_t *out, size_t out_capacity, size_t *out_length);
fwu_return_e fwu_proto_parse_error(const fwu_byte_t *payload, size_t payload_length, fwu_byte_t *code, fwu_byte_t *request_type);

/** PULL_START: encode is the host side. A valid target: printable ASCII (0x21..0x7E; no space, CR or LF, because they would break the
 *  HTTP request line), starts with '/', length 1..FWU_PROTO_PULL_TARGET_MAX; port cannot be 0. FWU_ERR_INVALID_ARG if invalid.
 *  parse (device side): FWU_ERR_BAD_LENGTH if the payload length does not match the length field or the target length is outside 1..96;
 *  a forbidden character / a target not starting with '/' / port 0 gives FWU_ERR_INVALID_ARG. The output is written only on FWU_OK. */
fwu_return_e fwu_proto_encode_pull_start(const fwu_byte_t ip[4], uint16_t port, const fwu_byte_t *target, size_t target_length,
                                         fwu_byte_t *out, size_t out_capacity, size_t *out_length);
fwu_return_e fwu_proto_parse_pull_start(const fwu_byte_t *payload, size_t payload_length, fwu_proto_pull_start_t *start);

/** PULL_STATUS (payload of the PULL_STARTED and PULL_STATUS responses). An unknown state number (> FWU_PROTO_PULL_FAILED) gives
 *  FWU_ERR_INVALID_ARG in both encode and parse; in parse FWU_ERR_BAD_LENGTH if the length is not 8. */
fwu_return_e fwu_proto_encode_pull_status(const fwu_proto_pull_status_t *status, fwu_byte_t *out, size_t out_capacity, size_t *out_length);
fwu_return_e fwu_proto_parse_pull_status(const fwu_byte_t *payload, size_t payload_length, fwu_proto_pull_status_t *status);

#ifdef __cplusplus
}
#endif

#endif /* FWU_PROTO_H_ */
