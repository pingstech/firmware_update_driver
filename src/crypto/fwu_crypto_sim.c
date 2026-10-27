/**
 * @file    fwu_crypto_sim.c
 * @brief   Fake cryptography port (see fwu_crypto_sim.h for the rules and WARNINGS): real SHA-256,
 *          fake and INSECURE signature.
 */

#include <stddef.h>
#include "fwu_crypto_sim.h"

/* SHA-256 constants (FIPS 180-4): fractional parts of the cube roots of the first 64 primes and the square roots of the first 8 primes.
 * The values were regenerated with Python (integer root) and verified against hashlib digests. */
static const uint32_t SHA_K[64] = {
    0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u, 0x3956C25Bu, 0x59F111F1u, 0x923F82A4u, 0xAB1C5ED5u,
    0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u, 0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u, 0xC19BF174u,
    0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu, 0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu,
    0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u, 0xC6E00BF3u, 0xD5A79147u, 0x06CA6351u, 0x14292967u,
    0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu, 0x53380D13u, 0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u,
    0xA2BFE8A1u, 0xA81A664Bu, 0xC24B8B70u, 0xC76C51A3u, 0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u,
    0x19A4C116u, 0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u, 0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu, 0x682E6FF3u,
    0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u, 0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u
};

static const uint32_t SHA_H0[8] = {
    0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au, 0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u
};

/* ------------------------------------------------------------------ SHA-256 (REAL) */

static uint32_t sha_rotr(uint32_t x, uint32_t n)
{
    return (x >> n) | (x << (32u - n));
}

static void sha_init(fwu_crypto_sim_sha_t *s)
{
    for(uint32_t i = 0u; i < 8u; i++) {
        s->h[i] = SHA_H0[i];
    }
    s->block_length = 0u;
    s->total_length = 0u;
}

/** Processes the 64-element block (each element is limited to 0..255: on C28x the upper bits may be garbage). */
static void sha_compress(fwu_crypto_sim_sha_t *s)
{
    uint32_t w[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;

    for(uint32_t i = 0u; i < 16u; i++) {
        w[i] = ((uint32_t)(s->block[(4u * i) + 0u] & FWU_BYTE_MASK) << 24) |
               ((uint32_t)(s->block[(4u * i) + 1u] & FWU_BYTE_MASK) << 16) |
               ((uint32_t)(s->block[(4u * i) + 2u] & FWU_BYTE_MASK) << 8) |
               ((uint32_t)(s->block[(4u * i) + 3u] & FWU_BYTE_MASK));
    }
    for(uint32_t i = 16u; i < 64u; i++) {
        const uint32_t s0 = sha_rotr(w[i - 15u], 7u) ^ sha_rotr(w[i - 15u], 18u) ^ (w[i - 15u] >> 3);
        const uint32_t s1 = sha_rotr(w[i - 2u], 17u) ^ sha_rotr(w[i - 2u], 19u) ^ (w[i - 2u] >> 10);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }

    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
    e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];

    for(uint32_t i = 0u; i < 64u; i++) {
        const uint32_t big_s1 = sha_rotr(e, 6u) ^ sha_rotr(e, 11u) ^ sha_rotr(e, 25u);
        const uint32_t ch = (e & f) ^ ((~e) & g);
        const uint32_t t1 = h + big_s1 + ch + SHA_K[i] + w[i];
        const uint32_t big_s0 = sha_rotr(a, 2u) ^ sha_rotr(a, 13u) ^ sha_rotr(a, 22u);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = big_s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

/** Appends one element to the block (does NOT change the total length: for padding); processes the block when full. */
static void sha_push(fwu_crypto_sim_sha_t *s, uint32_t value)
{
    s->block[s->block_length] = (fwu_byte_t)(value & FWU_BYTE_MASK);
    s->block_length++;
    if(s->block_length == 64u) {
        sha_compress(s);
        s->block_length = 0u;
    }
}

static void sha_update(fwu_crypto_sim_sha_t *s, const fwu_byte_t *data, uint32_t length)
{
    for(uint32_t i = 0u; i < length; i++) {
        sha_push(s, (uint32_t)(data[i] & FWU_BYTE_MASK));
        s->total_length++;
    }
}

/** Appends padding + length and writes the digest to `digest` (32 elements). */
static void sha_final(fwu_crypto_sim_sha_t *s, fwu_byte_t *digest)
{
    const uint32_t bits_high = s->total_length >> 29;   /* total length * 8 (64 bit), in two parts */
    const uint32_t bits_low = s->total_length << 3;

    sha_push(s, 0x80u);
    while(s->block_length != 56u) {
        sha_push(s, 0u);
    }
    for(uint32_t i = 0u; i < 4u; i++) {
        sha_push(s, (bits_high >> (24u - (8u * i))) & 0xFFu);
    }
    for(uint32_t i = 0u; i < 4u; i++) {
        sha_push(s, (bits_low >> (24u - (8u * i))) & 0xFFu);
    }
    for(uint32_t i = 0u; i < 8u; i++) {
        for(uint32_t j = 0u; j < 4u; j++) {
            digest[(4u * i) + j] = (fwu_byte_t)((s->h[i] >> (24u - (8u * j))) & 0xFFu);
        }
    }
}

void fwu_crypto_sim_sha256(const fwu_byte_t *data, uint32_t length, fwu_byte_t *digest)
{
    fwu_crypto_sim_sha_t s;
    sha_init(&s);
    sha_update(&s, data, length);
    sha_final(&s, digest);
}

/* ------------------------------------------------------------------ fake signature (INSECURE) */

void fwu_crypto_sim_sign(const fwu_byte_t *public_key, const fwu_byte_t *message, uint32_t message_length,
                         fwu_byte_t *signature)
{
    fwu_crypto_sim_sha_t s;

    sha_init(&s);                                   /* first half: SHA-256(public key || message) */
    sha_update(&s, public_key, FWU_CRYPTO_ED25519_PUBLIC_KEY_SIZE);
    sha_update(&s, message, message_length);
    sha_final(&s, &signature[0]);

    sha_init(&s);                                   /* second half: SHA-256(message || public key) */
    sha_update(&s, message, message_length);
    sha_update(&s, public_key, FWU_CRYPTO_ED25519_PUBLIC_KEY_SIZE);
    sha_final(&s, &signature[FWU_CRYPTO_SHA256_DIGEST_SIZE]);
}

/* ------------------------------------------------------------------ port functions */

static fwu_return_e sim_sha256_begin(void *context)
{
    fwu_crypto_sim_t *sim = (fwu_crypto_sim_t *)context;
    sim->begin_calls++;
    if(sim->fail_begin != FWU_OK) {
        return sim->fail_begin;
    }
    sha_init(&sim->sha);
    return FWU_OK;
}

static fwu_return_e sim_sha256_update(void *context, const fwu_byte_t *data, uint32_t length)
{
    fwu_crypto_sim_t *sim = (fwu_crypto_sim_t *)context;
    sim->update_calls++;
    if(sim->fail_update != FWU_OK) {
        return sim->fail_update;
    }
    sha_update(&sim->sha, data, length);
    return FWU_OK;
}

static fwu_return_e sim_sha256_finish(void *context, fwu_byte_t *digest)
{
    fwu_crypto_sim_t *sim = (fwu_crypto_sim_t *)context;
    sim->finish_calls++;
    if(sim->fail_finish != FWU_OK) {
        return sim->fail_finish;
    }
    sha_final(&sim->sha, digest);
    return FWU_OK;
}

static fwu_return_e sim_ed25519_verify(void *context, const fwu_byte_t *message, uint32_t message_length,
                                       const fwu_byte_t *signature, const fwu_byte_t *public_key)
{
    fwu_crypto_sim_t *sim = (fwu_crypto_sim_t *)context;
    fwu_byte_t expected[FWU_CRYPTO_ED25519_SIGNATURE_SIZE];

    sim->verify_calls++;
    sim->last_verify_message_length = message_length;
    if(sim->fail_verify != FWU_OK) {
        return sim->fail_verify;
    }

    fwu_crypto_sim_sign(public_key, message, message_length, expected);
    for(uint32_t i = 0u; i < FWU_CRYPTO_ED25519_SIGNATURE_SIZE; i++) {
        if((signature[i] & FWU_BYTE_MASK) != expected[i]) {
            return FWU_ERR_BAD_SIGNATURE;
        }
    }
    return FWU_OK;
}

fwu_return_e fwu_crypto_sim_init(fwu_crypto_sim_t *sim)
{
    if(sim == NULL) {
        return FWU_ERR_INVALID_ARG;
    }
    sim->callback.sha256_begin   = sim_sha256_begin;
    sim->callback.sha256_update  = sim_sha256_update;
    sim->callback.sha256_finish  = sim_sha256_finish;
    sim->callback.ed25519_verify = sim_ed25519_verify;
    sha_init(&sim->sha);
    sim->fail_begin  = FWU_OK;
    sim->fail_update = FWU_OK;
    sim->fail_finish = FWU_OK;
    sim->fail_verify = FWU_OK;
    sim->begin_calls = 0u;
    sim->update_calls = 0u;
    sim->finish_calls = 0u;
    sim->verify_calls = 0u;
    sim->last_verify_message_length = 0u;
    return fwu_crypto_init(&sim->crypto, sim, &sim->callback);
}
