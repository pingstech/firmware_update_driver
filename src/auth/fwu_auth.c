/**
 * @file    fwu_auth.c
 * @brief   Authorization implementation (see fwu_auth.h for the flow, message format and rules).
 */

#include <stddef.h>
#include "fwu_auth.h"
#include "fwu_bytes.h"

/* ------------------------------------------------------------------ common */

fwu_return_e fwu_auth_build_message(uint32_t product_id, const fwu_byte_t *challenge, uint32_t challenge_length,
                                    fwu_byte_t *out, uint32_t capacity, uint32_t *out_length)
{
    if((challenge == NULL) || (out == NULL) || (out_length == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    if((challenge_length < FWU_AUTH_CHALLENGE_MIN) || (challenge_length > FWU_AUTH_CHALLENGE_MAX)) {
        return FWU_ERR_BAD_LENGTH;
    }
    if(capacity < (FWU_AUTH_MESSAGE_HEADER + challenge_length)) {
        return FWU_ERR_BUFFER_TOO_SMALL;
    }
    out[0] = (fwu_byte_t)0x46u;                     /* 'F' */
    out[1] = (fwu_byte_t)0x57u;                     /* 'W' */
    out[2] = (fwu_byte_t)0x55u;                     /* 'U' */
    out[3] = (fwu_byte_t)0x41u;                     /* 'A' */
    out[4] = (fwu_byte_t)FWU_AUTH_VERSION;
    fwu_write_u32_big_endian(&out[5], product_id);
    for(uint32_t i = 0u; i < challenge_length; i++) {
        out[FWU_AUTH_MESSAGE_HEADER + i] = (fwu_byte_t)(challenge[i] & FWU_BYTE_MASK);
    }
    *out_length = FWU_AUTH_MESSAGE_HEADER + challenge_length;
    return FWU_OK;
}

fwu_return_e fwu_auth_build_response(fwu_byte_t key_id, const fwu_byte_t *signature, fwu_byte_t *out, uint32_t capacity,
                                     uint32_t *out_length)
{
    if((signature == NULL) || (out == NULL) || (out_length == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    if(capacity < FWU_AUTH_RESPONSE_SIZE) {
        return FWU_ERR_BUFFER_TOO_SMALL;
    }
    out[0] = (fwu_byte_t)(key_id & FWU_BYTE_MASK);
    for(uint32_t i = 0u; i < FWU_CRYPTO_ED25519_SIGNATURE_SIZE; i++) {
        out[1u + i] = (fwu_byte_t)(signature[i] & FWU_BYTE_MASK);
    }
    *out_length = FWU_AUTH_RESPONSE_SIZE;
    return FWU_OK;
}

/* ------------------------------------------------------------------ device */

static fwu_return_e auth_begin(void *context, fwu_byte_t *challenge, uint32_t capacity, uint32_t *length)
{
    fwu_auth_t *a = (fwu_auth_t *)context;
    fwu_byte_t nonce[FWU_AUTH_NONCE_SIZE];
    uint32_t needed;
    bool all_zero = true;
    bool same;

    if((a == NULL) || (!a->initialized) || (challenge == NULL) || (length == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    same = a->have_last;                                /* on the first challenge there is no previous nonce to compare with */
    needed = FWU_AUTH_NONCE_SIZE + a->config.device_id_length;
    if(capacity < needed) {
        return FWU_ERR_BUFFER_TOO_SMALL;                /* the RNG is not touched at all */
    }
    if(a->config.rng(a->config.rng_context, nonce, FWU_AUTH_NONCE_SIZE) != FWU_OK) {
        return FWU_ERR_CRYPTO_FAILURE;                  /* source faulty: authorization is NOT opened */
    }
    for(uint32_t i = 0u; i < FWU_AUTH_NONCE_SIZE; i++) {
        nonce[i] = (fwu_byte_t)(nonce[i] & FWU_BYTE_MASK);
        if(nonce[i] != 0u) {
            all_zero = false;
        }
        if(nonce[i] != a->last_nonce[i]) {
            same = false;
        }
    }
    if(all_zero || same) {
        return FWU_ERR_CRYPTO_FAILURE;                  /* stuck/obviously broken RNG: the same challenge is not issued a second time */
    }
    for(uint32_t i = 0u; i < FWU_AUTH_NONCE_SIZE; i++) {
        challenge[i] = nonce[i];
        a->last_nonce[i] = nonce[i];
    }
    for(uint32_t i = 0u; i < a->config.device_id_length; i++) {
        challenge[FWU_AUTH_NONCE_SIZE + i] = a->device_id[i];
    }
    a->have_last = true;
    *length = needed;
    return FWU_OK;
}

static fwu_return_e auth_check(void *context, const fwu_byte_t *challenge, uint32_t challenge_length,
                               const fwu_byte_t *response, uint32_t response_length)
{
    fwu_auth_t *a = (fwu_auth_t *)context;
    fwu_byte_t message[FWU_AUTH_MESSAGE_MAX];
    uint32_t message_length = 0u;
    const fwu_verify_key_t *key = NULL;
    fwu_return_e status;

    if((a == NULL) || (!a->initialized) || (challenge == NULL) || (response == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    if(challenge_length != (FWU_AUTH_NONCE_SIZE + a->config.device_id_length)) {
        return FWU_ERR_INVALID_ARG;                     /* the challenge stored by the session is not in the format we produce */
    }
    for(uint32_t i = 0u; i < a->config.device_id_length; i++) {
        if((challenge[FWU_AUTH_NONCE_SIZE + i] & FWU_BYTE_MASK) != a->device_id[i]) {
            return FWU_ERR_INVALID_ARG;                 /* a challenge from another device */
        }
    }
    if(response_length != FWU_AUTH_RESPONSE_SIZE) {
        return FWU_ERR_BAD_LENGTH;
    }
    for(uint32_t i = 0u; i < a->config.operator_key_count; i++) {
        if(a->config.operator_keys[i].id == (fwu_byte_t)(response[0] & FWU_BYTE_MASK)) {
            key = &a->config.operator_keys[i];
        }
    }
    if(key == NULL) {
        return FWU_ERR_KEY_REJECTED;                    /* cheap rejection: the signature computation is not entered */
    }
    status = fwu_auth_build_message(a->config.product_id, challenge, challenge_length, message, FWU_AUTH_MESSAGE_MAX, &message_length);
    if(status != FWU_OK) {
        return status;
    }
    return fwu_crypto_ed25519_verify(a->config.crypto, message, message_length, &response[1], key->public_key);
}

fwu_return_e fwu_auth_init(fwu_auth_t *auth, const fwu_auth_config_t *config)
{
    if((auth == NULL) || (config == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    auth->initialized = false;
    if((config->crypto == NULL) || (config->rng == NULL) || (config->operator_keys == NULL) || (config->operator_key_count == 0u) ||
       (config->device_id == NULL) || (config->device_id_length == 0u) || (config->device_id_length > FWU_AUTH_DEVICE_ID_MAX)) {
        return FWU_ERR_INVALID_ARG;
    }
    for(uint32_t i = 0u; i < config->operator_key_count; i++) {
        for(uint32_t j = i + 1u; j < config->operator_key_count; j++) {
            if(config->operator_keys[i].id == config->operator_keys[j].id) {
                return FWU_ERR_INVALID_ARG;             /* same id on two keys: which one? */
            }
        }
    }
    auth->config = *config;
    for(uint32_t i = 0u; i < FWU_AUTH_DEVICE_ID_MAX; i++) {
        auth->device_id[i] = (i < config->device_id_length) ? (fwu_byte_t)(config->device_id[i] & FWU_BYTE_MASK) : (fwu_byte_t)0u;
    }
    for(uint32_t i = 0u; i < FWU_AUTH_NONCE_SIZE; i++) {
        auth->last_nonce[i] = 0u;
    }
    auth->have_last = false;
    auth->hook.begin = auth_begin;
    auth->hook.check = auth_check;
    auth->hook.context = auth;
    auth->initialized = true;
    return FWU_OK;
}

const fwu_session_auth_t *fwu_auth_hook(const fwu_auth_t *auth)
{
    return ((auth != NULL) && auth->initialized) ? &auth->hook : NULL;
}

/* ------------------------------------------------------------------ host */

fwu_return_e fwu_auth_host_respond(void *context, const fwu_byte_t *challenge, uint32_t challenge_length,
                                   fwu_byte_t *response, uint32_t capacity, uint32_t *response_length)
{
    const fwu_auth_host_t *host = (const fwu_auth_host_t *)context;
    fwu_byte_t message[FWU_AUTH_MESSAGE_MAX];
    fwu_byte_t signature[FWU_CRYPTO_ED25519_SIGNATURE_SIZE];
    uint32_t message_length = 0u;
    fwu_byte_t key_id = 0u;
    fwu_return_e status;

    if((host == NULL) || (host->sign == NULL) || (challenge == NULL) || (response == NULL) || (response_length == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    if(capacity < FWU_AUTH_RESPONSE_SIZE) {
        return FWU_ERR_BUFFER_TOO_SMALL;
    }
    status = fwu_auth_build_message(host->product_id, challenge, challenge_length, message, FWU_AUTH_MESSAGE_MAX, &message_length);
    if(status != FWU_OK) {
        return status;
    }
    status = host->sign(host->sign_context, message, message_length, &key_id, signature);
    if(status != FWU_OK) {
        return status;
    }
    return fwu_auth_build_response(key_id, signature, response, capacity, response_length);
}
