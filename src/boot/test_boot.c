/**
 * @file    test_boot.c
 * @brief   fwu_boot (bootloader çekirdeği) kendi kendini kontrol eden testi. Sahte flash + sahte kriptografi üzerinde "sahte MCU":
 *          her açılış = güç geri gelir (reset) + fwu_boot_start. Uygulama rolü: fwu_boot_init + commit / fwu_boot_app_finish_trial.
 *          Değişmezler: (1) uygulamaya YALNIZCA tam eski ya da tam yeni imajla atlanır; (2) başarısızlıkta ASLA atlanmaz;
 *          (3) sahte imzalı / bozuk / eski sürümlü paket asla uygulamaya girmez; (4) sayaçlar yalnızca onaydan sonra ve
 *          yalnızca ikisi birlikte (25, 1) yükselir; (5) her flash işleminde güç kesintisi sonrası kurtarma.
 *
 * Düzen (22 sayfa x 256, granül 8): 0 bootloader | 1..8 uygulama | 9 manifest | 10..16 imaj (7 sayfa) | 17,18 aktivasyon günlüğü |
 *   19 geçici | 20,21 sürüm sayaçları. (Uygulama sıfırdan başlamaz: ofset bağlama hataları görünür; bootloader sayfası hiç değişmemeli.) Paket: 700 baytlık imaj (3 sayfa), sürüm 25, anahtar sürümü 1, ürün 12345.
 *
 * Derleme (boot klasöründen):
 *   gcc -Wall -Wextra -Wpedantic -Wconversion -std=c99 -I. -I.. -I../error -I../bytes -I../crc -I../ring_buffer -I../frame -I../flash -I../journal -I../activation -I../manifest -I../crypto -I../verify -I../counters fwu_boot.c ../counters/fwu_counters.c ../verify/fwu_verify.c ../manifest/fwu_manifest.c ../crypto/fwu_crypto.c ../crypto/fwu_crypto_sim.c ../activation/fwu_activate.c ../journal/fwu_journal_ab.c ../journal/fwu_journal.c ../flash/fwu_flash.c ../flash/fwu_flash_sim.c test_boot.c -o test_boot && ./test_boot
 */

#include <stdio.h>
#include "fwu_boot.h"
#include "fwu_bytes.h"
#include "fwu_crc.h"
#include "fwu_crypto_sim.h"
#include "fwu_flash_sim.h"

static int g_failures = 0;

static void check(const char *name, int condition)
{
    printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    if(!condition) {
        g_failures++;
    }
}

static void expect(const char *name, fwu_return_e got, fwu_return_e want)
{
    if(got == want) {
        printf("[PASS] %s\n", name);
    } else {
        printf("[FAIL] %s  (got %d, expected %d)\n", name, (int)got, (int)want);
        g_failures++;
    }
}

/* ------------------------------------------------------------------ dünya */

FWU_FLASH_SIM_DEFINE(tb, 22, 256, 8);

#define PAGE            256u
#define APP_OFFSET      (1u * PAGE)
#define APP_BYTES       (8u * PAGE)
#define MANIFEST_OFFSET (9u * PAGE)
#define IMAGE_OFFSET    (10u * PAGE)
#define IMAGE_MAX       (7u * PAGE)
#define COUNTER_A       (20u * PAGE)
#define COUNTER_B       (21u * PAGE)
#define FLASH_BYTES     (22u * PAGE)
#define PRODUCT         12345u
#define IMG_SIZE        700u
#define IMG_PAGES       3u

static tb_t                g_sim;
static fwu_crypto_sim_t    g_cs;
static fwu_byte_t          g_pk[3][32];
static fwu_verify_key_t    g_keys[2];
static fwu_byte_t          g_vwork[64];
static fwu_byte_t          g_awork[64];
static fwu_boot_config_t   g_cfg;
static fwu_boot_t          g_boot;
static fwu_byte_t          g_manifest[138];
static fwu_byte_t          g_image[IMAGE_MAX];

typedef struct {
    uint32_t valid_calls;
    uint32_t wd_calls;
    bool     app_ok;
    uint32_t last_order;        /* app_valid (1) ve watchdog (2) sırası için */
    uint32_t valid_order;
    uint32_t wd_order;
} hook_ctx_t;
static hook_ctx_t g_hook;

static bool hook_app_valid(void *context)
{
    hook_ctx_t *h = (hook_ctx_t *)context;
    h->valid_calls++;
    h->last_order++;
    h->valid_order = h->last_order;
    return h->app_ok;
}

static void hook_watchdog(void *context)
{
    hook_ctx_t *h = (hook_ctx_t *)context;
    h->wd_calls++;
    h->last_order++;
    h->wd_order = h->last_order;
}

static fwu_byte_t img_byte(uint32_t i)
{
    return (fwu_byte_t)((i * 13u + 29u) & 0xFFu);
}

static fwu_byte_t old_byte(uint32_t i)
{
    return (fwu_byte_t)((i * 7u + 3u) & 0xFFu);
}

static fwu_byte_t boot_byte(uint32_t i)
{
    return (fwu_byte_t)((i * 11u + 101u) & 0xFFu);
}

static void put_u32(fwu_byte_t *m, uint32_t offset, uint32_t value)
{
    fwu_write_u32_big_endian(&m[offset], value);
}

typedef struct {
    uint32_t version;
    uint32_t signer;        /* 0 = cihazın güvendiği anahtar, 2 = saldırgan */
    uint32_t product;
    uint32_t key_id;
    uint32_t key_version;
    uint32_t size;
    int32_t  flip_index;    /* host imajında bu bayt manifest'ten SONRA bozulur (-1 yok) */
} pkg_t;

static pkg_t default_pkg(void)
{
    pkg_t p;
    p.version = 25u; p.signer = 0u; p.product = PRODUCT; p.key_id = 1u; p.key_version = 1u; p.size = IMG_SIZE; p.flip_index = -1;
    return p;
}

static void build_package(const pkg_t *p)
{
    for(uint32_t i = 0u; i < IMAGE_MAX; i++) {
        g_image[i] = img_byte(i);
    }
    g_manifest[0] = 0x46u; g_manifest[1] = 0x57u; g_manifest[2] = 0x4Du; g_manifest[3] = 0x46u;
    g_manifest[4] = 0x01u;
    g_manifest[5] = (fwu_byte_t)p->key_id;
    g_manifest[6] = 0x00u; g_manifest[7] = 0x00u;
    put_u32(g_manifest, 8u, p->product);
    put_u32(g_manifest, 12u, p->version);
    g_manifest[16] = (fwu_byte_t)((p->key_version >> 8) & 0xFFu);
    g_manifest[17] = (fwu_byte_t)(p->key_version & 0xFFu);
    put_u32(g_manifest, 18u, p->size);
    fwu_crypto_sim_sha256(g_image, p->size, &g_manifest[22]);
    for(uint32_t i = 0u; i < 20u; i++) {
        g_manifest[54u + i] = (fwu_byte_t)(0x30u + i);
    }
    fwu_crypto_sim_sign(g_pk[p->signer], g_manifest, 74u, &g_manifest[74]);
    if(p->flip_index >= 0) {
        g_image[p->flip_index] = (fwu_byte_t)(g_image[p->flip_index] ^ 0x08u);
    }
}

/** Uygulamanın indirmeyi bitirdiğini taklit eder: staging silinir, manifest ve imaj doğrudan flash belleğine yazılır. */
static void stage_package(const pkg_t *p)
{
    build_package(p);
    for(uint32_t i = MANIFEST_OFFSET; i < MANIFEST_OFFSET + 8u * PAGE; i++) {
        g_sim.base.memory[i] = 0xFFu;
    }
    for(uint32_t i = 0u; i < 138u; i++) {
        g_sim.base.memory[MANIFEST_OFFSET + i] = g_manifest[i];
    }
    for(uint32_t i = 0u; i < p->size; i++) {
        g_sim.base.memory[IMAGE_OFFSET + i] = g_image[i];
    }
}

static fwu_boot_layout_t layout(void)
{
    fwu_boot_layout_t l;
    l.app_offset = APP_OFFSET;
    l.staging_manifest_offset = MANIFEST_OFFSET;
    l.staging_image_offset = IMAGE_OFFSET;
    l.image_region_length = IMAGE_MAX;
    l.journal_a_offset = 17u * PAGE;
    l.journal_b_offset = 18u * PAGE;
    l.journal_length = PAGE;
    l.scratch_offset = 19u * PAGE;
    l.scratch_pages = 1u;
    l.counter_a_offset = COUNTER_A;
    l.counter_b_offset = COUNTER_B;
    l.counter_length = PAGE;
    return l;
}

static void make_config(fwu_boot_config_t *c, fwu_boot_verify_e verify, fwu_activate_mode_e mode, bool anti_rollback)
{
    c->policy.verify = verify;
    c->policy.mode = mode;
    c->policy.anti_rollback = anti_rollback;
    c->layout = layout();
    c->flash = &g_sim.base.flash;
    c->crypto = &g_cs.crypto;
    c->keys = g_keys;
    c->key_count = 2u;
    c->product_id = PRODUCT;
    c->max_trial_boots = 2u;
    c->verify_work = g_vwork;
    c->verify_work_length = 64u;
    c->activate_work = g_awork;
    c->activate_work_length = 64u;
    c->app_valid = hook_app_valid;
    c->watchdog_arm = hook_watchdog;
    c->context = &g_hook;
}

/** Temiz dünya: boş flash, eski uygulama, kripto, anahtarlar, kancalar; paket HAZIRLANMAZ. */
static void world(fwu_boot_verify_e verify, fwu_activate_mode_e mode, bool anti_rollback)
{
    (void)tb_init(&g_sim);
    for(uint32_t i = 0u; i < 32u; i++) {
        g_pk[0][i] = (fwu_byte_t)(i + 1u);
        g_pk[1][i] = (fwu_byte_t)(i + 101u);
        g_pk[2][i] = (fwu_byte_t)(i + 201u);
        g_keys[0].public_key[i] = g_pk[0][i];
        g_keys[1].public_key[i] = g_pk[1][i];
    }
    g_keys[0].id = 1u;
    g_keys[1].id = 2u;
    (void)fwu_crypto_sim_init(&g_cs);
    for(uint32_t i = 0u; i < APP_BYTES; i++) {
        g_sim.base.memory[APP_OFFSET + i] = old_byte(i);
    }
    for(uint32_t i = 0u; i < PAGE; i++) {
        g_sim.base.memory[i] = boot_byte(i);
    }
    g_hook.valid_calls = 0u; g_hook.wd_calls = 0u; g_hook.app_ok = true;
    g_hook.last_order = 0u; g_hook.valid_order = 0u; g_hook.wd_order = 0u;
    make_config(&g_cfg, verify, mode, anti_rollback);
}

static int app_is_old(void)
{
    for(uint32_t i = 0u; i < APP_BYTES; i++) {
        if((g_sim.base.memory[APP_OFFSET + i] & 0xFFu) != old_byte(i)) {
            return 0;
        }
    }
    return 1;
}

/** Bootloader sayfasi HICBIR durumda degismemeli (aktivasyon yalnizca uygulama, staging, gunluk, gecici, sayac sayfalarina yazar). */
static int boot_page_intact(void)
{
    for(uint32_t i = 0u; i < PAGE; i++) {
        if((g_sim.base.memory[i] & 0xFFu) != boot_byte(i)) {
            return 0;
        }
    }
    return 1;
}

/** Yeni uygulama: ilk 700 = imaj, 700..767 = 0xFF (sayfa dolgusu), gerisi eski. */
static int app_is_new(void)
{
    for(uint32_t i = 0u; i < IMG_SIZE; i++) {
        if((g_sim.base.memory[APP_OFFSET + i] & 0xFFu) != img_byte(i)) {
            return 0;
        }
    }
    for(uint32_t i = IMG_SIZE; i < IMG_PAGES * PAGE; i++) {
        if((g_sim.base.memory[APP_OFFSET + i] & 0xFFu) != 0xFFu) {
            return 0;
        }
    }
    for(uint32_t i = IMG_PAGES * PAGE; i < APP_BYTES; i++) {
        if((g_sim.base.memory[APP_OFFSET + i] & 0xFFu) != old_byte(i)) {
            return 0;
        }
    }
    return 1;
}

static int counter_pages_untouched(void)
{
    for(uint32_t i = COUNTER_A; i < COUNTER_B + PAGE; i++) {
        if((g_sim.base.memory[i] & 0xFFu) != 0xFFu) {
            return 0;
        }
    }
    return 1;
}

static fwu_boot_decision_e boot_cycle(fwu_boot_result_t *res)
{
    fwu_flash_sim_reset(&g_sim.base);
    return fwu_boot_start(&g_boot, &g_cfg, res);
}

static fwu_boot_decision_e boot_cycle_cut(fwu_boot_result_t *res, uint32_t cut_after)
{
    fwu_flash_sim_reset(&g_sim.base);
    fwu_flash_sim_cut_power_after(&g_sim.base, cut_after);
    return fwu_boot_start(&g_boot, &g_cfg, res);
}

/** Uygulama rolü: yapılandırmayı kur ve staging'deki paketin aktivasyonunu commit et. */
static fwu_return_e app_commit(uint32_t pages)
{
    fwu_return_e status;
    fwu_flash_sim_reset(&g_sim.base);
    status = fwu_boot_init(&g_boot, &g_cfg);
    if(status != FWU_OK) {
        return status;
    }
    return fwu_activate_commit(fwu_boot_activate(&g_boot), pages);
}

static void counters_now(uint32_t *image_version, uint16_t *key_version)
{
    *image_version = 0xDEADu;
    *key_version = 0xBEEFu;
    (void)fwu_counters_get(&g_boot.counters, image_version, key_version);
}

/* ------------------------------------------------------------------ 1. doğrulama ve bağlama */

/** Saf POLITIKA ihlali: init INVALID_ARG ve start KAL + neden CONFIG (aşağı akıştaki bir kontrol maskeleyemez). */
static void reject(const char *name, const fwu_boot_config_t *c)
{
    fwu_boot_t b;
    fwu_boot_result_t r;
    const int ok = (fwu_boot_init(&b, c) == FWU_ERR_INVALID_ARG) && (fwu_boot_start(&b, c, &r) == FWU_BOOT_STAY) &&
                   (r.reason == FWU_BOOT_REASON_CONFIG) && (r.status == FWU_ERR_INVALID_ARG) && (r.decision == FWU_BOOT_STAY);
    check(name, ok);
}

static void test_validation(void)
{
    fwu_boot_config_t c;
    fwu_boot_t b;
    fwu_boot_result_t r;

    printf("\n--- 1. yapilandirma ve politika kurallari ---\n");
    world(FWU_BOOT_VERIFY_SIGNED, FWU_ACTIVATE_MODE_SWAP, true);
    expect("gecerli (SIGNED + SWAP + anti-rollback)", fwu_boot_init(&b, &g_cfg), FWU_OK);
    expect("boot NULL", fwu_boot_init(NULL, &g_cfg), FWU_ERR_INVALID_ARG);
    expect("config NULL", fwu_boot_init(&b, NULL), FWU_ERR_INVALID_ARG);
    c = g_cfg; c.flash = NULL;
    reject("flash NULL", &c);
    c = g_cfg; c.policy.anti_rollback = false; c.policy.verify = (fwu_boot_verify_e)3;
    reject("verify degeri gecersiz (3)", &c);
    c = g_cfg; c.policy.anti_rollback = false; c.policy.mode = (fwu_activate_mode_e)2;
    reject("mod degeri gecersiz (2)", &c);
    c = g_cfg; c.activate_work = NULL;
    reject("aktivasyon tamponu NULL", &c);
    c = g_cfg; c.activate_work_length = 0u;
    reject("aktivasyon tamponu uzunlugu 0", &c);
    c = g_cfg; c.crypto = NULL;
    reject("SIGNED, crypto NULL", &c);
    c = g_cfg; c.verify_work = NULL;
    reject("SIGNED, dogrulama tamponu NULL", &c);
    c = g_cfg; c.verify_work_length = 0u;
    reject("SIGNED, dogrulama tamponu uzunlugu 0", &c);
    c = g_cfg; c.keys = NULL;
    reject("SIGNED, anahtar dizisi NULL", &c);
    c = g_cfg; c.key_count = 0u;
    reject("SIGNED, anahtar sayisi 0", &c);
    c = g_cfg; c.key_count = 1u;
    expect("SIGNED, 1 anahtar (sinir)", fwu_boot_init(&b, &c), FWU_OK);
    c = g_cfg; c.policy.verify = FWU_BOOT_VERIFY_HASH; c.policy.anti_rollback = false; c.keys = NULL; c.key_count = 0u;
    expect("HASH, anahtar gerekmez", fwu_boot_init(&b, &c), FWU_OK);
    c.crypto = NULL;
    reject("HASH, crypto NULL", &c);
    c = g_cfg; c.policy.verify = FWU_BOOT_VERIFY_HASH; c.policy.anti_rollback = false; c.verify_work = NULL;
    reject("HASH, dogrulama tamponu NULL", &c);
    c = g_cfg; c.policy.verify = FWU_BOOT_VERIFY_NONE; c.policy.anti_rollback = false; c.crypto = NULL; c.verify_work = NULL; c.verify_work_length = 0u; c.keys = NULL; c.key_count = 0u;
    expect("NONE (yalniz gelistirme): crypto, tampon, anahtar gerekmez", fwu_boot_init(&b, &c), FWU_OK);

    /* anti-rollback kurallari */
    c = g_cfg; c.policy.mode = FWU_ACTIVATE_MODE_COPY;
    reject("anti-rollback + COPY: reddedilir (onay asamasi yok, sayac hic yukselmez)", &c);
    c = g_cfg; c.policy.verify = FWU_BOOT_VERIFY_HASH;
    reject("anti-rollback + HASH: reddedilir (imzasiz manifest'ten sayac yukseltilemez)", &c);
    c = g_cfg; c.policy.verify = FWU_BOOT_VERIFY_NONE;
    reject("anti-rollback + NONE: reddedilir", &c);

    /* aktivasyon kurulum hatalari yayilir */
    c = g_cfg; c.max_trial_boots = 0u;
    expect("SWAP, max_trial_boots 0: aktivasyondan INVALID_ARG", fwu_boot_init(&b, &c), FWU_ERR_INVALID_ARG);
    c = g_cfg; c.max_trial_boots = 256u;
    expect("SWAP, max_trial_boots 256", fwu_boot_init(&b, &c), FWU_ERR_INVALID_ARG);
    c = g_cfg; c.max_trial_boots = 255u;
    expect("SWAP, max_trial_boots 255 (sinir)", fwu_boot_init(&b, &c), FWU_OK);
    c = g_cfg; c.max_trial_boots = 1u;
    expect("SWAP, max_trial_boots 1 (sinir)", fwu_boot_init(&b, &c), FWU_OK);
    c = g_cfg; c.layout.scratch_pages = 0u;
    expect("SWAP, gecici sayfa 0", fwu_boot_init(&b, &c), FWU_ERR_INVALID_ARG);
    c = g_cfg; c.layout.counter_a_offset = 22u * PAGE;
    expect("anti-rollback, sayac bolgesi flash disi: sayac kurulumundan OUT_OF_RANGE", fwu_boot_init(&b, &c), FWU_ERR_OUT_OF_RANGE);
    c = g_cfg; c.policy.anti_rollback = false; c.policy.verify = FWU_BOOT_VERIFY_SIGNED; c.layout.counter_a_offset = 22u * PAGE;
    expect("anti-rollback YOK: sayac bolgesi hic okunmaz (flash disi olsa da gecerli)", fwu_boot_init(&b, &c), FWU_OK);

    /* basarisiz init nesneyi kullanilamaz yapar */
    expect("once basarili", fwu_boot_init(&g_boot, &g_cfg), FWU_OK);
    check("... activate ve verify erisimcileri dolu", fwu_boot_activate(&g_boot) != NULL && fwu_boot_verify_config(&g_boot) != NULL);
    c = g_cfg; c.key_count = 0u;
    expect("sonra basarisiz init", fwu_boot_init(&g_boot, &c), FWU_ERR_INVALID_ARG);
    check("... nesne KULLANILAMAZ: erisimciler NULL", fwu_boot_activate(&g_boot) == NULL && fwu_boot_verify_config(&g_boot) == NULL);
    expect("... finish_trial INVALID_ARG", fwu_boot_app_finish_trial(&g_boot, true), FWU_ERR_INVALID_ARG);
    check("erisimciler NULL'a dayanikli", fwu_boot_activate(NULL) == NULL && fwu_boot_verify_config(NULL) == NULL);
    expect("finish_trial: boot NULL", fwu_boot_app_finish_trial(NULL, true), FWU_ERR_INVALID_ARG);

    /* baglama */
    world(FWU_BOOT_VERIFY_SIGNED, FWU_ACTIVATE_MODE_SWAP, true);
    (void)fwu_boot_init(&g_boot, &g_cfg);
    {
        const fwu_verify_config_t *v = fwu_boot_verify_config(&g_boot);
        check("dogrulama: manifest 2048, imaj 2304, kapasite 1792, urun 12345", v->manifest_offset == MANIFEST_OFFSET && v->image_offset == IMAGE_OFFSET &&
              v->max_image_size == IMAGE_MAX && v->product_id == PRODUCT);
        check("dogrulama: imza ZORUNLU (SIGNED), 2 anahtar, min surumler (0, 0) (taze sayac)", v->require_signature && v->key_count == 2u && v->keys == g_keys &&
              v->min_image_version == 0u && v->min_key_version == 0u && v->crypto == &g_cs.crypto);
        check("aktivasyon: SWAP, require_verify, dogrulama kancasi, ONAY KANCASI (sayac) bagli", g_boot.activate_config.mode == FWU_ACTIVATE_MODE_SWAP &&
              g_boot.activate_config.require_verify && g_boot.activate_config.verify == fwu_verify_activation_hook &&
              g_boot.activate_config.verify_context == (void *)&g_boot.verify && g_boot.activate_config.on_confirmed == fwu_counters_on_confirmed &&
              g_boot.activate_config.on_confirmed_context == (void *)&g_boot.confirm);
        check("aktivasyon: staging ofseti = IMAJ ofseti (manifest sayfasi degil), bolge 1792, gecici sayfa 1, max deneme 2",
              g_boot.activate_config.staging_offset == IMAGE_OFFSET && g_boot.activate_config.region_length == IMAGE_MAX &&
              g_boot.activate_config.scratch_pages == 1u && g_boot.activate_config.max_trial_boots == 2u);
    }
    world(FWU_BOOT_VERIFY_SIGNED, FWU_ACTIVATE_MODE_SWAP, false);
    (void)fwu_boot_init(&g_boot, &g_cfg);
    check("anti-rollback YOK: onay kancasi YOK, min surumler 0", g_boot.activate_config.on_confirmed == NULL && g_boot.activate_config.on_confirmed_context == NULL &&
          fwu_boot_verify_config(&g_boot)->min_image_version == 0u && fwu_boot_verify_config(&g_boot)->min_key_version == 0u);
    check("yapilandirma KOPYASI saklandi (urun, deneme sayisi, anahtar sayisi, duzen)", g_boot.config.product_id == PRODUCT && g_boot.config.max_trial_boots == 2u &&
          g_boot.config.key_count == 2u && g_boot.config.layout.app_offset == APP_OFFSET && g_boot.config.flash == &g_sim.base.flash);
    check("aktivasyon: UYGULAMA ofseti duzenden (bootloader sayfasi degil), journal ve gecici sayfalar duzenden",
          g_boot.activate_config.app_offset == APP_OFFSET && g_boot.activate_config.journal_a_offset == 17u * PAGE && g_boot.activate_config.journal_b_offset == 18u * PAGE &&
          g_boot.activate_config.journal_length == PAGE && g_boot.activate_config.scratch_offset == 19u * PAGE && g_boot.activate_config.work == g_awork &&
          g_boot.activate_config.work_length == 64u && g_boot.activate_config.flash == &g_sim.base.flash);
    world(FWU_BOOT_VERIFY_HASH, FWU_ACTIVATE_MODE_COPY, false);
    (void)fwu_boot_init(&g_boot, &g_cfg);
    check("HASH: dogrulama kancasi var, imza ZORUNLU DEGIL, COPY", g_boot.activate_config.require_verify && g_boot.activate_config.verify == fwu_verify_activation_hook &&
          !fwu_boot_verify_config(&g_boot)->require_signature && g_boot.activate_config.mode == FWU_ACTIVATE_MODE_COPY);
    world(FWU_BOOT_VERIFY_NONE, FWU_ACTIVATE_MODE_COPY, false);
    (void)fwu_boot_init(&g_boot, &g_cfg);
    check("NONE: dogrulama kancasi YOK, require_verify false", !g_boot.activate_config.require_verify && g_boot.activate_config.verify == NULL &&
          g_boot.activate_config.verify_context == NULL);

    /* fwu_boot_start gecersiz yapilandirmada: KAL */
    world(FWU_BOOT_VERIFY_SIGNED, FWU_ACTIVATE_MODE_SWAP, true);
    c = g_cfg; c.key_count = 0u;
    check("start, gecersiz yapilandirma: KAL, neden CONFIG, durum INVALID_ARG, sonuc NONE, deneme yok",
          fwu_boot_start(&b, &c, &r) == FWU_BOOT_STAY && r.decision == FWU_BOOT_STAY && r.reason == FWU_BOOT_REASON_CONFIG &&
          r.status == FWU_ERR_INVALID_ARG && r.outcome == FWU_ACT_OUTCOME_NONE && !r.trial);
    check("... bekcI ve app_valid CAGRILMADI", g_hook.wd_calls == 0u && g_hook.valid_calls == 0u);
    for(unsigned k = 0u; k < 2u; k++) {
        /* sonuc yapisi onceden FARKLI degerlerle kirletilir: hicbir alan eski cagriyla maskelenemez */
        r.decision = FWU_BOOT_JUMP_APP; r.reason = FWU_BOOT_REASON_NO_APP; r.status = FWU_ERR_BAD_BLOCK; r.outcome = FWU_ACT_OUTCOME_REJECTED;
        r.phase = FWU_ACT_PHASE_TRIAL; r.trial = true;
        check(k == 0u ? "start: boot NULL -> KAL; sonuc: karar KAL, neden CONFIG, durum INVALID_ARG, sonuc NONE, evre IDLE, deneme yok" :
                        "start: config NULL -> KAL; sonuc: karar KAL, neden CONFIG, durum INVALID_ARG, sonuc NONE, evre IDLE, deneme yok",
              ((k == 0u) ? fwu_boot_start(NULL, &g_cfg, &r) : fwu_boot_start(&b, NULL, &r)) == FWU_BOOT_STAY && r.decision == FWU_BOOT_STAY &&
              r.reason == FWU_BOOT_REASON_CONFIG && r.status == FWU_ERR_INVALID_ARG && r.outcome == FWU_ACT_OUTCOME_NONE &&
              r.phase == FWU_ACT_PHASE_IDLE && !r.trial);
    }
    check("start: result NULL kabul (KAL, cokme yok)", fwu_boot_start(&b, &c, NULL) == FWU_BOOT_STAY);
    check("start: result NULL ve gecerli yapilandirma -> ATLA", fwu_boot_start(&b, &g_cfg, NULL) == FWU_BOOT_JUMP_APP);
}

/* ------------------------------------------------------------------ 2. akislar */

static void test_swap_ar_flow(void)
{
    fwu_boot_result_t r;
    uint32_t iv;
    uint16_t kv;
    pkg_t p = default_pkg();

    printf("\n--- 2. SIGNED + SWAP + anti-rollback: guncelle, dene, onayla ---\n");
    world(FWU_BOOT_VERIFY_SIGNED, FWU_ACTIVATE_MODE_SWAP, true);
    check("bekleyen is yok: ATLA, sonuc NONE, evre IDLE, deneme yok, neden NONE, durum OK",
          boot_cycle(&r) == FWU_BOOT_JUMP_APP && r.decision == FWU_BOOT_JUMP_APP && r.reason == FWU_BOOT_REASON_NONE && r.status == FWU_OK &&
          r.outcome == FWU_ACT_OUTCOME_NONE && r.phase == FWU_ACT_PHASE_IDLE && !r.trial);
    check("... app_valid 1 kez cagrildi, bekci cagrilmadi, uygulama ESKI, sayaclar (0, 0)", g_hook.valid_calls == 1u && g_hook.wd_calls == 0u && app_is_old());
    counters_now(&iv, &kv);
    check("... sayac (0, 0) ve sayac sayfalari dokunulmamis", iv == 0u && kv == 0u && counter_pages_untouched());

    stage_package(&p);
    expect("uygulama: commit (3 sayfa)", app_commit(IMG_PAGES), FWU_OK);
    check("commit uygulamaya DOKUNMADI (commit en son)", app_is_old());
    check("1. acilis: ATLA, sonuc TRIAL, evre TRIAL, deneme true, durum OK",
          boot_cycle(&r) == FWU_BOOT_JUMP_APP && r.outcome == FWU_ACT_OUTCOME_TRIAL && r.phase == FWU_ACT_PHASE_TRIAL && r.trial && r.status == FWU_OK);
    check("... uygulama TAM YENI, bekci TAM 1 kez, app_valid BEKCIDEN ONCE (sira 1, sonra 2)", app_is_new() && g_hook.wd_calls == 1u &&
          g_hook.wd_order == g_hook.valid_order + 1u);
    counters_now(&iv, &kv);
    check("... onaydan ONCE sayac hala (0, 0)", iv == 0u && kv == 0u && counter_pages_untouched());

    expect("uygulama kendini denedi: finish_trial(saglikli)", fwu_boot_app_finish_trial(&g_boot, true), FWU_OK);
    counters_now(&iv, &kv);
    check("onay SONRASI sayac (25, 1): imaj ve anahtar surumu birlikte", iv == 25u && kv == 1u);
    check("2. acilis: ATLA, sonuc NONE, deneme yok, bekci cagrilmadi (hala 1)",
          boot_cycle(&r) == FWU_BOOT_JUMP_APP && r.outcome == FWU_ACT_OUTCOME_NONE && !r.trial && g_hook.wd_calls == 1u && app_is_new());
    counters_now(&iv, &kv);
    check("... sayac FLASH'tan yeniden okundu: (25, 1)", iv == 25u && kv == 1u);
    expect("finish_trial(saglikli) deneme yokken: idempotent OK", fwu_boot_app_finish_trial(&g_boot, true), FWU_OK);
    expect("finish_trial(SAGLIKSIZ) deneme yokken: hicbir sey yapmaz (geri donus istemez)", fwu_boot_app_finish_trial(&g_boot, false), FWU_OK);
    check("... sonraki acilis hala ATLA, TRIAL yok, uygulama yeni", boot_cycle(&r) == FWU_BOOT_JUMP_APP && r.outcome == FWU_ACT_OUTCOME_NONE && app_is_new());

    /* anti-rollback: eski surum reddedilir */
    p.version = 24u;
    stage_package(&p);
    expect("surum 24 paketi commit", app_commit(IMG_PAGES), FWU_OK);
    check("surum 24 (< sayac 25): ATLA ama sonuc REJECTED, uygulama DEGISMEDI (yeni)", boot_cycle(&r) == FWU_BOOT_JUMP_APP && r.outcome == FWU_ACT_OUTCOME_REJECTED && app_is_new() && !r.trial);
    check("... bekci cagrilmadi (hala 1), sayac hala (25, 1)", g_hook.wd_calls == 1u);
    counters_now(&iv, &kv);
    check("... sayac (25, 1)", iv == 25u && kv == 1u);

    /* esit surum kabul, sonra 26 */
    p.version = 25u;
    stage_package(&p);
    expect("surum 25 (esit) commit", app_commit(IMG_PAGES), FWU_OK);
    check("surum 25 (esit): KABUL (TRIAL)", boot_cycle(&r) == FWU_BOOT_JUMP_APP && r.outcome == FWU_ACT_OUTCOME_TRIAL && r.trial);
    expect("... onay", fwu_boot_app_finish_trial(&g_boot, true), FWU_OK);
    p.version = 26u;
    stage_package(&p);
    expect("surum 26 commit", app_commit(IMG_PAGES), FWU_OK);
    check("surum 26: KABUL (TRIAL)", boot_cycle(&r) == FWU_BOOT_JUMP_APP && r.outcome == FWU_ACT_OUTCOME_TRIAL);
    expect("... onay", fwu_boot_app_finish_trial(&g_boot, true), FWU_OK);
    counters_now(&iv, &kv);
    check("sayac (26, 1)", iv == 26u && kv == 1u);
    check("bekci her deneme onyuklemesinde 1 kez: toplam 3", g_hook.wd_calls == 3u);
    check("bootloader sayfasi hic degismedi", boot_page_intact());

    /* sayac sonraki acilista dogrulama yapilandirmasina KOPYALANIR (imaj ve anahtar surumu) */
    (void)boot_cycle(&r);
    check("sonraki acilis: dogrulama yapilandirmasinda min imaj surumu 26, min anahtar surumu 1 (sayactan)",
          fwu_boot_verify_config(&g_boot)->min_image_version == 26u && fwu_boot_verify_config(&g_boot)->min_key_version == 1u);
    p.version = 27u; p.key_version = 0u;
    stage_package(&p);
    expect("surum 27 ama ANAHTAR SURUMU 0 paketi commit", app_commit(IMG_PAGES), FWU_OK);
    check("anahtar surumu 0 (< min 1): REDDEDILDI (REJECTED), uygulama degismedi, bekci cagrilmadi (hala 3)",
          boot_cycle(&r) == FWU_BOOT_JUMP_APP && r.outcome == FWU_ACT_OUTCOME_REJECTED && app_is_new() && g_hook.wd_calls == 3u);
    p.key_version = 1u;
    stage_package(&p);
    (void)app_commit(IMG_PAGES);
    check("surum 27, anahtar surumu 1: KABUL", boot_cycle(&r) == FWU_BOOT_JUMP_APP && r.outcome == FWU_ACT_OUTCOME_TRIAL);
}

static void test_revert_flows(void)
{
    fwu_boot_result_t r;
    uint32_t iv;
    uint16_t kv;
    pkg_t p = default_pkg();

    printf("\n--- 3. geri donus: uygulama saglıksiz / deneme hakki biter ---\n");
    world(FWU_BOOT_VERIFY_SIGNED, FWU_ACTIVATE_MODE_SWAP, true);
    stage_package(&p);
    (void)app_commit(IMG_PAGES);
    (void)boot_cycle(&r);
    check("deneme basladi (yeni uygulama)", r.trial && app_is_new());
    expect("uygulama saglıksiz: finish_trial(false) geri donus ister", fwu_boot_app_finish_trial(&g_boot, false), FWU_OK);
    check("... istek anlik uygulamayi DEGISTIRMEZ (yeniden baslatinca olur)", app_is_new());
    expect("... ikinci cagri: REVERT kaydi yazildi (evre REVERTING), tekrar istek BAD_STATE", fwu_boot_app_finish_trial(&g_boot, false), FWU_ERR_BAD_STATE);
    check("acilis: ATLA, sonuc REVERTED, deneme yok, uygulama TAM ESKI", boot_cycle(&r) == FWU_BOOT_JUMP_APP && r.outcome == FWU_ACT_OUTCOME_REVERTED && !r.trial && app_is_old());
    check("... bekci yalniz ilk deneme icin (1), sayac (0, 0) ve sayac sayfalari dokunulmamis", g_hook.wd_calls == 1u && counter_pages_untouched());
    counters_now(&iv, &kv);
    check("... sayac (0, 0)", iv == 0u && kv == 0u);
    check("sonraki acilis: normal, ESKI uygulama", boot_cycle(&r) == FWU_BOOT_JUMP_APP && r.outcome == FWU_ACT_OUTCOME_NONE && app_is_old());

    /* deneme hakki biter: max_trial_boots 2 -> 1. ve 2. acilis TRIAL, 3. acilis geri doner */
    world(FWU_BOOT_VERIFY_SIGNED, FWU_ACTIVATE_MODE_SWAP, true);
    stage_package(&p);
    (void)app_commit(IMG_PAGES);
    check("1. acilis: TRIAL(1)", boot_cycle(&r) == FWU_BOOT_JUMP_APP && r.outcome == FWU_ACT_OUTCOME_TRIAL && r.trial && app_is_new());
    check("uygulama KILITLENDI/onaylamadi, 2. acilis: hala TRIAL (2. hak), bekci tekrar", boot_cycle(&r) == FWU_BOOT_JUMP_APP && r.outcome == FWU_ACT_OUTCOME_TRIAL && r.trial &&
          app_is_new() && g_hook.wd_calls == 2u);
    check("3. acilis: hak bitti -> geri donus: sonuc REVERTED, uygulama TAM ESKI, bekci cagrilmadi (hala 2)",
          boot_cycle(&r) == FWU_BOOT_JUMP_APP && r.outcome == FWU_ACT_OUTCOME_REVERTED && !r.trial && app_is_old() && g_hook.wd_calls == 2u);
    counters_now(&iv, &kv);
    check("onaylanmayan surum sayaci YUKSELTMEDI: (0, 0)", iv == 0u && kv == 0u && counter_pages_untouched());
}

static void test_copy_and_levels(void)
{
    fwu_boot_result_t r;
    pkg_t p = default_pkg();
    fwu_byte_t garbage[3u * PAGE];

    printf("\n--- 4. COPY ve dogrulama seviyeleri ---\n");
    world(FWU_BOOT_VERIFY_SIGNED, FWU_ACTIVATE_MODE_COPY, false);
    stage_package(&p);
    (void)app_commit(IMG_PAGES);
    check("COPY + SIGNED: ATLA, sonuc ACTIVATED, deneme YOK, bekci YOK, uygulama TAM YENI",
          boot_cycle(&r) == FWU_BOOT_JUMP_APP && r.outcome == FWU_ACT_OUTCOME_ACTIVATED && !r.trial && g_hook.wd_calls == 0u && app_is_new());
    check("... sayac sayfalari dokunulmadi (anti-rollback yok)", counter_pages_untouched());

    /* SIGNED: reddedilen paketler (uygulama eski kalir, ATLA ama REJECTED) */
    {
        pkg_t bad[5];
        const char *names[5] = { "saldirgan anahtariyla imzali", "bozuk imaj (ozet uyusmuyor)", "baska urun", "bilinmeyen anahtar kimligi", "commit sayfa sayisi imaji kapsamiyor" };
        for(unsigned k = 0u; k < 5u; k++) {
            bad[k] = default_pkg();
        }
        bad[0].signer = 2u;
        bad[1].flip_index = 300;
        bad[2].product = 999u;
        bad[3].key_id = 9u;
        for(unsigned k = 0u; k < 5u; k++) {
            char text[160];
            world(FWU_BOOT_VERIFY_SIGNED, FWU_ACTIVATE_MODE_COPY, false);
            stage_package(&bad[k]);
            (void)app_commit(k == 4u ? 1u : IMG_PAGES);      /* 4: commit 1 sayfa (256) < imaj 700: IMAGE_TOO_LARGE */
            (void)snprintf(text, sizeof(text), "SIGNED + %s: ATLA ama REDDEDILDI (REJECTED), uygulama ESKI, bekci YOK", names[k]);
            check(text, boot_cycle(&r) == FWU_BOOT_JUMP_APP && r.outcome == FWU_ACT_OUTCOME_REJECTED && app_is_old() && g_hook.wd_calls == 0u);
            stage_package(&p);
            (void)app_commit(IMG_PAGES);
            (void)snprintf(text, sizeof(text), "... ardindan GECERLI paket (%s sonrasi) kabul edilir: REJECTED yeni guncellemeyi zehirlemez", names[k]);
            check(text, boot_cycle(&r) == FWU_BOOT_JUMP_APP && r.outcome == FWU_ACT_OUTCOME_ACTIVATED && app_is_new());
        }
    }

    /* HASH: imza yok -> sahte imza kabul, ozet uyusmazligi red */
    world(FWU_BOOT_VERIFY_HASH, FWU_ACTIVATE_MODE_COPY, false);
    p = default_pkg(); p.signer = 2u;
    stage_package(&p);
    (void)app_commit(IMG_PAGES);
    check("HASH: sahte imza KABUL (imza kontrol edilmez, yetki yok: yalniz gelistirme)", boot_cycle(&r) == FWU_BOOT_JUMP_APP && r.outcome == FWU_ACT_OUTCOME_ACTIVATED && app_is_new());
    world(FWU_BOOT_VERIFY_HASH, FWU_ACTIVATE_MODE_COPY, false);
    p = default_pkg(); p.flip_index = 123;
    stage_package(&p);
    (void)app_commit(IMG_PAGES);
    check("HASH: bozuk imaj (ozet uyusmuyor) REDDEDILIR, uygulama ESKI", boot_cycle(&r) == FWU_BOOT_JUMP_APP && r.outcome == FWU_ACT_OUTCOME_REJECTED && app_is_old());
    world(FWU_BOOT_VERIFY_HASH, FWU_ACTIVATE_MODE_COPY, false);
    p = default_pkg(); p.product = 999u;
    stage_package(&p);
    (void)app_commit(IMG_PAGES);
    check("HASH: baska urun REDDEDILIR", boot_cycle(&r) == FWU_BOOT_JUMP_APP && r.outcome == FWU_ACT_OUTCOME_REJECTED && app_is_old());

    /* NONE: dogrulama yok, her sey kopyalanir (yalniz gelistirme) */
    world(FWU_BOOT_VERIFY_NONE, FWU_ACTIVATE_MODE_COPY, false);
    for(uint32_t i = 0u; i < 3u * PAGE; i++) {
        garbage[i] = (fwu_byte_t)((i * 5u + 11u) & 0xFFu);
        g_sim.base.memory[IMAGE_OFFSET + i] = garbage[i];
    }
    (void)app_commit(IMG_PAGES);
    {
        int copied = 1;
        check("NONE: ATLA, sonuc ACTIVATED", boot_cycle(&r) == FWU_BOOT_JUMP_APP && r.outcome == FWU_ACT_OUTCOME_ACTIVATED);
        for(uint32_t i = 0u; i < 3u * PAGE; i++) {
            if((g_sim.base.memory[APP_OFFSET + i] & 0xFFu) != (garbage[i] & 0xFFu)) {
                copied = 0;
            }
        }
        check("... DOGRULAMA YOK: staging'deki cop 3 sayfa aynen uygulamaya kopyalandi (bu yuzden yalniz DEV)", copied);
    }
}

/* ------------------------------------------------------------------ 5. kapali hata (fail-closed) */

static void test_fail_closed(void)
{
    fwu_boot_result_t r;
    pkg_t p = default_pkg();
    fwu_boot_config_t c;

    printf("\n--- 5. basarisizlikta ASLA atlanmaz ---\n");

    /* guc kesintisi aktivasyon sirasinda */
    world(FWU_BOOT_VERIFY_SIGNED, FWU_ACTIVATE_MODE_COPY, false);
    stage_package(&p);
    (void)app_commit(IMG_PAGES);
    check("aktivasyon sirasinda guc kesintisi (20. islem): KAL, neden ACTIVATE_RUN, durum FLASH_POWER_LOSS, sonuc NONE",
          boot_cycle_cut(&r, 20u) == FWU_BOOT_STAY && r.reason == FWU_BOOT_REASON_ACTIVATE_RUN && r.status == FWU_ERR_FLASH_POWER_LOSS &&
          r.outcome == FWU_ACT_OUTCOME_NONE && !r.trial);
    check("... uygulama YARIM (ne eski ne yeni): ATLANMADI", !app_is_old() && !app_is_new());
    check("... bekci ve app_valid CAGRILMADI", g_hook.wd_calls == 0u && g_hook.valid_calls == 0u);
    check("sonraki acilis: kurtarir, ATLA, uygulama TAM YENI", boot_cycle(&r) == FWU_BOOT_JUMP_APP && r.outcome == FWU_ACT_OUTCOME_ACTIVATED && app_is_new());

    /* sayaclar okunamiyor: guc yokken baslat */
    world(FWU_BOOT_VERIFY_SIGNED, FWU_ACTIVATE_MODE_SWAP, true);
    fwu_flash_sim_cut_power_after(&g_sim.base, 0u);
    (void)fwu_flash_erase(&g_sim.base.flash, 15u * PAGE, PAGE);         /* kesinti tetiklenir: artik tum islemler (okuma dahil) POWER_LOSS */
    check("guc yok + anti-rollback: sayaclar OKUNAMADI -> KAL, neden COUNTERS, durum FLASH_POWER_LOSS (sessizce acilmaz)",
          fwu_boot_start(&g_boot, &g_cfg, &r) == FWU_BOOT_STAY && r.reason == FWU_BOOT_REASON_COUNTERS && r.status == FWU_ERR_FLASH_POWER_LOSS);
    world(FWU_BOOT_VERIFY_SIGNED, FWU_ACTIVATE_MODE_COPY, false);
    fwu_flash_sim_cut_power_after(&g_sim.base, 0u);
    (void)fwu_flash_erase(&g_sim.base.flash, 15u * PAGE, PAGE);
    check("guc yok + anti-rollback YOK: aktivasyon gunlugu OKUNAMADI -> KAL, neden ACTIVATE_INIT, durum FLASH_POWER_LOSS",
          fwu_boot_start(&g_boot, &g_cfg, &r) == FWU_BOOT_STAY && r.reason == FWU_BOOT_REASON_ACTIVATE_INIT && r.status == FWU_ERR_FLASH_POWER_LOSS);

    /* sayac bolgesi gecersiz: anti-rollback ile KAL, anti-rollback YOK iken sayac hic kullanilmaz */
    world(FWU_BOOT_VERIFY_SIGNED, FWU_ACTIVATE_MODE_SWAP, true);
    g_cfg.layout.counter_a_offset = 22u * PAGE;
    check("sayac bolgesi flash disi + anti-rollback: KAL, neden COUNTERS, durum OUT_OF_RANGE",
          boot_cycle(&r) == FWU_BOOT_STAY && r.reason == FWU_BOOT_REASON_COUNTERS && r.status == FWU_ERR_OUT_OF_RANGE);
    world(FWU_BOOT_VERIFY_SIGNED, FWU_ACTIVATE_MODE_SWAP, false);
    g_cfg.layout.counter_a_offset = 22u * PAGE;
    check("... anti-rollback YOK: sayac okunmaz, ATLA", boot_cycle(&r) == FWU_BOOT_JUMP_APP);

    /* aktivasyon kurulamiyor: SWAP kaydi, COPY yapilandirmasi */
    world(FWU_BOOT_VERIFY_SIGNED, FWU_ACTIVATE_MODE_SWAP, false);
    stage_package(&p);
    (void)app_commit(IMG_PAGES);
    c = g_cfg; c.policy.mode = FWU_ACTIVATE_MODE_COPY;
    check("SWAP commit'i varken COPY yapilandirmasi: KAL, neden ACTIVATE_INIT, durum BAD_STATE (baska stratejinin kaydi)",
          (fwu_flash_sim_reset(&g_sim.base), fwu_boot_start(&g_boot, &c, &r)) == FWU_BOOT_STAY && r.reason == FWU_BOOT_REASON_ACTIVATE_INIT && r.status == FWU_ERR_BAD_STATE);
    check("... uygulama DEGISMEDI", app_is_old());
    check("dogru (SWAP) yapilandirmayla sonraki acilis: ATLA, TRIAL", boot_cycle(&r) == FWU_BOOT_JUMP_APP && r.outcome == FWU_ACT_OUTCOME_TRIAL);

    /* uygulama yok */
    world(FWU_BOOT_VERIFY_SIGNED, FWU_ACTIVATE_MODE_SWAP, false);
    g_hook.app_ok = false;
    check("app_valid false (bos/gecersiz uygulama): KAL, neden NO_APP, durum OK, bekci YOK", boot_cycle(&r) == FWU_BOOT_STAY && r.reason == FWU_BOOT_REASON_NO_APP &&
          r.status == FWU_OK && g_hook.valid_calls == 1u && g_hook.wd_calls == 0u);
    stage_package(&p);
    (void)app_commit(IMG_PAGES);
    check("deneme bekliyor + app_valid false: KAL (NO_APP), deneme true ama bekci CAGRILMADI (atlanmiyor)",
          boot_cycle(&r) == FWU_BOOT_STAY && r.reason == FWU_BOOT_REASON_NO_APP && r.trial && r.outcome == FWU_ACT_OUTCOME_TRIAL && g_hook.wd_calls == 0u);
    g_hook.app_ok = true;
    check("app_valid true olunca: ATLA (2. deneme hakki), bekci 1 kez", boot_cycle(&r) == FWU_BOOT_JUMP_APP && r.trial && g_hook.wd_calls == 1u);
    {
        fwu_boot_config_t nc;
        world(FWU_BOOT_VERIFY_SIGNED, FWU_ACTIVATE_MODE_SWAP, false);
        nc = g_cfg; nc.app_valid = NULL; nc.watchdog_arm = NULL;
        stage_package(&p);
        (void)app_commit(IMG_PAGES);
        fwu_flash_sim_reset(&g_sim.base);
        check("kancalar NULL (isteğe bagli): denetleme/bekci yok, ATLA", fwu_boot_start(&g_boot, &nc, &r) == FWU_BOOT_JUMP_APP && r.trial);
    }

    /* kripto "yapilamadi" */
    world(FWU_BOOT_VERIFY_SIGNED, FWU_ACTIVATE_MODE_COPY, false);
    stage_package(&p);
    (void)app_commit(IMG_PAGES);
    g_cs.fail_verify = FWU_ERR_CRYPTO_FAILURE;
    check("imza dogrulamasi YAPILAMADI (kripto hatasi): KAL, ACTIVATE_RUN, durum CRYPTO_FAILURE, REDDEDILMEDI",
          boot_cycle(&r) == FWU_BOOT_STAY && r.reason == FWU_BOOT_REASON_ACTIVATE_RUN && r.status == FWU_ERR_CRYPTO_FAILURE && r.outcome == FWU_ACT_OUTCOME_NONE);
    check("... uygulama ESKI", app_is_old());
    g_cs.fail_verify = FWU_OK;
    check("kripto duzelince sonraki acilis: guncelleme TAMAMLANIR (red yazilmamisti)", boot_cycle(&r) == FWU_BOOT_JUMP_APP && r.outcome == FWU_ACT_OUTCOME_ACTIVATED && app_is_new());
}

/* ------------------------------------------------------------------ 6. uygulama rolu */

static void test_app_role(void)
{
    fwu_boot_result_t r;
    uint32_t iv;
    uint16_t kv;
    pkg_t p = default_pkg();

    printf("\n--- 6. uygulama rolu: finish_trial ---\n");
    world(FWU_BOOT_VERIFY_SIGNED, FWU_ACTIVATE_MODE_SWAP, true);
    stage_package(&p);
    expect("commit", app_commit(IMG_PAGES), FWU_OK);
    expect("commit verildi ama bootloader henuz calismadi: finish_trial BAD_STATE (evre ACTIVATING)", fwu_boot_app_finish_trial(&g_boot, true), FWU_ERR_BAD_STATE);
    (void)boot_cycle(&r);

    /* onay kancasi basarisiz: CONFIRMED yazilir, kanca ayni acilista yeniden denenir */
    g_cs.fail_verify = FWU_ERR_CRYPTO_FAILURE;
    expect("onay kancasi: kripto YAPILAMADI: finish_trial hatayi aynen dondurur", fwu_boot_app_finish_trial(&g_boot, true), FWU_ERR_CRYPTO_FAILURE);
    counters_now(&iv, &kv);
    check("... sayac DEGISMEDI (0, 0)", iv == 0u && kv == 0u);
    g_cs.fail_verify = FWU_OK;
    expect("ayni acilista tekrar: kanca yeniden calisir, OK", fwu_boot_app_finish_trial(&g_boot, true), FWU_OK);
    counters_now(&iv, &kv);
    check("... sayac (25, 1)", iv == 25u && kv == 1u);
    {
        const uint32_t calls = g_cs.verify_calls;
        expect("bir daha: deneme yok, RAM bayragi temiz, hicbir sey yapmaz", fwu_boot_app_finish_trial(&g_boot, true), FWU_OK);
        check("... kanca YENIDEN CALISMADI (imza dogrulamasi sayisi ayni): bayrak basarida temizlendi", g_cs.verify_calls == calls);
    }
    check("sonraki acilis: ATLA, TRIAL yok, uygulama yeni, sayac (25, 1) flash'ta", boot_cycle(&r) == FWU_BOOT_JUMP_APP && !r.trial && app_is_new());
    counters_now(&iv, &kv);
    check("... (25, 1)", iv == 25u && kv == 1u);

    /* onay kancasi hatasi + yeniden baslatma: sayac geride kalir (guvenli yon), kalici yeniden deneme YOK */
    world(FWU_BOOT_VERIFY_SIGNED, FWU_ACTIVATE_MODE_SWAP, true);
    stage_package(&p);
    (void)app_commit(IMG_PAGES);
    (void)boot_cycle(&r);
    g_cs.fail_verify = FWU_ERR_CRYPTO_FAILURE;
    (void)fwu_boot_app_finish_trial(&g_boot, true);
    g_cs.fail_verify = FWU_OK;
    check("kanca hatasindan sonra YENIDEN BASLATMA: ATLA, deneme yok (CONFIRMED yazilmisti)", boot_cycle(&r) == FWU_BOOT_JUMP_APP && !r.trial && app_is_new());
    expect("... yeni acilista finish_trial kancayi yeniden CALISTIRMAZ (bayrak yalniz RAM)", fwu_boot_app_finish_trial(&g_boot, true), FWU_OK);
    counters_now(&iv, &kv);
    check("... sayac geride (0, 0): guvenli yon (README acik konu 28), uygulama calisiyor", iv == 0u && kv == 0u);
}

/* ------------------------------------------------------------------ 7. guc kesintisi taramalari */

typedef enum { SC_BOOT = 0, SC_CONFIRM = 1, SC_REVERT = 2 } scenario_e;

typedef struct {
    uint32_t points;
    uint32_t clean_ops;
    uint32_t violations;
    uint32_t stays;
    uint32_t jumps_new;
    uint32_t jumps_old;
    uint32_t counters_behind;
    uint32_t counters_ok;
} sweep_t;

static void setup_scenario(scenario_e sc, fwu_boot_verify_e verify, fwu_activate_mode_e mode, bool ar)
{
    fwu_boot_result_t r;
    pkg_t p = default_pkg();

    world(verify, mode, ar);
    stage_package(&p);
    (void)app_commit(IMG_PAGES);
    if(sc != SC_BOOT) {
        (void)boot_cycle(&r);                           /* deneme basladi */
        if(sc == SC_REVERT) {
            (void)fwu_boot_app_finish_trial(&g_boot, false);
        }
    }
}

static void note_jump(sweep_t *s, scenario_e sc, bool ar)
{
    uint32_t iv;
    uint16_t kv;

    if(!boot_page_intact()) {
        s->violations++;                                /* bootloader sayfasi degisti */
    }
    if(app_is_new()) {
        s->jumps_new++;
    } else if(app_is_old()) {
        s->jumps_old++;
    } else {
        s->violations++;                                /* atlandi ama uygulama ne eski ne yeni */
    }
    (void)sc;
    if(ar) {
        counters_now(&iv, &kv);
        if(!(((iv == 0u) && (kv == 0u)) || ((iv == 25u) && (kv == 1u)))) {
            s->violations++;                            /* karisik sayac */
        }
    }
}

/** Bir senaryoyu, kesinti noktasi n = 0, 1, ... icin calistirir; n, temiz kosunun islem sayisina ulasinca biter. */
static void sweep(scenario_e sc, fwu_boot_verify_e verify, fwu_activate_mode_e mode, bool ar, sweep_t *s)
{
    fwu_boot_result_t r;
    uint32_t iv;
    uint16_t kv;

    s->points = 0u; s->violations = 0u; s->stays = 0u; s->jumps_new = 0u; s->jumps_old = 0u; s->counters_behind = 0u; s->counters_ok = 0u;

    /* temiz kosu: islem sayisi */
    setup_scenario(sc, verify, mode, ar);
    if(sc == SC_CONFIRM) {
        fwu_flash_sim_reset(&g_sim.base);
        (void)fwu_boot_app_finish_trial(&g_boot, true);
    } else {
        (void)boot_cycle(&r);
    }
    s->clean_ops = fwu_flash_sim_operations(&g_sim.base);

    for(uint32_t n = 0u; n < 4000u; n++) {
        bool cut;
        bool jumped = false;
        bool first_jump_done = false;
        fwu_return_e fin = FWU_OK;

        setup_scenario(sc, verify, mode, ar);
        if(sc == SC_CONFIRM) {
            fwu_flash_sim_reset(&g_sim.base);
            fwu_flash_sim_cut_power_after(&g_sim.base, n);
            fin = fwu_boot_app_finish_trial(&g_boot, true);
            cut = !fwu_flash_sim_is_powered(&g_sim.base);
            if(cut && (fin == FWU_OK)) {
                s->violations++;                        /* kesintiye ragmen basarili dondu */
            }
        } else {
            jumped = (boot_cycle_cut(&r, n) == FWU_BOOT_JUMP_APP);
            cut = !fwu_flash_sim_is_powered(&g_sim.base);
            if(cut && jumped) {
                s->violations++;                        /* ALTIN KURAL: basarisizlikta atlanmaz */
            }
            if(jumped) {
                note_jump(s, sc, ar);
                first_jump_done = true;
            } else {
                s->stays++;
            }
        }
        if(!cut) {
            break;                                      /* tum islemler kesintisiz bitti: tarama tamam */
        }
        s->points++;

        /* kurtarma: guc geri gelir; en cok 6 acilis */
        {
            bool recovered = false;
            for(unsigned boot_no = 0u; boot_no < 6u; boot_no++) {
                if(boot_cycle(&r) == FWU_BOOT_JUMP_APP) {
                    note_jump(s, sc, ar);
                    first_jump_done = true;
                    recovered = true;
                    if(sc == SC_CONFIRM && r.trial) {   /* onay kaydi kesintiyle kayboldu: uygulama yeniden onaylar */
                        (void)fwu_boot_app_finish_trial(&g_boot, true);
                        continue;
                    }
                    if(!(sc == SC_BOOT && r.trial)) {
                        break;
                    }
                    /* SC_BOOT, deneme basladi: uygulama onaylar, sonra normal acilis */
                    (void)fwu_boot_app_finish_trial(&g_boot, true);
                    continue;
                }
            }
            if(!recovered || !first_jump_done) {
                s->violations++;                        /* kurtarilamadi */
            }
        }
        (void)jumped;
        /* son durum */
        fwu_flash_sim_reset(&g_sim.base);
        (void)fwu_boot_start(&g_boot, &g_cfg, &r);
        if(sc == SC_REVERT) {
            if(!app_is_old() || r.trial) {
                s->violations++;                        /* geri donus tamamlanmali: TAM ESKI */
            }
            if(ar) {
                counters_now(&iv, &kv);
                if((iv != 0u) || (kv != 0u)) {
                    s->violations++;
                }
            }
        } else {
            if(!app_is_new()) {
                s->violations++;                        /* guncelleme tamamlanmali: TAM YENI */
            }
            if(ar) {
                counters_now(&iv, &kv);
                if((iv == 25u) && (kv == 1u)) {
                    s->counters_ok++;
                } else if((iv == 0u) && (kv == 0u)) {
                    s->counters_behind++;
                } else {
                    s->violations++;
                }
            }
        }
    }
}

static void test_sweeps(void)
{
    sweep_t s;

    printf("\n--- 7. guc kesintisi taramalari: HER flash isleminde kesinti, kurtarma, degismezler ---\n");

    sweep(SC_BOOT, FWU_BOOT_VERIFY_SIGNED, FWU_ACTIVATE_MODE_COPY, false, &s);
    printf("(bilgi: COPY guncelleme acilisi: %u islem, %u kesinti noktasi, ATLA yeni %u eski %u, KAL %u)\n", (unsigned)s.clean_ops, (unsigned)s.points,
           (unsigned)s.jumps_new, (unsigned)s.jumps_old, (unsigned)s.stays);
    check("COPY + SIGNED acilis taramasi: nokta sayisi = temiz kosunun islem sayisi (hicbir nokta atlanmadi)", s.points == s.clean_ops && s.clean_ops > 100u);
    check("... ihlal 0 (kesintide ASLA ATLAMA, atlanirken uygulama hep tam eski/yeni, kurtarma hep tam YENI)", s.violations == 0u);
    check("... kesinti noktalarinin cogunda KAL (yarim uygulama), hicbirinde yarim ATLAMA yok", s.stays == s.points && s.jumps_new > 0u);

    sweep(SC_BOOT, FWU_BOOT_VERIFY_SIGNED, FWU_ACTIVATE_MODE_SWAP, true, &s);
    printf("(bilgi: SWAP + anti-rollback guncelleme acilisi: %u islem, %u kesinti noktasi, ATLA yeni %u eski %u, sayac (25,1) %u geride %u)\n", (unsigned)s.clean_ops,
           (unsigned)s.points, (unsigned)s.jumps_new, (unsigned)s.jumps_old, (unsigned)s.counters_ok, (unsigned)s.counters_behind);
    check("SWAP + anti-rollback acilis taramasi: nokta sayisi = temiz islem sayisi", s.points == s.clean_ops && s.clean_ops > 200u);
    check("... ihlal 0 (kesinti sonrasi kurtarma + onay: uygulama TAM YENI, sayac HEP (25, 1): kesinti onayi etkilemez)", s.violations == 0u && s.counters_ok == s.points && s.counters_behind == 0u);

    sweep(SC_CONFIRM, FWU_BOOT_VERIFY_SIGNED, FWU_ACTIVATE_MODE_SWAP, true, &s);
    printf("(bilgi: onay: %u islem, %u kesinti noktasi, sayac (25,1) %u, GERIDE %u)\n", (unsigned)s.clean_ops, (unsigned)s.points, (unsigned)s.counters_ok, (unsigned)s.counters_behind);
    check("onay taramasi: nokta sayisi = temiz islem sayisi (CONFIRMED kaydi 2 granul + sayac kaydi 2 granul = 4)", s.points == s.clean_ops && s.clean_ops == 4u);
    check("... ihlal 0: uygulama hep TAM YENI, sayac hep (0,0) veya (25,1), asla karisik", s.violations == 0u);
    check("... kesinti sayaci geride birakabildi (CONFIRMED yazildi, sayac yazilmadi: GUVENLI yon) ve diger noktalarda yeniden onayla yukseldi",
          s.counters_behind >= 1u && s.counters_ok >= 1u && (s.counters_behind + s.counters_ok) == s.points);

    sweep(SC_REVERT, FWU_BOOT_VERIFY_SIGNED, FWU_ACTIVATE_MODE_SWAP, true, &s);
    printf("(bilgi: geri donus acilisi: %u islem, %u kesinti noktasi, ATLA eski %u, KAL %u)\n", (unsigned)s.clean_ops, (unsigned)s.points, (unsigned)s.jumps_old, (unsigned)s.stays);
    check("geri donus taramasi: nokta sayisi = temiz islem sayisi", s.points == s.clean_ops && s.clean_ops > 50u);
    check("... ihlal 0: kesinti sonrasi her kurtarmada uygulama TAM ESKI, sayac (0, 0)", s.violations == 0u);
}

/* ------------------------------------------------------------------ 8. sozlesme parmak izi */

/** Bagimsiz FNV-1a 32 (yayimlanmis vektorlerle dogrulanir). */
static uint32_t fnv1a(const fwu_byte_t *data, size_t length)
{
    uint32_t h = 2166136261u;
    for(size_t i = 0u; i < length; i++) {
        h ^= (uint32_t)(data[i] & 0xFFu);
        h *= 16777619u;
    }
    return h;
}

static void test_fingerprint(void)
{
    fwu_boot_config_t a;
    fwu_boot_config_t b;
    uint32_t fa = 0u;
    uint32_t fb = 0u;
    fwu_byte_t info[FWU_BOOT_INFO_SIZE];
    size_t n = 0u;
    fwu_byte_t ser[22u * 4u];
    fwu_flash_t other_flash;

    printf("\n--- 8. sozlesme parmak izi ve bootloader kaydi ---\n");
    {
        const fwu_byte_t a_[1] = { 0x61u };
        const fwu_byte_t foobar[6] = { 0x66u, 0x6Fu, 0x6Fu, 0x62u, 0x61u, 0x72u };
        check("bagimsiz FNV-1a yayimlanmis vektorlerle: 'a' = 0xE40C292C, 'foobar' = 0xBF9CF968", fnv1a(a_, 1u) == 0xE40C292Cu && fnv1a(foobar, 6u) == 0xBF9CF968u);
    }
    world(FWU_BOOT_VERIFY_SIGNED, FWU_ACTIVATE_MODE_SWAP, true);
    a = g_cfg;
    expect("fingerprint", fwu_boot_fingerprint(&a, &fa), FWU_OK);
    {
        /* ELLE yazilmis serilestirme: surum, verify, mod, ar, urun, deneme, geometri (256, 8, 5632), 12 duzen alani */
        const uint32_t fields[22] = { FWU_BOOT_CONTRACT_VERSION, 2u, 1u, 1u, PRODUCT, 2u, 256u, 8u, 22u * 256u,
                                      APP_OFFSET, MANIFEST_OFFSET, IMAGE_OFFSET, IMAGE_MAX, 17u * 256u, 18u * 256u, 256u, 19u * 256u, 1u, COUNTER_A, COUNTER_B, 256u, 0u };
        size_t used = 0u;
        for(unsigned i = 0u; i < 21u; i++) {
            fwu_write_u32_big_endian(&ser[used], fields[i]);
            used += 4u;
        }
        check("parmak izi = ELLE siralanmis 21 alanin buyuk endian FNV-1a'si (alan sirasi ve kapsami sabit)", fa == fnv1a(ser, used));
    }
    expect("fingerprint: ayni yapilandirma ayni deger", fwu_boot_fingerprint(&a, &fb), FWU_OK);
    check("... deterministik", fa == fb);
    expect("fingerprint config NULL", fwu_boot_fingerprint(NULL, &fb), FWU_ERR_INVALID_ARG);
    expect("fingerprint cikis NULL", fwu_boot_fingerprint(&a, NULL), FWU_ERR_INVALID_ARG);
    b = a; b.flash = NULL;
    expect("fingerprint flash NULL", fwu_boot_fingerprint(&b, &fb), FWU_ERR_INVALID_ARG);

    /* her alan parmak izini degistirir */
    {
        int all_differ = 1;
        int idx_bad = -1;
        for(unsigned k = 0u; k < 20u; k++) {
            uint32_t f2 = 0u;
            b = a;
            other_flash = *a.flash;
            switch(k) {
            case 0: b.policy.verify = FWU_BOOT_VERIFY_HASH; break;
            case 1: b.policy.mode = FWU_ACTIVATE_MODE_COPY; break;
            case 2: b.policy.anti_rollback = false; break;
            case 3: b.product_id++; break;
            case 4: b.max_trial_boots++; break;
            case 5: other_flash.geometry.page_length++; b.flash = &other_flash; break;
            case 6: other_flash.geometry.write_granule++; b.flash = &other_flash; break;
            case 7: other_flash.geometry.flash_length++; b.flash = &other_flash; break;
            case 8: b.layout.app_offset++; break;
            case 9: b.layout.staging_manifest_offset++; break;
            case 10: b.layout.staging_image_offset++; break;
            case 11: b.layout.image_region_length++; break;
            case 12: b.layout.journal_a_offset++; break;
            case 13: b.layout.journal_b_offset++; break;
            case 14: b.layout.journal_length++; break;
            case 15: b.layout.scratch_offset++; break;
            case 16: b.layout.scratch_pages++; break;
            case 17: b.layout.counter_a_offset++; break;
            case 18: b.layout.counter_b_offset++; break;
            case 19: b.layout.counter_length++; break;
            default: b.layout.counter_length++; break;
            }
            (void)fwu_boot_fingerprint(&b, &f2);
            if(f2 == fa) {
                all_differ = 0;
                idx_bad = (int)k;
            }
        }
        if(!all_differ) {
            printf("(bilgi: parmak izini degistirmeyen alan numarasi %d)\n", idx_bad);
        }
        check("20 alanin HER BIRI (verify, mod, anti-rollback, urun, deneme, 3 geometri, 12 duzen) parmak izini degistirir", all_differ);
    }
    /* girmeyen alanlar */
    b = a; b.key_count = 1u; b.keys = NULL; b.crypto = NULL; b.verify_work = NULL; b.activate_work = NULL; b.app_valid = NULL; b.watchdog_arm = NULL; b.context = NULL;
    b.verify_work_length = 1u; b.activate_work_length = 1u;
    (void)fwu_boot_fingerprint(&b, &fb);
    check("anahtarlar, crypto, tamponlar ve kancalar parmak izine GIRMEZ (yetkili kopya bootloader'da)", fb == fa);

    /* kayit */
    expect("info encode", fwu_boot_info_encode(&a, info, sizeof(info) / sizeof(info[0]), &n), FWU_OK);
    check("kayit 12 bayt: 'FWBI', surum 0x0001, parmak izi buyuk endian", n == 12u && info[0] == 0x46u && info[1] == 0x57u && info[2] == 0x42u && info[3] == 0x49u &&
          info[4] == 0x00u && info[5] == 0x01u && fwu_read_u32_big_endian(&info[6]) == fa);
    check("... CRC-16/MODBUS (ilk 10 bayt) buyuk endian", fwu_read_u16_big_endian(&info[10]) == fwu_crc16_calculate_w_start_value(FWU_CRC16_INIT, info, 10u));
    expect("info encode: kapasite 11", fwu_boot_info_encode(&a, info, 11u, &n), FWU_ERR_BUFFER_TOO_SMALL);
    expect("info encode: kapasite 12 (sinir)", fwu_boot_info_encode(&a, info, 12u, &n), FWU_OK);
    expect("info encode: config NULL", fwu_boot_info_encode(NULL, info, 12u, &n), FWU_ERR_INVALID_ARG);
    expect("info encode: cikis NULL", fwu_boot_info_encode(&a, NULL, 12u, &n), FWU_ERR_INVALID_ARG);
    expect("info encode: uzunluk NULL", fwu_boot_info_encode(&a, info, 12u, NULL), FWU_ERR_INVALID_ARG);
    b = a; b.flash = NULL;
    expect("info encode: flash NULL", fwu_boot_info_encode(&b, info, 12u, &n), FWU_ERR_INVALID_ARG);

    (void)fwu_boot_info_encode(&a, info, 12u, &n);
    expect("info check: uyuyor", fwu_boot_info_check(&a, info, 12u), FWU_OK);
    expect("info check: fazla uzunluk (ekstra bayt yok sayilir)", fwu_boot_info_check(&a, info, 20u), FWU_OK);
    expect("info check: 11 bayt", fwu_boot_info_check(&a, info, 11u), FWU_ERR_NOT_ENOUGH_DATA);
    expect("info check: config NULL", fwu_boot_info_check(NULL, info, 12u), FWU_ERR_INVALID_ARG);
    expect("info check: kayit NULL", fwu_boot_info_check(&a, NULL, 12u), FWU_ERR_INVALID_ARG);
    {
        int all_mismatch = 1;
        for(unsigned k = 0u; k < 12u; k++) {
            fwu_byte_t bad[FWU_BOOT_INFO_SIZE];
            for(unsigned i = 0u; i < 12u; i++) {
                bad[i] = info[i];
            }
            bad[k] = (fwu_byte_t)(bad[k] ^ 0x01u);
            if(fwu_boot_info_check(&a, bad, 12u) != FWU_ERR_CONTRACT_MISMATCH) {
                all_mismatch = 0;
            }
        }
        check("kayittaki HER bayt (magic 4, surum 2, parmak izi 4, CRC 2) bozulunca CONTRACT_MISMATCH", all_mismatch);
    }
    {
        fwu_byte_t crafted[FWU_BOOT_INFO_SIZE];
        uint16_t crc;
        for(unsigned i = 0u; i < 12u; i++) {
            crafted[i] = info[i];
        }
        {
            int all_magic = 1;
            for(unsigned k = 0u; k < 4u; k++) {
                for(unsigned i = 0u; i < 12u; i++) {
                    crafted[i] = info[i];
                }
                crafted[k] = (fwu_byte_t)(crafted[k] ^ 0x20u);                /* magic'in k. bayti yanlis, CRC yeniden gecerli */
                crc = fwu_crc16_calculate_w_start_value(FWU_CRC16_INIT, crafted, 10u);
                fwu_write_u16_big_endian(&crafted[10], crc);
                if(fwu_boot_info_check(&a, crafted, 12u) != FWU_ERR_CONTRACT_MISMATCH) {
                    all_magic = 0;
                }
            }
            check("magic'in HER bayti ayri ayri yanlis ama CRC GECERLI: CONTRACT_MISMATCH (4 bayt da ayri denetlenir)", all_magic);
        }
        for(unsigned i = 0u; i < 12u; i++) {
            crafted[i] = info[i];
        }
        for(unsigned i = 0u; i < 12u; i++) {
            crafted[i] = info[i];
        }
        crafted[5] = 0x02u;                                              /* surum 2, CRC yeniden gecerli */
        crc = fwu_crc16_calculate_w_start_value(FWU_CRC16_INIT, crafted, 10u);
        fwu_write_u16_big_endian(&crafted[10], crc);
        expect("surum 2 ama CRC GECERLI: CONTRACT_MISMATCH (surum ayri denetlenir)", fwu_boot_info_check(&a, crafted, 12u), FWU_ERR_CONTRACT_MISMATCH);
        for(unsigned i = 0u; i < 12u; i++) {
            crafted[i] = info[i];
        }
        crafted[9] = (fwu_byte_t)(crafted[9] ^ 0x01u);                   /* parmak izi farkli, CRC yeniden gecerli */
        crc = fwu_crc16_calculate_w_start_value(FWU_CRC16_INIT, crafted, 10u);
        fwu_write_u16_big_endian(&crafted[10], crc);
        expect("parmak izi farkli ama CRC GECERLI: CONTRACT_MISMATCH (parmak izi ayri denetlenir)", fwu_boot_info_check(&a, crafted, 12u), FWU_ERR_CONTRACT_MISMATCH);
    }
    b = a; b.product_id++;
    expect("baska urun kimligiyle derlenmis uygulama: CONTRACT_MISMATCH", fwu_boot_info_check(&b, info, 12u), FWU_ERR_CONTRACT_MISMATCH);
    b = a; b.policy.mode = FWU_ACTIVATE_MODE_COPY;
    expect("baska aktivasyon moduyla derlenmis uygulama: CONTRACT_MISMATCH", fwu_boot_info_check(&b, info, 12u), FWU_ERR_CONTRACT_MISMATCH);
    b = a; b.layout.staging_image_offset += 256u;
    expect("baska flash duzeniyle derlenmis uygulama: CONTRACT_MISMATCH", fwu_boot_info_check(&b, info, 12u), FWU_ERR_CONTRACT_MISMATCH);
    {
        fwu_boot_config_t w = a;
        w.flash = NULL;
        expect("info check: flash NULL (parmak izi hesaplanamaz)", fwu_boot_info_check(&w, info, 12u), FWU_ERR_INVALID_ARG);
    }
#ifdef C2000
    {
        fwu_byte_t dirty[FWU_BOOT_INFO_SIZE];
        for(unsigned i = 0u; i < 12u; i++) {
            dirty[i] = (fwu_byte_t)(info[i] | 0x100u);
        }
        expect("C2000 taklidi: bayt ust bitleri kirli (0x1XX): yine uyuyor (alt 8 bit)", fwu_boot_info_check(&a, dirty, 12u), FWU_OK);
    }
#endif
}

int main(void)
{
    test_validation();
    test_swap_ar_flow();
    test_revert_flows();
    test_copy_and_levels();
    test_fail_closed();
    test_app_role();
    test_sweeps();
    test_fingerprint();

    printf("\nResult: %s (%d failed)\n", g_failures == 0 ? "ALL TESTS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
