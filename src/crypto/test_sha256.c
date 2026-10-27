/**
 * @file    test_sha256.c
 * @brief   fwu_sha256 ve fwu_crypto_sw (yazılım kriptografi portu) testi.
 *
 * Beklenen değerler gerçekleme kodundan DEĞİL, bağımsız kaynaklardan gelir:
 *   - FIPS 180-4 / NIST resmî test vektörleri: "", "abc", 56 baytlık ve 112 baytlık iki bloklu mesajlar, 1.000.000 x 'a'.
 *   - Python hashlib ile hesaplanmış, dolgu sınırlarını zorlayan özetler (0, 1, 55, 56, 57, 63, 64, 65, 119, 120, 121, 127, 128,
 *     129, 200, 1000 eleman; mesaj i-inci bayt = (i * 7 + 3) & 0xFF).
 * Ek olarak: HER bölünme noktasında parça parça = tek seferde, bayt bayt, rastgele parçalar; ikinci bir gerçekleme olan sahte
 * portla (fwu_crypto_sim) 0..300 eleman çapraz denetim; sarmalayıcı (fwu_crypto) ile birlikte; Ed25519 yer tutucusu asla "geçerli"
 * demez; C28x taklidinde kirli üst bitler.
 *
 * Derleme (crypto klasöründen):
 *   gcc -Wall -Wextra -Wpedantic -Wconversion -std=c99 -I. -I.. -I../error fwu_sha256.c fwu_crypto_sw.c fwu_crypto.c fwu_crypto_sim.c test_sha256.c -o test_sha256 && ./test_sha256
 */

#include <stdio.h>
#include <string.h>
#include "fwu_sha256.h"
#include "fwu_crypto_sw.h"
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

static const char *const HEX = "0123456789ABCDEF";

static void to_hex(const fwu_byte_t *digest, char *out)
{
    for(unsigned i = 0u; i < FWU_SHA256_DIGEST_SIZE; i++) {
        out[2u * i] = HEX[(digest[i] >> 4) & 0xFu];
        out[(2u * i) + 1u] = HEX[digest[i] & 0xFu];
    }
    out[64] = '\0';
}

static void sha_all(const fwu_byte_t *data, uint32_t length, char *hex)
{
    fwu_sha256_t s;
    fwu_byte_t d[FWU_SHA256_DIGEST_SIZE];

    fwu_sha256_init(&s);
    fwu_sha256_update(&s, data, length);
    fwu_sha256_final(&s, d);
    to_hex(d, hex);
}

static void text_bytes(const char *text, fwu_byte_t *out)
{
    for(size_t i = 0u; text[i] != '\0'; i++) {
        out[i] = (fwu_byte_t)(unsigned char)text[i];
    }
}

static fwu_byte_t pattern_byte(uint32_t i)
{
    return (fwu_byte_t)((i * 7u + 3u) & 0xFFu);
}

static const struct { uint32_t length; const char *digest; } k_pattern[] = {
    { 0u, "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855" },
    { 1u, "084FED08B978AF4D7D196A7446A86B58009E636B611DB16211B65A9AADFF29C5" },
    { 55u, "E7313D333C272E639F790978283F9EB392E843D0F29B7016828BB1DAA4AAC70B" },
    { 56u, "4324D65F3C103567F5589C710BC08F8523F929A9272E3AF36FC968E52ABC6C27" },
    { 57u, "35DF609437DCFEA3279283AB79FD554E2BF78F8F7AE2DE532D8EE300B09E8F73" },
    { 63u, "81C80242132F230C3BD41B3E63BBCFF16107339549214A99614FF26664625055" },
    { 64u, "39E3D7B6B5D075D37D053AD89B24B41BEF4F3C29760C84447CAB3F3BE1882241" },
    { 65u, "AACCA6FF74FDBB296D165A45CECFA04E5127BC008770FBBDD48006F2D2FAE95E" },
    { 119u, "9CE7368E4DAF32341631B492E80359DC9F594B48453CD0DD5BF0B19279CC177E" },
    { 120u, "7836B787757E95E58B3CA5AEC90B1B004E8DEBA1E50E9675AF9CABF1A13A04B5" },
    { 121u, "1189A98A00C71BC1848EA8BDC9700B442BEE0BE7C3F45172303F1AB0B6F1617E" },
    { 127u, "A8D23E75D936F303D248888D9B165EE543F4CBAFCAD3C9DD2A79BD84FAA11D07" },
    { 128u, "D2742F1F4AC6BB7CA2B239EE18402BA8B3F9F8E652D2A72973C2B9BA11C08CF6" },
    { 129u, "307F8FC2C1622B92762E818D39A185D4D667AD49A4B07CEAE1F4AFA008A93EC4" },
    { 200u, "2C7E18C942EF065B526A2D4E5546283749CD3DDFB51D8FC71F42717363685F46" },
    { 1000u, "1E9BC38CBF860B9EC31918B065F9B52476C549A782E0E7990BED8CE3868D2371" },
};

static void test_official_vectors(void)
{
    static fwu_byte_t buf[1100];
    char hex[65];

    printf("\n--- 1. resmi vektorler ---\n");
    sha_all(buf, 0u, hex);
    check("\"\" (bos mesaj)", strcmp(hex, "E3B0C44298FC1C149AFBF4C8996FB92427AE41E4649B934CA495991B7852B855") == 0);
    text_bytes("abc", buf);
    sha_all(buf, 3u, hex);
    check("\"abc\" (FIPS 180-4 ornegi)", strcmp(hex, "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD") == 0);
    text_bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", buf);
    sha_all(buf, 56u, hex);
    check("56 baytlik mesaj (dolgu yeni bloga tasar)", strcmp(hex, "248D6A61D20638B8E5C026930C3E6039A33CE45964FF2167F6ECEDD419DB06C1") == 0);
    text_bytes("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu", buf);
    sha_all(buf, 112u, hex);
    check("112 baytlik mesaj (iki blok)", strcmp(hex, "CF5B16A778AF8380036CE59E7B0492370B249B11E8F07A51AFAC45037AFEE9D1") == 0);
    {
        fwu_sha256_t s;
        fwu_byte_t d[FWU_SHA256_DIGEST_SIZE];
        fwu_byte_t chunk[1000];
        for(unsigned i = 0u; i < 1000u; i++) {
            chunk[i] = (fwu_byte_t)'a';
        }
        fwu_sha256_init(&s);
        for(unsigned i = 0u; i < 1000u; i++) {
            fwu_sha256_update(&s, chunk, 1000u);
        }
        fwu_sha256_final(&s, d);
        to_hex(d, hex);
        check("1.000.000 x 'a' (uzun mesaj, bit sayaci 8.000.000)", strcmp(hex, "CDC76E5C9914FB9281A1C7E284D73E67F1809A48A497200E046D39CCC7112CD0") == 0);
    }
    for(unsigned k = 0u; k < sizeof(k_pattern) / sizeof(k_pattern[0]); k++) {
        char name[80];
        for(uint32_t i = 0u; i < k_pattern[k].length; i++) {
            buf[i] = pattern_byte(i);
        }
        sha_all(buf, k_pattern[k].length, hex);
        (void)snprintf(name, sizeof(name), "hashlib vektoru: %u eleman (dolgu siniri)", (unsigned)k_pattern[k].length);
        check(name, strcmp(hex, k_pattern[k].digest) == 0);
    }
}

static void test_streaming(void)
{
    static fwu_byte_t buf[300];
    char whole[65];
    char part[65];
    int all_splits = 1;
    int byte_by_byte = 1;
    int random_chunks = 1;
    uint32_t rng = 777u;

    printf("\n--- 2. parca parca = tek seferde ---\n");
    for(uint32_t i = 0u; i < 300u; i++) {
        buf[i] = pattern_byte(i);
    }
    sha_all(buf, 200u, whole);
    for(uint32_t split = 0u; split <= 200u; split++) {
        fwu_sha256_t s;
        fwu_byte_t d[FWU_SHA256_DIGEST_SIZE];
        fwu_sha256_init(&s);
        fwu_sha256_update(&s, buf, split);
        fwu_sha256_update(&s, &buf[split], 200u - split);
        fwu_sha256_final(&s, d);
        to_hex(d, part);
        if(strcmp(whole, part) != 0) {
            all_splits = 0;
        }
    }
    check("200 baytlik mesaj: HER bolunme noktasi (0..200) tek seferdekiyle ayni", all_splits);
    {
        fwu_sha256_t s;
        fwu_byte_t d[FWU_SHA256_DIGEST_SIZE];
        fwu_sha256_init(&s);
        for(uint32_t i = 0u; i < 200u; i++) {
            fwu_sha256_update(&s, &buf[i], 1u);
        }
        fwu_sha256_final(&s, d);
        to_hex(d, part);
        byte_by_byte = (strcmp(whole, part) == 0);
    }
    check("bayt bayt (200 cagri) tek seferdekiyle ayni", byte_by_byte);
    for(unsigned trial = 0u; trial < 200u; trial++) {
        fwu_sha256_t s;
        fwu_byte_t d[FWU_SHA256_DIGEST_SIZE];
        uint32_t done = 0u;
        fwu_sha256_init(&s);
        while(done < 300u) {
            uint32_t n;
            rng = (rng * 1664525u) + 1013904223u;
            n = (rng >> 20) % 70u;
            if(n > (300u - done)) {
                n = 300u - done;
            }
            fwu_sha256_update(&s, &buf[done], n);          /* n = 0 serbest */
            done += n;
        }
        fwu_sha256_final(&s, d);
        to_hex(d, part);
        sha_all(buf, 300u, whole);
        if(strcmp(whole, part) != 0) {
            random_chunks = 0;
        }
    }
    check("200 deneme: rastgele parcalar (0..69 bayt, bos dahil) tek seferdekiyle ayni", random_chunks);
    {
        fwu_sha256_t s;
        fwu_byte_t d1[FWU_SHA256_DIGEST_SIZE];
        fwu_byte_t d2[FWU_SHA256_DIGEST_SIZE];
        fwu_sha256_init(&s);
        fwu_sha256_update(&s, buf, 100u);
        fwu_sha256_final(&s, d1);
        fwu_sha256_init(&s);
        fwu_sha256_update(&s, buf, 100u);
        fwu_sha256_final(&s, d2);
        check("final'den sonra init: ayni girdi ayni ozet (durum tamamen sifirlaniyor)", memcmp(d1, d2, FWU_SHA256_DIGEST_SIZE * sizeof(fwu_byte_t)) == 0);
    }
}

static void test_cross_check(void)
{
    static fwu_byte_t buf[301];
    int same = 1;

    printf("\n--- 3. ikinci gerceklemeyle (fwu_crypto_sim) capraz denetim: 0..300 eleman ---\n");
    for(uint32_t i = 0u; i < 301u; i++) {
        buf[i] = (fwu_byte_t)((i * 31u + 17u) & 0xFFu);
    }
    for(uint32_t n = 0u; n <= 300u; n++) {
        char a[65];
        fwu_byte_t d[FWU_SHA256_DIGEST_SIZE];
        char b[65];
        sha_all(buf, n, a);
        fwu_crypto_sim_sha256(buf, n, d);
        to_hex(d, b);
        if(strcmp(a, b) != 0) {
            same = 0;
        }
    }
    check("301 farkli uzunlukta iki bagimsiz gerceklemenin ozeti ayni", same);
}

static void test_port(void)
{
    fwu_crypto_sw_t sw;
    fwu_crypto_t crypto;
    fwu_byte_t d[FWU_CRYPTO_SHA256_DIGEST_SIZE];
    fwu_byte_t msg[3];
    char hex[65];
    const fwu_byte_t pk[32] = { 0 };
    const fwu_byte_t sig[64] = { 0 };

    printf("\n--- 4. yazilim portu (fwu_crypto_sw) ve sarmalayici ---\n");
    expect("init", fwu_crypto_sw_init(&sw, &crypto), FWU_OK);
    expect("init: sw NULL", fwu_crypto_sw_init(NULL, &crypto), FWU_ERR_INVALID_ARG);
    expect("init: crypto NULL", fwu_crypto_sw_init(&sw, NULL), FWU_ERR_INVALID_ARG);
    text_bytes("abc", msg);
    expect("begin", fwu_crypto_sha256_begin(&crypto), FWU_OK);
    expect("update", fwu_crypto_sha256_update(&crypto, msg, 3u), FWU_OK);
    expect("finish", fwu_crypto_sha256_finish(&crypto, d), FWU_OK);
    to_hex(d, hex);
    check("sarmalayici uzerinden 'abc' resmi ozet", strcmp(hex, "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD") == 0);
    expect("begin (tekrar, onceki hesap bitmisti)", fwu_crypto_sha256_begin(&crypto), FWU_OK);
    expect("hesap acikken yeni begin: BAD_STATE (sarmalayici korur)", fwu_crypto_sha256_begin(&crypto), FWU_ERR_BAD_STATE);
    expect("update + finish", fwu_crypto_sha256_update(&crypto, msg, 3u), FWU_OK);
    expect("... finish", fwu_crypto_sha256_finish(&crypto, d), FWU_OK);
    to_hex(d, hex);
    check("... BAD_STATE hesabi bozmadi: yine ayni resmi ozet", strcmp(hex, "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD") == 0);
    expect("Ed25519 yer tutucusu: HER ZAMAN CRYPTO_FAILURE (asla 'gecerli' demez)", fwu_crypto_ed25519_verify(&crypto, msg, 3u, sig, pk), FWU_ERR_CRYPTO_FAILURE);
    {
        fwu_byte_t forged[64];
        for(unsigned i = 0u; i < 64u; i++) {
            forged[i] = (fwu_byte_t)(i * 3u + 1u);
        }
        expect("... uydurma imzayla da CRYPTO_FAILURE (imzali profil kapali hata)", fwu_crypto_ed25519_verify(&crypto, msg, 3u, forged, pk), FWU_ERR_CRYPTO_FAILURE);
    }
    {
        /* iki ayri port nesnesi birbirini etkilemez */
        fwu_crypto_sw_t sw2;
        fwu_crypto_t crypto2;
        fwu_byte_t d2[FWU_CRYPTO_SHA256_DIGEST_SIZE];
        (void)fwu_crypto_sw_init(&sw2, &crypto2);
        (void)fwu_crypto_sha256_begin(&crypto);
        (void)fwu_crypto_sha256_begin(&crypto2);
        (void)fwu_crypto_sha256_update(&crypto, msg, 3u);
        (void)fwu_crypto_sha256_update(&crypto2, msg, 2u);
        (void)fwu_crypto_sha256_finish(&crypto, d);
        (void)fwu_crypto_sha256_finish(&crypto2, d2);
        check("iki bagimsiz port: ayni anda farkli mesajlar, ozetler farkli ve dogru", memcmp(d, d2, FWU_CRYPTO_SHA256_DIGEST_SIZE * sizeof(fwu_byte_t)) != 0);
        {
            fwu_byte_t ab[2];
            char expected[65];
            char got[65];
            text_bytes("ab", ab);
            sha_all(ab, 2u, expected);                      /* bagimsiz: tek seferde, ayri durumla */
            to_hex(d2, got);
            check("... ikincisi 'ab' ozetiyle ayni (iki port birbirinin durumunu bozmadi)", strcmp(expected, got) == 0);
            check("... birincisi 'abc' resmi ozeti", (to_hex(d, got), strcmp(got, "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD") == 0));
        }
    }
#ifdef C2000
    {
        fwu_byte_t dirty[3];
        for(unsigned i = 0u; i < 3u; i++) {
            dirty[i] = (fwu_byte_t)(msg[i] | 0x100u);
        }
        sha_all(dirty, 3u, hex);
        check("C2000 taklidi: ust bitleri kirli girdi 'abc' ozetini verir (alt 8 bit)", strcmp(hex, "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD") == 0);
    }
#endif
}

/** Bit sayacinin UST kelimesi ve tasima: pratik testle erisilemez (2^29 bayt gerekir), bu yuzden durum alanlari dogrudan kurulur.
 *  Beklenen ozetler saf Python bir SHA-256 referansindan (hashlib ile dogrulanmis) ve OZEL uzunluk alaniyla hesaplandi. */
static void test_length_field(void)
{
    fwu_sha256_t s;
    fwu_byte_t d[FWU_SHA256_DIGEST_SIZE];
    fwu_byte_t abc[3];
    fwu_byte_t sixty[60];
    char hex[65];

    printf("\n--- 5. uzunluk alani: ust kelime ve tasima ---\n");
    text_bytes("abc", abc);
    fwu_sha256_init(&s);
    s.bits_hi = 0x01020304u;
    s.bits_lo = 0xFFFFFFF0u;                                /* +24 bit -> alt kelime tasar: ust kelime 0x01020305, alt 0x00000008 */
    fwu_sha256_update(&s, abc, 3u);
    check("tasima: alt kelime 0x00000008, ust kelime 1 artti (0x01020305)", s.bits_lo == 0x00000008u && s.bits_hi == 0x01020305u);
    fwu_sha256_final(&s, d);
    to_hex(d, hex);
    check("ust kelimesi sifirdan farkli uzunluk alani: Python referansiyla ayni ozet (ust kelime 4 baytinin HER BIRI dogru yazildi)",
          strcmp(hex, "72C36C94F0F47F7ED3A9BED47D6DE905326E10784BAD319B40C3BC4AC99CBD07") == 0);
    for(uint32_t i = 0u; i < 60u; i++) {
        sixty[i] = pattern_byte(i);
    }
    fwu_sha256_init(&s);
    s.bits_hi = 0xA1B2C3D4u;
    s.bits_lo = 0u;
    fwu_sha256_update(&s, sixty, 60u);
    check("tasima yok: alt kelime 480, ust kelime ayni", s.bits_lo == 480u && s.bits_hi == 0xA1B2C3D4u);
    fwu_sha256_final(&s, d);
    to_hex(d, hex);
    check("iki bloklu dolgu + ust kelime 0xA1B2C3D4: Python referansiyla ayni ozet", strcmp(hex, "5145BDCFB541D05F3C48391ED37549DEED12E16D4FD4FECEB08A1D7409B72833") == 0);
    fwu_sha256_init(&s);
    check("init: bit sayaci ve bekleyen bayt sayisi sifirlanir (dirty durumdan)", (s.bits_hi == 0u) && (s.bits_lo == 0u) && (s.block_length == 0u));
}

/** Hicbir islev kendi tamponunun (durum yapisi ve ozet dizisi) bir bayt otesine yazmaz. */
static void test_no_overrun(void)
{
    static fwu_byte_t buf[140];
    int state_ok = 1;
    int digest_ok = 1;

    printf("\n--- 6. tampon tasmasi yok ---\n");
    for(uint32_t i = 0u; i < 140u; i++) {
        buf[i] = pattern_byte(i);
    }
    for(uint32_t n = 0u; n <= 130u; n++) {
        struct {
            fwu_sha256_t s;
            fwu_byte_t   guard[16];
        } w;
        fwu_byte_t d[FWU_SHA256_DIGEST_SIZE + 8u];

        for(unsigned i = 0u; i < 16u; i++) {
            w.guard[i] = (fwu_byte_t)0xEEu;
        }
        for(unsigned i = 0u; i < sizeof(d) / sizeof(d[0]); i++) {
            d[i] = (fwu_byte_t)0xEEu;
        }
        fwu_sha256_init(&w.s);
        fwu_sha256_update(&w.s, buf, n);
        fwu_sha256_final(&w.s, d);
        for(unsigned i = 0u; i < 16u; i++) {
            if(w.guard[i] != (fwu_byte_t)0xEEu) {
                state_ok = 0;
            }
        }
        for(unsigned i = FWU_SHA256_DIGEST_SIZE; i < FWU_SHA256_DIGEST_SIZE + 8u; i++) {
            if(d[i] != (fwu_byte_t)0xEEu) {
                digest_ok = 0;
            }
        }
    }
    check("0..130 eleman: durum yapisinin ARDINDAKI bekci baytlar bozulmadi (dolgu dongusu tasmiyor)", state_ok);
    check("0..130 eleman: ozet dizisinin ARDINDAKI 8 bekci bayt bozulmadi (tam 32 eleman yazilir)", digest_ok);
}

int main(void)
{
    test_official_vectors();
    test_streaming();
    test_cross_check();
    test_port();
    test_length_field();
    test_no_overrun();

    printf("\nResult: %s (%d failed)\n", g_failures == 0 ? "ALL TESTS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
