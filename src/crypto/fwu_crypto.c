/**
 * @file    fwu_crypto.c
 * @brief   Cryptography wrappers (see fwu_crypto.h for the rules).
 */

#include <stddef.h>
#include "fwu_crypto.h"

/** Is the object chain usable (object, table, four functions)? */
static bool crypto_chain_ok(const fwu_crypto_t *self)
{
    return (self != NULL) && (self->callback != NULL) && (self->callback->sha256_begin != NULL) &&
           (self->callback->sha256_update != NULL) && (self->callback->sha256_finish != NULL) &&
           (self->callback->ed25519_verify != NULL);
}

/** Normalises a port result: FWU_OK is kept, everything else becomes FWU_ERR_CRYPTO_FAILURE. */
static fwu_return_e crypto_normalize(fwu_return_e port_status)
{
    return (port_status == FWU_OK) ? FWU_OK : FWU_ERR_CRYPTO_FAILURE;
}

fwu_return_e fwu_crypto_init(fwu_crypto_t *self, void *context, const fwu_crypto_callback_t *callback)
{
    if((self == NULL) || (callback == NULL) || (callback->sha256_begin == NULL) ||
       (callback->sha256_update == NULL) || (callback->sha256_finish == NULL) ||
       (callback->ed25519_verify == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    self->context  = context;
    self->callback = callback;
    self->hashing  = false;
    return FWU_OK;
}

fwu_return_e fwu_crypto_sha256_begin(fwu_crypto_t *self)
{
    fwu_return_e status;

    if(!crypto_chain_ok(self)) {
        return FWU_ERR_INVALID_ARG;
    }
    if(self->hashing) {
        return FWU_ERR_BAD_STATE;           /* previous digest not finished */
    }
    status = crypto_normalize(self->callback->sha256_begin(self->context));
    self->hashing = (status == FWU_OK);
    return status;
}

fwu_return_e fwu_crypto_sha256_update(fwu_crypto_t *self, const fwu_byte_t *data, uint32_t length)
{
    fwu_return_e status;

    if(!crypto_chain_ok(self)) {
        return FWU_ERR_INVALID_ARG;
    }
    if((length > 0u) && (data == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    if(!self->hashing) {
        return FWU_ERR_BAD_STATE;           /* begin was not called */
    }
    if(length == 0u) {
        return FWU_OK;
    }
    status = crypto_normalize(self->callback->sha256_update(self->context, data, length));
    if(status != FWU_OK) {
        self->hashing = false;              /* digest invalid: a new begin is required */
    }
    return status;
}

fwu_return_e fwu_crypto_sha256_finish(fwu_crypto_t *self, fwu_byte_t *digest)
{
    fwu_return_e status;

    if((!crypto_chain_ok(self)) || (digest == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    if(!self->hashing) {
        return FWU_ERR_BAD_STATE;           /* begin was not called */
    }
    status = crypto_normalize(self->callback->sha256_finish(self->context, digest));
    self->hashing = false;                  /* finish always closes the digest */
    return status;
}

fwu_return_e fwu_crypto_ed25519_verify(fwu_crypto_t *self, const fwu_byte_t *message, uint32_t message_length,
                                       const fwu_byte_t *signature, const fwu_byte_t *public_key)
{
    fwu_return_e port_status;

    if((!crypto_chain_ok(self)) || (message == NULL) || (message_length == 0u) || (signature == NULL) ||
       (public_key == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    port_status = self->callback->ed25519_verify(self->context, message, message_length, signature, public_key);

    /* "Valid" ONLY if the port returns FWU_OK; "invalid" only if the port returns BAD_SIGNATURE; everything else is "could not be performed". */
    if(port_status == FWU_OK) {
        return FWU_OK;
    }
    if(port_status == FWU_ERR_BAD_SIGNATURE) {
        return FWU_ERR_BAD_SIGNATURE;
    }
    return FWU_ERR_CRYPTO_FAILURE;
}
