/**
 * @file    test_crypto.c
 * @brief   fwu_crypto sarmalayıcıları için kendi kendini kontrol eden test (sahte kriptografi OLMADAN,
 *          testin kendi küçük "stub" portuyla: porta tam olarak ne gittiği ve ne döndüğü kontrol edilir).
 *
 * Kapsam:
 *   1. Boyut sabitleri (elle yazılmış: 32, 32, 64).
 *   2. fwu_crypto_init: her NULL durumu, başarıda alanlar, hashing sıfırlanır.
 *   3. Zincir kontrolü: nesne/tablo/dört işlevden her birinin NULL olması, her sarmalayıcıda INVALID_ARG
 *      (port çağrılmaz).
 *   4. SHA-256 durum makinesi: begin/update/finish sırası, çift begin, begin'siz update/finish, finish sonrası.
 *   5. Argüman hataları özeti etkilemez; length == 0 port çağırmaz.
 *   6. Port hataları: CRYPTO_FAILURE'a çevrilir, özet kapatılır (finish her durumda kapatır).
 *   7. Ed25519: argümanlar aynen porta gider; FWU_OK YALNIZCA port FWU_OK dönerse; BAD_SIGNATURE korunur;
 *      başka her kod CRYPTO_FAILURE; sarmalayıcı özet durumuna dokunmaz.
 *
 * Derleme (crypto klasöründen):
 *   gcc -Wall -Wextra -Wpedantic -Wconversion -std=c99 -I. -I.. -I../error fwu_crypto.c test_crypto.c -o test_crypto && ./test_crypto
 */

#include <stdio.h>
#include "fwu_crypto.h"

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

/* ------------------------------------------------------------------ stub port */

typedef struct {
    uint32_t            begin_calls;
    uint32_t            update_calls;
    uint32_t            finish_calls;
    uint32_t            verify_calls;
    fwu_return_e        begin_return;
    fwu_return_e        update_return;
    fwu_return_e        finish_return;
    fwu_return_e        verify_return;
    void               *seen_context;
    const fwu_byte_t   *seen_data;
    uint32_t            seen_length;
    fwu_byte_t         *seen_digest;
    const fwu_byte_t   *seen_message;
    uint32_t            seen_message_length;
    const fwu_byte_t   *seen_signature;
    const fwu_byte_t   *seen_key;
} stub_t;

static fwu_return_e stub_begin(void *context)
{
    stub_t *s = (stub_t *)context;
    s->begin_calls++;
    s->seen_context = context;
    return s->begin_return;
}

static fwu_return_e stub_update(void *context, const fwu_byte_t *data, uint32_t length)
{
    stub_t *s = (stub_t *)context;
    s->update_calls++;
    s->seen_data = data;
    s->seen_length = length;
    return s->update_return;
}

static fwu_return_e stub_finish(void *context, fwu_byte_t *digest)
{
    stub_t *s = (stub_t *)context;
    s->finish_calls++;
    s->seen_digest = digest;
    return s->finish_return;
}

static fwu_return_e stub_verify(void *context, const fwu_byte_t *message, uint32_t message_length,
                                const fwu_byte_t *signature, const fwu_byte_t *public_key)
{
    stub_t *s = (stub_t *)context;
    s->verify_calls++;
    s->seen_context = context;
    s->seen_message = message;
    s->seen_message_length = message_length;
    s->seen_signature = signature;
    s->seen_key = public_key;
    return s->verify_return;
}

static const fwu_crypto_callback_t g_table = { stub_begin, stub_update, stub_finish, stub_verify };

static void stub_reset(stub_t *s)
{
    s->begin_calls = 0u;
    s->update_calls = 0u;
    s->finish_calls = 0u;
    s->verify_calls = 0u;
    s->begin_return = FWU_OK;
    s->update_return = FWU_OK;
    s->finish_return = FWU_OK;
    s->verify_return = FWU_OK;
    s->seen_context = NULL;
    s->seen_data = NULL;
    s->seen_length = 0u;
    s->seen_digest = NULL;
    s->seen_message = NULL;
    s->seen_message_length = 0u;
    s->seen_signature = NULL;
    s->seen_key = NULL;
}

static uint32_t total_calls(const stub_t *s)
{
    return s->begin_calls + s->update_calls + s->finish_calls + s->verify_calls;
}

/** Hazır bir nesne kurar. */
static void setup(fwu_crypto_t *c, stub_t *s)
{
    stub_reset(s);
    (void)fwu_crypto_init(c, s, &g_table);
}

/* ------------------------------------------------------------------ testler */

static void test_constants_and_init(void)
{
    stub_t s;
    fwu_crypto_t c;
    fwu_crypto_callback_t t;

    printf("\n--- sabitler ve init ---\n");
    check("ozet 32, acik anahtar 32, imza 64 (elle)",
          FWU_CRYPTO_SHA256_DIGEST_SIZE == 32u && FWU_CRYPTO_ED25519_PUBLIC_KEY_SIZE == 32u &&
          FWU_CRYPTO_ED25519_SIGNATURE_SIZE == 64u);

    stub_reset(&s);
    c.hashing = true;
    expect("gecerli init", fwu_crypto_init(&c, &s, &g_table), FWU_OK);
    check("... context ve tablo saklandi, hashing false", c.context == &s && c.callback == &g_table && !c.hashing);
    expect("self NULL", fwu_crypto_init(NULL, &s, &g_table), FWU_ERR_INVALID_ARG);
    expect("tablo NULL", fwu_crypto_init(&c, &s, NULL), FWU_ERR_INVALID_ARG);
    expect("context NULL olabilir (port durum tutmuyorsa)", fwu_crypto_init(&c, NULL, &g_table), FWU_OK);

    t = g_table; t.sha256_begin = NULL;
    expect("sha256_begin NULL", fwu_crypto_init(&c, &s, &t), FWU_ERR_INVALID_ARG);
    t = g_table; t.sha256_update = NULL;
    expect("sha256_update NULL", fwu_crypto_init(&c, &s, &t), FWU_ERR_INVALID_ARG);
    t = g_table; t.sha256_finish = NULL;
    expect("sha256_finish NULL", fwu_crypto_init(&c, &s, &t), FWU_ERR_INVALID_ARG);
    t = g_table; t.ed25519_verify = NULL;
    expect("ed25519_verify NULL", fwu_crypto_init(&c, &s, &t), FWU_ERR_INVALID_ARG);
}

static void test_chain_checks(void)
{
    stub_t s;
    fwu_crypto_t c;
    fwu_crypto_callback_t t;
    fwu_byte_t buf[64];
    fwu_byte_t key[32];
    int ok = 1;

    printf("\n--- zincir kontrolu: her sarmalayici NULL nesne/tablo/islevde INVALID_ARG, port cagrilmaz ---\n");
    for(uint32_t i = 0u; i < 64u; i++) {
        buf[i] = (fwu_byte_t)i;
    }
    for(uint32_t i = 0u; i < 32u; i++) {
        key[i] = (fwu_byte_t)i;
    }

    stub_reset(&s);
    expect("begin: self NULL", fwu_crypto_sha256_begin(NULL), FWU_ERR_INVALID_ARG);
    expect("update: self NULL", fwu_crypto_sha256_update(NULL, buf, 4u), FWU_ERR_INVALID_ARG);
    expect("finish: self NULL", fwu_crypto_sha256_finish(NULL, buf), FWU_ERR_INVALID_ARG);
    expect("verify: self NULL", fwu_crypto_ed25519_verify(NULL, buf, 10u, buf, key), FWU_ERR_INVALID_ARG);

    c.context = &s; c.callback = NULL; c.hashing = true;
    expect("begin: tablo NULL", fwu_crypto_sha256_begin(&c), FWU_ERR_INVALID_ARG);
    expect("update: tablo NULL", fwu_crypto_sha256_update(&c, buf, 4u), FWU_ERR_INVALID_ARG);
    expect("finish: tablo NULL", fwu_crypto_sha256_finish(&c, buf), FWU_ERR_INVALID_ARG);
    expect("verify: tablo NULL", fwu_crypto_ed25519_verify(&c, buf, 10u, buf, key), FWU_ERR_INVALID_ARG);

    /* Dort islevden BIRI NULL iken HER sarmalayici reddeder (hepsi zorunlu: eksik tablo kurulumda yakalanmali). */
    for(unsigned which = 0u; which < 4u; which++) {
        t = g_table;
        if(which == 0u) { t.sha256_begin = NULL; }
        if(which == 1u) { t.sha256_update = NULL; }
        if(which == 2u) { t.sha256_finish = NULL; }
        if(which == 3u) { t.ed25519_verify = NULL; }
        c.context = &s; c.callback = &t; c.hashing = true;
        if(fwu_crypto_sha256_begin(&c) != FWU_ERR_INVALID_ARG) { ok = 0; }
        if(fwu_crypto_sha256_update(&c, buf, 4u) != FWU_ERR_INVALID_ARG) { ok = 0; }
        if(fwu_crypto_sha256_finish(&c, buf) != FWU_ERR_INVALID_ARG) { ok = 0; }
        if(fwu_crypto_ed25519_verify(&c, buf, 10u, buf, key) != FWU_ERR_INVALID_ARG) { ok = 0; }
    }
    check("tablodaki herhangi bir islev NULL: 4 sarmalayicinin hepsi INVALID_ARG", ok);
    check("hicbir durumda port cagrilmadi", total_calls(&s) == 0u);
}

static void test_state_machine(void)
{
    stub_t s;
    fwu_crypto_t c;
    fwu_byte_t buf[8] = { 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u };
    fwu_byte_t digest[32];

    printf("\n--- SHA-256 durum makinesi ---\n");
    setup(&c, &s);
    expect("begin'siz update", fwu_crypto_sha256_update(&c, buf, 8u), FWU_ERR_BAD_STATE);
    expect("begin'siz finish", fwu_crypto_sha256_finish(&c, digest), FWU_ERR_BAD_STATE);
    check("... port cagrilmadi", total_calls(&s) == 0u);

    expect("begin", fwu_crypto_sha256_begin(&c), FWU_OK);
    check("... port 1 kez cagrildi, context dogru, hashing true", s.begin_calls == 1u && s.seen_context == &s && c.hashing);
    expect("acikken ikinci begin", fwu_crypto_sha256_begin(&c), FWU_ERR_BAD_STATE);
    check("... port ikinci kez CAGRILMADI, ozet hala acik", s.begin_calls == 1u && c.hashing);

    expect("update", fwu_crypto_sha256_update(&c, buf, 8u), FWU_OK);
    check("... port ayni veri ve uzunlukla cagrildi", s.update_calls == 1u && s.seen_data == buf && s.seen_length == 8u);
    expect("ikinci update", fwu_crypto_sha256_update(&c, &buf[2], 3u), FWU_OK);
    check("... ikinci cagri dogru isaretci ve uzunluk", s.update_calls == 2u && s.seen_data == &buf[2] && s.seen_length == 3u);

    expect("finish", fwu_crypto_sha256_finish(&c, digest), FWU_OK);
    check("... port ayni digest tamponuyla cagrildi, hashing false", s.finish_calls == 1u && s.seen_digest == digest && !c.hashing);
    expect("finish sonrasi update", fwu_crypto_sha256_update(&c, buf, 8u), FWU_ERR_BAD_STATE);
    expect("finish sonrasi ikinci finish", fwu_crypto_sha256_finish(&c, digest), FWU_ERR_BAD_STATE);
    check("... port yeniden cagrilmadi", s.update_calls == 2u && s.finish_calls == 1u);
    expect("yeni bir ozet baslatilabilir", fwu_crypto_sha256_begin(&c), FWU_OK);
    check("... ikinci begin cagrisi yapildi", s.begin_calls == 2u && c.hashing);
}

static void test_argument_errors_keep_hash_open(void)
{
    stub_t s;
    fwu_crypto_t c;
    fwu_byte_t buf[8] = { 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u };
    fwu_byte_t digest[32];

    printf("\n--- argument hatalari ozeti etkilemez; length 0 ---\n");
    setup(&c, &s);
    (void)fwu_crypto_sha256_begin(&c);

    expect("update: data NULL, length > 0", fwu_crypto_sha256_update(&c, NULL, 4u), FWU_ERR_INVALID_ARG);
    check("... port cagrilmadi, ozet hala acik", s.update_calls == 0u && c.hashing);
    expect("update: length 0, data NULL (gecerli, bos)", fwu_crypto_sha256_update(&c, NULL, 0u), FWU_OK);
    expect("update: length 0, data dolu", fwu_crypto_sha256_update(&c, buf, 0u), FWU_OK);
    check("... length 0 port'u CAGIRMAZ", s.update_calls == 0u && c.hashing);
    expect("finish: digest NULL", fwu_crypto_sha256_finish(&c, NULL), FWU_ERR_INVALID_ARG);
    check("... port cagrilmadi, ozet hala acik", s.finish_calls == 0u && c.hashing);
    expect("sonra gecerli update", fwu_crypto_sha256_update(&c, buf, 8u), FWU_OK);
    expect("sonra gecerli finish", fwu_crypto_sha256_finish(&c, digest), FWU_OK);

    /* Ozet kapaliyken length 0: kapali ozete update yine BAD_STATE (once durum) */
    expect("kapali ozete update (length 0 bile olsa)", fwu_crypto_sha256_update(&c, NULL, 0u), FWU_ERR_BAD_STATE);
    expect("kapali ozete update (data NULL, length > 0): once arguman hatasi", fwu_crypto_sha256_update(&c, NULL, 4u), FWU_ERR_INVALID_ARG);
}

static void test_port_errors(void)
{
    stub_t s;
    fwu_crypto_t c;
    fwu_byte_t buf[8] = { 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u };
    fwu_byte_t digest[32];
    const fwu_return_e weird[] = { FWU_ERR_FLASH_WRITE, FWU_ERR_BAD_SIGNATURE, FWU_ERR_CRC_MISMATCH, FWU_ERR_INVALID_ARG, (fwu_return_e)99 };
    int normalized = 1;

    printf("\n--- port hatalari: CRYPTO_FAILURE'a cevrilir, ozet kapatilir ---\n");

    setup(&c, &s);
    s.begin_return = FWU_ERR_FLASH_WRITE;
    expect("begin: port hatasi", fwu_crypto_sha256_begin(&c), FWU_ERR_CRYPTO_FAILURE);
    check("... ozet ACILMADI", !c.hashing);
    expect("... update artik BAD_STATE", fwu_crypto_sha256_update(&c, buf, 8u), FWU_ERR_BAD_STATE);
    s.begin_return = FWU_OK;
    expect("... sorun giderilince begin basarili", fwu_crypto_sha256_begin(&c), FWU_OK);

    s.update_return = FWU_ERR_FLASH_WRITE;
    expect("update: port hatasi", fwu_crypto_sha256_update(&c, buf, 8u), FWU_ERR_CRYPTO_FAILURE);
    check("... ozet KAPATILDI (yarim ozet gecersiz)", !c.hashing);
    expect("... sonraki update BAD_STATE", fwu_crypto_sha256_update(&c, buf, 8u), FWU_ERR_BAD_STATE);
    expect("... finish BAD_STATE (yarim ozetten sonuc alinmaz)", fwu_crypto_sha256_finish(&c, digest), FWU_ERR_BAD_STATE);
    check("... finish'te port CAGRILMADI", s.finish_calls == 0u);
    s.update_return = FWU_OK;
    expect("... yeniden begin gerekir ve calisir", fwu_crypto_sha256_begin(&c), FWU_OK);

    s.finish_return = FWU_ERR_FLASH_WRITE;
    expect("finish: port hatasi", fwu_crypto_sha256_finish(&c, digest), FWU_ERR_CRYPTO_FAILURE);
    check("... finish hata verse de ozeti KAPATIR", !c.hashing);

    /* her port kodu CRYPTO_FAILURE'a normallesir (BAD_SIGNATURE dahil: ozet islemlerinde anlamsiz) */
    for(unsigned k = 0u; k < 5u; k++) {
        setup(&c, &s);
        s.begin_return = weird[k];
        if(fwu_crypto_sha256_begin(&c) != FWU_ERR_CRYPTO_FAILURE) { normalized = 0; }
        s.begin_return = FWU_OK;
        (void)fwu_crypto_sha256_begin(&c);
        s.update_return = weird[k];
        if(fwu_crypto_sha256_update(&c, buf, 8u) != FWU_ERR_CRYPTO_FAILURE) { normalized = 0; }
        s.update_return = FWU_OK;
        (void)fwu_crypto_sha256_begin(&c);
        s.finish_return = weird[k];
        if(fwu_crypto_sha256_finish(&c, digest) != FWU_ERR_CRYPTO_FAILURE) { normalized = 0; }
    }
    check("begin/update/finish: her turlu port hatasi (BAD_SIGNATURE, CRC, saçma kod) CRYPTO_FAILURE olur", normalized);
}

static void test_verify(void)
{
    stub_t s;
    fwu_crypto_t c;
    fwu_byte_t msg[74];
    fwu_byte_t sig[64];
    fwu_byte_t key[32];
    int never_ok = 1;
    int bad_sig_kept = 1;

    printf("\n--- Ed25519 dogrulama ---\n");
    for(uint32_t i = 0u; i < 74u; i++) { msg[i] = (fwu_byte_t)i; }
    for(uint32_t i = 0u; i < 64u; i++) { sig[i] = (fwu_byte_t)i; }
    for(uint32_t i = 0u; i < 32u; i++) { key[i] = (fwu_byte_t)i; }
    setup(&c, &s);

    expect("message NULL", fwu_crypto_ed25519_verify(&c, NULL, 74u, sig, key), FWU_ERR_INVALID_ARG);
    expect("message_length 0", fwu_crypto_ed25519_verify(&c, msg, 0u, sig, key), FWU_ERR_INVALID_ARG);
    expect("signature NULL", fwu_crypto_ed25519_verify(&c, msg, 74u, NULL, key), FWU_ERR_INVALID_ARG);
    expect("public_key NULL", fwu_crypto_ed25519_verify(&c, msg, 74u, sig, NULL), FWU_ERR_INVALID_ARG);
    check("... hicbirinde port cagrilmadi", s.verify_calls == 0u);

    expect("port FWU_OK donerse FWU_OK", fwu_crypto_ed25519_verify(&c, msg, 74u, sig, key), FWU_OK);
    check("... port AYNI isaretci ve uzunluklarla cagrildi (mesaj, imza, anahtar, context)",
          s.verify_calls == 1u && s.seen_message == msg && s.seen_message_length == 74u && s.seen_signature == sig &&
          s.seen_key == key && s.seen_context == &s);
    expect("tek elemanli mesaj gecerli", fwu_crypto_ed25519_verify(&c, msg, 1u, sig, key), FWU_OK);
    check("... uzunluk 1 olarak iletildi", s.seen_message_length == 1u);

    s.verify_return = FWU_ERR_BAD_SIGNATURE;
    expect("port BAD_SIGNATURE donerse BAD_SIGNATURE", fwu_crypto_ed25519_verify(&c, msg, 74u, sig, key), FWU_ERR_BAD_SIGNATURE);
    s.verify_return = FWU_ERR_CRYPTO_FAILURE;
    expect("port CRYPTO_FAILURE donerse CRYPTO_FAILURE", fwu_crypto_ed25519_verify(&c, msg, 74u, sig, key), FWU_ERR_CRYPTO_FAILURE);

    /* FWU_OK YALNIZCA port FWU_OK donerse: 1..99 arasi her kod icin sonuc OK olmamali; yalniz 50 BAD_SIGNATURE kalmali. */
    for(int code = 1; code < 100; code++) {
        fwu_return_e got;
        s.verify_return = (fwu_return_e)code;
        got = fwu_crypto_ed25519_verify(&c, msg, 74u, sig, key);
        if(got == FWU_OK) {
            never_ok = 0;
        }
        if((code == (int)FWU_ERR_BAD_SIGNATURE) ? (got != FWU_ERR_BAD_SIGNATURE) : (got != FWU_ERR_CRYPTO_FAILURE)) {
            bad_sig_kept = 0;
        }
    }
    check("port hangi sifir-disi kodu donerse donsun sonuc ASLA FWU_OK olmaz (guvenlik)", never_ok);
    check("yalniz BAD_SIGNATURE korunur, diger tum kodlar CRYPTO_FAILURE olur (CRC_MISMATCH 'imaj gecersiz' sanilmaz)", bad_sig_kept);

    /* verify, acik bir SHA-256 hesabina dokunmaz */
    setup(&c, &s);
    (void)fwu_crypto_sha256_begin(&c);
    expect("acik ozet varken verify", fwu_crypto_ed25519_verify(&c, msg, 74u, sig, key), FWU_OK);
    check("... ozet hala acik, begin yeniden cagrilmadi", c.hashing && s.begin_calls == 1u);
    s.verify_return = FWU_ERR_BAD_SIGNATURE;
    (void)fwu_crypto_ed25519_verify(&c, msg, 74u, sig, key);
    check("... verify hatasi da ozeti kapatmaz", c.hashing);
}

int main(void)
{
    test_constants_and_init();
    test_chain_checks();
    test_state_machine();
    test_argument_errors_keep_hash_open();
    test_port_errors();
    test_verify();

    printf("\nResult: %s (%d failed)\n", g_failures == 0 ? "ALL TESTS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
