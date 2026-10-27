/**
 * @file    test_world.h
 * @brief   TEST DESTEĞİ (yalnızca test): gerçek oturum + gerçek aktivasyon + gerçek doğrulama + sahte flash/kriptografi
 *          üzerinde, cihaz tarafının RX/TX donanımı hariç tamamı hazır bir "dünya". test_link.c ve client/test_client.c kullanır.
 *          Bir test dosyası bunu TEK KEZ include eder (durum static değişkenlerdedir).
 *
 * Düzen: 19 sayfa x 256, granül 8:  0..7 uygulama | 8 manifest | 9..15 imaj (7 sayfa) | 16,17 aktivasyon journal | 18 geçici.
 */

#ifndef TEST_WORLD_H_
#define TEST_WORLD_H_

#include <stdbool.h>
#include <stdio.h>
#include "fwu_session.h"
#include "fwu_link.h"
#include "fwu_parser.h"
#include "fwu_ringbuf.h"
#include "fwu_bytes.h"
#include "fwu_crypto_sim.h"
#include "fwu_flash_sim.h"

FWU_FLASH_SIM_DEFINE(tw, 19, 256, 8);
FWU_RINGBUF_DEFINE(tw_rb, 4096);

#define TW_PAGE             256u
#define TW_MANIFEST_OFFSET  2048u
#define TW_IMAGE_OFFSET     2304u
#define TW_IMAGE_MAX        1792u
#define TW_PRODUCT          12345u
#define TW_CHUNK            64u
#define TW_IMG_SIZE         700u
#define TW_TIMEOUT_MS       5000u

static tw_t                  tw_sim;
static fwu_crypto_sim_t      tw_cs;
static fwu_verify_config_t   tw_vc;
static fwu_activate_config_t tw_ac;
static fwu_activate_t        tw_act;
static fwu_session_config_t  tw_cfg;
static fwu_session_t         tw_session;
static fwu_byte_t            tw_work[TW_CHUNK];
static fwu_byte_t            tw_vwork[64];
static fwu_byte_t            tw_awork[64];
static fwu_byte_t            tw_pk[3][32];
static fwu_verify_key_t      tw_keys[2];
static fwu_byte_t            tw_manifest[138];                  /* host'un elindeki manifest */
static fwu_byte_t            tw_image[TW_IMAGE_MAX + 64u];      /* host'un elindeki imaj */

/* cihaz tarafı RX hattı */
static tw_rb_t               tw_rx;
static fwu_parser_t          tw_parser;
static fwu_link_t            tw_link;

typedef struct {
    uint32_t begin_calls;
    uint32_t check_calls;
} tw_auth_ctx_t;
static tw_auth_ctx_t         tw_auth_ctx;

static inline fwu_byte_t tw_img_byte(uint32_t i)
{
    return (fwu_byte_t)((i * 13u + 29u) & 0xFFu);
}

static inline fwu_byte_t tw_old_byte(uint32_t i)
{
    return (fwu_byte_t)((i * 7u + 3u) & 0xFFu);
}

static inline fwu_return_e tw_auth_begin(void *context, fwu_byte_t *challenge, uint32_t capacity, uint32_t *length)
{
    tw_auth_ctx_t *a = (tw_auth_ctx_t *)context;

    a->begin_calls++;
    if(capacity < 16u) {
        return FWU_ERR_INVALID_ARG;
    }
    for(uint32_t i = 0u; i < 16u; i++) {
        challenge[i] = (fwu_byte_t)((i + (a->begin_calls * 16u)) & 0xFFu);
    }
    *length = 16u;
    return FWU_OK;
}

static inline fwu_return_e tw_auth_check(void *context, const fwu_byte_t *challenge, uint32_t challenge_length,
                                         const fwu_byte_t *response, uint32_t response_length)
{
    tw_auth_ctx_t *a = (tw_auth_ctx_t *)context;

    a->check_calls++;
    if(response_length != challenge_length) {
        return FWU_ERR_BAD_SIGNATURE;
    }
    for(uint32_t i = 0u; i < challenge_length; i++) {
        if((response[i] & 0xFFu) != (fwu_byte_t)((challenge[i] ^ 0x5Au) & 0xFFu)) {
            return FWU_ERR_BAD_SIGNATURE;
        }
    }
    return FWU_OK;
}

static const fwu_session_auth_t tw_auth = { tw_auth_begin, tw_auth_check, &tw_auth_ctx };

/** Yetkilendirme yanıtı (test host'u için): meydan okuma XOR 0x5A. */
static inline void tw_auth_response(const fwu_byte_t *challenge, size_t length, fwu_byte_t *response)
{
    for(size_t i = 0u; i < length; i++) {
        response[i] = (fwu_byte_t)((challenge[i] ^ 0x5Au) & 0xFFu);
    }
}

static inline void tw_put_u32(fwu_byte_t *m, uint32_t offset, uint32_t value)
{
    m[offset] = (fwu_byte_t)((value >> 24) & 0xFFu);
    m[offset + 1u] = (fwu_byte_t)((value >> 16) & 0xFFu);
    m[offset + 2u] = (fwu_byte_t)((value >> 8) & 0xFFu);
    m[offset + 3u] = (fwu_byte_t)(value & 0xFFu);
}

/** Host'un paketini (imaj + imzali manifest) kurar. signer: 0 = cihazin guvendigi anahtar, 2 = saldirgan. */
static inline void tw_make_package(uint32_t size, uint32_t version, uint32_t signer)
{
    for(uint32_t i = 0u; i < (uint32_t)(sizeof(tw_image) / sizeof(tw_image[0])); i++) {
        tw_image[i] = tw_img_byte(i);
    }
    tw_manifest[0] = 0x46u; tw_manifest[1] = 0x57u; tw_manifest[2] = 0x4Du; tw_manifest[3] = 0x46u;
    tw_manifest[4] = 0x01u;
    tw_manifest[5] = 1u;                                    /* anahtar kimligi */
    tw_manifest[6] = 0x00u; tw_manifest[7] = 0x00u;
    tw_put_u32(tw_manifest, 8u, TW_PRODUCT);
    tw_put_u32(tw_manifest, 12u, version);
    tw_manifest[16] = 0u; tw_manifest[17] = 1u;             /* anahtar surumu 1 */
    tw_put_u32(tw_manifest, 18u, size);
    fwu_crypto_sim_sha256(tw_image, size, &tw_manifest[22]);
    for(uint32_t i = 0u; i < 20u; i++) {
        tw_manifest[54u + i] = (fwu_byte_t)(0x30u + i);
    }
    fwu_crypto_sim_sign(tw_pk[signer], tw_manifest, 74u, &tw_manifest[74]);
}

/** Temiz dunya: bos flash, eski uygulama, gecerli paket, aktivasyon bosta, oturum bosta, RX hatti bos. */
static inline void tw_world(bool with_auth, fwu_activate_mode_e mode)
{
    (void)tw_init(&tw_sim);
    for(uint32_t i = 0u; i < 32u; i++) {
        tw_pk[0][i] = (fwu_byte_t)(i + 1u);
        tw_pk[1][i] = (fwu_byte_t)(i + 101u);
        tw_pk[2][i] = (fwu_byte_t)(i + 201u);
        tw_keys[0].public_key[i] = tw_pk[0][i];
        tw_keys[1].public_key[i] = tw_pk[1][i];
    }
    tw_keys[0].id = 1u;
    tw_keys[1].id = 2u;
    (void)fwu_crypto_sim_init(&tw_cs);
    tw_auth_ctx.begin_calls = 0u;
    tw_auth_ctx.check_calls = 0u;
    for(uint32_t i = 0u; i < 8u * TW_PAGE; i++) {
        tw_sim.base.memory[i] = tw_old_byte(i);
    }

    tw_vc.crypto = &tw_cs.crypto;
    tw_vc.manifest_offset = TW_MANIFEST_OFFSET;
    tw_vc.image_offset = TW_IMAGE_OFFSET;
    tw_vc.max_image_size = TW_IMAGE_MAX;
    tw_vc.product_id = TW_PRODUCT;
    tw_vc.min_image_version = 25u;
    tw_vc.min_key_version = 1u;
    tw_vc.keys = tw_keys;
    tw_vc.key_count = 2u;
    tw_vc.require_signature = true;
    tw_vc.work = tw_vwork;
    tw_vc.work_length = 64u;

    tw_ac.flash = &tw_sim.base.flash;
    tw_ac.mode = mode;
    tw_ac.app_offset = 0u;
    tw_ac.staging_offset = TW_IMAGE_OFFSET;
    tw_ac.region_length = TW_IMAGE_MAX;
    tw_ac.journal_a_offset = 16u * TW_PAGE;
    tw_ac.journal_b_offset = 17u * TW_PAGE;
    tw_ac.journal_length = TW_PAGE;
    tw_ac.scratch_offset = 18u * TW_PAGE;
    tw_ac.scratch_pages = 1u;
    tw_ac.work = tw_awork;
    tw_ac.work_length = 64u;
    tw_ac.max_trial_boots = 2u;
    tw_ac.require_verify = true;
    tw_ac.verify = fwu_verify_activation_hook;
    tw_ac.verify_context = &tw_vc;
    tw_ac.on_confirmed = NULL;
    tw_ac.on_confirmed_context = NULL;
    (void)fwu_activate_init(&tw_act, &tw_ac);

    tw_cfg.flash = &tw_sim.base.flash;
    tw_cfg.activate = &tw_act;
    tw_cfg.verify = &tw_vc;
    tw_cfg.auth = with_auth ? &tw_auth : NULL;
    tw_cfg.require_auth = with_auth;
    tw_cfg.running_image_version = 20u;
    tw_cfg.chunk_size = TW_CHUNK;
    tw_cfg.idle_timeout_ms = TW_TIMEOUT_MS;
    tw_cfg.auth_max_failures = 3u;
    tw_cfg.auth_lockout_ms = 5000u;
    tw_cfg.work = tw_work;
    tw_cfg.work_length = TW_CHUNK;
    (void)fwu_session_init(&tw_session, &tw_cfg);

    (void)tw_rb_init(&tw_rx);
    (void)fwu_parser_init(&tw_parser, &tw_rx.base);
    tw_make_package(TW_IMG_SIZE, 25u, 0u);
}

/** Cihaz tarafi link'i verilen tasiyiciya baglar. */
static inline fwu_return_e tw_link_setup(const fwu_transport_t *transport)
{
    fwu_link_config_t lc;

    lc.session = &tw_session;
    lc.parser = &tw_parser;
    lc.transport = transport;
    return fwu_link_init(&tw_link, &lc);
}

static inline int tw_app_is_old(void)
{
    for(uint32_t i = 0u; i < 8u * TW_PAGE; i++) {
        if((tw_sim.base.memory[i] & 0xFFu) != tw_old_byte(i)) {
            return 0;
        }
    }
    return 1;
}

/** Yeni uygulama: ilk 700 = imaj, 700..767 = 0xFF (sayfa dolgusu), gerisi eski. */
static inline int tw_app_is_new(void)
{
    for(uint32_t i = 0u; i < TW_IMG_SIZE; i++) {
        if((tw_sim.base.memory[i] & 0xFFu) != tw_img_byte(i)) {
            return 0;
        }
    }
    for(uint32_t i = TW_IMG_SIZE; i < 3u * TW_PAGE; i++) {
        if((tw_sim.base.memory[i] & 0xFFu) != 0xFFu) {
            return 0;
        }
    }
    for(uint32_t i = 3u * TW_PAGE; i < 8u * TW_PAGE; i++) {
        if((tw_sim.base.memory[i] & 0xFFu) != tw_old_byte(i)) {
            return 0;
        }
    }
    return 1;
}

/** Bootloader: guc geri gelir, aktivasyon yeniden acilir ve bekleyen isi calistirir. */
static inline fwu_return_e tw_bootloader_run(fwu_activate_outcome_e *outcome)
{
    fwu_flash_sim_reset(&tw_sim.base);
    if(fwu_activate_init(&tw_act, &tw_ac) != FWU_OK) {
        return FWU_ERR_BAD_STATE;
    }
    return fwu_activate_run(&tw_act, outcome);
}

#endif /* TEST_WORLD_H_ */
