/**
 * @file    fwu_auth.h
 * @brief   REAL IMPLEMENTATION OF AUTHORIZATION (task G9d): provides the authorization hook of the session (fwu_session_auth_t) and the
 *          response hook of the host (fwu_client_auth_fn). ASYMMETRIC challenge-response: the operator holds a PRIVATE key, the device
 *          holds only the operator's PUBLIC key (no secret: a memory dump of one device does not open the whole fleet). There is NO new
 *          cryptographic operation: verification is the existing Ed25519 port (fwu_crypto.h).
 *
 * PURPOSE: the signature and hash guarantee the authenticity of the image; this module determines WHO may update the device (repeatedly
 * sending a validly signed image would wear out the flash and keep the device busy, fwu_session.h). Operator keys are SEPARATE slots
 * from the image signing keys (role separation: even if an image signing key leaks, access to the device is not opened).
 *
 * FLOW
 *   host: AUTH_REQ  ->  device: begin(): challenge = nonce (32 bytes, RNG hook) || device id (1..16 bytes)  -> AUTH_CHALLENGE
 *   host: message = "FWUA" | version(1) | product id(4, BE) | the WHOLE challenge; signature = Ed25519(operator private key, message)
 *         response = key id(1) | signature(64)  (65 bytes)  -> AUTH_RESP
 *   device: check(): select the operator public key by key id, build the message in the SAME format, verify the signature -> AUTH_OK
 *   The message builder (fwu_auth_build_message) is SHARED by host and device: the host signs exactly the bytes the device verifies.
 *
 * THE RESPONSE IS BOUND TO: this challenge (nonce), this device (id), this product, this protocol version. Replaying an old response in another
 * session, on another device or for another product is useless. Constraint: on a repeated AUTH_REQ with the same SEQ the session
 * re-issues the SAME challenge (fwu_session.h, a lost response is idempotent); the challenge stays valid until a response is accepted, a new-SEQ AUTH_REQ, ABORT, the
 * start of a download, or an idle timeout (README open issue 61).
 *
 * RANDOMNESS (RNG hook): MANDATORY, NO SOFTWARE FALLBACK (weak randomness makes authorization meaningless). The source is the integration's job
 *   (hardware RNG on STM32G0B1 [UNVERIFIED: not verified against RM0444]; the equivalent on C2000 is [UNVERIFIED] unknown). The core only catches an obvious fault:
 *   if the RNG returns an error, the nonce is all zeros, or it equals the previous nonce, begin fails (FWU_ERR_CRYPTO_FAILURE) and authorization
 *   is NOT opened (fail-closed).
 *   WARNING: the previous nonce is kept in RAM only. An RNG that returns to the SAME sequence after a restart (a fixed-seed PRNG) allows an old response to be
 *   replayed (shown in the test). The RNG must be unique across restarts as well (hardware RNG or a seed that changes on restart).
 *
 * LOCKOUT: the RAM lockout in the session is sufficient: signatures cannot be guessed, brute force is pointless; a persistent lockout wears the flash, with no benefit.
 *
 * ERROR CODES (check): invalid signature BAD_SIGNATURE (50); unknown operator key id KEY_REJECTED (54, the signature computation is not
 *   entered); if the response length is not 65 BAD_LENGTH (31); crypto "could not be performed" CRYPTO_FAILURE (51). All count as a failed attempt in the
 *   session (lockout after auth_max_failures in a row), including CRYPTO_FAILURE (a device fault behaves like an attack too, a deliberate simplification).
 *
 * No dynamic memory; the module is not called from an ISR.
 */

#ifndef FWU_AUTH_H_
#define FWU_AUTH_H_

#include <stdbool.h>
#include <stdint.h>
#include "fwu_error.h"
#include "fwu_types.h"
#include "fwu_crypto.h"
#include "fwu_verify.h"
#include "fwu_session.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FWU_AUTH_VERSION            1u      /**< Message format version. */
#define FWU_AUTH_NONCE_SIZE         32u
#define FWU_AUTH_DEVICE_ID_MAX      16u     /**< The STM32 chip UID is 12 bytes. */
#define FWU_AUTH_CHALLENGE_MIN      (FWU_AUTH_NONCE_SIZE + 1u)
#define FWU_AUTH_CHALLENGE_MAX      (FWU_AUTH_NONCE_SIZE + FWU_AUTH_DEVICE_ID_MAX)     /**< 48 <= FWU_PROTO_AUTH_CHALLENGE_MAX (64). */
#define FWU_AUTH_MESSAGE_HEADER     9u      /**< "FWUA"(4) + version(1) + product id(4). */
#define FWU_AUTH_MESSAGE_MAX        (FWU_AUTH_MESSAGE_HEADER + FWU_AUTH_CHALLENGE_MAX)  /**< 57. */
#define FWU_AUTH_RESPONSE_SIZE      (1u + FWU_CRYPTO_ED25519_SIGNATURE_SIZE)            /**< key id + signature = 65. */

/* ------------------------------------------------------------------ common (host and device) */

/**
 * @brief  Builds the signed message: "FWUA" | version(1) | product id(4, big endian) | challenge.
 * @param  challenge_length  FWU_AUTH_CHALLENGE_MIN..FWU_AUTH_CHALLENGE_MAX (nonce + at least 1 byte of device id).
 * @return FWU_OK; INVALID_ARG for NULL; BAD_LENGTH for a length out of range; BUFFER_TOO_SMALL for capacity < 9 + challenge_length.
 */
fwu_return_e fwu_auth_build_message(uint32_t product_id, const fwu_byte_t *challenge, uint32_t challenge_length,
                                    fwu_byte_t *out, uint32_t capacity, uint32_t *out_length);

/** Builds the response: key id(1) | signature(64). @return FWU_OK; INVALID_ARG for NULL; BUFFER_TOO_SMALL for capacity < 65. */
fwu_return_e fwu_auth_build_response(fwu_byte_t key_id, const fwu_byte_t *signature, fwu_byte_t *out, uint32_t capacity,
                                     uint32_t *out_length);

/* ------------------------------------------------------------------ device side */

/** Random byte source: writes `length` bytes to `out`. Anything other than FWU_OK = source faulty (authorization is not opened). */
typedef fwu_return_e (*fwu_auth_rng_fn)(void *context, fwu_byte_t *out, uint32_t length);

typedef struct {
    fwu_crypto_t           *crypto;             /**< Ed25519 verification (mandatory). */
    fwu_auth_rng_fn         rng;                /**< Mandatory. */
    void                   *rng_context;
    const fwu_byte_t       *device_id;          /**< Device-specific id (e.g. the chip UID); init COPIES it. */
    uint32_t                device_id_length;   /**< 1..FWU_AUTH_DEVICE_ID_MAX. */
    uint32_t                product_id;
    const fwu_verify_key_t *operator_keys;      /**< Operator public keys (SEPARATE from the image keys; ids must be unique). */
    uint32_t                operator_key_count; /**< >= 1. */
} fwu_auth_config_t;

/** State. Must stay FIXED in place: `hook.context` points to this structure. Do not touch the fields. */
typedef struct {
    fwu_auth_config_t    config;
    fwu_byte_t           device_id[FWU_AUTH_DEVICE_ID_MAX];
    fwu_byte_t           last_nonce[FWU_AUTH_NONCE_SIZE];
    bool                 have_last;
    bool                 initialized;
    fwu_session_auth_t   hook;
} fwu_auth_t;

/**
 * @brief  Sets up the device side and prepares the session hook.
 * @return FWU_OK; FWU_ERR_INVALID_ARG for NULL, crypto/rng/key array NULL, key count 0, device id NULL or length out of range,
 *         two operator keys with the same id (the object is NOT considered set up).
 */
fwu_return_e fwu_auth_init(fwu_auth_t *auth, const fwu_auth_config_t *config);

/** The hook to be used by the session (fwu_session_config_t.auth); NULL if not set up. Pass this for require_auth. */
const fwu_session_auth_t *fwu_auth_hook(const fwu_auth_t *auth);

/* ------------------------------------------------------------------ host (operator tool) side */

/** Signing hook: signs `message` with the operator private key; writes the key id and the 64-byte signature. Anything other than FWU_OK = failed. */
typedef fwu_return_e (*fwu_auth_sign_fn)(void *context, const fwu_byte_t *message, uint32_t message_length,
                                         fwu_byte_t *key_id, fwu_byte_t *signature);

typedef struct {
    uint32_t          product_id;       /**< Id of the product being updated (goes into the signed message). */
    fwu_auth_sign_fn  sign;             /**< Mandatory. */
    void             *sign_context;
} fwu_auth_host_t;

/**
 * @brief  fwu_client_auth_fn signature: given to the client's `auth` and `auth_context` fields (fwu_auth_host_respond, &host).
 *         Builds the message from the challenge, has it signed, produces the response (65 bytes).
 * @return FWU_OK; INVALID_ARG for NULL; BAD_LENGTH for a challenge length out of range; BUFFER_TOO_SMALL for capacity < 65;
 *         a signing error as is.
 */
fwu_return_e fwu_auth_host_respond(void *context, const fwu_byte_t *challenge, uint32_t challenge_length,
                                   fwu_byte_t *response, uint32_t capacity, uint32_t *response_length);

#ifdef __cplusplus
}
#endif

#endif /* FWU_AUTH_H_ */
