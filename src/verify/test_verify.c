/**
 * @file    test_verify.c
 * @brief   fwu_verify (doğrulama hattı) için kendi kendini kontrol eden test: sahte flash + sahte kriptografi
 *          üzerinde, ardından GERÇEK aktivasyonla (fwu_activate) uçtan uca.
 *
 * Her reddetme için yalnızca sonuç kodu değil, PAHALI İŞLEMLERE (imza, hash) girilip girilmediği de kontrol
 * edilir (sahte kriptografinin çağrı sayaçlarıyla): ucuz kontrol geçmeyen imaj imzaya, imzası geçmeyen imaj
 * hash'e hiç ulaşmamalıdır.
 *
 * Kapsam:
 *   1. Geçerli imaj: sonuç, manifest çıktısı, sayaçlar, farklı okuma tamponu boyutları, sınır boyutlar.
 *   2. Yapı hataları, ürün, sürüm, boyut, anahtar kontrolleri (sınır değerleriyle) ve "ucuz kontrol imzaya girmez".
 *   3. İmza: imzanın, imzalanan alanın (sha256, commit hash, sürüm, anahtar kimliği) bozulması; yanlış anahtar.
 *   4. Hash: imajın her bölgesinde tek bit; manifest hash'i değişip yeniden imzalanırsa; imaj sonrası bayt.
 *   5. DEV kipi (imza yok): imza/anahtar atlanır, geri kalan kontroller sürer.
 *   6. Hatalar: kripto hataları, flash okuma hataları (manifest ve imaj ortasında), özetin kapatılması.
 *   7. Argümanlar; aktivasyon kancası (ofset ve commit alanı kontrolü).
 *   8. Aktivasyonla uçtan uca: COPY ve SWAP, sahte/bozuk imaj REDDEDİLİR ve uygulamaya hiç dokunulmaz.
 *   9. fwu_verify_manifest: yalnızca manifest (yapı, ürün, anahtar, imza); sürüm/boyut/hash'e BAKMAZ, imaj okunmaz.
 *  10. fwu_verify_offer: RAM'deki manifest baytlari (oturum teklifi): yapi, urun, surum, boyut, anahtar, imza; imaj/hash yok.
 *
 * Derleme (verify klasöründen):
 *   gcc -Wall -Wextra -Wpedantic -Wconversion -std=c99 -I. -I.. -I../error -I../bytes -I../crc -I../flash -I../journal -I../activation -I../manifest -I../crypto fwu_verify.c ../manifest/fwu_manifest.c ../crypto/fwu_crypto.c ../crypto/fwu_crypto_sim.c ../activation/fwu_activate.c ../journal/fwu_journal_ab.c ../journal/fwu_journal.c ../flash/fwu_flash.c ../flash/fwu_flash_sim.c test_verify.c -o test_verify && ./test_verify
 */

#include <stdio.h>
#include "fwu_verify.h"
#include "fwu_activate.h"
#include "fwu_crypto_sim.h"
#include "fwu_flash_sim.h"

/* 19 sayfa x 256, granül 8:  sayfa 0..7 uygulama | 8 manifest | 9..15 imaj (7 sayfa) | 16,17 journal | 18 geçici */
FWU_FLASH_SIM_DEFINE(vf, 19, 256, 8);

#define PAGE            256u
#define MANIFEST_OFFSET 2048u
#define IMAGE_OFFSET    2304u
#define IMAGE_MAX       1792u
#define PRODUCT         12345u

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

/* ------------------------------------------------------------------ paket kurma (doğrulamadan BAĞIMSIZ) */

static fwu_byte_t g_pk[3][32];
static fwu_verify_key_t g_keys[2];
static fwu_byte_t g_work[1024];

static void init_keys(void)
{
    for(uint32_t i = 0u; i < 32u; i++) {
        g_pk[0][i] = (fwu_byte_t)(i + 1u);          /* anahtar 1 */
        g_pk[1][i] = (fwu_byte_t)(i + 101u);        /* anahtar 2 */
        g_pk[2][i] = (fwu_byte_t)(i + 201u);        /* SALDIRGANIN anahtarı: cihazda YOK */
        g_keys[0].public_key[i] = g_pk[0][i];
        g_keys[1].public_key[i] = g_pk[1][i];
    }
    g_keys[0].id = 1u;
    g_keys[1].id = 2u;
}

static fwu_byte_t img_byte(uint32_t i)
{
    return (fwu_byte_t)((i * 13u + 29u) & 0xFFu);
}

typedef struct {
    uint32_t   product;
    uint32_t   version;
    uint32_t   size;
    fwu_byte_t key_id;
    uint32_t   key_version;
} pkg_t;

static pkg_t good_pkg(void)
{
    pkg_t p;
    p.product = PRODUCT;
    p.version = 25u;
    p.size = 700u;
    p.key_id = 1u;
    p.key_version = 1u;
    return p;
}

/** Hangi anahtarla imzalanacak: id 1 -> anahtar 1, id 2 -> anahtar 2, başka -> saldırganın anahtarı. */
static const fwu_byte_t *signing_key(uint32_t key_index)
{
    return g_pk[key_index];
}

static void put_u32(fwu_byte_t *m, uint32_t offset, uint32_t value)
{
    m[offset] = (fwu_byte_t)((value >> 24) & 0xFFu);
    m[offset + 1u] = (fwu_byte_t)((value >> 16) & 0xFFu);
    m[offset + 2u] = (fwu_byte_t)((value >> 8) & 0xFFu);
    m[offset + 3u] = (fwu_byte_t)(value & 0xFFu);
}

/** İmajı (7 sayfa deseni) ve manifest'i flash belleğine DOĞRUDAN yazar; `signer` imzalayan anahtarın indeksi. */
static void write_package(vf_t *sim, const pkg_t *p, uint32_t signer)
{
    fwu_byte_t raw[138];

    for(uint32_t i = 0u; i < IMAGE_MAX; i++) {
        sim->base.memory[IMAGE_OFFSET + i] = img_byte(i);
    }
    raw[0] = 0x46u; raw[1] = 0x57u; raw[2] = 0x4Du; raw[3] = 0x46u;
    raw[4] = 0x01u;
    raw[5] = p->key_id;
    raw[6] = 0x00u; raw[7] = 0x00u;
    put_u32(raw, 8u, p->product);
    put_u32(raw, 12u, p->version);
    raw[16] = (fwu_byte_t)((p->key_version >> 8) & 0xFFu);
    raw[17] = (fwu_byte_t)(p->key_version & 0xFFu);
    put_u32(raw, 18u, p->size);
    fwu_crypto_sim_sha256(&sim->base.memory[IMAGE_OFFSET], p->size, &raw[22]);
    for(uint32_t i = 0u; i < 20u; i++) {
        raw[54u + i] = (fwu_byte_t)(0x30u + i);
    }
    fwu_crypto_sim_sign(signing_key(signer), raw, 74u, &raw[74]);
    for(uint32_t i = 0u; i < 138u; i++) {
        sim->base.memory[MANIFEST_OFFSET + i] = raw[i];
    }
}

/** Mevcut manifest'in ilk 74 elemanını `signer` anahtarıyla yeniden imzalar (saldırganın "geçerli imzası"). */
static void resign(vf_t *sim, uint32_t signer)
{
    fwu_byte_t sig[64];
    fwu_crypto_sim_sign(signing_key(signer), &sim->base.memory[MANIFEST_OFFSET], 74u, sig);
    for(uint32_t i = 0u; i < 64u; i++) {
        sim->base.memory[MANIFEST_OFFSET + 74u + i] = sig[i];
    }
}

static void flip_manifest_bit(vf_t *sim, uint32_t index, fwu_byte_t mask)
{
    sim->base.memory[MANIFEST_OFFSET + index] = (fwu_byte_t)(sim->base.memory[MANIFEST_OFFSET + index] ^ mask);
}

static void flip_image_bit(vf_t *sim, uint32_t index)
{
    sim->base.memory[IMAGE_OFFSET + index] = (fwu_byte_t)(sim->base.memory[IMAGE_OFFSET + index] ^ 0x04u);
}

static void make_cfg(fwu_verify_config_t *c, fwu_crypto_sim_t *cs)
{
    c->crypto = &cs->crypto;
    c->manifest_offset = MANIFEST_OFFSET;
    c->image_offset = IMAGE_OFFSET;
    c->max_image_size = IMAGE_MAX;
    c->product_id = PRODUCT;
    c->min_image_version = 25u;
    c->min_key_version = 1u;
    c->keys = g_keys;
    c->key_count = 2u;
    c->require_signature = true;
    c->work = g_work;
    c->work_length = 64u;
}

static int no_crypto_ops(const fwu_crypto_sim_t *c)
{
    return (c->verify_calls == 0u) && (c->begin_calls == 0u) && (c->update_calls == 0u) && (c->finish_calls == 0u);
}

static int signature_checked_but_no_hash(const fwu_crypto_sim_t *c)
{
    return (c->verify_calls == 1u) && (c->begin_calls == 0u) && (c->update_calls == 0u) && (c->finish_calls == 0u);
}

static int hash_was_computed(const fwu_crypto_sim_t *c)
{
    return (c->begin_calls == 1u) && (c->finish_calls == 1u) && (c->update_calls >= 1u);
}

/** Bir paket kurup doğrular: sonuç kodu ve (isteğe bağlı) çıktı değişmedi kontrolü. */
static fwu_return_e run_verify(vf_t *sim, fwu_crypto_sim_t *cs, const fwu_verify_config_t *cfg, fwu_manifest_t *out)
{
    (void)fwu_crypto_sim_init(cs);
    return fwu_verify_image(cfg, &sim->base.flash, out);
}

/* ------------------------------------------------------------------ 1. geçerli imaj */

static void test_valid(void)
{
    static vf_t sim;
    static fwu_crypto_sim_t cs;
    fwu_verify_config_t cfg;
    fwu_manifest_t m;
    pkg_t p = good_pkg();
    const uint32_t sizes[] = { 1u, 7u, 64u, 100u, 700u, 701u, 1000u };
    const uint32_t works[] = { 1u, 7u, 64u, 100u, 700u, 701u, 1000u };
    int works_ok = 1;
    int updates_ok = 1;

    printf("\n--- gecerli imaj ---\n");
    (void)vf_init(&sim);
    init_keys();
    write_package(&sim, &p, 0u);
    make_cfg(&cfg, &cs);
    m.product_id = 0xDEADBEEFu;
    expect("gecerli paket", run_verify(&sim, &cs, &cfg, &m), FWU_OK);
    check("manifest cikisi: urun, surum, boyut, key_id, key_version",
          m.product_id == PRODUCT && m.image_version == 25u && m.image_size == 700u && m.key_id == 1u && m.key_version == 1u);
    check("sayaclar: imza 1 kez, hash 1 begin + 1 finish, update = ceil(700/64) = 11",
          cs.verify_calls == 1u && cs.begin_calls == 1u && cs.finish_calls == 1u && cs.update_calls == 11u);
    check("imzalanan mesaj 74 eleman", cs.last_verify_message_length == 74u);
    check("ozet kapali (hashing false)", !cs.crypto.hashing);
    expect("manifest cikisi NULL olabilir", run_verify(&sim, &cs, &cfg, NULL), FWU_OK);
    expect("art arda ikinci dogrulama (ozet kapanmisti)", fwu_verify_image(&cfg, &sim.base.flash, &m), FWU_OK);

    for(unsigned k = 0u; k < 7u; k++) {
        cfg.work_length = works[k];
        if(run_verify(&sim, &cs, &cfg, &m) != FWU_OK) {
            works_ok = 0;
        }
        if(cs.update_calls != ((700u + works[k] - 1u) / works[k])) {
            updates_ok = 0;
        }
    }
    check("okuma tamponu 1, 7, 64, 100, 700, 701, 1000: hep gecerli", works_ok);
    check("update cagri sayisi her tampon boyutunda ceil(700 / tampon)", updates_ok);

    cfg.work_length = 64u;
    for(unsigned k = 0u; k < 7u; k++) {
        int ok;
        p = good_pkg();
        p.size = sizes[k];
        write_package(&sim, &p, 0u);
        ok = (run_verify(&sim, &cs, &cfg, &m) == FWU_OK) && (m.image_size == sizes[k]);
        check("imaj boyutu 1, 7, 64, 100, 700, 701, 1000: gecerli", ok);
    }
    p = good_pkg();
    p.size = IMAGE_MAX;
    write_package(&sim, &p, 0u);
    expect("imaj boyutu tam kapasite (1792)", run_verify(&sim, &cs, &cfg, &m), FWU_OK);
    p.size = IMAGE_MAX + 1u;
    write_package(&sim, &p, 0u);
    expect("imaj boyutu kapasite + 1", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_IMAGE_TOO_LARGE);
    check("... imzaya ve hash'e girilmedi", no_crypto_ops(&cs));
}

/* ------------------------------------------------------------------ 2. yapı ve ilke kontrolleri */

static void test_structure_and_policy(void)
{
    static vf_t sim;
    static fwu_crypto_sim_t cs;
    fwu_verify_config_t cfg;
    fwu_manifest_t m;
    pkg_t p;

    printf("\n--- yapi ve ilke kontrolleri: ucuz kontrol imzaya girmez ---\n");
    (void)vf_init(&sim);
    init_keys();
    make_cfg(&cfg, &cs);

    p = good_pkg();
    write_package(&sim, &p, 0u);
    flip_manifest_bit(&sim, 0u, 0x01u);
    expect("magic bozuk", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_BAD_MANIFEST);
    check("... imza ve hash yok", no_crypto_ops(&cs));
    write_package(&sim, &p, 0u);
    flip_manifest_bit(&sim, 4u, 0x03u);
    expect("bicim surumu bozuk", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_BAD_MANIFEST);
    write_package(&sim, &p, 0u);
    flip_manifest_bit(&sim, 7u, 0x01u);
    expect("ayrilmis alan sifir degil", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_BAD_MANIFEST);
    p.size = 0u;
    write_package(&sim, &p, 0u);
    expect("imaj boyutu 0", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_BAD_MANIFEST);
    check("... imza ve hash yok", no_crypto_ops(&cs));

    /* urun */
    p = good_pkg();
    p.product = PRODUCT + 1u;
    write_package(&sim, &p, 0u);
    expect("baska urun (+1)", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_PRODUCT_MISMATCH);
    check("... imza ve hash yok", no_crypto_ops(&cs));
    p.product = PRODUCT - 1u;
    write_package(&sim, &p, 0u);
    expect("baska urun (-1)", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_PRODUCT_MISMATCH);
    p.product = 0u;
    write_package(&sim, &p, 0u);
    expect("urun 0", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_PRODUCT_MISMATCH);

    /* surum: en dusuk 25, esit kabul */
    p = good_pkg();
    p.version = 24u;
    write_package(&sim, &p, 0u);
    expect("surum 24 < 25", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_VERSION_REJECTED);
    check("... imza ve hash yok", no_crypto_ops(&cs));
    p.version = 0u;
    write_package(&sim, &p, 0u);
    expect("surum 0", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_VERSION_REJECTED);
    p.version = 25u;
    write_package(&sim, &p, 0u);
    expect("surum 25 (esit, kabul)", run_verify(&sim, &cs, &cfg, &m), FWU_OK);
    p.version = 26u;
    write_package(&sim, &p, 0u);
    expect("surum 26", run_verify(&sim, &cs, &cfg, &m), FWU_OK);
    p.version = 0xFFFFFFFFu;
    write_package(&sim, &p, 0u);
    expect("surum 0xFFFFFFFF", run_verify(&sim, &cs, &cfg, &m), FWU_OK);
    cfg.min_image_version = 0u;
    p.version = 0u;
    write_package(&sim, &p, 0u);
    expect("en dusuk surum 0 iken surum 0 kabul", run_verify(&sim, &cs, &cfg, &m), FWU_OK);
    cfg.min_image_version = 0xFFFFFFFFu;
    p.version = 0xFFFFFFFEu;
    write_package(&sim, &p, 0u);
    expect("en dusuk surum 0xFFFFFFFF iken 0xFFFFFFFE", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_VERSION_REJECTED);
    cfg.min_image_version = 25u;

    /* anahtar */
    p = good_pkg();
    p.key_id = 3u;
    write_package(&sim, &p, 0u);
    expect("bilinmeyen anahtar kimligi (3)", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_KEY_REJECTED);
    check("... imza ve hash yok", no_crypto_ops(&cs));
    p.key_id = 0u;
    write_package(&sim, &p, 0u);
    expect("bilinmeyen anahtar kimligi (0)", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_KEY_REJECTED);
    p = good_pkg();
    p.key_version = 0u;
    write_package(&sim, &p, 0u);
    expect("anahtar surumu 0 < asgari 1", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_KEY_REJECTED);
    check("... imza ve hash yok", no_crypto_ops(&cs));
    p.key_version = 1u;
    write_package(&sim, &p, 0u);
    expect("anahtar surumu 1 (esit, kabul)", run_verify(&sim, &cs, &cfg, &m), FWU_OK);
    cfg.min_key_version = 2u;
    expect("asgari anahtar surumu 2'ye cikinca ayni paket", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_KEY_REJECTED);
    p.key_version = 2u;
    write_package(&sim, &p, 0u);
    expect("anahtar surumu 2", run_verify(&sim, &cs, &cfg, &m), FWU_OK);
    p.key_version = 0xFFFFu;
    write_package(&sim, &p, 0u);
    expect("anahtar surumu 0xFFFF", run_verify(&sim, &cs, &cfg, &m), FWU_OK);
    cfg.min_key_version = 1u;
    p = good_pkg();
    p.key_id = 2u;
    write_package(&sim, &p, 1u);
    expect("ikinci anahtar (kimlik 2, anahtar 2 ile imzali)", run_verify(&sim, &cs, &cfg, &m), FWU_OK);
    cfg.key_count = 1u;
    expect("yalniz tek anahtar guvenilir iken kimlik 2", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_KEY_REJECTED);
    cfg.key_count = 0u;
    cfg.keys = NULL;
    p = good_pkg();
    write_package(&sim, &p, 0u);
    expect("hic guvenilir anahtar yok", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_KEY_REJECTED);

    /* kontrol sirasi: urun hatasi anahtar hatasindan once */
    make_cfg(&cfg, &cs);
    p = good_pkg();
    p.product = PRODUCT + 1u;
    p.key_id = 3u;
    p.version = 1u;
    write_package(&sim, &p, 0u);
    expect("urun + surum + anahtar hepsi hatali: ilk kontrol (urun)", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_PRODUCT_MISMATCH);
    p.product = PRODUCT;
    write_package(&sim, &p, 0u);
    expect("surum + anahtar hatali: surum onde", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_VERSION_REJECTED);
    p.version = 25u;
    p.size = IMAGE_MAX + 1u;
    write_package(&sim, &p, 0u);
    expect("boyut + anahtar hatali: boyut onde", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_IMAGE_TOO_LARGE);
}

/* ------------------------------------------------------------------ 3. imza */

static void test_signature(void)
{
    static vf_t sim;
    static fwu_crypto_sim_t cs;
    fwu_verify_config_t cfg;
    fwu_manifest_t m;
    pkg_t p = good_pkg();
    int sig_ok = 1;
    int commit_ok = 1;
    int sha_ok = 1;
    int no_hash = 1;

    printf("\n--- imza: imzanin ve imzalanan alanin bozulmasi ---\n");
    (void)vf_init(&sim);
    init_keys();
    make_cfg(&cfg, &cs);

    for(uint32_t i = 0u; i < 64u; i++) {
        write_package(&sim, &p, 0u);
        flip_manifest_bit(&sim, 74u + i, 0x01u);
        if(run_verify(&sim, &cs, &cfg, &m) != FWU_ERR_BAD_SIGNATURE) {
            sig_ok = 0;
        }
        if(!signature_checked_but_no_hash(&cs)) {
            no_hash = 0;
        }
    }
    check("imzanin 64 elemanindan her birinde tek bit bozulma -> BAD_SIGNATURE", sig_ok);
    check("... her birinde imza dogrulandi AMA hash'e GIRILMEDI (imza hash'ten once)", no_hash);

    for(uint32_t i = 0u; i < 20u; i++) {
        write_package(&sim, &p, 0u);
        flip_manifest_bit(&sim, 54u + i, 0x80u);
        if(run_verify(&sim, &cs, &cfg, &m) != FWU_ERR_BAD_SIGNATURE) {
            commit_ok = 0;
        }
    }
    check("commit hash'in 20 elemanindan her birinde bozulma -> BAD_SIGNATURE (imzalanan alanda)", commit_ok);

    for(uint32_t i = 0u; i < 32u; i++) {
        write_package(&sim, &p, 0u);
        flip_manifest_bit(&sim, 22u + i, 0x01u);
        if(run_verify(&sim, &cs, &cfg, &m) != FWU_ERR_BAD_SIGNATURE) {
            sha_ok = 0;
        }
    }
    check("manifest'teki SHA-256'nin 32 elemanindan her birinde bozulma -> BAD_SIGNATURE (hash adimina gelmeden)", sha_ok);

    write_package(&sim, &p, 0u);
    put_u32(&sim.base.memory[MANIFEST_OFFSET], 12u, 30u);
    expect("imzadan sonra surum 25 -> 30 yapildi (saldirgan surum yukseltti)", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_BAD_SIGNATURE);
    write_package(&sim, &p, 0u);
    put_u32(&sim.base.memory[MANIFEST_OFFSET], 18u, 701u);
    expect("imzadan sonra imaj boyutu degistirildi", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_BAD_SIGNATURE);
    write_package(&sim, &p, 0u);
    flip_manifest_bit(&sim, 5u, 0x03u);
    expect("imzadan sonra key_id 1 -> 2 yapildi (baska anahtarla dogrulanir)", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_BAD_SIGNATURE);
    write_package(&sim, &p, 1u);
    expect("anahtar 2 ile imzali ama key_id 1", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_BAD_SIGNATURE);
    write_package(&sim, &p, 2u);
    expect("SALDIRGANIN anahtariyla imzali (cihazda yok), key_id 1", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_BAD_SIGNATURE);
    for(uint32_t i = 0u; i < 64u; i++) {
        sim.base.memory[MANIFEST_OFFSET + 74u + i] = 0u;
    }
    expect("imza tamamen sifir", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_BAD_SIGNATURE);
    for(uint32_t i = 0u; i < 64u; i++) {
        sim.base.memory[MANIFEST_OFFSET + 74u + i] = 0xFFu;
    }
    expect("imza tamamen 0xFF", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_BAD_SIGNATURE);
}

/* ------------------------------------------------------------------ 4. hash */

static void test_hash(void)
{
    static vf_t sim;
    static fwu_crypto_sim_t cs;
    fwu_verify_config_t cfg;
    fwu_manifest_t m;
    pkg_t p = good_pkg();
    const uint32_t positions[] = { 0u, 1u, 63u, 64u, 255u, 256u, 511u, 512u, 698u, 699u };
    int ok = 1;
    int computed = 1;

    printf("\n--- hash: imajin bozulmasi ---\n");
    (void)vf_init(&sim);
    init_keys();
    make_cfg(&cfg, &cs);

    for(unsigned k = 0u; k < 10u; k++) {
        write_package(&sim, &p, 0u);
        flip_image_bit(&sim, positions[k]);
        if(run_verify(&sim, &cs, &cfg, &m) != FWU_ERR_HASH_MISMATCH) {
            ok = 0;
        }
        if((cs.verify_calls != 1u) || !hash_was_computed(&cs) || cs.crypto.hashing) {
            computed = 0;
        }
    }
    check("imajin 10 farkli yerinde (ilk, son, sayfa ve parca sinirlari) tek bit -> HASH_MISMATCH", ok);
    check("... imza dogrulandi, hash HESAPLANDI, ozet kapandi", computed);

    write_package(&sim, &p, 0u);
    flip_image_bit(&sim, 700u);
    expect("imajin bittigi yerden SONRAKI bayt bozuk (700. eleman)", run_verify(&sim, &cs, &cfg, &m), FWU_OK);
    flip_image_bit(&sim, 1791u);
    expect("imaj alaninin sonu bozuk", run_verify(&sim, &cs, &cfg, &m), FWU_OK);

    /* manifest'teki ozetin HER elemani karsilastirilir (yalniz ilk/son elemanlara bakan bir kontrol yakalanir) */
    {
        int all_sha_bytes = 1;
        for(uint32_t i = 0u; i < 32u; i++) {
            write_package(&sim, &p, 0u);
            flip_manifest_bit(&sim, 22u + i, 0x01u);
            resign(&sim, 0u);
            if(run_verify(&sim, &cs, &cfg, &m) != FWU_ERR_HASH_MISMATCH) {
                all_sha_bytes = 0;
            }
        }
        check("manifest SHA-256'sinin 32 elemanindan her biri bozulup GECERLI imzalansa -> HASH_MISMATCH", all_sha_bytes);
    }
    write_package(&sim, &p, 0u);
    flip_image_bit(&sim, 10u);
    m.product_id = 0xDEADBEEFu;
    m.image_size = 0xDEADBEEFu;
    expect("hash reddi", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_HASH_MISMATCH);
    check("... manifest cikisi DEGISMEDI (cikis yalniz tum kontroller gectikten sonra yazilir)",
          m.product_id == 0xDEADBEEFu && m.image_size == 0xDEADBEEFu);

    /* saldirgan GECERLI imza uretebilseydi bile (anahtari var): hash yine tutmalidir */
    write_package(&sim, &p, 0u);
    flip_manifest_bit(&sim, 22u, 0x01u);
    resign(&sim, 0u);
    expect("manifest hash'i bozuk ama GECERLI imzali (imzalayan anahtar sahibi hatali)", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_HASH_MISMATCH);
    write_package(&sim, &p, 0u);
    put_u32(&sim.base.memory[MANIFEST_OFFSET], 18u, 699u);
    resign(&sim, 0u);
    expect("imaj boyutu 699 (gecerli imzali), hash 700 elemana ait", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_HASH_MISMATCH);
    write_package(&sim, &p, 0u);
    put_u32(&sim.base.memory[MANIFEST_OFFSET], 18u, 701u);
    resign(&sim, 0u);
    expect("imaj boyutu 701 (gecerli imzali)", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_HASH_MISMATCH);
}

/* ------------------------------------------------------------------ 5. DEV kipi */

static void test_dev_mode(void)
{
    static vf_t sim;
    static fwu_crypto_sim_t cs;
    fwu_verify_config_t cfg;
    fwu_manifest_t m;
    pkg_t p = good_pkg();

    printf("\n--- DEV kipi (imza zorunlu degil) ---\n");
    (void)vf_init(&sim);
    init_keys();
    make_cfg(&cfg, &cs);
    cfg.require_signature = false;
    cfg.keys = NULL;
    cfg.key_count = 0u;

    write_package(&sim, &p, 0u);
    for(uint32_t i = 0u; i < 64u; i++) {
        sim.base.memory[MANIFEST_OFFSET + 74u + i] = 0x00u;
    }
    expect("imza sifir ve anahtar yok: kabul", run_verify(&sim, &cs, &cfg, &m), FWU_OK);
    check("... imza HIC dogrulanmadi, hash hesaplandi", cs.verify_calls == 0u && hash_was_computed(&cs));
    p.key_id = 99u;
    p.key_version = 0u;
    write_package(&sim, &p, 0u);
    expect("bilinmeyen anahtar ve surum 0: kabul (anahtar kontrolu yok)", run_verify(&sim, &cs, &cfg, &m), FWU_OK);

    write_package(&sim, &p, 0u);
    flip_image_bit(&sim, 10u);
    expect("imaj bozuk: yine HASH_MISMATCH", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_HASH_MISMATCH);
    p = good_pkg();
    p.product = 1u;
    write_package(&sim, &p, 0u);
    expect("baska urun: yine reddedilir", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_PRODUCT_MISMATCH);
    p = good_pkg();
    p.version = 1u;
    write_package(&sim, &p, 0u);
    expect("eski surum: yine reddedilir", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_VERSION_REJECTED);
    p = good_pkg();
    p.size = IMAGE_MAX + 1u;
    write_package(&sim, &p, 0u);
    expect("cok buyuk imaj: yine reddedilir", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_IMAGE_TOO_LARGE);
    p = good_pkg();
    write_package(&sim, &p, 0u);
    flip_manifest_bit(&sim, 1u, 0x01u);
    expect("manifest yapisi bozuk: yine reddedilir", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_BAD_MANIFEST);
}

/* ------------------------------------------------------------------ 6. hatalar */

#define FAULT_READ FWU_ERR_ERASE_ALIGNMENT      /* enjekte okuma hatasi: baska yoldan uretilemeyen bir kod */

typedef struct {
    fwu_flash_t          flash;
    fwu_flash_callback_t callback;
    vf_t                *sim;
    uint32_t             fail_read_offset;
} faulty_t;

static fwu_return_e faulty_erase(void *context, uint32_t offset, uint32_t length)
{
    faulty_t *f = (faulty_t *)context;
    return fwu_flash_erase(&f->sim->base.flash, offset, length);
}

static fwu_return_e faulty_write(void *context, uint32_t offset, uint32_t length, const fwu_byte_t *data)
{
    faulty_t *f = (faulty_t *)context;
    return fwu_flash_write(&f->sim->base.flash, offset, length, data);
}

static fwu_return_e faulty_read(void *context, uint32_t offset, uint32_t length, fwu_byte_t *data)
{
    faulty_t *f = (faulty_t *)context;
    if((f->fail_read_offset >= offset) && (f->fail_read_offset < (offset + length))) {
        return FAULT_READ;
    }
    return fwu_flash_read(&f->sim->base.flash, offset, length, data);
}

static void faulty_init(faulty_t *f, vf_t *sim)
{
    f->callback.flash_erase = faulty_erase;
    f->callback.flash_write = faulty_write;
    f->callback.flash_read = faulty_read;
    f->flash.context = f;
    f->flash.callback = &f->callback;
    f->flash.geometry = sim->base.flash.geometry;
    f->sim = sim;
    f->fail_read_offset = 0xFFFFFFFFu;
}

static void test_failures(void)
{
    static vf_t sim;
    static fwu_crypto_sim_t cs;
    static faulty_t faulty;
    fwu_verify_config_t cfg;
    fwu_manifest_t m;
    pkg_t p = good_pkg();
    const uint32_t cut_points[] = { IMAGE_OFFSET, IMAGE_OFFSET + 63u, IMAGE_OFFSET + 64u, IMAGE_OFFSET + 640u, IMAGE_OFFSET + 699u };
    int closed_ok = 1;
    int code_ok = 1;
    int recover_ok = 1;

    printf("\n--- hatalar: kripto ve flash; 'yapilamadi' REDDEDILMEZ, ozet kapanir ---\n");
    (void)vf_init(&sim);
    init_keys();
    write_package(&sim, &p, 0u);
    make_cfg(&cfg, &cs);

    (void)fwu_crypto_sim_init(&cs);
    cs.fail_verify = FWU_ERR_CRYPTO_FAILURE;
    expect("imza dogrulama yapilamadi", fwu_verify_image(&cfg, &sim.base.flash, &m), FWU_ERR_CRYPTO_FAILURE);
    check("... hash'e girilmedi", cs.begin_calls == 0u);
    cs.fail_verify = FWU_ERR_BAD_SIGNATURE;
    expect("kripto portu BAD_SIGNATURE doner", fwu_verify_image(&cfg, &sim.base.flash, &m), FWU_ERR_BAD_SIGNATURE);
    cs.fail_verify = FWU_ERR_FLASH_WRITE;
    expect("kripto portu saçma kod doner: CRYPTO_FAILURE (imaj reddedilmez)", fwu_verify_image(&cfg, &sim.base.flash, &m), FWU_ERR_CRYPTO_FAILURE);
    cs.fail_verify = FWU_OK;

    cs.fail_begin = FWU_ERR_CRYPTO_FAILURE;
    expect("SHA-256 begin yapilamadi", fwu_verify_image(&cfg, &sim.base.flash, &m), FWU_ERR_CRYPTO_FAILURE);
    check("... ozet acik degil", !cs.crypto.hashing);
    cs.fail_begin = FWU_OK;
    cs.fail_update = FWU_ERR_CRYPTO_FAILURE;
    expect("SHA-256 update yapilamadi", fwu_verify_image(&cfg, &sim.base.flash, &m), FWU_ERR_CRYPTO_FAILURE);
    check("... ozet KAPATILDI", !cs.crypto.hashing);
    cs.fail_update = FWU_OK;
    cs.fail_finish = FWU_ERR_CRYPTO_FAILURE;
    expect("SHA-256 finish yapilamadi", fwu_verify_image(&cfg, &sim.base.flash, &m), FWU_ERR_CRYPTO_FAILURE);
    check("... ozet KAPALI", !cs.crypto.hashing);
    cs.fail_finish = FWU_OK;
    expect("sorunlar giderilince ayni paket gecerli (temiz basladi)", fwu_verify_image(&cfg, &sim.base.flash, &m), FWU_OK);

    /* flash: manifest okunamaz */
    faulty_init(&faulty, &sim);
    faulty.fail_read_offset = MANIFEST_OFFSET + 10u;
    (void)fwu_crypto_sim_init(&cs);
    expect("manifest okunamadi: hata aynen iletilir (REDDEDILMEZ)", fwu_verify_image(&cfg, &faulty.flash, &m), FAULT_READ);
    check("... kripto hic cagrilmadi", no_crypto_ops(&cs));

    /* flash: imajin ORTASINDA okuma hatasi: ozet kapanmali */
    for(unsigned k = 0u; k < 5u; k++) {
        faulty.fail_read_offset = cut_points[k];
        (void)fwu_crypto_sim_init(&cs);
        if(fwu_verify_image(&cfg, &faulty.flash, &m) != FAULT_READ) {
            code_ok = 0;
        }
        if(cs.crypto.hashing) {
            closed_ok = 0;
        }
        faulty.fail_read_offset = 0xFFFFFFFFu;
        if(fwu_verify_image(&cfg, &faulty.flash, &m) != FWU_OK) {
            recover_ok = 0;
        }
    }
    check("imajin 5 farkli yerinde okuma hatasi: hata aynen iletilir", code_ok);
    check("... SHA-256 ozeti kapatilir (hashing false)", closed_ok);
    check("... hata giderilince ayni kripto nesnesiyle dogrulama calisir", recover_ok);

    /* guc kesintisi: sahte flash her okumada hata verir */
    (void)fwu_crypto_sim_init(&cs);
    fwu_flash_sim_reset(&sim.base);
    fwu_flash_sim_cut_power_after(&sim.base, 0u);
    (void)fwu_flash_erase(&sim.base.flash, 0u, PAGE);                   /* gucu keser */
    expect("guc kesik: okuma hatasi (FLASH_POWER_LOSS) iletilir", fwu_verify_image(&cfg, &sim.base.flash, &m), FWU_ERR_FLASH_POWER_LOSS);
    fwu_flash_sim_reset(&sim.base);
}

/* ------------------------------------------------------------------ 7. argumanlar ve aktivasyon kancasi */

static void test_arguments_and_hook(void)
{
    static vf_t sim;
    static fwu_crypto_sim_t cs;
    fwu_verify_config_t cfg;
    fwu_manifest_t m;
    pkg_t p = good_pkg();

    printf("\n--- argumanlar ve aktivasyon kancasi ---\n");
    (void)vf_init(&sim);
    init_keys();
    write_package(&sim, &p, 0u);
    make_cfg(&cfg, &cs);
    (void)fwu_crypto_sim_init(&cs);

    expect("config NULL", fwu_verify_image(NULL, &sim.base.flash, &m), FWU_ERR_INVALID_ARG);
    expect("flash NULL", fwu_verify_image(&cfg, NULL, &m), FWU_ERR_INVALID_ARG);
    cfg.crypto = NULL;
    expect("crypto NULL", fwu_verify_image(&cfg, &sim.base.flash, &m), FWU_ERR_INVALID_ARG);
    make_cfg(&cfg, &cs);
    cfg.work = NULL;
    expect("work NULL", fwu_verify_image(&cfg, &sim.base.flash, &m), FWU_ERR_INVALID_ARG);
    make_cfg(&cfg, &cs);
    cfg.work_length = 0u;
    expect("work_length 0", fwu_verify_image(&cfg, &sim.base.flash, &m), FWU_ERR_INVALID_ARG);
    make_cfg(&cfg, &cs);
    cfg.keys = NULL;
    expect("key_count 2 ama keys NULL", fwu_verify_image(&cfg, &sim.base.flash, &m), FWU_ERR_INVALID_ARG);
    make_cfg(&cfg, &cs);
    cfg.image_offset = 0xFFFFFFF0u;
    cfg.max_image_size = 0x100u;
    expect("image_offset + max_image_size 32 bit'te sarar", fwu_verify_image(&cfg, &sim.base.flash, &m), FWU_ERR_INVALID_ARG);
    cfg.image_offset = 0xFFFFFE00u;
    cfg.max_image_size = 0x1FFu;
    check("sarmanin hemen altinda (toplam tam 0xFFFFFFFF): arguman gecerli, INVALID_ARG DEGIL",
          fwu_verify_image(&cfg, &sim.base.flash, &m) != FWU_ERR_INVALID_ARG);

    /* hatada cikti degismez */
    make_cfg(&cfg, &cs);
    p.version = 1u;
    write_package(&sim, &p, 0u);
    m.product_id = 0xDEADBEEFu;
    m.image_size = 0xDEADBEEFu;
    expect("reddedilen imaj", fwu_verify_image(&cfg, &sim.base.flash, &m), FWU_ERR_VERSION_REJECTED);
    check("... manifest cikisi DEGISMEDI", m.product_id == 0xDEADBEEFu && m.image_size == 0xDEADBEEFu);

    /* aktivasyon kancasi */
    p = good_pkg();
    write_package(&sim, &p, 0u);
    expect("kanca: context NULL", fwu_verify_activation_hook(NULL, &sim.base.flash, IMAGE_OFFSET, 768u), FWU_ERR_INVALID_ARG);
    expect("kanca: flash NULL", fwu_verify_activation_hook(&cfg, NULL, IMAGE_OFFSET, 768u), FWU_ERR_INVALID_ARG);
    expect("kanca: ofset image_offset'ten farkli (+1)", fwu_verify_activation_hook(&cfg, &sim.base.flash, IMAGE_OFFSET + 1u, 768u), FWU_ERR_INVALID_ARG);
    expect("kanca: ofset image_offset'ten farkli (-1)", fwu_verify_activation_hook(&cfg, &sim.base.flash, IMAGE_OFFSET - 1u, 768u), FWU_ERR_INVALID_ARG);
    (void)fwu_crypto_sim_init(&cs);
    expect("kanca: commit alani 699 < imaj boyutu 700", fwu_verify_activation_hook(&cfg, &sim.base.flash, IMAGE_OFFSET, 699u), FWU_ERR_IMAGE_TOO_LARGE);
    check("... imza ve hash'e girilmedi", no_crypto_ops(&cs));
    expect("kanca: commit alani 0", fwu_verify_activation_hook(&cfg, &sim.base.flash, IMAGE_OFFSET, 0u), FWU_ERR_IMAGE_TOO_LARGE);
    expect("kanca: commit alani tam 700", fwu_verify_activation_hook(&cfg, &sim.base.flash, IMAGE_OFFSET, 700u), FWU_OK);
    expect("kanca: commit alani 768 (3 sayfa)", fwu_verify_activation_hook(&cfg, &sim.base.flash, IMAGE_OFFSET, 768u), FWU_OK);
    expect("kanca: commit alani cok genis (1792)", fwu_verify_activation_hook(&cfg, &sim.base.flash, IMAGE_OFFSET, 1792u), FWU_OK);
    flip_image_bit(&sim, 5u);
    expect("kanca: bozuk imaj, yeterli alan", fwu_verify_activation_hook(&cfg, &sim.base.flash, IMAGE_OFFSET, 768u), FWU_ERR_HASH_MISMATCH);

    /* commit alani kapasiteden GENIS olsa bile imaj kapasiteyi asamaz */
    p = good_pkg();
    p.size = IMAGE_MAX + 1u;
    write_package(&sim, &p, 0u);
    (void)fwu_crypto_sim_init(&cs);
    expect("kanca: imaj kapasite + 1, commit alani 4096 (cok genis)", fwu_verify_activation_hook(&cfg, &sim.base.flash, IMAGE_OFFSET, 4096u), FWU_ERR_IMAGE_TOO_LARGE);
    check("... imza ve hash'e girilmedi", no_crypto_ops(&cs));

    /* C28x: anahtar kimligi kirli ust bitli olsa da anahtar bulunur (8 bitlik platformda etkisiz) */
    p = good_pkg();
    write_package(&sim, &p, 0u);
    g_keys[0].id = (fwu_byte_t)(1u | 0xAB00u);
    expect("anahtar kimligi kirli ust bitli (| 0xAB00): anahtar yine bulunur", run_verify(&sim, &cs, &cfg, &m), FWU_OK);
    g_keys[0].id = 1u;
}

/* ------------------------------------------------------------------ 8. aktivasyonla uctan uca */

static fwu_byte_t g_act_work[64];

static void act_config(fwu_activate_config_t *c, vf_t *sim, fwu_verify_config_t *vc, fwu_activate_mode_e mode)
{
    c->flash = &sim->base.flash;
    c->mode = mode;
    c->app_offset = 0u;
    c->staging_offset = IMAGE_OFFSET;       /* aktivasyon imaji gorur: manifest sayfasi takasa/kopyaya girmez */
    c->region_length = IMAGE_MAX;
    c->journal_a_offset = 16u * PAGE;
    c->journal_b_offset = 17u * PAGE;
    c->journal_length = PAGE;
    c->scratch_offset = 18u * PAGE;
    c->scratch_pages = 1u;
    c->work = g_act_work;
    c->work_length = 64u;
    c->max_trial_boots = 2u;
    c->require_verify = true;
    c->verify = fwu_verify_activation_hook;
    c->verify_context = vc;
    c->on_confirmed = NULL;
    c->on_confirmed_context = NULL;
}

static fwu_byte_t old_byte(uint32_t i)
{
    return (fwu_byte_t)((i * 5u + 1u) & 0xFFu);
}

static void fill_old_app(vf_t *sim)
{
    for(uint32_t i = 0u; i < IMAGE_MAX; i++) {
        sim->base.memory[i] = old_byte(i);
    }
}

static int app_is_old(const vf_t *sim)
{
    for(uint32_t i = 0u; i < IMAGE_MAX; i++) {
        if((sim->base.memory[i] & 0xFFu) != old_byte(i)) {
            return 0;
        }
    }
    return 1;
}

static int app_has_image(const vf_t *sim, uint32_t size)
{
    for(uint32_t i = 0u; i < size; i++) {
        if((sim->base.memory[i] & 0xFFu) != img_byte(i)) {
            return 0;
        }
    }
    return 1;
}

static int app_never_erased(const vf_t *sim)
{
    for(uint32_t page = 0u; page < 8u; page++) {
        if(sim->base.erase_counts[page] != 0u) {
            return 0;
        }
    }
    return 1;
}

/** commit + yeniden baslat + run. */
static fwu_return_e activate_flow(fwu_activate_t *act, const fwu_activate_config_t *c, vf_t *sim, uint32_t pages,
                                  fwu_activate_outcome_e *outcome)
{
    fwu_return_e status = fwu_activate_init(act, c);
    if(status != FWU_OK) {
        return status;
    }
    status = fwu_activate_commit(act, pages);
    if(status != FWU_OK) {
        return status;
    }
    fwu_flash_sim_reset(&sim->base);
    status = fwu_activate_init(act, c);
    if(status != FWU_OK) {
        return status;
    }
    return fwu_activate_run(act, outcome);
}

static void test_activation_end_to_end(void)
{
    static vf_t sim;
    static fwu_crypto_sim_t cs;
    static fwu_activate_t act;
    fwu_verify_config_t vc;
    fwu_activate_config_t ac;
    fwu_activate_outcome_e outcome = FWU_ACT_OUTCOME_NONE;
    pkg_t p;

    printf("\n--- aktivasyonla uctan uca (dogrulama hatti = aktivasyonun verify kancasi) ---\n");
    init_keys();
    (void)fwu_crypto_sim_init(&cs);
    make_cfg(&vc, &cs);

    /* COPY: gecerli imaj */
    (void)vf_init(&sim);
    fill_old_app(&sim);
    p = good_pkg();
    write_package(&sim, &p, 0u);
    act_config(&ac, &sim, &vc, FWU_ACTIVATE_MODE_COPY);
    expect("COPY: gecerli imaj (700 eleman = 3 sayfa)", activate_flow(&act, &ac, &sim, 3u, &outcome), FWU_OK);
    check("... ACTIVATED, uygulama = imaj (ilk 700 eleman)", outcome == FWU_ACT_OUTCOME_ACTIVATED && app_has_image(&sim, 700u));
    check("... imza ve hash dogrulandi", cs.verify_calls == 1u && cs.finish_calls == 1u);

    /* COPY: bozuk imaj */
    (void)vf_init(&sim);
    fill_old_app(&sim);
    write_package(&sim, &p, 0u);
    flip_image_bit(&sim, 300u);
    act_config(&ac, &sim, &vc, FWU_ACTIVATE_MODE_COPY);
    expect("COPY: bozuk imaj (hash tutmuyor)", activate_flow(&act, &ac, &sim, 3u, &outcome), FWU_OK);
    check("... REJECTED, uygulamaya HIC dokunulmadi (eski surum, hic silinmedi)",
          outcome == FWU_ACT_OUTCOME_REJECTED && app_is_old(&sim) && app_never_erased(&sim) &&
          act.last_state == FWU_ACT_STATE_REJECTED);

    /* COPY: sahte imza */
    (void)vf_init(&sim);
    fill_old_app(&sim);
    write_package(&sim, &p, 2u);
    expect("COPY: saldirganin anahtariyla imzali imaj", activate_flow(&act, &ac, &sim, 3u, &outcome), FWU_OK);
    check("... REJECTED, uygulama eski", outcome == FWU_ACT_OUTCOME_REJECTED && app_is_old(&sim) && app_never_erased(&sim));

    /* COPY: commit eksik sayfa verdi */
    (void)vf_init(&sim);
    fill_old_app(&sim);
    write_package(&sim, &p, 0u);
    expect("COPY: commit 2 sayfa (512 < 700): kesik kopyalanirdi", activate_flow(&act, &ac, &sim, 2u, &outcome), FWU_OK);
    check("... REJECTED (IMAGE_TOO_LARGE), uygulama eski", outcome == FWU_ACT_OUTCOME_REJECTED && app_is_old(&sim) && app_never_erased(&sim));

    /* COPY: eski surum, yanlis urun, reddedilen anahtar */
    p = good_pkg();
    p.version = 3u;
    (void)vf_init(&sim);
    fill_old_app(&sim);
    write_package(&sim, &p, 0u);
    (void)activate_flow(&act, &ac, &sim, 3u, &outcome);
    check("COPY: eski surum (anti-rollback) -> REJECTED, uygulama eski", outcome == FWU_ACT_OUTCOME_REJECTED && app_is_old(&sim));
    p = good_pkg();
    p.product = 7u;
    (void)vf_init(&sim);
    fill_old_app(&sim);
    write_package(&sim, &p, 0u);
    (void)activate_flow(&act, &ac, &sim, 3u, &outcome);
    check("COPY: baska urunun imaji -> REJECTED, uygulama eski", outcome == FWU_ACT_OUTCOME_REJECTED && app_is_old(&sim));
    p = good_pkg();
    p.key_id = 9u;
    (void)vf_init(&sim);
    fill_old_app(&sim);
    write_package(&sim, &p, 0u);
    (void)activate_flow(&act, &ac, &sim, 3u, &outcome);
    check("COPY: bilinmeyen anahtar -> REJECTED, uygulama eski", outcome == FWU_ACT_OUTCOME_REJECTED && app_is_old(&sim));

    /* COPY: kripto hatasi = REDDEDILMEZ, hata iletilir, duzelince devam */
    p = good_pkg();
    (void)vf_init(&sim);
    fill_old_app(&sim);
    write_package(&sim, &p, 0u);
    (void)fwu_crypto_sim_init(&cs);
    cs.fail_verify = FWU_ERR_CRYPTO_FAILURE;
    expect("COPY: kripto yapilamadi: run hatayi dondurur (bootloader atlamaz)", activate_flow(&act, &ac, &sim, 3u, &outcome), FWU_ERR_CRYPTO_FAILURE);
    check("... REJECTED YAZILMADI, evre ACTIVATING, uygulama eski", act.phase == FWU_ACT_PHASE_ACTIVATING &&
          act.last_state == FWU_ACT_STATE_COMMIT && app_is_old(&sim) && app_never_erased(&sim));
    cs.fail_verify = FWU_OK;
    fwu_flash_sim_reset(&sim.base);
    (void)fwu_activate_init(&act, &ac);
    expect("... sorun giderilince sonraki acilista ayni commit tamamlanir", fwu_activate_run(&act, &outcome), FWU_OK);
    check("... ACTIVATED, uygulama = imaj", outcome == FWU_ACT_OUTCOME_ACTIVATED && app_has_image(&sim, 700u));

    /* SWAP: gecerli imaj -> TRIAL; sahte imza -> REJECTED */
    (void)fwu_crypto_sim_init(&cs);
    (void)vf_init(&sim);
    fill_old_app(&sim);
    write_package(&sim, &p, 0u);
    act_config(&ac, &sim, &vc, FWU_ACTIVATE_MODE_SWAP);
    expect("SWAP: gecerli imaj", activate_flow(&act, &ac, &sim, 3u, &outcome), FWU_OK);
    check("... TRIAL, uygulama = imaj", outcome == FWU_ACT_OUTCOME_TRIAL && app_has_image(&sim, 700u));
    {
        int staging_has_old = 1;
        for(uint32_t i = 0u; i < 3u * PAGE; i++) {
            if((sim.base.memory[IMAGE_OFFSET + i] & 0xFFu) != old_byte(i)) {
                staging_has_old = 0;
            }
        }
        check("... staging imaj bolgesinde ESKI imajin ilk 3 sayfasi (yer degisti)", staging_has_old);
        check("... manifest sayfasi takasa girmedi: manifest hala orada (magic 'F')",
              (sim.base.memory[MANIFEST_OFFSET] & 0xFFu) == 0x46u && (sim.base.memory[MANIFEST_OFFSET + 1u] & 0xFFu) == 0x57u);
    }
    (void)vf_init(&sim);
    fill_old_app(&sim);
    write_package(&sim, &p, 2u);
    expect("SWAP: sahte imza", activate_flow(&act, &ac, &sim, 3u, &outcome), FWU_OK);
    check("... REJECTED, uygulamaya ve geciciye hic dokunulmadi", outcome == FWU_ACT_OUTCOME_REJECTED && app_is_old(&sim) &&
          app_never_erased(&sim) && sim.base.erase_counts[18] == 0u);

    /* DEV kipi: imza yok, bozuk imaj yine reddedilir */
    (void)vf_init(&sim);
    fill_old_app(&sim);
    vc.require_signature = false;
    write_package(&sim, &p, 2u);
    act_config(&ac, &sim, &vc, FWU_ACTIVATE_MODE_COPY);
    expect("DEV kipi: sahte imzali ama saglam imaj", activate_flow(&act, &ac, &sim, 3u, &outcome), FWU_OK);
    check("... kabul (imza kontrol edilmez)", outcome == FWU_ACT_OUTCOME_ACTIVATED && app_has_image(&sim, 700u));
    (void)vf_init(&sim);
    fill_old_app(&sim);
    write_package(&sim, &p, 0u);
    flip_image_bit(&sim, 1u);
    (void)activate_flow(&act, &ac, &sim, 3u, &outcome);
    check("DEV kipi: bozuk imaj yine REJECTED", outcome == FWU_ACT_OUTCOME_REJECTED && app_is_old(&sim));
}

/* ------------------------------------------------------------------ 9. yalnızca manifest doğrulaması */

static void test_manifest_only(void)
{
    static vf_t sim;
    static fwu_crypto_sim_t cs;
    static faulty_t faulty;
    fwu_verify_config_t cfg;
    fwu_manifest_t m;
    pkg_t p = good_pkg();
    int unchanged_ok = 1;

    printf("\n--- fwu_verify_manifest: yalniz manifest (yapi, urun, anahtar, imza); surum/boyut/hash'e bakmaz ---\n");
    (void)vf_init(&sim);
    init_keys();
    write_package(&sim, &p, 0u);
    make_cfg(&cfg, &cs);

    /* gecerli paket */
    (void)fwu_crypto_sim_init(&cs);
    m.image_version = 0xDEADBEEFu;
    expect("gecerli paket", fwu_verify_manifest(&cfg, &sim.base.flash, &m), FWU_OK);
    check("... manifest alanlari: urun, surum, anahtar surumu, boyut, anahtar kimligi",
          m.product_id == PRODUCT && m.image_version == 25u && m.key_version == 1u && m.image_size == 700u && m.key_id == 1u);
    check("... imza dogrulandi (1 kez), HASH HESAPLANMADI", signature_checked_but_no_hash(&cs));
    check("... imzalanan alan 74 eleman", cs.last_verify_message_length == 74u);
    expect("manifest cikisi NULL olabilir", fwu_verify_manifest(&cfg, &sim.base.flash, NULL), FWU_OK);

    /* imaj bozuk ya da surum/boyut kabul disi olsa da manifest gercek: bu islev yalniz manifest'e bakar */
    flip_image_bit(&sim, 100u);
    (void)fwu_crypto_sim_init(&cs);
    expect("imajin bir biti bozuk: manifest yine gecerli", fwu_verify_manifest(&cfg, &sim.base.flash, &m), FWU_OK);
    check("... imaj okunmadi, hash yok", signature_checked_but_no_hash(&cs));
    expect("... ayni imaj tam hat ile HASH_MISMATCH", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_HASH_MISMATCH);
    write_package(&sim, &p, 0u);
    cfg.min_image_version = 999u;
    expect("surum asgarinin altinda: manifest gecerli", fwu_verify_manifest(&cfg, &sim.base.flash, &m), FWU_OK);
    expect("... ayni paket tam hat ile VERSION_REJECTED", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_VERSION_REJECTED);
    cfg.min_image_version = 25u;
    p.size = 1800u;                                     /* IMAGE_MAX (1792) asar, flash'a yine de sigar */
    write_package(&sim, &p, 0u);
    expect("boyut kapasiteyi asiyor: manifest gecerli", fwu_verify_manifest(&cfg, &sim.base.flash, &m), FWU_OK);
    expect("... ayni paket tam hat ile IMAGE_TOO_LARGE", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_IMAGE_TOO_LARGE);
    p = good_pkg();
    write_package(&sim, &p, 0u);

    /* reddedilen manifest'ler: sira ucuzdan pahaliya, cikis degismez */
    m.image_version = 0xDEADBEEFu;
    (void)fwu_crypto_sim_init(&cs);
    flip_manifest_bit(&sim, 0u, 0x01u);
    expect("magic bozuk", fwu_verify_manifest(&cfg, &sim.base.flash, &m), FWU_ERR_BAD_MANIFEST);
    check("... kripto hic cagrilmadi", no_crypto_ops(&cs));
    unchanged_ok = unchanged_ok && (m.image_version == 0xDEADBEEFu);
    write_package(&sim, &p, 0u);

    p.product = PRODUCT + 1u;
    write_package(&sim, &p, 0u);
    expect("baska urunun manifest'i", fwu_verify_manifest(&cfg, &sim.base.flash, &m), FWU_ERR_PRODUCT_MISMATCH);
    check("... kripto hic cagrilmadi", no_crypto_ops(&cs));
    unchanged_ok = unchanged_ok && (m.image_version == 0xDEADBEEFu);

    p = good_pkg();
    p.key_id = 3u;
    write_package(&sim, &p, 0u);
    expect("bilinmeyen anahtar kimligi", fwu_verify_manifest(&cfg, &sim.base.flash, &m), FWU_ERR_KEY_REJECTED);
    check("... imza dogrulamasina girilmedi", no_crypto_ops(&cs));
    p = good_pkg();
    p.key_version = 0u;
    write_package(&sim, &p, 0u);
    expect("anahtar surumu asgarinin altinda", fwu_verify_manifest(&cfg, &sim.base.flash, &m), FWU_ERR_KEY_REJECTED);
    check("... imza dogrulamasina girilmedi", no_crypto_ops(&cs));
    cfg.key_count = 0u;
    cfg.keys = NULL;
    p = good_pkg();
    write_package(&sim, &p, 0u);
    expect("hic guvenilir anahtar yok", fwu_verify_manifest(&cfg, &sim.base.flash, &m), FWU_ERR_KEY_REJECTED);
    make_cfg(&cfg, &cs);

    /* SALDIRI: imzali alan degistirilip imza yeniden uretilmeden sayac yukseltilmek istenir */
    write_package(&sim, &p, 0u);
    flip_manifest_bit(&sim, 15u, 0x80u);                /* surum alani (imzali bolge), imza ayni */
    (void)fwu_crypto_sim_init(&cs);
    expect("surum alani degistirilmis, imza eski", fwu_verify_manifest(&cfg, &sim.base.flash, &m), FWU_ERR_BAD_SIGNATURE);
    check("... imza dogrulandi (1 kez), hash yok", signature_checked_but_no_hash(&cs));
    unchanged_ok = unchanged_ok && (m.image_version == 0xDEADBEEFu);
    write_package(&sim, &p, 2u);                        /* saldirganin anahtariyla imzali, kimlik 1 */
    expect("saldirganin anahtariyla imzali", fwu_verify_manifest(&cfg, &sim.base.flash, &m), FWU_ERR_BAD_SIGNATURE);
    check("... hatada cikis degismedi (5 reddedilen durum)", unchanged_ok);

    /* kripto hatalari: yapilamadi */
    write_package(&sim, &p, 0u);
    (void)fwu_crypto_sim_init(&cs);
    cs.fail_verify = FWU_ERR_CRYPTO_FAILURE;
    expect("kripto: imza dogrulama yapilamadi", fwu_verify_manifest(&cfg, &sim.base.flash, &m), FWU_ERR_CRYPTO_FAILURE);
    cs.fail_verify = FWU_ERR_FLASH_WRITE;
    expect("kripto portu sacma kod doner: CRYPTO_FAILURE", fwu_verify_manifest(&cfg, &sim.base.flash, &m), FWU_ERR_CRYPTO_FAILURE);
    cs.fail_verify = FWU_OK;
    expect("hata giderilince gecerli", fwu_verify_manifest(&cfg, &sim.base.flash, &m), FWU_OK);

    /* flash okuma hatasi */
    faulty_init(&faulty, &sim);
    faulty.fail_read_offset = MANIFEST_OFFSET + 10u;
    (void)fwu_crypto_sim_init(&cs);
    expect("manifest okunamadi: hata aynen iletilir", fwu_verify_manifest(&cfg, &faulty.flash, &m), FAULT_READ);
    check("... kripto hic cagrilmadi", no_crypto_ops(&cs));

    /* DEV kipi: imza yok; ama yapi ve urun yine denetlenir */
    cfg.require_signature = false;
    p.key_id = 9u;                                      /* bilinmeyen kimlik + yanlis anahtarla imza */
    write_package(&sim, &p, 2u);
    (void)fwu_crypto_sim_init(&cs);
    expect("DEV: imza ve anahtar atlanir (bilinmeyen kimlik, yanlis imza)", fwu_verify_manifest(&cfg, &sim.base.flash, &m), FWU_OK);
    check("... kripto hic cagrilmadi", no_crypto_ops(&cs));
    p = good_pkg();
    p.product = PRODUCT + 1u;
    write_package(&sim, &p, 0u);
    expect("DEV: urun yine denetlenir", fwu_verify_manifest(&cfg, &sim.base.flash, &m), FWU_ERR_PRODUCT_MISMATCH);
    flip_manifest_bit(&sim, 1u, 0x01u);
    expect("DEV: yapi yine denetlenir", fwu_verify_manifest(&cfg, &sim.base.flash, &m), FWU_ERR_BAD_MANIFEST);
    make_cfg(&cfg, &cs);

    /* argumanlar: imaj okumasina ait alanlar KULLANILMAZ */
    p = good_pkg();
    write_package(&sim, &p, 0u);
    expect("config NULL", fwu_verify_manifest(NULL, &sim.base.flash, &m), FWU_ERR_INVALID_ARG);
    expect("flash NULL", fwu_verify_manifest(&cfg, NULL, &m), FWU_ERR_INVALID_ARG);
    cfg.crypto = NULL;
    expect("crypto NULL", fwu_verify_manifest(&cfg, &sim.base.flash, &m), FWU_ERR_INVALID_ARG);
    cfg.require_signature = false;
    expect("crypto NULL, DEV kipinde de (imza istenmese bile arayuz sozlesmesi)", fwu_verify_manifest(&cfg, &sim.base.flash, &m), FWU_ERR_INVALID_ARG);
    make_cfg(&cfg, &cs);
    cfg.keys = NULL;
    expect("key_count 2 ama keys NULL", fwu_verify_manifest(&cfg, &sim.base.flash, &m), FWU_ERR_INVALID_ARG);
    make_cfg(&cfg, &cs);
    cfg.work = NULL;
    cfg.work_length = 0u;
    cfg.image_offset = 0xFFFFFFF0u;
    cfg.max_image_size = 0xFFFFFFFFu;
    expect("work yok, imaj ofseti sarar: kullanilmayan alanlar, gecerli", fwu_verify_manifest(&cfg, &sim.base.flash, &m), FWU_OK);
    check("... hash durumu acik degil", !cs.crypto.hashing);
}

/* ------------------------------------------------------------------ 10. imaj teklifi (RAM'deki manifest baytlari) */

static void test_offer(void)
{
    static vf_t sim;
    static fwu_crypto_sim_t cs;
    fwu_verify_config_t cfg;
    fwu_manifest_t m;
    pkg_t p = good_pkg();
    const fwu_byte_t *raw;                                          /* manifest baytlari: "RAM'e gelmis" gibi kullanilir */
    int unchanged_ok = 1;

    printf("\n--- fwu_verify_offer: RAM'deki manifest (yapi, urun, surum, boyut, anahtar, imza); imaj yok, hash yok ---\n");
    (void)vf_init(&sim);
    raw = sim.base.memory + MANIFEST_OFFSET;
    init_keys();
    write_package(&sim, &p, 0u);
    make_cfg(&cfg, &cs);

    (void)fwu_crypto_sim_init(&cs);
    m.image_version = 0xDEADBEEFu;
    expect("gecerli teklif", fwu_verify_offer(&cfg, raw, 138u, &m), FWU_OK);
    check("... manifest alanlari", m.product_id == PRODUCT && m.image_version == 25u && m.key_version == 1u && m.image_size == 700u && m.key_id == 1u);
    check("... imza dogrulandi (1 kez), HASH YOK, 74 elemanlik bolge", signature_checked_but_no_hash(&cs) && cs.last_verify_message_length == 74u);
    expect("manifest cikisi NULL olabilir", fwu_verify_offer(&cfg, raw, 138u, NULL), FWU_OK);
    expect("fazladan bayt (200) yok sayilir", fwu_verify_offer(&cfg, raw, 200u, &m), FWU_OK);

    (void)fwu_crypto_sim_init(&cs);
    m.image_version = 0xDEADBEEFu;
    expect("kisa (137): NOT_ENOUGH_DATA", fwu_verify_offer(&cfg, raw, 137u, &m), FWU_ERR_NOT_ENOUGH_DATA);
    check("... kripto yok", no_crypto_ops(&cs));
    unchanged_ok = unchanged_ok && (m.image_version == 0xDEADBEEFu);
    expect("bos (0)", fwu_verify_offer(&cfg, raw, 0u, &m), FWU_ERR_NOT_ENOUGH_DATA);

    /* ucuz kontroller imzaya girmez; sinirlar */
    flip_manifest_bit(&sim, 0u, 0x01u);
    expect("magic bozuk", fwu_verify_offer(&cfg, raw, 138u, &m), FWU_ERR_BAD_MANIFEST);
    check("... kripto yok", no_crypto_ops(&cs));
    unchanged_ok = unchanged_ok && (m.image_version == 0xDEADBEEFu);
    p.product = PRODUCT + 1u;
    write_package(&sim, &p, 0u);
    expect("baska urun", fwu_verify_offer(&cfg, raw, 138u, &m), FWU_ERR_PRODUCT_MISMATCH);
    check("... kripto yok", no_crypto_ops(&cs));
    p = good_pkg();
    p.version = 24u;
    write_package(&sim, &p, 0u);
    expect("surum 24 < asgari 25", fwu_verify_offer(&cfg, raw, 138u, &m), FWU_ERR_VERSION_REJECTED);
    check("... kripto yok", no_crypto_ops(&cs));
    p.version = 25u;
    write_package(&sim, &p, 0u);
    expect("surum 25 (esit, kabul)", fwu_verify_offer(&cfg, raw, 138u, &m), FWU_OK);
    p.size = IMAGE_MAX;
    write_package(&sim, &p, 0u);
    expect("boyut tam kapasite (1792)", fwu_verify_offer(&cfg, raw, 138u, &m), FWU_OK);
    (void)fwu_crypto_sim_init(&cs);
    p.size = IMAGE_MAX + 1u;
    write_package(&sim, &p, 0u);
    expect("boyut kapasite + 1", fwu_verify_offer(&cfg, raw, 138u, &m), FWU_ERR_IMAGE_TOO_LARGE);
    check("... kripto yok", no_crypto_ops(&cs));
    p = good_pkg();
    p.key_id = 9u;
    write_package(&sim, &p, 0u);
    expect("bilinmeyen anahtar", fwu_verify_offer(&cfg, raw, 138u, &m), FWU_ERR_KEY_REJECTED);
    check("... kripto yok", no_crypto_ops(&cs));
    p = good_pkg();
    p.key_version = 0u;
    write_package(&sim, &p, 0u);
    expect("anahtar surumu asgarinin altinda", fwu_verify_offer(&cfg, raw, 138u, &m), FWU_ERR_KEY_REJECTED);
    check("... kripto yok", no_crypto_ops(&cs));

    /* sira: ucuzdan pahaliya (urun > surum > boyut > anahtar); reddedilen cagrilar cikisi degistirmez */
    m.image_version = 0xDEADBEEFu;
    p = good_pkg();
    p.product = PRODUCT + 1u; p.version = 1u; p.size = IMAGE_MAX + 1u; p.key_id = 9u;
    write_package(&sim, &p, 0u);
    expect("sira: her sey yanlis -> urun", fwu_verify_offer(&cfg, raw, 138u, &m), FWU_ERR_PRODUCT_MISMATCH);
    p.product = PRODUCT;
    write_package(&sim, &p, 0u);
    expect("sira: surum + boyut + anahtar yanlis -> surum", fwu_verify_offer(&cfg, raw, 138u, &m), FWU_ERR_VERSION_REJECTED);
    p.version = 25u;
    write_package(&sim, &p, 0u);
    expect("sira: boyut + anahtar yanlis -> boyut", fwu_verify_offer(&cfg, raw, 138u, &m), FWU_ERR_IMAGE_TOO_LARGE);
    p.size = 700u;
    write_package(&sim, &p, 0u);
    expect("sira: yalniz anahtar yanlis -> anahtar", fwu_verify_offer(&cfg, raw, 138u, &m), FWU_ERR_KEY_REJECTED);
    check("... reddedilen 4 cagrida cikis degismedi", m.image_version == 0xDEADBEEFu);
    check("... kisa/bozuk teklifte de cikis degismemisti", unchanged_ok);

    /* imza */
    m.image_version = 0xDEADBEEFu;
    p = good_pkg();
    write_package(&sim, &p, 2u);
    (void)fwu_crypto_sim_init(&cs);
    expect("saldirganin anahtariyla imzali", fwu_verify_offer(&cfg, raw, 138u, &m), FWU_ERR_BAD_SIGNATURE);
    check("... imza dogrulandi, hash yok", signature_checked_but_no_hash(&cs));
    write_package(&sim, &p, 0u);
    flip_manifest_bit(&sim, 12u, 0x01u);
    expect("imzali alan degistirilmis", fwu_verify_offer(&cfg, raw, 138u, &m), FWU_ERR_BAD_SIGNATURE);
    write_package(&sim, &p, 0u);
    (void)fwu_crypto_sim_init(&cs);
    cs.fail_verify = FWU_ERR_CRYPTO_FAILURE;
    expect("kripto: yapilamadi", fwu_verify_offer(&cfg, raw, 138u, &m), FWU_ERR_CRYPTO_FAILURE);
    cs.fail_verify = FWU_ERR_FLASH_WRITE;
    expect("kripto portu sacma kod: CRYPTO_FAILURE", fwu_verify_offer(&cfg, raw, 138u, &m), FWU_ERR_CRYPTO_FAILURE);
    cs.fail_verify = FWU_OK;
    check("... cikis hala degismedi", m.image_version == 0xDEADBEEFu);

    /* tam hat ile iliski: teklif imaj bozuk olsa da gecer, hash tam hatta yakalanir */
    flip_image_bit(&sim, 100u);
    expect("imaj bozuk: teklif yine gecerli (imaj yok sayilir)", fwu_verify_offer(&cfg, raw, 138u, &m), FWU_OK);
    expect("... tam hat HASH_MISMATCH", run_verify(&sim, &cs, &cfg, &m), FWU_ERR_HASH_MISMATCH);
    write_package(&sim, &p, 0u);

    /* DEV: imza ve anahtar atlanir; urun, surum, boyut yine denetlenir */
    cfg.require_signature = false;
    p.key_id = 9u;
    write_package(&sim, &p, 2u);
    (void)fwu_crypto_sim_init(&cs);
    expect("DEV: bilinmeyen kimlik + yanlis imza kabul", fwu_verify_offer(&cfg, raw, 138u, &m), FWU_OK);
    check("... kripto yok", no_crypto_ops(&cs));
    p = good_pkg();
    p.version = 1u;
    write_package(&sim, &p, 0u);
    expect("DEV: surum yine denetlenir", fwu_verify_offer(&cfg, raw, 138u, &m), FWU_ERR_VERSION_REJECTED);
    p = good_pkg();
    p.size = IMAGE_MAX + 1u;
    write_package(&sim, &p, 0u);
    expect("DEV: boyut yine denetlenir", fwu_verify_offer(&cfg, raw, 138u, &m), FWU_ERR_IMAGE_TOO_LARGE);
    make_cfg(&cfg, &cs);

    /* argumanlar; imaj okumasina ait alanlar KULLANILMAZ */
    p = good_pkg();
    write_package(&sim, &p, 0u);
    expect("config NULL", fwu_verify_offer(NULL, raw, 138u, &m), FWU_ERR_INVALID_ARG);
    expect("bayt NULL", fwu_verify_offer(&cfg, NULL, 138u, &m), FWU_ERR_INVALID_ARG);
    cfg.crypto = NULL;
    expect("crypto NULL", fwu_verify_offer(&cfg, raw, 138u, &m), FWU_ERR_INVALID_ARG);
    cfg.require_signature = false;
    expect("crypto NULL, DEV kipinde de", fwu_verify_offer(&cfg, raw, 138u, &m), FWU_ERR_INVALID_ARG);
    make_cfg(&cfg, &cs);
    cfg.keys = NULL;
    expect("key_count 2 ama keys NULL", fwu_verify_offer(&cfg, raw, 138u, &m), FWU_ERR_INVALID_ARG);
    make_cfg(&cfg, &cs);
    cfg.work = NULL;
    cfg.work_length = 0u;
    cfg.manifest_offset = 0xFFFFFFF0u;
    cfg.image_offset = 0xFFFFFFF0u;
    expect("work yok, ofsetler cop: kullanilmayan alanlar, gecerli", fwu_verify_offer(&cfg, raw, 138u, &m), FWU_OK);
}

int main(void)
{
    test_valid();
    test_structure_and_policy();
    test_signature();
    test_hash();
    test_dev_mode();
    test_failures();
    test_arguments_and_hook();
    test_activation_end_to_end();
    test_manifest_only();
    test_offer();

    printf("\nResult: %s (%d failed)\n", g_failures == 0 ? "ALL TESTS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
