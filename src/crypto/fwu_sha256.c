/**
 * @file    fwu_sha256.c
 * @brief   SHA-256 implementation (FIPS 180-4). See fwu_sha256.h for the interface and limits.
 */

#include <stddef.h>
#include "fwu_sha256.h"

/* FIPS 180-4, section 4.2.2: first 32 bits of the fractional parts of the cube roots of the first 64 primes. */
static const uint32_t SHA256_K[64] = {
    0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u, 0x3956C25Bu, 0x59F111F1u, 0x923F82A4u, 0xAB1C5ED5u,
    0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u, 0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u, 0xC19BF174u,
    0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu, 0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu,
    0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u, 0xC6E00BF3u, 0xD5A79147u, 0x06CA6351u, 0x14292967u,
    0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu, 0x53380D13u, 0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u,
    0xA2BFE8A1u, 0xA81A664Bu, 0xC24B8B70u, 0xC76C51A3u, 0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u,
    0x19A4C116u, 0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u, 0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu, 0x682E6FF3u,
    0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u, 0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u
};

/* FIPS 180-4, section 5.3.3: first 32 bits of the fractional parts of the square roots of the first 8 primes. */
static const uint32_t SHA256_H0[8] = {
    0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au, 0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u
};

static uint32_t sha_rotr(uint32_t value, uint32_t count)
{
    return (value >> count) | (value << (32u - count));
}

/** Processes a single 64-byte block. */
static void sha_compress(fwu_sha256_t *s, const fwu_byte_t *block)
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
        w[i] = ((uint32_t)(block[4u * i] & FWU_BYTE_MASK) << 24) | ((uint32_t)(block[(4u * i) + 1u] & FWU_BYTE_MASK) << 16) |
               ((uint32_t)(block[(4u * i) + 2u] & FWU_BYTE_MASK) << 8) | (uint32_t)(block[(4u * i) + 3u] & FWU_BYTE_MASK);
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
        const uint32_t t1 = h + big_s1 + ch + SHA256_K[i] + w[i];
        const uint32_t big_s0 = sha_rotr(a, 2u) ^ sha_rotr(a, 13u) ^ sha_rotr(a, 22u);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = big_s0 + maj;

        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

void fwu_sha256_init(fwu_sha256_t *s)
{
    for(uint32_t i = 0u; i < 8u; i++) {
        s->h[i] = SHA256_H0[i];
    }
    s->bits_hi = 0u;
    s->bits_lo = 0u;
    s->block_length = 0u;
}

void fwu_sha256_update(fwu_sha256_t *s, const fwu_byte_t *data, uint32_t length)
{
    for(uint32_t i = 0u; i < length; i++) {
        uint32_t previous;

        s->block[s->block_length] = (fwu_byte_t)(data[i] & FWU_BYTE_MASK);
        s->block_length++;
        previous = s->bits_lo;
        s->bits_lo += 8u;
        if(s->bits_lo < previous) {
            s->bits_hi++;                           /* low word overflowed */
        }
        if(s->block_length == FWU_SHA256_BLOCK_SIZE) {
            sha_compress(s, s->block);
            s->block_length = 0u;
        }
    }
}

void fwu_sha256_final(fwu_sha256_t *s, fwu_byte_t *digest)
{
    const uint32_t bits_hi = s->bits_hi;
    const uint32_t bits_lo = s->bits_lo;

    /* Padding: 0x80, then zeros until the length (8 bytes) fits at the end of the block. The length field was saved BEFORE padding. */
    s->block[s->block_length] = (fwu_byte_t)0x80u;
    s->block_length++;
    if(s->block_length > 56u) {
        while(s->block_length < FWU_SHA256_BLOCK_SIZE) {
            s->block[s->block_length] = 0u;
            s->block_length++;
        }
        sha_compress(s, s->block);
        s->block_length = 0u;
    }
    while(s->block_length < 56u) {
        s->block[s->block_length] = 0u;
        s->block_length++;
    }
    for(uint32_t i = 0u; i < 4u; i++) {
        s->block[56u + i] = (fwu_byte_t)((bits_hi >> (24u - (8u * i))) & 0xFFu);
        s->block[60u + i] = (fwu_byte_t)((bits_lo >> (24u - (8u * i))) & 0xFFu);
    }
    sha_compress(s, s->block);

    for(uint32_t i = 0u; i < 8u; i++) {
        digest[4u * i] = (fwu_byte_t)((s->h[i] >> 24) & 0xFFu);
        digest[(4u * i) + 1u] = (fwu_byte_t)((s->h[i] >> 16) & 0xFFu);
        digest[(4u * i) + 2u] = (fwu_byte_t)((s->h[i] >> 8) & 0xFFu);
        digest[(4u * i) + 3u] = (fwu_byte_t)(s->h[i] & 0xFFu);
    }
}
