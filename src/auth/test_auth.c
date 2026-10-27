/**
 * @file    test_auth.c
 * @brief   fwu_auth (asimetrik yetkilendirme) kendi kendini kontrol eden testi, katmanlar halinde:
 *   A. Mesaj ve yanıt biçimi: ELLE yazılmış baytlar, sınırlar.
 *   B. Cihaz: init kuralları, begin (RNG kullanımı, arıza/sıfır/takılı RNG'de KAPALI hata), check (her bayt bozulması,
 *      bağlama: ürün, cihaz, meydan okuma, anahtar kimliği, anahtar ucuz ret).
 *   C. Host: fwu_auth_host_respond.
 *   D. Gerçek oturumla: yeniden oynatma, başka cihaz/ürün, kilit ve kilidin sona ermesi, RNG arızası.
 *   E. Gerçek istemci + oturum + aktivasyonla uçtan uca (kusursuz hat), yetkisiz operatör, kilit.
 *   F. KAYIPLI HAT taramaları: gerçek yetkilendirme kancalarıyla yüzlerce tohum (G9 idempotent AUTH tekrarlarının sınavı).
 *
 * Derleme (auth klasöründen):
 *   gcc -Wall -Wextra -Wpedantic -Wconversion -std=c99 -I. -I.. -I../error -I../bytes -I../crc -I../ring_buffer -I../frame -I../flash -I../journal -I../activation -I../manifest -I../crypto -I../verify -I../session -I../transport -I../client fwu_auth.c ../client/fwu_client.c ../transport/fwu_link.c ../transport/fwu_linksim.c ../session/fwu_session.c ../session/fwu_proto.c ../verify/fwu_verify.c ../manifest/fwu_manifest.c ../crypto/fwu_crypto.c ../crypto/fwu_crypto_sim.c ../activation/fwu_activate.c ../journal/fwu_journal_ab.c ../journal/fwu_journal.c ../flash/fwu_flash.c ../flash/fwu_flash_sim.c ../frame/fwu_frame.c ../frame/fwu_decode.c ../frame/fwu_parser.c ../ring_buffer/fwu_ringbuf.c test_auth.c -o test_auth && ./test_auth
 */

#include "test_world.h"
#include "fwu_auth.h"
#include "fwu_client.h"
#include "fwu_linksim.h"

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

/* ------------------------------------------------------------------ test dunyasi: RNG, operator, cihaz */

typedef struct {
    uint32_t   state;
    uint32_t   calls;
    uint32_t   last_length;
    fwu_return_e fail;          /* FWU_OK disi: RNG bu kodu doner */
    bool       zero;            /* hep sifir uretir */
    bool       stuck;           /* hep ayni degeri uretir */
    const fwu_byte_t *fixed;    /* NULL degil: bu 32 baytlik nonce'u uretir */
    bool       dirty;           /* C2000 taklidinde ust bitleri kirli uretir */
    fwu_byte_t last[FWU_AUTH_NONCE_SIZE];
} rng_t;

static rng_t g_rng;
static bool g_keep_rng = false;        /* true: world() RNG dizisini SIFIRLAMAZ (cihaz yeniden baslasa da RNG ayni diziye donmez) */

static fwu_return_e rng_fn(void *context, fwu_byte_t *out, uint32_t length)
{
    rng_t *r = (rng_t *)context;
    r->calls++;
    r->last_length = length;
    if(r->fail != FWU_OK) {
        return r->fail;
    }
    for(uint32_t i = 0u; i < length; i++) {
        if(r->fixed != NULL) {
            out[i] = r->fixed[i];
        } else if(r->zero) {
            out[i] = 0u;
        } else if(r->stuck) {
            out[i] = 0x77u;
        } else {
            r->state = (r->state * 1664525u) + 1013904223u;
            out[i] = (fwu_byte_t)((r->state >> 16) & 0xFFu);
        }
        if(i < FWU_AUTH_NONCE_SIZE) {
            r->last[i] = out[i];
        }
        if(r->dirty) {
            out[i] = (fwu_byte_t)(out[i] | 0x100u);
        }
    }
    return FWU_OK;
}

static void rng_reset(void)
{
    g_rng.state = 12345u; g_rng.calls = 0u; g_rng.last_length = 0u; g_rng.fail = FWU_OK; g_rng.zero = false; g_rng.stuck = false; g_rng.fixed = NULL; g_rng.dirty = false;
}

static fwu_byte_t g_op_pk[3][32];               /* 0, 1: operator anahtarlari (kimlik 1, 2); 2: saldirgan */
static fwu_verify_key_t g_op_keys[2];
static const fwu_byte_t g_dev_id[12] = { 0xA0u, 0xA1u, 0xA2u, 0xA3u, 0xA4u, 0xA5u, 0xA6u, 0xA7u, 0xA8u, 0xA9u, 0xAAu, 0xABu };
static const fwu_byte_t g_dev_id_b[12] = { 0xB0u, 0xB1u, 0xB2u, 0xB3u, 0xB4u, 0xB5u, 0xB6u, 0xB7u, 0xB8u, 0xB9u, 0xBAu, 0xBBu };
static fwu_auth_t g_auth;
static fwu_auth_config_t g_acfg;

/* host imzalayici: ctx.slot'taki anahtarla imzalar; key_id slot'a gore */
typedef struct {
    uint32_t slot;              /* 0, 1 operator; 2 saldirgan (kimlik 1 sahte) */
    uint32_t calls;
    fwu_return_e fail;
    fwu_byte_t forced_id;       /* 0 degil: bu kimligi bildir */
} signer_t;
static signer_t g_signer;
static fwu_auth_host_t g_host;

static fwu_return_e sign_fn(void *context, const fwu_byte_t *message, uint32_t message_length, fwu_byte_t *key_id, fwu_byte_t *signature)
{
    signer_t *s = (signer_t *)context;
    s->calls++;
    if(s->fail != FWU_OK) {
        return s->fail;
    }
    fwu_crypto_sim_sign(g_op_pk[s->slot], message, message_length, signature);
    *key_id = (s->forced_id != 0u) ? s->forced_id : ((s->slot == 1u) ? (fwu_byte_t)2u : (fwu_byte_t)1u);
    return FWU_OK;
}

static void make_auth_config(fwu_auth_config_t *c)
{
    c->crypto = &tw_cs.crypto;
    c->rng = rng_fn;
    c->rng_context = &g_rng;
    c->device_id = g_dev_id;
    c->device_id_length = 12u;
    c->product_id = TW_PRODUCT;
    c->operator_keys = g_op_keys;
    c->operator_key_count = 2u;
}

/** Temiz dunya: oturum, aktivasyon, sahte flash, YETKILENDIRMELI oturum GERCEK kancayla. */
static void world(void)
{
    tw_world(true, FWU_ACTIVATE_MODE_COPY);
    for(uint32_t i = 0u; i < 32u; i++) {
        g_op_pk[0][i] = (fwu_byte_t)(i + 41u);
        g_op_pk[1][i] = (fwu_byte_t)(i + 141u);
        g_op_pk[2][i] = (fwu_byte_t)(i + 241u);
        g_op_keys[0].public_key[i] = g_op_pk[0][i];
        g_op_keys[1].public_key[i] = g_op_pk[1][i];
    }
    g_op_keys[0].id = 1u;
    g_op_keys[1].id = 2u;
    if(g_keep_rng) {
        g_rng.calls = 0u; g_rng.fail = FWU_OK; g_rng.zero = false; g_rng.stuck = false; g_rng.fixed = NULL; g_rng.dirty = false;
    } else {
        rng_reset();
    }
    make_auth_config(&g_acfg);
    (void)fwu_auth_init(&g_auth, &g_acfg);
    tw_cfg.auth = fwu_auth_hook(&g_auth);
    tw_cfg.require_auth = true;
    (void)fwu_session_init(&tw_session, &tw_cfg);
    g_signer.slot = 0u; g_signer.calls = 0u; g_signer.fail = FWU_OK; g_signer.forced_id = 0u;
    g_host.product_id = TW_PRODUCT;
    g_host.sign = sign_fn;
    g_host.sign_context = &g_signer;
}

static int same_bytes(const fwu_byte_t *a, const fwu_byte_t *b, size_t n)
{
    for(size_t i = 0u; i < n; i++) {
        if((a[i] & 0xFFu) != (b[i] & 0xFFu)) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ A. mesaj ve yanit bicimi */

static void test_formats(void)
{
    fwu_byte_t challenge[FWU_AUTH_CHALLENGE_MAX];
    fwu_byte_t out[FWU_AUTH_MESSAGE_MAX];
    fwu_byte_t sig[64];
    fwu_byte_t resp[FWU_AUTH_RESPONSE_SIZE];
    uint32_t n = 99u;

    printf("\n--- A. mesaj ve yanit bicimi ---\n");
    for(unsigned i = 0u; i < 32u; i++) {
        challenge[i] = (fwu_byte_t)(0x10u + i);
    }
    for(unsigned i = 0u; i < 12u; i++) {
        challenge[32u + i] = (fwu_byte_t)(0xA0u + i);
    }
    check("sabitler: nonce 32, cihaz kimligi <= 16, meydan okuma 33..48 (<= protokol siniri 64), mesaj <= 57, yanit 65",
          FWU_AUTH_NONCE_SIZE == 32u && FWU_AUTH_DEVICE_ID_MAX == 16u && FWU_AUTH_CHALLENGE_MIN == 33u && FWU_AUTH_CHALLENGE_MAX == 48u &&
          FWU_AUTH_CHALLENGE_MAX <= FWU_PROTO_AUTH_CHALLENGE_MAX && FWU_AUTH_MESSAGE_MAX == 57u && FWU_AUTH_RESPONSE_SIZE == 65u &&
          FWU_AUTH_RESPONSE_SIZE <= FWU_PROTO_AUTH_RESPONSE_MAX);
    expect("mesaj kur: 44 bayt meydan okuma", fwu_auth_build_message(12345u, challenge, 44u, out, sizeof(out) / sizeof(out[0]), &n), FWU_OK);
    check("mesaj 53 bayt: 'FWUA', surum 01, urun 12345 = 00 00 30 39 (buyuk endian)",
          n == 53u && out[0] == 0x46u && out[1] == 0x57u && out[2] == 0x55u && out[3] == 0x41u && out[4] == 0x01u &&
          out[5] == 0x00u && out[6] == 0x00u && out[7] == 0x30u && out[8] == 0x39u);
    check("... ardindan meydan okuma BIREBIR (nonce 0x10.. 32 bayt, sonra cihaz kimligi 0xA0.. 12 bayt)", same_bytes(&out[9], challenge, 44u) && out[9] == 0x10u && out[40] == 0x2Fu && out[41] == 0xA0u && out[52] == 0xABu);
    expect("mesaj: urun 0xFFFFFFFF, sinir deger", fwu_auth_build_message(0xFFFFFFFFu, challenge, 44u, out, 57u, &n), FWU_OK);
    check("... urun baytlari FF FF FF FF", out[5] == 0xFFu && out[6] == 0xFFu && out[7] == 0xFFu && out[8] == 0xFFu);
    expect("meydan okuma 32 (cihaz kimligi YOK): BAD_LENGTH", fwu_auth_build_message(1u, challenge, 32u, out, 57u, &n), FWU_ERR_BAD_LENGTH);
    expect("meydan okuma 33 (alt sinir)", fwu_auth_build_message(1u, challenge, 33u, out, 57u, &n), FWU_OK);
    check("... mesaj 42 bayt", n == 42u);
    expect("meydan okuma 48 (ust sinir)", fwu_auth_build_message(1u, challenge, 48u, out, 57u, &n), FWU_OK);
    check("... mesaj 57 bayt", n == 57u);
    expect("meydan okuma 49: BAD_LENGTH", fwu_auth_build_message(1u, challenge, 49u, out, 57u, &n), FWU_ERR_BAD_LENGTH);
    expect("meydan okuma 0: BAD_LENGTH", fwu_auth_build_message(1u, challenge, 0u, out, 57u, &n), FWU_ERR_BAD_LENGTH);
    expect("capacity 41 (42 gerekir)", fwu_auth_build_message(1u, challenge, 33u, out, 41u, &n), FWU_ERR_BUFFER_TOO_SMALL);
    expect("capacity 42 (sinir)", fwu_auth_build_message(1u, challenge, 33u, out, 42u, &n), FWU_OK);
    expect("meydan okuma NULL", fwu_auth_build_message(1u, NULL, 44u, out, 57u, &n), FWU_ERR_INVALID_ARG);
    expect("cikis NULL", fwu_auth_build_message(1u, challenge, 44u, NULL, 57u, &n), FWU_ERR_INVALID_ARG);
    expect("uzunluk cikisi NULL", fwu_auth_build_message(1u, challenge, 44u, out, 57u, NULL), FWU_ERR_INVALID_ARG);
#ifdef C2000
    {
        fwu_byte_t dirty[FWU_AUTH_CHALLENGE_MAX];
        for(unsigned i = 0u; i < 44u; i++) {
            dirty[i] = (fwu_byte_t)(challenge[i] | 0x100u);
        }
        (void)fwu_auth_build_message(1u, dirty, 44u, out, 57u, &n);
        check("C2000 taklidi: ust bitleri kirli meydan okuma: mesaj baytlari temiz (alt 8 bit)", same_bytes(&out[9], challenge, 44u) && (out[9] & 0xFF00u) == 0u);
    }
#endif

    {
        /* TAM kapasiteli tampon + ardinda bekci bayt: hicbir islev tamponun disina bir bayt bile yazmaz */
        fwu_byte_t guard[70];
        for(unsigned i = 0u; i < 70u; i++) {
            guard[i] = 0xEEu;
        }
        (void)fwu_auth_build_message(1u, challenge, 33u, guard, 42u, &n);
        check("mesaj (kapasite 42, tam dolu): tamponun ARDINDAKI bayt (42.) bozulmadi", guard[42] == 0xEEu && guard[41] == challenge[32]);
        for(unsigned i = 0u; i < 70u; i++) {
            guard[i] = 0xEEu;
        }
        (void)fwu_auth_build_message(1u, challenge, 48u, guard, 57u, &n);
        check("mesaj (kapasite 57, tam dolu): 57. bayt bozulmadi", guard[57] == 0xEEu && guard[56] == challenge[47]);
    }
    for(unsigned i = 0u; i < 64u; i++) {
        sig[i] = (fwu_byte_t)(0xC0u + i);
    }
    {
        fwu_byte_t guard[70];
        for(unsigned i = 0u; i < 70u; i++) {
            guard[i] = 0xEEu;
        }
        (void)fwu_auth_build_response(7u, sig, guard, 65u, &n);
        check("yanit (kapasite 65, tam dolu): 65. bayt bozulmadi", guard[65] == 0xEEu && guard[64] == 0xFFu);
    }
    expect("yanit kur", fwu_auth_build_response(7u, sig, resp, 65u, &n), FWU_OK);
    check("yanit 65 bayt: anahtar kimligi 07, sonra 64 baytlik imza BIREBIR", n == 65u && resp[0] == 7u && same_bytes(&resp[1], sig, 64u) && resp[1] == 0xC0u && resp[64] == 0xFFu);
    expect("yanit: capacity 64", fwu_auth_build_response(7u, sig, resp, 64u, &n), FWU_ERR_BUFFER_TOO_SMALL);
    expect("yanit: imza NULL", fwu_auth_build_response(7u, NULL, resp, 65u, &n), FWU_ERR_INVALID_ARG);
    expect("yanit: cikis NULL", fwu_auth_build_response(7u, sig, NULL, 65u, &n), FWU_ERR_INVALID_ARG);
    expect("yanit: uzunluk NULL", fwu_auth_build_response(7u, sig, resp, 65u, NULL), FWU_ERR_INVALID_ARG);
}

/* ------------------------------------------------------------------ B. cihaz */

static void build_response_for_len(const fwu_byte_t *challenge, uint32_t length, uint32_t slot, fwu_byte_t *resp)
{
    uint32_t n = 0u;
    g_signer.slot = slot;
    g_signer.fail = FWU_OK;
    (void)fwu_auth_host_respond(&g_host, challenge, length, resp, FWU_AUTH_RESPONSE_SIZE, &n);
}

static void test_init_and_begin(void)
{
    fwu_auth_config_t c;
    fwu_auth_t a;
    fwu_byte_t ch[64];
    uint32_t len = 0u;
    const fwu_session_auth_t *hook;
    fwu_byte_t first[FWU_AUTH_NONCE_SIZE];

    printf("\n--- B1. init ve begin ---\n");
    world();
    make_auth_config(&c);
    expect("gecerli", fwu_auth_init(&a, &c), FWU_OK);
    expect("auth NULL", fwu_auth_init(NULL, &c), FWU_ERR_INVALID_ARG);
    expect("config NULL", fwu_auth_init(&a, NULL), FWU_ERR_INVALID_ARG);
    c.crypto = NULL;
    expect("crypto NULL", fwu_auth_init(&a, &c), FWU_ERR_INVALID_ARG);
    make_auth_config(&c); c.rng = NULL;
    expect("rng NULL", fwu_auth_init(&a, &c), FWU_ERR_INVALID_ARG);
    make_auth_config(&c); c.operator_keys = NULL;
    expect("operator anahtar dizisi NULL", fwu_auth_init(&a, &c), FWU_ERR_INVALID_ARG);
    make_auth_config(&c); c.operator_key_count = 0u;
    expect("operator anahtar sayisi 0", fwu_auth_init(&a, &c), FWU_ERR_INVALID_ARG);
    make_auth_config(&c); c.operator_key_count = 1u;
    expect("1 anahtar (sinir)", fwu_auth_init(&a, &c), FWU_OK);
    make_auth_config(&c); c.device_id = NULL;
    expect("cihaz kimligi NULL", fwu_auth_init(&a, &c), FWU_ERR_INVALID_ARG);
    make_auth_config(&c); c.device_id_length = 0u;
    expect("cihaz kimligi uzunlugu 0", fwu_auth_init(&a, &c), FWU_ERR_INVALID_ARG);
    make_auth_config(&c); c.device_id_length = 1u;
    expect("cihaz kimligi 1 bayt (alt sinir)", fwu_auth_init(&a, &c), FWU_OK);
    {
        static const fwu_byte_t id16[16] = { 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u, 15u, 16u };
        fwu_byte_t chb[64];
        uint32_t lb = 0u;
        fwu_auth_t b16;
        make_auth_config(&c); c.device_id = id16; c.device_id_length = 16u;
        expect("cihaz kimligi 16 bayt (ust sinir)", fwu_auth_init(&b16, &c), FWU_OK);
        expect("... meydan okuma 48 bayt: capacity 47 yetmez", fwu_auth_hook(&b16)->begin(fwu_auth_hook(&b16)->context, chb, 47u, &lb), FWU_ERR_BUFFER_TOO_SMALL);
        expect("... capacity 48 (sinir)", fwu_auth_hook(&b16)->begin(fwu_auth_hook(&b16)->context, chb, 48u, &lb), FWU_OK);
        check("... meydan okuma 48 bayt, son 16 bayt = cihaz kimligi (1..16)", lb == 48u && chb[32] == 1u && chb[47] == 16u);
        {
            fwu_byte_t rb[FWU_AUTH_RESPONSE_SIZE];
            build_response_for_len(chb, lb, 0u, rb);
            expect("... 48 baytlik meydan okuma icin operator yaniti: OK (mesaj 57 bayt)", fwu_auth_hook(&b16)->check(fwu_auth_hook(&b16)->context, chb, lb, rb, 65u), FWU_OK);
        }
    }
    make_auth_config(&c); c.device_id_length = 17u;
    expect("cihaz kimligi 17 bayt", fwu_auth_init(&a, &c), FWU_ERR_INVALID_ARG);
    g_op_keys[1].id = 1u;
    make_auth_config(&c);
    expect("iki operator anahtari AYNI kimlikle: reddedilir", fwu_auth_init(&a, &c), FWU_ERR_INVALID_ARG);
    g_op_keys[1].id = 2u;
    check("basarisiz init: kanca NULL (nesne kullanilamaz)", fwu_auth_hook(&a) == NULL);
    check("kanca NULL'a dayanikli", fwu_auth_hook(NULL) == NULL);
    make_auth_config(&c);
    (void)fwu_auth_init(&a, &c);
    hook = fwu_auth_hook(&a);
    check("kanca: begin ve check dolu, context = auth nesnesi", hook != NULL && hook->begin != NULL && hook->check != NULL && hook->context == (void *)&a);

    /* begin */
    rng_reset();
    expect("begin", hook->begin(hook->context, ch, 64u, &len), FWU_OK);
    check("meydan okuma 44 bayt, RNG 1 kez ve 32 bayt istendi", len == 44u && g_rng.calls == 1u && g_rng.last_length == 32u);
    check("... ilk 32 bayt RNG'nin urettigi nonce, sonraki 12 bayt cihaz kimligi", same_bytes(ch, g_rng.last, 32u) && same_bytes(&ch[32], g_dev_id, 12u));
    for(unsigned i = 0u; i < 32u; i++) {
        first[i] = ch[i];
    }
    expect("ikinci begin", hook->begin(hook->context, ch, 64u, &len), FWU_OK);
    check("... yeni nonce (oncekinden farkli)", !same_bytes(ch, first, 32u) && g_rng.calls == 2u);
    g_rng.calls = 0u;
    expect("capacity 43 (44 gerekir)", hook->begin(hook->context, ch, 43u, &len), FWU_ERR_BUFFER_TOO_SMALL);
    check("... RNG'ye DOKUNULMADI", g_rng.calls == 0u);
    {
        fwu_byte_t guard[50];
        for(unsigned i = 0u; i < 50u; i++) {
            guard[i] = 0xEEu;
        }
        expect("capacity 44 (sinir)", hook->begin(hook->context, guard, 44u, &len), FWU_OK);
        check("... tamponun ARDINDAKI bayt (44.) bozulmadi", guard[44] == 0xEEu && guard[43] == g_dev_id[11]);
    }
    expect("begin: context NULL", fwu_auth_hook(&a)->begin(NULL, ch, 64u, &len), FWU_ERR_INVALID_ARG);
    expect("begin: challenge NULL", hook->begin(hook->context, NULL, 64u, &len), FWU_ERR_INVALID_ARG);
    expect("begin: length NULL", hook->begin(hook->context, ch, 64u, NULL), FWU_ERR_INVALID_ARG);

    /* RNG arizalari: KAPALI hata, durum ilerlemez */
    rng_reset();
    g_rng.fail = FWU_ERR_FLASH_POWER_LOSS;
    expect("RNG hata doner (herhangi bir kod): CRYPTO_FAILURE (yetkilendirme acilmaz)", hook->begin(hook->context, ch, 64u, &len), FWU_ERR_CRYPTO_FAILURE);
    g_rng.fail = FWU_OK;
    expect("RNG duzelince begin yine calisir", hook->begin(hook->context, ch, 64u, &len), FWU_OK);
    rng_reset();
    g_rng.zero = true;
    expect("RNG hep SIFIR uretiyor: CRYPTO_FAILURE", hook->begin(hook->context, ch, 64u, &len), FWU_ERR_CRYPTO_FAILURE);
    rng_reset();
    g_rng.stuck = true;
    {
        /* yeni nesne: ilk takili deger kabul edilir, ikincisi AYNI nonce: reddedilir */
        fwu_auth_t b;
        (void)fwu_auth_init(&b, &c);
        expect("RNG takili: ilk meydan okuma (karsilastirilacak onceki yok) kabul", fwu_auth_hook(&b)->begin(fwu_auth_hook(&b)->context, ch, 64u, &len), FWU_OK);
        expect("... ikinci AYNI nonce: CRYPTO_FAILURE (tekrar meydan okuma verilmez)", fwu_auth_hook(&b)->begin(fwu_auth_hook(&b)->context, ch, 64u, &len), FWU_ERR_CRYPTO_FAILURE);
        g_rng.stuck = false;
        expect("RNG duzelince yine calisir", fwu_auth_hook(&b)->begin(fwu_auth_hook(&b)->context, ch, 64u, &len), FWU_OK);
    }
    /* nonce kenar desenleri: yalniz 1 bayt sifir degilse "sifir" DEGILDIR; yalniz 1 bayt farkliysa "ayni" DEGILDIR */
    {
        fwu_auth_t b;
        fwu_byte_t pat_first[FWU_AUTH_NONCE_SIZE];
        fwu_byte_t pat_first2[FWU_AUTH_NONCE_SIZE];
        fwu_byte_t pat_last[FWU_AUTH_NONCE_SIZE];
        for(unsigned i = 0u; i < 32u; i++) {
            pat_first[i] = 0u; pat_first2[i] = 0u; pat_last[i] = 0u;
        }
        pat_first[0] = 1u; pat_first2[0] = 2u; pat_last[31] = 1u;
        rng_reset();
        (void)fwu_auth_init(&b, &c);
        g_rng.fixed = pat_first;
        expect("nonce yalniz ILK bayt sifir degil: sifir sayilmaz, kabul", fwu_auth_hook(&b)->begin(fwu_auth_hook(&b)->context, ch, 64u, &len), FWU_OK);
        g_rng.fixed = pat_first;
        expect("... ayni nonce ikinci kez: reddedilir", fwu_auth_hook(&b)->begin(fwu_auth_hook(&b)->context, ch, 64u, &len), FWU_ERR_CRYPTO_FAILURE);
        g_rng.fixed = pat_first2;
        expect("nonce YALNIZ ILK BAYTTA farkli (1 -> 2): 'ayni' sayilmaz, kabul", fwu_auth_hook(&b)->begin(fwu_auth_hook(&b)->context, ch, 64u, &len), FWU_OK);
        g_rng.fixed = pat_last;
        expect("nonce yalniz SON bayt sifir degil: kabul", fwu_auth_hook(&b)->begin(fwu_auth_hook(&b)->context, ch, 64u, &len), FWU_OK);
        g_rng.fixed = pat_first2;
        expect("onceki nonce'tan yalniz son baytta fark (son bayt 1 -> 0, ilk 0 -> 2): kabul", fwu_auth_hook(&b)->begin(fwu_auth_hook(&b)->context, ch, 64u, &len), FWU_OK);
        g_rng.fixed = NULL;
    }
    /* kirli bellekte init: onceki nonce ve "var" bayragi bellekten MIRAS ALINMAZ */
    {
        fwu_auth_t d;
        fwu_byte_t stale[FWU_AUTH_NONCE_SIZE];
        unsigned char *raw = (unsigned char *)&d;
        for(size_t i = 0u; i < sizeof(d); i++) {
            raw[i] = 0xA5u;
        }
        for(unsigned i = 0u; i < 32u; i++) {
            stale[i] = 0xA5u;
        }
        rng_reset();
        expect("kirli (0xA5 dolu) bellekte init", fwu_auth_init(&d, &c), FWU_OK);
        g_rng.fixed = stale;
        expect("... ilk begin, nonce eski bellek artigina (0xA5...) esit olsa da KABUL (onceki meydan okuma yok)", fwu_auth_hook(&d)->begin(fwu_auth_hook(&d)->context, ch, 64u, &len), FWU_OK);
        g_rng.fixed = NULL;
    }
#ifdef C2000
    {
        fwu_auth_t b;
        rng_reset();
        g_rng.dirty = true;
        (void)fwu_auth_init(&b, &c);
        expect("C2000 taklidi: RNG ust bitleri kirli uretiyor", fwu_auth_hook(&b)->begin(fwu_auth_hook(&b)->context, ch, 64u, &len), FWU_OK);
        {
            int clean = 1;
            for(unsigned i = 0u; i < 32u; i++) {
                if((ch[i] & 0xFF00u) != 0u) {
                    clean = 0;
                }
            }
            check("... meydan okuma nonce'u TEMIZ (alt 8 bit)", clean);
        }
        g_rng.dirty = false;
    }
#endif
    /* nonce yalniz 1 bayt farkliysa gecerli sayilir; yalniz son bayt sifir olmayan nonce sifir degildir */
    {
        fwu_auth_t b;
        rng_reset();
        (void)fwu_auth_init(&b, &c);
        (void)fwu_auth_hook(&b)->begin(fwu_auth_hook(&b)->context, ch, 64u, &len);
        g_rng.state = 12345u;                           /* ayni dizi: ayni ilk nonce'u TEKRAR uretir */
        expect("RNG ayni nonce'u ikinci kez uretti (durum geri sarildi): reddedilir", fwu_auth_hook(&b)->begin(fwu_auth_hook(&b)->context, ch, 64u, &len), FWU_ERR_CRYPTO_FAILURE);
    }
}

static void build_response_for(const fwu_byte_t *challenge, uint32_t length, uint32_t slot, fwu_byte_t *resp)
{
    uint32_t n = 0u;
    g_signer.slot = slot;
    g_signer.fail = FWU_OK;
    (void)fwu_auth_host_respond(&g_host, challenge, length, resp, FWU_AUTH_RESPONSE_SIZE, &n);
}

static void test_check(void)
{
    const fwu_session_auth_t *hook;
    fwu_byte_t ch[64];
    fwu_byte_t resp[FWU_AUTH_RESPONSE_SIZE];
    fwu_byte_t bad[FWU_AUTH_RESPONSE_SIZE];
    uint32_t len = 0u;
    int all_sig_rejected = 1;

    printf("\n--- B2. check: gecerli yanit, her bayt bozulmasi, baglama ---\n");
    world();
    hook = fwu_auth_hook(&g_auth);
    (void)hook->begin(hook->context, ch, 64u, &len);
    build_response_for(ch, len, 0u, resp);
    expect("operator 1 (kimlik 1) dogru yanit: OK", hook->check(hook->context, ch, len, resp, 65u), FWU_OK);
    check("... yanit kimligi 1", resp[0] == 1u);
    {
        fwu_byte_t resp2[FWU_AUTH_RESPONSE_SIZE];
        build_response_for(ch, len, 1u, resp2);
        check("operator 2 (kimlik 2, kendi anahtari) dogru yanit: OK (ikinci anahtar yuvasi)", resp2[0] == 2u && hook->check(hook->context, ch, len, resp2, 65u) == FWU_OK);
    }
    {
        fwu_byte_t resp3[FWU_AUTH_RESPONSE_SIZE];
        build_response_for(ch, len, 2u, resp3);
        expect("SALDIRGAN anahtariyla imza (kimlik 1 bildirilir): BAD_SIGNATURE", hook->check(hook->context, ch, len, resp3, 65u), FWU_ERR_BAD_SIGNATURE);
    }
    {
        fwu_byte_t resp4[FWU_AUTH_RESPONSE_SIZE];
        build_response_for(ch, len, 0u, resp4);
        resp4[0] = 2u;                                       /* operator 1'in imzasi, kimlik 2: yanlis anahtarla dogrulanir */
        expect("kimlik 2 ama imza operator 1'in: BAD_SIGNATURE", hook->check(hook->context, ch, len, resp4, 65u), FWU_ERR_BAD_SIGNATURE);
    }

    /* her yanit baytinin bozulmasi */
    for(unsigned k = 1u; k < 65u; k++) {
        for(unsigned i = 0u; i < 65u; i++) {
            bad[i] = resp[i];
        }
        bad[k] = (fwu_byte_t)(bad[k] ^ 0x01u);
        if(hook->check(hook->context, ch, len, bad, 65u) != FWU_ERR_BAD_SIGNATURE) {
            all_sig_rejected = 0;
        }
    }
    check("imzanin 64 baytindan HER BIRI ayri ayri bozulunca BAD_SIGNATURE", all_sig_rejected);
    for(unsigned i = 0u; i < 65u; i++) {
        bad[i] = resp[i];
    }
    bad[0] = 0u;
    {
        const uint32_t before = tw_cs.verify_calls;
        expect("bilinmeyen anahtar kimligi (0): KEY_REJECTED", hook->check(hook->context, ch, len, bad, 65u), FWU_ERR_KEY_REJECTED);
        check("... ucuz ret: imza dogrulamasina GIRILMEDI", tw_cs.verify_calls == before);
        bad[0] = 3u;
        expect("bilinmeyen anahtar kimligi (3): KEY_REJECTED", hook->check(hook->context, ch, len, bad, 65u), FWU_ERR_KEY_REJECTED);
        bad[0] = 255u;
        expect("bilinmeyen anahtar kimligi (255)", hook->check(hook->context, ch, len, bad, 65u), FWU_ERR_KEY_REJECTED);
    }

    /* yanit uzunlugu */
    expect("yanit 64 bayt: BAD_LENGTH", hook->check(hook->context, ch, len, resp, 64u), FWU_ERR_BAD_LENGTH);
    expect("yanit 66 bayt: BAD_LENGTH", hook->check(hook->context, ch, len, resp, 66u), FWU_ERR_BAD_LENGTH);
    expect("yanit 0 bayt: BAD_LENGTH", hook->check(hook->context, ch, len, resp, 0u), FWU_ERR_BAD_LENGTH);
    expect("check: context NULL", hook->check(NULL, ch, len, resp, 65u), FWU_ERR_INVALID_ARG);
    expect("check: challenge NULL", hook->check(hook->context, NULL, len, resp, 65u), FWU_ERR_INVALID_ARG);
    expect("check: response NULL", hook->check(hook->context, ch, len, NULL, 65u), FWU_ERR_INVALID_ARG);

    /* meydan okuma tutarliligi */
    expect("meydan okuma uzunlugu 43 (44 gerekir): INVALID_ARG", hook->check(hook->context, ch, 43u, resp, 65u), FWU_ERR_INVALID_ARG);
    expect("meydan okuma uzunlugu 45: INVALID_ARG", hook->check(hook->context, ch, 45u, resp, 65u), FWU_ERR_INVALID_ARG);
    {
        fwu_byte_t other[64];
        for(unsigned i = 0u; i < 44u; i++) {
            other[i] = ch[i];
        }
        other[43] = (fwu_byte_t)(other[43] ^ 0x01u);
        expect("meydan okuma BASKA cihazin kimligini tasiyor: INVALID_ARG", hook->check(hook->context, other, 44u, resp, 65u), FWU_ERR_INVALID_ARG);
        other[43] = ch[43];
        other[32] = (fwu_byte_t)(other[32] ^ 0x01u);
        expect("kimligin ILK baytinda fark: INVALID_ARG", hook->check(hook->context, other, 44u, resp, 65u), FWU_ERR_INVALID_ARG);
    }

    /* baglama: nonce, urun, cihaz, eski meydan okuma */
    {
        fwu_byte_t other[64];
        for(unsigned i = 0u; i < 44u; i++) {
            other[i] = ch[i];
        }
        other[0] = (fwu_byte_t)(other[0] ^ 0x01u);
        expect("NONCE'un ilk baytinda 1 bit fark (imza o meydan okumaya ait degil): BAD_SIGNATURE", hook->check(hook->context, other, 44u, resp, 65u), FWU_ERR_BAD_SIGNATURE);
        for(unsigned i = 0u; i < 44u; i++) {
            other[i] = ch[i];
        }
        other[31] = (fwu_byte_t)(other[31] ^ 0x80u);
        expect("NONCE'un SON baytinda fark: BAD_SIGNATURE", hook->check(hook->context, other, 44u, resp, 65u), FWU_ERR_BAD_SIGNATURE);
    }
    {
        /* eski yanit, yeni meydan okuma: yeniden oynatma */
        fwu_byte_t ch2[64];
        uint32_t len2 = 0u;
        (void)hook->begin(hook->context, ch2, 64u, &len2);
        expect("ESKI yanit YENI meydan okumaya (yeniden oynatma): BAD_SIGNATURE", hook->check(hook->context, ch2, len2, resp, 65u), FWU_ERR_BAD_SIGNATURE);
    }
    {
        /* baska urun */
        fwu_auth_t b;
        fwu_auth_config_t c2;
        make_auth_config(&c2);
        c2.product_id = TW_PRODUCT + 1u;
        (void)fwu_auth_init(&b, &c2);
        expect("BASKA URUN icin kurulmus cihaz, ayni meydan okuma ve yanit: BAD_SIGNATURE", fwu_auth_hook(&b)->check(fwu_auth_hook(&b)->context, ch, len, resp, 65u), FWU_ERR_BAD_SIGNATURE);
        c2.product_id = TW_PRODUCT;
        (void)fwu_auth_init(&b, &c2);
        expect("... ayni urun: OK (ayni sonuc, durumsuz dogrulama)", fwu_auth_hook(&b)->check(fwu_auth_hook(&b)->context, ch, len, resp, 65u), FWU_OK);
    }
    {
        /* baska cihaz: kendi meydan okumasi, A cihazinin yaniti */
        fwu_auth_t b;
        fwu_auth_config_t c2;
        fwu_byte_t chb[64];
        uint32_t lenb = 0u;
        make_auth_config(&c2);
        c2.device_id = g_dev_id_b;
        (void)fwu_auth_init(&b, &c2);
        (void)fwu_auth_hook(&b)->begin(fwu_auth_hook(&b)->context, chb, 64u, &lenb);
        check("B cihazinin meydan okumasi KENDI kimligini tasir", lenb == 44u && same_bytes(&chb[32], g_dev_id_b, 12u));
        expect("A cihazi icin uretilmis yanit B cihazinda: BAD_SIGNATURE (cihaza bagli)", fwu_auth_hook(&b)->check(fwu_auth_hook(&b)->context, chb, lenb, resp, 65u), FWU_ERR_BAD_SIGNATURE);
        build_response_for(chb, lenb, 0u, resp);
        expect("B icin dogru uretilmis yanit B'de: OK", fwu_auth_hook(&b)->check(fwu_auth_hook(&b)->context, chb, lenb, resp, 65u), FWU_OK);
    }

    /* kripto yapilamadi */
    tw_cs.fail_verify = FWU_ERR_CRYPTO_FAILURE;
    build_response_for(ch, len, 0u, resp);
    expect("kripto 'yapilamadi': CRYPTO_FAILURE aynen iletilir (imza gecersiz DEMEK DEGIL)", hook->check(hook->context, ch, len, resp, 65u), FWU_ERR_CRYPTO_FAILURE);
    tw_cs.fail_verify = FWU_OK;
    expect("kripto duzelince ayni yanit: OK", hook->check(hook->context, ch, len, resp, 65u), FWU_OK);

    /* anahtar kimligi arama: en sondaki anahtar da bulunur */
    {
        fwu_auth_t b;
        fwu_auth_config_t c2;
        fwu_verify_key_t ks[3];
        make_auth_config(&c2);
        ks[0].id = 9u; ks[1].id = 8u; ks[2].id = 7u;
        for(unsigned i = 0u; i < 32u; i++) {
            ks[0].public_key[i] = g_op_pk[2][i]; ks[1].public_key[i] = g_op_pk[1][i]; ks[2].public_key[i] = g_op_pk[0][i];
        }
        c2.operator_keys = ks; c2.operator_key_count = 3u;
        (void)fwu_auth_init(&b, &c2);
        build_response_for(ch, len, 0u, resp);
        resp[0] = 7u;
        expect("3 anahtar: SON yuvadaki anahtar (kimlik 7) bulunur", fwu_auth_hook(&b)->check(fwu_auth_hook(&b)->context, ch, len, resp, 65u), FWU_OK);
        c2.operator_key_count = 2u;
        (void)fwu_auth_init(&b, &c2);
        expect("anahtar sayisi 2 iken kimlik 7 (3. yuva) YOK sayilir: KEY_REJECTED (sayi siniri uygulaniyor)", fwu_auth_hook(&b)->check(fwu_auth_hook(&b)->context, ch, len, resp, 65u), FWU_ERR_KEY_REJECTED);
    }
#ifdef C2000
    {
        fwu_byte_t dirty[FWU_AUTH_RESPONSE_SIZE];
        fwu_byte_t chd[64];
        build_response_for(ch, len, 0u, resp);
        for(unsigned i = 0u; i < 65u; i++) {
            dirty[i] = (fwu_byte_t)(resp[i] | 0x100u);
        }
        for(unsigned i = 0u; i < 44u; i++) {
            chd[i] = (fwu_byte_t)(ch[i] | 0x100u);
        }
        expect("C2000 taklidi: kirli ust bitli yanit ve meydan okuma: OK (alt 8 bit)", hook->check(hook->context, chd, len, dirty, 65u), FWU_OK);
    }
#endif
}

static void test_host(void)
{
    fwu_byte_t ch[48];
    fwu_byte_t resp[FWU_AUTH_RESPONSE_SIZE];
    uint32_t n = 0u;
    fwu_auth_host_t h;
    const fwu_session_auth_t *hook;

    printf("\n--- C. host: fwu_auth_host_respond ---\n");
    world();
    for(unsigned i = 0u; i < 44u; i++) {
        ch[i] = (fwu_byte_t)(i * 3u + 1u);
    }
    ch[32] = g_dev_id[0];
    expect("respond", fwu_auth_host_respond(&g_host, ch, 44u, resp, 65u, &n), FWU_OK);
    check("... 65 bayt, imzalayici 1 kez cagrildi", n == 65u && g_signer.calls == 1u);
    {
        /* imzanin dayandigi mesaj = ortak kurucunun mesaji */
        fwu_byte_t msg[FWU_AUTH_MESSAGE_MAX];
        fwu_byte_t sig[64];
        uint32_t ml = 0u;
        (void)fwu_auth_build_message(TW_PRODUCT, ch, 44u, msg, 57u, &ml);
        fwu_crypto_sim_sign(g_op_pk[0], msg, ml, sig);
        check("yanitin imzasi, ORTAK mesaj kurucusunun mesajindan bagimsiz hesaplanan imzayla ayni", same_bytes(&resp[1], sig, 64u));
    }
    expect("capacity 64: BUFFER_TOO_SMALL (imzalayiciya gidilmeden)", fwu_auth_host_respond(&g_host, ch, 44u, resp, 64u, &n), FWU_ERR_BUFFER_TOO_SMALL);
    expect("meydan okuma 10 bayt: BAD_LENGTH", fwu_auth_host_respond(&g_host, ch, 10u, resp, 65u, &n), FWU_ERR_BAD_LENGTH);
    expect("meydan okuma 49: BAD_LENGTH", fwu_auth_host_respond(&g_host, ch, 49u, resp, 65u, &n), FWU_ERR_BAD_LENGTH);
    g_signer.calls = 0u;
    (void)fwu_auth_host_respond(&g_host, ch, 10u, resp, 65u, &n);
    check("... bozuk meydan okumada imzalayici CAGRILMADI (operatore sacma bir sey imzalatilmaz)", g_signer.calls == 0u);
    g_signer.fail = FWU_ERR_CRYPTO_FAILURE;
    expect("imzalayici hata dondurur: aynen iletilir", fwu_auth_host_respond(&g_host, ch, 44u, resp, 65u, &n), FWU_ERR_CRYPTO_FAILURE);
    g_signer.fail = FWU_OK;
    expect("context NULL", fwu_auth_host_respond(NULL, ch, 44u, resp, 65u, &n), FWU_ERR_INVALID_ARG);
    h = g_host; h.sign = NULL;
    expect("sign NULL", fwu_auth_host_respond(&h, ch, 44u, resp, 65u, &n), FWU_ERR_INVALID_ARG);
    expect("meydan okuma NULL", fwu_auth_host_respond(&g_host, NULL, 44u, resp, 65u, &n), FWU_ERR_INVALID_ARG);
    g_signer.calls = 0u;
    expect("yanit NULL", fwu_auth_host_respond(&g_host, ch, 44u, NULL, 65u, &n), FWU_ERR_INVALID_ARG);
    expect("uzunluk NULL", fwu_auth_host_respond(&g_host, ch, 44u, resp, 65u, NULL), FWU_ERR_INVALID_ARG);
    expect("meydan okuma NULL (tekrar)", fwu_auth_host_respond(&g_host, NULL, 44u, resp, 65u, &n), FWU_ERR_INVALID_ARG);
    check("... bu NULL argumanlarda imzalayici HIC CAGRILMADI (bos is icin operator anahtari kullanilmaz)", g_signer.calls == 0u);
    {
        /* host'un urun kimligi yanlissa cihaz reddeder */
        fwu_byte_t chd[64];
        uint32_t ld = 0u;
        hook = fwu_auth_hook(&g_auth);
        (void)hook->begin(hook->context, chd, 64u, &ld);
        h = g_host; h.product_id = TW_PRODUCT + 5u;
        (void)fwu_auth_host_respond(&h, chd, ld, resp, 65u, &n);
        expect("host YANLIS urun kimligiyle imzaladi: cihaz BAD_SIGNATURE", hook->check(hook->context, chd, ld, resp, 65u), FWU_ERR_BAD_SIGNATURE);
        (void)fwu_auth_host_respond(&g_host, chd, ld, resp, 65u, &n);
        expect("dogru urun kimligiyle: OK", hook->check(hook->context, chd, ld, resp, 65u), FWU_OK);
    }
}

/* ------------------------------------------------------------------ D. gercek oturumla */

static uint32_t g_now;
static fwu_byte_t g_rbuf[FWU_FRAME_MAX_SIZE];
static fwu_frame_view_t g_rv;
static size_t g_rn;

static void xchg(fwu_byte_t type, fwu_byte_t seq, const fwu_byte_t *payload, size_t length)
{
    fwu_byte_t frame[FWU_FRAME_MAX_SIZE];
    size_t n = 0u;
    fwu_frame_view_t v;

    (void)fwu_frame_encode(type, seq, payload, length, frame, FWU_FRAME_MAX_SIZE, &n);
    (void)fwu_frame_decode(frame, n, &v);
    g_rn = 0u;
    (void)fwu_session_on_frame(&tw_session, &v, g_now, g_rbuf, FWU_FRAME_MAX_SIZE, &g_rn);
    if(g_rn > 0u) {
        (void)fwu_frame_decode(g_rbuf, g_rn, &g_rv);
    }
}

static int resp_is(fwu_byte_t type)
{
    return (g_rn > 0u) && (g_rv.type == type);
}

static int resp_error(fwu_return_e code)
{
    return resp_is(FWU_MSG_ERROR) && (g_rv.payload_length == 2u) && ((g_rv.payload[0] & 0xFFu) == (fwu_byte_t)code);
}

static void test_session_flows(void)
{
    const fwu_byte_t one[1] = { 1u };
    const fwu_byte_t zero[1] = { 0u };
    fwu_byte_t c1[64];
    fwu_byte_t r1[FWU_AUTH_RESPONSE_SIZE];
    uint32_t n = 0u;
    uint32_t c1_len;

    printf("\n--- D. gercek oturumla: akis, yeniden oynatma, kilit, RNG arizasi ---\n");
    world();
    g_now = 1000u;
    xchg(FWU_MSG_HELLO, 1u, one, 1u);
    check("HELLO: INFO, yetkilendirme zorunlu bayragi (bit 0)", resp_is(FWU_MSG_INFO) && (g_rv.payload[1] & 0x01u) == 0x01u);
    xchg(FWU_MSG_AUTH_REQ, 2u, zero, 1u);
    check("AUTH_REQ: AUTH_CHALLENGE 44 bayt (nonce 32 + cihaz kimligi 12)", resp_is(FWU_MSG_AUTH_CHALLENGE) && g_rv.payload_length == 44u);
    check("... nonce = RNG'nin urettigi, cihaz kimligi = yapilandirmadaki", same_bytes(g_rv.payload, g_rng.last, 32u) && same_bytes(&g_rv.payload[32], g_dev_id, 12u));
    c1_len = (uint32_t)g_rv.payload_length;
    for(uint32_t i = 0u; i < c1_len; i++) {
        c1[i] = g_rv.payload[i];
    }
    xchg(FWU_MSG_AUTH_REQ, 2u, zero, 1u);
    check("AYNI SEQ ile AUTH_REQ tekrari (kayip yanit): AYNI meydan okuma, RNG'ye tekrar GIDILMEZ", resp_is(FWU_MSG_AUTH_CHALLENGE) && same_bytes(g_rv.payload, c1, 44u) && g_rng.calls == 1u);
    build_response_for(c1, c1_len, 0u, r1);
    xchg(FWU_MSG_AUTH_RESP, 3u, r1, 65u);
    check("operator yaniti: AUTH_OK, oturum YETKILI", resp_is(FWU_MSG_AUTH_OK) && fwu_session_get_state(&tw_session) == FWU_SESSION_AUTHENTICATED);
    xchg(FWU_MSG_AUTH_RESP, 3u, r1, 65u);
    check("AUTH_OK kayboldu, ayni yanit tekrari: AUTH_OK yeniden (idempotent), hata sayaci 0", resp_is(FWU_MSG_AUTH_OK) && tw_session.auth_failures == 0u);

    /* yeniden oynatma: yeni oturum, eski yanit (RNG dizisi korunur: gercek RNG yeniden baslatmada ayni diziye donmez) */
    g_keep_rng = true;
    world();
    g_now = 2000u;
    xchg(FWU_MSG_AUTH_REQ, 1u, zero, 1u);
    check("yeni oturum: YENI meydan okuma (oncekinden farkli nonce)", resp_is(FWU_MSG_AUTH_CHALLENGE) && !same_bytes(g_rv.payload, c1, 32u));
    xchg(FWU_MSG_AUTH_RESP, 2u, r1, 65u);
    check("ESKI yanit yeni oturumda (yeniden oynatma): ERROR BAD_SIGNATURE, oturum BOSTA", resp_error(FWU_ERR_BAD_SIGNATURE) && fwu_session_get_state(&tw_session) == FWU_SESSION_IDLE);
    check("... basarisiz deneme sayildi (1)", tw_session.auth_failures == 1u);
    g_keep_rng = false;

    /* UYARI: RNG yeniden baslatmada AYNI diziye donerse (sabit tohumlu PRNG) yeniden oynatma ACILIR: nonce karsilastirmasi yalniz RAM'de */
    world();
    g_now = 2500u;
    xchg(FWU_MSG_AUTH_REQ, 1u, zero, 1u);
    xchg(FWU_MSG_AUTH_RESP, 2u, r1, 65u);
    check("RNG yeniden baslatmada AYNI diziye dondu (kotu RNG): eski yanit KABUL edilir (RNG zorunlulugunun nedeni)", resp_is(FWU_MSG_AUTH_OK));

    /* yetkisiz operator ve kilit */
    world();
    g_now = 3000u;
    for(unsigned k = 0u; k < 3u; k++) {
        fwu_byte_t c[64];
        fwu_byte_t r[FWU_AUTH_RESPONSE_SIZE];
        xchg(FWU_MSG_AUTH_REQ, (fwu_byte_t)(10u + 2u * k), zero, 1u);
        for(uint32_t i = 0u; i < g_rv.payload_length; i++) {
            c[i] = g_rv.payload[i];
        }
        build_response_for(c, (uint32_t)g_rv.payload_length, 2u, r);          /* saldirgan anahtari */
        xchg(FWU_MSG_AUTH_RESP, (fwu_byte_t)(11u + 2u * k), r, 65u);
        check(k == 2u ? "3. basarisiz deneme (saldirgan): BAD_SIGNATURE ve KILIT" : "basarisiz deneme (saldirgan): BAD_SIGNATURE", resp_error(FWU_ERR_BAD_SIGNATURE));
    }
    xchg(FWU_MSG_AUTH_REQ, 30u, zero, 1u);
    check("kilitliyken AUTH_REQ: LOCKED_OUT (RNG'ye dokunulmaz)", resp_error(FWU_ERR_LOCKED_OUT) && g_rng.calls == 3u);
    g_now = 3000u + 4999u;
    xchg(FWU_MSG_AUTH_REQ, 31u, zero, 1u);
    check("kilit bitmeden 1 ms once: hala LOCKED_OUT", resp_error(FWU_ERR_LOCKED_OUT));
    g_now = 3000u + 5000u;
    xchg(FWU_MSG_AUTH_REQ, 32u, zero, 1u);
    check("kilit suresi dolunca: yeni meydan okuma", resp_is(FWU_MSG_AUTH_CHALLENGE));
    {
        fwu_byte_t c[64];
        fwu_byte_t r[FWU_AUTH_RESPONSE_SIZE];
        for(uint32_t i = 0u; i < g_rv.payload_length; i++) {
            c[i] = g_rv.payload[i];
        }
        build_response_for(c, (uint32_t)g_rv.payload_length, 0u, r);
        xchg(FWU_MSG_AUTH_RESP, 33u, r, 65u);
        check("... gercek operatorun yaniti: AUTH_OK (kilit sonrasi temiz baslangic)", resp_is(FWU_MSG_AUTH_OK));
    }

    /* RNG arizasi: AUTH_REQ hata, oturum BOSTA, sonraki deneme calisir */
    world();
    g_now = 4000u;
    g_rng.fail = FWU_ERR_CRYPTO_FAILURE;
    xchg(FWU_MSG_AUTH_REQ, 1u, zero, 1u);
    check("RNG arizali: AUTH_REQ'e ERROR CRYPTO_FAILURE, oturum BOSTA (yetkilendirme acilmadi)", resp_error(FWU_ERR_CRYPTO_FAILURE) && fwu_session_get_state(&tw_session) == FWU_SESSION_IDLE);
    xchg(FWU_MSG_REQUEST_DOWNLOAD, 2u, tw_manifest, 138u);
    check("... indirme istegi BAD_STATE (yetkilendirmesiz indirme yok)", resp_error(FWU_ERR_BAD_STATE));
    g_rng.fail = FWU_OK;
    xchg(FWU_MSG_AUTH_REQ, 3u, zero, 1u);
    check("RNG duzelince yeni AUTH_REQ: meydan okuma", resp_is(FWU_MSG_AUTH_CHALLENGE));
    (void)n;
}

/* ------------------------------------------------------------------ E. istemci + oturum, kusursuz tel */

#define RESP_TIMEOUT 100u

static fwu_client_t g_client;
static fwu_client_config_t g_cc;

static void make_client_config(void)
{
    g_cc.manifest = tw_manifest;
    g_cc.image = tw_image;
    g_cc.image_size = TW_IMG_SIZE;
    g_cc.auth = fwu_auth_host_respond;
    g_cc.auth_context = &g_host;
    g_cc.expected_product = 0u;
    g_cc.response_timeout_ms = RESP_TIMEOUT;
    g_cc.max_retries = 8u;
    g_cc.verify_timeout_ms = 2000u;
    g_cc.max_restarts = 2u;
}

/** Kusursuz tel: istemci cercevesi dogrudan oturuma, yanit dogrudan istemciye. */
static void run_perfect(uint32_t max_ms)
{
    (void)fwu_client_init(&g_client, &g_cc);
    (void)fwu_client_start(&g_client, g_now);
    for(uint32_t ms = 0u; (ms < max_ms) && !fwu_client_is_finished(&g_client); ms++) {
        fwu_byte_t buf[FWU_FRAME_MAX_SIZE];
        size_t n = 0u;
        fwu_frame_view_t v;

        g_now++;
        (void)fwu_client_poll(&g_client, g_now, buf, FWU_FRAME_MAX_SIZE, &n);
        if(n > 0u) {
            fwu_byte_t out[FWU_FRAME_MAX_SIZE];
            size_t on = 0u;
            if(fwu_frame_decode(buf, n, &v) == FWU_OK) {
                (void)fwu_session_on_frame(&tw_session, &v, g_now, out, FWU_FRAME_MAX_SIZE, &on);
                if((on > 0u) && (fwu_frame_decode(out, on, &v) == FWU_OK)) {
                    (void)fwu_client_on_frame(&g_client, &v, g_now);
                }
            }
        }
        {
            fwu_byte_t out[FWU_FRAME_MAX_SIZE];
            size_t on = 0u;
            (void)fwu_session_poll(&tw_session, g_now, out, FWU_FRAME_MAX_SIZE, &on);
            if((on > 0u) && (fwu_frame_decode(out, on, &v) == FWU_OK)) {
                (void)fwu_client_on_frame(&g_client, &v, g_now);
            }
        }
    }
}

static fwu_client_result_t cresult(void)
{
    fwu_client_result_t r;
    (void)fwu_client_get_result(&g_client, &r);
    return r;
}

static void test_end_to_end(void)
{
    fwu_activate_outcome_e outcome = FWU_ACT_OUTCOME_NONE;
    fwu_client_result_t r;

    printf("\n--- E. istemci + oturum + aktivasyon, gercek yetkilendirme ---\n");
    world();
    make_client_config();
    g_now = 1000u;
    run_perfect(20000u);
    r = cresult();
    check("operator yetkilendirmesi + tam guncelleme: istemci DONE", r.state == FWU_CLIENT_DONE && r.failure == FWU_CLIENT_FAIL_NONE);
    check("... 17 cerceve (15 + AUTH_REQ + AUTH_RESP), tekrar yok", r.frames_sent == 17u && r.retransmits == 0u && r.restarts == 0u);
    check("... RNG 1 kez, imzalayici 1 kez", g_rng.calls == 1u && g_signer.calls == 1u);
    check("... uygulama bu ana kadar ESKI (commit en son)", tw_app_is_old());
    expect("bootloader", tw_bootloader_run(&outcome), FWU_OK);
    check("... ACTIVATED, uygulama TAM YENI", outcome == FWU_ACT_OUTCOME_ACTIVATED && tw_app_is_new());

    /* yetkisiz operator: indirme hic baslamaz */
    world();
    make_client_config();
    g_signer.slot = 2u;
    g_now = 1000u;
    run_perfect(20000u);
    r = cresult();
    check("saldirgan anahtariyla imzalayan istemci: FAILED(DEVICE_ERROR, BAD_SIGNATURE 50)", r.state == FWU_CLIENT_FAILED && r.failure == FWU_CLIENT_FAIL_DEVICE_ERROR && r.device_error == FWU_ERR_BAD_SIGNATURE);
    check("... cihaza HICBIR indirme gitmedi: flash islemi 0, uygulama ESKI, oturum BOSTA", tw_sim.base.operations_done == 0u && tw_app_is_old() && fwu_session_get_state(&tw_session) == FWU_SESSION_IDLE);
    {
        fwu_activate_outcome_e o = FWU_ACT_OUTCOME_NONE;
        expect("bootloader", tw_bootloader_run(&o), FWU_OK);
        check("... bekleyen is yok, uygulama ESKI", o == FWU_ACT_OUTCOME_NONE && tw_app_is_old());
    }

    /* bilinmeyen anahtar kimligi */
    world();
    make_client_config();
    g_signer.forced_id = 9u;
    g_now = 1000u;
    run_perfect(20000u);
    r = cresult();
    check("bilinmeyen operator anahtar kimligi: FAILED(DEVICE_ERROR, KEY_REJECTED 54)", r.failure == FWU_CLIENT_FAIL_DEVICE_ERROR && r.device_error == FWU_ERR_KEY_REJECTED && tw_sim.base.operations_done == 0u);

    /* kilit: 3 basarisiz istemci, sonra dogru operator da kilitli */
    world();
    make_client_config();
    g_now = 1000u;
    g_signer.slot = 2u;
    for(unsigned k = 0u; k < 3u; k++) {
        run_perfect(20000u);
    }
    r = cresult();
    check("3 art arda basarisiz yetkilendirme: oturum KILITLI", tw_session.locked && r.device_error == FWU_ERR_BAD_SIGNATURE);
    g_signer.slot = 0u;
    run_perfect(20000u);
    r = cresult();
    check("kilitliyken DOGRU operator bile: FAILED(DEVICE_ERROR, LOCKED_OUT 62), indirme yok", r.failure == FWU_CLIENT_FAIL_DEVICE_ERROR && r.device_error == FWU_ERR_LOCKED_OUT && tw_sim.base.operations_done == 0u);
    g_now += 5000u;
    run_perfect(20000u);
    r = cresult();
    check("kilit suresi dolunca dogru operator: DONE", r.state == FWU_CLIENT_DONE);
    expect("bootloader", tw_bootloader_run(&outcome), FWU_OK);
    check("... uygulama TAM YENI", tw_app_is_new());

    /* RNG tamamen arizali: yetkilendirme acilmaz, indirme yok */
    world();
    make_client_config();
    g_rng.zero = true;
    g_now = 1000u;
    run_perfect(20000u);
    r = cresult();
    check("RNG arizali cihaz: istemci FAILED(DEVICE_ERROR, CRYPTO_FAILURE 51), indirme yok (kapali hata)", r.failure == FWU_CLIENT_FAIL_DEVICE_ERROR && r.device_error == FWU_ERR_CRYPTO_FAILURE &&
          tw_sim.base.operations_done == 0u);
}

/* ------------------------------------------------------------------ F. kayipli hat */

static fwu_linksim_t g_h2d;
static fwu_linksim_t g_d2h;

static void profile_set(fwu_linksim_profile_t *p, uint32_t drop, uint32_t dup, uint32_t corrupt, uint32_t trunc, uint32_t delay, uint32_t garbage, uint32_t delay_max)
{
    p->drop_pm = drop; p->duplicate_pm = dup; p->corrupt_pm = corrupt; p->truncate_pm = trunc; p->delay_pm = delay; p->garbage_pm = garbage; p->delay_max_ms = delay_max;
}

/** Cerceve basina bir kayipli obek; bozuk obek cerceve cozulemedigi icin dusurulur. Doner: bitti mi. */
static bool run_lossy(uint32_t seed, const fwu_linksim_profile_t *profile, uint32_t max_ms)
{
    uint32_t t0 = g_now;

    (void)fwu_linksim_init(&g_h2d, profile, seed * 2u + 1u);
    (void)fwu_linksim_init(&g_d2h, profile, seed * 2u + 2u);
    (void)fwu_client_init(&g_client, &g_cc);
    (void)fwu_client_start(&g_client, g_now);
    for(uint32_t ms = 0u; (ms < max_ms) && !fwu_client_is_finished(&g_client); ms++) {
        fwu_byte_t buf[FWU_LINKSIM_CHUNK_MAX];
        fwu_byte_t out[FWU_FRAME_MAX_SIZE];
        fwu_frame_view_t v;
        size_t k;
        size_t n = 0u;
        size_t on = 0u;

        g_now = t0 + ms + 1u;
        while((k = fwu_linksim_receive(&g_d2h, g_now, buf, sizeof(buf) / sizeof(buf[0]))) > 0u) {
            if(fwu_frame_decode(buf, k, &v) == FWU_OK) {
                (void)fwu_client_on_frame(&g_client, &v, g_now);
            }
        }
        (void)fwu_client_poll(&g_client, g_now, buf, FWU_FRAME_MAX_SIZE, &n);
        if(n > 0u) {
            (void)fwu_linksim_send(&g_h2d, buf, n, g_now);
        }
        while((k = fwu_linksim_receive(&g_h2d, g_now, buf, sizeof(buf) / sizeof(buf[0]))) > 0u) {
            if(fwu_frame_decode(buf, k, &v) == FWU_OK) {
                on = 0u;
                (void)fwu_session_on_frame(&tw_session, &v, g_now, out, FWU_FRAME_MAX_SIZE, &on);
                if(on > 0u) {
                    (void)fwu_linksim_send(&g_d2h, out, on, g_now);
                }
            }
        }
        on = 0u;
        (void)fwu_session_poll(&tw_session, g_now, out, FWU_FRAME_MAX_SIZE, &on);
        if(on > 0u) {
            (void)fwu_linksim_send(&g_d2h, out, on, g_now);
        }
    }
    return fwu_client_is_finished(&g_client);
}

static void test_lossy(void)
{
    uint32_t done = 0u;
    uint32_t violations = 0u;
    uint32_t total_begin = 0u;
    uint32_t total_retx = 0u;
    fwu_linksim_profile_t p;

    printf("\n--- F. kayipli hat: gercek yetkilendirme kancalariyla yuzlerce tohum ---\n");
    profile_set(&p, 100u, 50u, 50u, 30u, 100u, 30u, 200u);
    for(uint32_t seed = 1u; seed <= 150u; seed++) {
        fwu_activate_outcome_e outcome = FWU_ACT_OUTCOME_NONE;
        fwu_client_result_t r;

        world();
        make_client_config();
        g_cc.max_retries = 30u;
        g_cc.max_restarts = 3u;
        g_now = 1000u;
        (void)run_lossy(seed, &p, 120000u);
        r = cresult();
        total_begin += g_rng.calls;
        total_retx += r.retransmits;
        if(r.state == FWU_CLIENT_DONE) {
            done++;
            if(tw_bootloader_run(&outcome) != FWU_OK || !tw_app_is_new()) {
                violations++;                           /* DONE ama uygulama tam yeni degil */
            }
        } else {
            printf("(tani: tohum %u basarisiz: durum %d, basarisizlik %d, cihaz hatasi %d, tekrar %u)\n", (unsigned)seed, (int)r.state, (int)r.failure,
                   (int)r.device_error, (unsigned)r.retransmits);
        }
        if(tw_session.auth_failures != 0u) {
            violations++;                               /* dogru operator, kayipli hatta bile HIC basarisiz deneme sayilmamali (kilit riski) */
        }
    }
    check("orta kayipli hat (%10 kayip, tekrar, bozulma, kesilme, gecikme, cop), 150 tohum: HEPSI DONE", done == 150u);
    check("... her koşuda uygulama TAM YENI, ve dogru operator icin hata sayaci hep 0 (kayip/tekrar yuzunden haksiz kilit YOK)", violations == 0u);
    printf("(bilgi: toplam meydan okuma uretimi %u, toplam istemci yeniden gonderimi %u)\n", (unsigned)total_begin, (unsigned)total_retx);
    check("... tarama anlamli: yeniden gonderim goruldu, meydan okuma sayisi tohum sayisindan buyuk degil ama >= tohum sayisi", total_retx > 200u && total_begin >= 150u);

    /* yetkisiz operator, kayipli hat: HICBIR tohumda indirme/commit yok */
    {
        uint32_t leaks = 0u;
        for(uint32_t seed = 1u; seed <= 60u; seed++) {
            fwu_activate_outcome_e outcome = FWU_ACT_OUTCOME_NONE;
            world();
            make_client_config();
            g_cc.max_retries = 30u;
            g_cc.max_restarts = 3u;
            g_signer.slot = 2u;
            g_now = 1000u;
            (void)run_lossy(seed, &p, 60000u);
            if((cresult().state == FWU_CLIENT_DONE) || (tw_sim.base.operations_done != 0u)) {
                leaks++;
            }
            (void)tw_bootloader_run(&outcome);
            if(!tw_app_is_old()) {
                leaks++;
            }
        }
        check("SALDIRGAN operator + kayipli hat, 60 tohum: hicbirinde DONE, flash islemi veya yeni uygulama YOK", leaks == 0u);
    }
}

int main(void)
{
    test_formats();
    test_init_and_begin();
    test_check();
    test_host();
    test_session_flows();
    test_end_to_end();
    test_lossy();

    printf("\nResult: %s (%d failed)\n", g_failures == 0 ? "ALL TESTS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
