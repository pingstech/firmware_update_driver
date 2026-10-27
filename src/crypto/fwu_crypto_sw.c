/**
 * @file    fwu_crypto_sw.c
 * @brief   Software cryptography port (SHA-256 real, no Ed25519). See fwu_crypto_sw.h for details.
 */

#include <stddef.h>
#include "fwu_crypto_sw.h"

static fwu_return_e sw_sha256_begin(void *context)
{
    fwu_crypto_sw_t *sw = (fwu_crypto_sw_t *)context;

    if(sw == NULL) {
        return FWU_ERR_INVALID_ARG;
    }
    fwu_sha256_init(&sw->sha);
    return FWU_OK;
}

static fwu_return_e sw_sha256_update(void *context, const fwu_byte_t *data, uint32_t length)
{
    fwu_crypto_sw_t *sw = (fwu_crypto_sw_t *)context;

    if((sw == NULL) || (data == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    fwu_sha256_update(&sw->sha, data, length);
    return FWU_OK;
}

static fwu_return_e sw_sha256_finish(void *context, fwu_byte_t *digest)
{
    fwu_crypto_sw_t *sw = (fwu_crypto_sw_t *)context;

    if((sw == NULL) || (digest == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    fwu_sha256_final(&sw->sha, digest);
    return FWU_OK;
}

static fwu_return_e sw_ed25519_verify(void *context, const fwu_byte_t *message, uint32_t message_length,
                                      const fwu_byte_t *signature, const fwu_byte_t *public_key)
{
    (void)context;
    (void)message;
    (void)message_length;
    (void)signature;
    (void)public_key;
    return FWU_ERR_CRYPTO_FAILURE;      /* NO Ed25519 implementation: never says "valid" (fail-closed) */
}

fwu_return_e fwu_crypto_sw_init(fwu_crypto_sw_t *sw, fwu_crypto_t *crypto)
{
    if((sw == NULL) || (crypto == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    fwu_sha256_init(&sw->sha);
    sw->callback.sha256_begin = sw_sha256_begin;
    sw->callback.sha256_update = sw_sha256_update;
    sw->callback.sha256_finish = sw_sha256_finish;
    sw->callback.ed25519_verify = sw_ed25519_verify;
    return fwu_crypto_init(crypto, sw, &sw->callback);
}
