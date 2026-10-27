/**
 * @file    test_crypto_sim.c
 * @brief   Sahte kriptografi portunun (fwu_crypto_sim) testi, gerçek arayüz sarmalayıcılarıyla birlikte.
 *
 * Beklenen değerler gerçekleme kodundan DEĞİL, bağımsız kaynaklardan gelir:
 *   - SHA-256: FIPS 180-4 / NIST resmî test vektörleri ("", "abc", 56 bayt, 1.000.000 x 'a') ve Python hashlib
 *     ile hesaplanmış, dolgu sınırlarını (55/56/57/63/64/65/119/120/127/128/129 eleman) zorlayan özetler.
 *   - Sahte imza: Python ile hesaplanmış bilinen-cevap vektörü.
 * Kapsam:
 *   1. SHA-256 doğruluğu (resmî vektörler, dolgu sınırları, tek seferde = parça parça, HER bölünme noktası).
 *   2. Sarmalayıcı + sahte port birlikte: sayaçlar, hata enjeksiyonu, kirli üst bitler (C28x).
 *   3. Sahte imza: bilinen cevap, geçerli imza, imzanın/mesajın/anahtarın HER elemanının tek bit bozulması.
 *
 * Derleme (crypto klasöründen):
 *   gcc -Wall -Wextra -Wpedantic -Wconversion -std=c99 -I. -I.. -I../error fwu_crypto.c fwu_crypto_sim.c test_crypto_sim.c -o test_crypto_sim && ./test_crypto_sim
 */

#include <stdio.h>
#include "fwu_crypto_sim.h"

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

/** "ba7816bf..." gibi onaltılık metni `count` elemana çevirir. */
static void from_hex(const char *hex, fwu_byte_t *out, uint32_t count)
{
    for(uint32_t i = 0u; i < count; i++) {
        uint32_t value = 0u;
        for(uint32_t k = 0u; k < 2u; k++) {
            const char ch = hex[(2u * i) + k];
            uint32_t digit = 0u;
            if((ch >= '0') && (ch <= '9')) {
                digit = (uint32_t)(ch - '0');
            } else if((ch >= 'a') && (ch <= 'f')) {
                digit = (uint32_t)(ch - 'a') + 10u;
            }
            value = (value << 4) | digit;
        }
        out[i] = (fwu_byte_t)value;
    }
}

static int digest_is(const fwu_byte_t *digest, const char *hex)
{
    fwu_byte_t want[32];
    from_hex(hex, want, 32u);
    for(uint32_t i = 0u; i < 32u; i++) {
        if((digest[i] & 0xFFu) != want[i]) {
            return 0;
        }
    }
    return 1;
}

/** Bağımsız desen: i. eleman = (i*7+3) & 0xFF (Python tarafında aynı formül). */
static void make_pattern(fwu_byte_t *out, uint32_t count)
{
    for(uint32_t i = 0u; i < count; i++) {
        out[i] = (fwu_byte_t)((i * 7u + 3u) & 0xFFu);
    }
}

static void test_sha256_vectors(void)
{
    static fwu_byte_t pat[256];
    fwu_byte_t digest[32];
    const char *abc = "abc";
    const char *l448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    fwu_byte_t text[56];
    fwu_byte_t chunk[1000];
    fwu_crypto_sim_t sim;
    int ok = 1;
    const struct { uint32_t length; const char *hex; } known[] = {
        { 1u,   "084fed08b978af4d7d196a7446a86b58009e636b611db16211b65a9aadff29c5" },
        { 55u,  "e7313d333c272e639f790978283f9eb392e843d0f29b7016828bb1daa4aac70b" },
        { 56u,  "4324d65f3c103567f5589c710bc08f8523f929a9272e3af36fc968e52abc6c27" },
        { 57u,  "35df609437dcfea3279283ab79fd554e2bf78f8f7ae2de532d8ee300b09e8f73" },
        { 63u,  "81c80242132f230c3bd41b3e63bbcff16107339549214a99614ff26664625055" },
        { 64u,  "39e3d7b6b5d075d37d053ad89b24b41bef4f3c29760c84447cab3f3be1882241" },
        { 65u,  "aacca6ff74fdbb296d165a45cecfa04e5127bc008770fbbdd48006f2d2fae95e" },
        { 119u, "9ce7368e4daf32341631b492e80359dc9f594b48453cd0dd5bf0b19279cc177e" },
        { 120u, "7836b787757e95e58b3ca5aec90b1b004e8deba1e50e9675af9cabf1a13a04b5" },
        { 127u, "a8d23e75d936f303d248888d9b165ee543f4cbafcad3c9dd2a79bd84faa11d07" },
        { 128u, "d2742f1f4ac6bb7ca2b239ee18402ba8b3f9f8e652d2a72973c2b9ba11c08cf6" },
        { 129u, "307f8fc2c1622b92762e818d39a185d4d667ad49a4b07ceae1f4afa008a93ec4" },
        { 200u, "2c7e18c942ef065b526a2d4e5546283749cd3ddfb51d8fc71f42717363685f46" }
    };

    printf("\n--- SHA-256: resmi vektorler ve dolgu sinirlari ---\n");
    make_pattern(pat, 256u);

    fwu_crypto_sim_sha256(pat, 0u, digest);
    check("bos mesaj (FIPS): e3b0c442...b855", digest_is(digest, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));

    for(uint32_t i = 0u; i < 3u; i++) {
        text[i] = (fwu_byte_t)abc[i];
    }
    fwu_crypto_sim_sha256(text, 3u, digest);
    check("\"abc\" (FIPS): ba7816bf...15ad", digest_is(digest, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

    for(uint32_t i = 0u; i < 56u; i++) {
        text[i] = (fwu_byte_t)l448[i];
    }
    fwu_crypto_sim_sha256(text, 56u, digest);
    check("56 baytlik \"abcdbcde...\" (FIPS, iki blok): 248d6a61...06c1", digest_is(digest, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

    for(unsigned k = 0u; k < 13u; k++) {
        fwu_crypto_sim_sha256(pat, known[k].length, digest);
        if(!digest_is(digest, known[k].hex)) {
            ok = 0;
            printf("    uzunluk %u: uyusmadi\n", (unsigned)known[k].length);
        }
    }
    check("13 desen uzunlugu (1,55,56,57,63,64,65,119,120,127,128,129,200): hashlib ile ayni", ok);

    /* 1.000.000 x 'a' (FIPS): parca parca, 1000 cagri x 1000 eleman */
    for(uint32_t i = 0u; i < 1000u; i++) {
        chunk[i] = (fwu_byte_t)'a';
    }
    (void)fwu_crypto_sim_init(&sim);
    (void)fwu_crypto_sha256_begin(&sim.crypto);
    for(uint32_t i = 0u; i < 1000u; i++) {
        (void)fwu_crypto_sha256_update(&sim.crypto, chunk, 1000u);
    }
    expect("1.000.000 x 'a': finish", fwu_crypto_sha256_finish(&sim.crypto, digest), FWU_OK);
    check("1.000.000 x 'a' (FIPS): cdc76e5c...2cd0", digest_is(digest, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

static void test_sha256_streaming(void)
{
    static fwu_byte_t pat[256];
    fwu_byte_t one_shot[32];
    fwu_byte_t streamed[32];
    fwu_crypto_sim_t sim;
    int split2_ok = 1;
    int split3_ok = 1;
    int bytewise_ok = 1;

    printf("\n--- SHA-256: parca parca = tek seferde (her bolunme noktasi) ---\n");
    make_pattern(pat, 256u);
    (void)fwu_crypto_sim_init(&sim);

    /* 130 elemanlik mesaj: her olasi tek bolunme (0..130) */
    fwu_crypto_sim_sha256(pat, 130u, one_shot);
    for(uint32_t cut = 0u; cut <= 130u; cut++) {
        (void)fwu_crypto_sha256_begin(&sim.crypto);
        (void)fwu_crypto_sha256_update(&sim.crypto, pat, cut);
        (void)fwu_crypto_sha256_update(&sim.crypto, &pat[cut], 130u - cut);
        (void)fwu_crypto_sha256_finish(&sim.crypto, streamed);
        for(uint32_t i = 0u; i < 32u; i++) {
            if(streamed[i] != one_shot[i]) {
                split2_ok = 0;
            }
        }
    }
    check("130 eleman, her tek bolunme noktasi (0..130): ayni ozet", split2_ok);

    /* iki bolunmeli (uc parca): blok sinirlarinin etrafinda */
    for(uint32_t a = 50u; a <= 70u; a++) {
        for(uint32_t b = a; b <= 130u; b += 7u) {
            (void)fwu_crypto_sha256_begin(&sim.crypto);
            (void)fwu_crypto_sha256_update(&sim.crypto, pat, a);
            (void)fwu_crypto_sha256_update(&sim.crypto, &pat[a], b - a);
            (void)fwu_crypto_sha256_update(&sim.crypto, &pat[b], 130u - b);
            (void)fwu_crypto_sha256_finish(&sim.crypto, streamed);
            for(uint32_t i = 0u; i < 32u; i++) {
                if(streamed[i] != one_shot[i]) {
                    split3_ok = 0;
                }
            }
        }
    }
    check("uc parca, blok siniri etrafinda: ayni ozet", split3_ok);

    /* bayt bayt */
    (void)fwu_crypto_sha256_begin(&sim.crypto);
    for(uint32_t i = 0u; i < 130u; i++) {
        (void)fwu_crypto_sha256_update(&sim.crypto, &pat[i], 1u);
    }
    (void)fwu_crypto_sha256_finish(&sim.crypto, streamed);
    for(uint32_t i = 0u; i < 32u; i++) {
        if(streamed[i] != one_shot[i]) {
            bytewise_ok = 0;
        }
    }
    check("130 eleman, bayt bayt: ayni ozet", bytewise_ok);

    /* ara bos update'ler (length 0) ozeti degistirmez */
    (void)fwu_crypto_sha256_begin(&sim.crypto);
    (void)fwu_crypto_sha256_update(&sim.crypto, pat, 64u);
    (void)fwu_crypto_sha256_update(&sim.crypto, pat, 0u);
    (void)fwu_crypto_sha256_update(&sim.crypto, NULL, 0u);
    (void)fwu_crypto_sha256_update(&sim.crypto, &pat[64], 66u);
    (void)fwu_crypto_sha256_finish(&sim.crypto, streamed);
    {
        int same = 1;
        for(uint32_t i = 0u; i < 32u; i++) {
            if(streamed[i] != one_shot[i]) {
                same = 0;
            }
        }
        check("araya bos update'ler (length 0, data NULL dahil) girse de 130 elemanin ozeti ayni", same);
    }
}

static void test_sim_port(void)
{
    static fwu_byte_t pat[256];
    fwu_crypto_sim_t sim;
    fwu_byte_t digest[32];
    fwu_byte_t dirty[130];
    fwu_byte_t clean_digest[32];
    fwu_byte_t dirty_digest[32];
    int dirty_same = 1;

    printf("\n--- sahte port + sarmalayici: sayaclar, hata enjeksiyonu, kirli ust bitler ---\n");
    make_pattern(pat, 256u);
    expect("init: NULL", fwu_crypto_sim_init(NULL), FWU_ERR_INVALID_ARG);
    /* init, onceden kirli bir yapiyi tamamen temizlemeli (yigin cop ile dolu olabilir) */
    sim.begin_calls = 7u;
    sim.update_calls = 7u;
    sim.finish_calls = 7u;
    sim.verify_calls = 7u;
    sim.last_verify_message_length = 7u;
    sim.fail_begin = FWU_ERR_BAD_SIGNATURE;
    sim.fail_update = FWU_ERR_BAD_SIGNATURE;
    sim.fail_finish = FWU_ERR_BAD_SIGNATURE;
    sim.fail_verify = FWU_ERR_BAD_SIGNATURE;
    sim.crypto.hashing = true;
    expect("init", fwu_crypto_sim_init(&sim), FWU_OK);
    check("init kirli yapiyi temizledi: son mesaj uzunlugu 0", sim.last_verify_message_length == 0u);
    check("init: sayaclar sifir, hata enjeksiyonu kapali, hashing false",
          sim.begin_calls == 0u && sim.update_calls == 0u && sim.finish_calls == 0u && sim.verify_calls == 0u &&
          sim.fail_begin == FWU_OK && sim.fail_update == FWU_OK && sim.fail_finish == FWU_OK &&
          sim.fail_verify == FWU_OK && !sim.crypto.hashing);

    (void)fwu_crypto_sha256_begin(&sim.crypto);
    (void)fwu_crypto_sha256_update(&sim.crypto, pat, 10u);
    (void)fwu_crypto_sha256_update(&sim.crypto, pat, 0u);
    (void)fwu_crypto_sha256_update(&sim.crypto, pat, 20u);
    (void)fwu_crypto_sha256_finish(&sim.crypto, digest);
    check("sayaclar: begin 1, update 2 (length 0 sayilmaz), finish 1",
          sim.begin_calls == 1u && sim.update_calls == 2u && sim.finish_calls == 1u && sim.verify_calls == 0u);

    /* hata enjeksiyonu: sarmalayici CRYPTO_FAILURE'a cevirir */
    sim.fail_begin = FWU_ERR_FLASH_WRITE;
    expect("enjekte begin hatasi", fwu_crypto_sha256_begin(&sim.crypto), FWU_ERR_CRYPTO_FAILURE);
    sim.fail_begin = FWU_OK;
    (void)fwu_crypto_sha256_begin(&sim.crypto);
    sim.fail_update = FWU_ERR_CRYPTO_FAILURE;
    expect("enjekte update hatasi", fwu_crypto_sha256_update(&sim.crypto, pat, 5u), FWU_ERR_CRYPTO_FAILURE);
    check("... ozet kapandi", !sim.crypto.hashing);
    sim.fail_update = FWU_OK;
    (void)fwu_crypto_sha256_begin(&sim.crypto);
    sim.fail_finish = FWU_ERR_CRYPTO_FAILURE;
    expect("enjekte finish hatasi", fwu_crypto_sha256_finish(&sim.crypto, digest), FWU_ERR_CRYPTO_FAILURE);
    sim.fail_finish = FWU_OK;

    /* kirli ust bitler (C28x): ozet temiz veriyle ayni olmali */
    for(uint32_t i = 0u; i < 130u; i++) {
        dirty[i] = (fwu_byte_t)(pat[i] | 0xAB00u);
    }
    fwu_crypto_sim_sha256(pat, 130u, clean_digest);
    fwu_crypto_sim_sha256(dirty, 130u, dirty_digest);
    for(uint32_t i = 0u; i < 32u; i++) {
        if(clean_digest[i] != dirty_digest[i]) {
            dirty_same = 0;
        }
        if((dirty_digest[i] & 0xFF00u) != 0u) {
            dirty_same = 0;
        }
    }
    check("kirli ust bitli veri, temiz veriyle AYNI ozeti verir; cikti elemanlari 0..255", dirty_same);
}

static void test_fake_signature(void)
{
    fwu_crypto_sim_t sim;
    fwu_byte_t key[32];
    fwu_byte_t msg[74];
    fwu_byte_t sig[64];
    fwu_byte_t want[64];
    fwu_byte_t tampered_sig[64];
    fwu_byte_t tampered_msg[74];
    fwu_byte_t tampered_key[32];
    int sig_ok = 1;
    int msg_ok = 1;
    int key_ok = 1;
    int bit_ok = 1;
    int dirty_ok = 1;

    printf("\n--- sahte imza: bilinen cevap ve her elemanin bozulmasi ---\n");
    (void)fwu_crypto_sim_init(&sim);
    for(uint32_t i = 0u; i < 32u; i++) {
        key[i] = (fwu_byte_t)i;
    }
    make_pattern(msg, 74u);

    fwu_crypto_sim_sign(key, msg, 74u, sig);
    from_hex("cf524baa82c9979c02ee3b27e6b47652100d502a9e0354a161f582742b9db2e348bf732286d6b2c6c1e2dec7c82d252e35f13a9b2896aff13457f22f3dc41cf2", want, 64u);
    {
        int same = 1;
        for(uint32_t i = 0u; i < 64u; i++) {
            if(sig[i] != want[i]) {
                same = 0;
            }
        }
        check("bilinen cevap (Python): sahte imza = SHA256(anahtar||mesaj) + SHA256(mesaj||anahtar)", same);
    }

    expect("gecerli imza", fwu_crypto_ed25519_verify(&sim.crypto, msg, 74u, sig, key), FWU_OK);
    check("... sayac ve mesaj uzunlugu", sim.verify_calls == 1u && sim.last_verify_message_length == 74u);

    /* imzanin HER elemaninda tek bit bozulma */
    for(uint32_t i = 0u; i < 64u; i++) {
        for(uint32_t k = 0u; k < 64u; k++) {
            tampered_sig[k] = sig[k];
        }
        tampered_sig[i] = (fwu_byte_t)(tampered_sig[i] ^ 0x01u);
        if(fwu_crypto_ed25519_verify(&sim.crypto, msg, 74u, tampered_sig, key) != FWU_ERR_BAD_SIGNATURE) {
            sig_ok = 0;
        }
    }
    check("imzanin 64 elemanindan her birinde tek bit bozulma -> BAD_SIGNATURE", sig_ok);

    /* mesajin HER elemaninda */
    for(uint32_t i = 0u; i < 74u; i++) {
        for(uint32_t k = 0u; k < 74u; k++) {
            tampered_msg[k] = msg[k];
        }
        tampered_msg[i] = (fwu_byte_t)(tampered_msg[i] ^ 0x80u);
        if(fwu_crypto_ed25519_verify(&sim.crypto, tampered_msg, 74u, sig, key) != FWU_ERR_BAD_SIGNATURE) {
            msg_ok = 0;
        }
    }
    check("mesajin 74 elemanindan her birinde tek bit bozulma -> BAD_SIGNATURE", msg_ok);

    /* anahtarin HER elemaninda (yanlis anahtar) */
    for(uint32_t i = 0u; i < 32u; i++) {
        for(uint32_t k = 0u; k < 32u; k++) {
            tampered_key[k] = key[k];
        }
        tampered_key[i] = (fwu_byte_t)(tampered_key[i] ^ 0x10u);
        if(fwu_crypto_ed25519_verify(&sim.crypto, msg, 74u, sig, tampered_key) != FWU_ERR_BAD_SIGNATURE) {
            key_ok = 0;
        }
    }
    check("anahtarin 32 elemanindan her birinde bozulma (yanlis anahtar) -> BAD_SIGNATURE", key_ok);

    expect("mesaj bir eleman kisaltilmis (73)", fwu_crypto_ed25519_verify(&sim.crypto, msg, 73u, sig, key), FWU_ERR_BAD_SIGNATURE);
    {
        fwu_byte_t longer[75];
        for(uint32_t i = 0u; i < 74u; i++) {
            longer[i] = msg[i];
        }
        longer[74] = 0u;
        expect("mesaja bir eleman eklenmis (75)", fwu_crypto_ed25519_verify(&sim.crypto, longer, 75u, sig, key), FWU_ERR_BAD_SIGNATURE);
    }
    for(uint32_t i = 0u; i < 64u; i++) {
        tampered_sig[i] = 0u;
    }
    expect("sifir imza", fwu_crypto_ed25519_verify(&sim.crypto, msg, 74u, tampered_sig, key), FWU_ERR_BAD_SIGNATURE);
    {
        fwu_byte_t swapped[64];
        for(uint32_t i = 0u; i < 32u; i++) {
            swapped[i] = sig[32u + i];
            swapped[32u + i] = sig[i];
        }
        expect("imzanin iki yarisi yer degistirmis", fwu_crypto_ed25519_verify(&sim.crypto, msg, 74u, swapped, key), FWU_ERR_BAD_SIGNATURE);
    }
    {
        fwu_byte_t sig2[64];
        fwu_byte_t msg1[1];
        msg1[0] = 0x55u;
        fwu_crypto_sim_sign(key, msg1, 1u, sig2);
        expect("1 elemanlik mesaj, kendi imzasiyla", fwu_crypto_ed25519_verify(&sim.crypto, msg1, 1u, sig2, key), FWU_OK);
    }

    /* kirli ust bitler (C28x): kirli imza/mesaj/anahtar yine dogrulanir */
    {
        fwu_byte_t dsig[64];
        fwu_byte_t dmsg[74];
        fwu_byte_t dkey[32];
        for(uint32_t i = 0u; i < 64u; i++) { dsig[i] = (fwu_byte_t)(sig[i] | 0xAB00u); }
        for(uint32_t i = 0u; i < 74u; i++) { dmsg[i] = (fwu_byte_t)(msg[i] | 0xAB00u); }
        for(uint32_t i = 0u; i < 32u; i++) { dkey[i] = (fwu_byte_t)(key[i] | 0xAB00u); }
        if(fwu_crypto_ed25519_verify(&sim.crypto, dmsg, 74u, dsig, dkey) != FWU_OK) {
            dirty_ok = 0;
        }
    }
    check("kirli ust bitli imza, mesaj ve anahtar yine GECERLI sayilir (elemanlar 0..255 ile sinirli)", dirty_ok);

    /* enjekte edilen dogrulama sonuclari */
    sim.fail_verify = FWU_ERR_BAD_SIGNATURE;
    expect("enjekte BAD_SIGNATURE (gecerli girdiye bile)", fwu_crypto_ed25519_verify(&sim.crypto, msg, 74u, sig, key), FWU_ERR_BAD_SIGNATURE);
    sim.fail_verify = FWU_ERR_CRYPTO_FAILURE;
    expect("enjekte CRYPTO_FAILURE", fwu_crypto_ed25519_verify(&sim.crypto, msg, 74u, sig, key), FWU_ERR_CRYPTO_FAILURE);
    sim.fail_verify = FWU_ERR_FLASH_WRITE;
    expect("enjekte saçma kod -> CRYPTO_FAILURE", fwu_crypto_ed25519_verify(&sim.crypto, msg, 74u, sig, key), FWU_ERR_CRYPTO_FAILURE);
    sim.fail_verify = FWU_OK;
    expect("hata giderilince tekrar gecerli", fwu_crypto_ed25519_verify(&sim.crypto, msg, 74u, sig, key), FWU_OK);

    /* tek bit ayrimi: bozulan ve bozulmayan ayni imza farkli sonuc veriyor (ayni imza hem gecerli hem gecersiz olamaz) */
    bit_ok = (fwu_crypto_ed25519_verify(&sim.crypto, msg, 74u, sig, key) == FWU_OK);
    check("bozulmamis imza hala gecerli (testin kendi tamponlari bozulmadi)", bit_ok);
}

int main(void)
{
    test_sha256_vectors();
    test_sha256_streaming();
    test_sim_port();
    test_fake_signature();

    printf("\nResult: %s (%d failed)\n", g_failures == 0 ? "ALL TESTS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
