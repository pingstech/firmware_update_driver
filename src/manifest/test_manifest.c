/**
 * @file    test_manifest.c
 * @brief   fwu_manifest için kendi kendini kontrol eden test.
 *
 * Beklentiler modülün veya başlığın kendi sabitlerinden DEĞİL, elle yazılmış sayılardan ve baytlardan
 * gelir (ortak mod hatası olmasın: başlıktaki bir ofset yanlışsa test de aynı yanlışı paylaşmaz).
 * Kapsam:
 *   1. Biçim sabitleri (ofset, boyut, toplam) elle yazılmış değerlere karşı.
 *   2. Bilinen örnek: her alan elle yazılmış değerle; büyük endian; diziler bayt bayt kopyalanır.
 *   3. Argümanlar (NULL), kısa/uzun tampon (NOT_ENOUGH_DATA sınırı 137/138, fazlası yok sayılır).
 *   4. Yapı hataları: magic (4 baytın her biri), biçim sürümü, ayrılmış alan (2 baytın her biri),
 *      imaj boyutu 0; her biri tek başına BAD_MANIFEST; hata sırası.
 *   5. Hatada çıktı DEĞİŞMEZ; girdi DEĞİŞMEZ; imza ve diğer alan değerleri yapıyı etkilemez.
 *   6. Kirli üst bitler (C28x): her bayt `| 0xAB00` ile kirletilse de aynı sonuç.
 *
 * Derleme (manifest klasöründen):
 *   gcc -Wall -Wextra -Wpedantic -Wconversion -std=c99 -I. -I.. -I../error -I../bytes fwu_manifest.c test_manifest.c -o test_manifest && ./test_manifest
 */

#include <stdio.h>
#include "fwu_manifest.h"

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

#define TOTAL 138u          /* elle: 74 imzalanan + 64 imza */

/** Elle yazılmış geçerli manifest (ofsetler SABİT SAYI olarak, başlıktan bağımsız). */
static void build_valid(fwu_byte_t *m)
{
    m[0] = 0x46u; m[1] = 0x57u; m[2] = 0x4Du; m[3] = 0x46u;      /* magic "FWMF" */
    m[4] = 0x01u;                                                 /* biçim sürümü */
    m[5] = 0x02u;                                                 /* key_id */
    m[6] = 0x00u; m[7] = 0x00u;                                   /* ayrılmış */
    m[8] = 0x00u; m[9] = 0x00u; m[10] = 0x30u; m[11] = 0x39u;     /* product_id = 12345 */
    m[12] = 0x00u; m[13] = 0x00u; m[14] = 0x00u; m[15] = 0x19u;   /* image_version = 25 */
    m[16] = 0x00u; m[17] = 0x03u;                                 /* key_version = 3 */
    m[18] = 0x00u; m[19] = 0x03u; m[20] = 0x60u; m[21] = 0x00u;   /* image_size = 221184 */
    for(uint32_t i = 0u; i < 32u; i++) {
        m[22u + i] = (fwu_byte_t)(0xA0u + i);                     /* sha256: A0..BF */
    }
    for(uint32_t i = 0u; i < 20u; i++) {
        m[54u + i] = (fwu_byte_t)(0x10u + i);                     /* commit hash: 10..23 */
    }
    for(uint32_t i = 0u; i < 64u; i++) {
        m[74u + i] = (fwu_byte_t)((i * 3u + 1u) & 0xFFu);         /* imza: 1, 4, 7, ... */
    }
}

/** Çıktıyı belirgin bir işaretle doldurur (hatada DEĞİŞMEDİĞİNİ anlamak için). */
static void poison(fwu_manifest_t *x)
{
    x->format_version = 0xEEu;
    x->key_id = 0xEEu;
    x->product_id = 0xDEADBEEFu;
    x->image_version = 0xDEADBEEFu;
    x->key_version = 0xBEEFu;
    x->image_size = 0xDEADBEEFu;
    for(uint32_t i = 0u; i < 32u; i++) {
        x->sha256[i] = 0xEEu;
    }
    for(uint32_t i = 0u; i < 20u; i++) {
        x->commit_hash[i] = 0xEEu;
    }
    for(uint32_t i = 0u; i < 64u; i++) {
        x->signature[i] = 0xEEu;
    }
}

static int is_poisoned(const fwu_manifest_t *x)
{
    int ok = (x->format_version == 0xEEu) && (x->key_id == 0xEEu) && (x->product_id == 0xDEADBEEFu) &&
             (x->image_version == 0xDEADBEEFu) && (x->key_version == 0xBEEFu) && (x->image_size == 0xDEADBEEFu);
    for(uint32_t i = 0u; i < 32u; i++) {
        ok = ok && (x->sha256[i] == 0xEEu);
    }
    for(uint32_t i = 0u; i < 20u; i++) {
        ok = ok && (x->commit_hash[i] == 0xEEu);
    }
    for(uint32_t i = 0u; i < 64u; i++) {
        ok = ok && (x->signature[i] == 0xEEu);
    }
    return ok;
}

/** Hatalı bir girdiyi dener: beklenen hata kodu dönmeli VE çıktı değişmemeli. */
static void expect_rejected(const char *name, const fwu_byte_t *m, uint32_t length, fwu_return_e want)
{
    fwu_manifest_t out;
    fwu_return_e got;
    poison(&out);
    got = fwu_manifest_parse(m, length, &out);
    if((got == want) && is_poisoned(&out)) {
        printf("[PASS] %s\n", name);
    } else {
        printf("[FAIL] %s  (got %d, expected %d, cikti %s)\n", name, (int)got, (int)want,
               is_poisoned(&out) ? "degismedi" : "DEGISTI");
        g_failures++;
    }
}

static void test_constants(void)
{
    printf("\n--- bicim sabitleri (elle yazilmis degerlere karsi) ---\n");
    check("magic 0x46574D46", FWU_MANIFEST_MAGIC == 0x46574D46u);
    check("bicim surumu 1", FWU_MANIFEST_FORMAT_VERSION == 1u);
    check("alan boyutlari: 4,1,1,2,4,4,2,4,32,20,64",
          FWU_MANIFEST_MAGIC_SIZE == 4u && FWU_MANIFEST_FORMAT_SIZE == 1u && FWU_MANIFEST_KEY_ID_SIZE == 1u &&
          FWU_MANIFEST_RESERVED_SIZE == 2u && FWU_MANIFEST_PRODUCT_ID_SIZE == 4u &&
          FWU_MANIFEST_IMAGE_VERSION_SIZE == 4u && FWU_MANIFEST_KEY_VERSION_SIZE == 2u &&
          FWU_MANIFEST_IMAGE_SIZE_SIZE == 4u && FWU_MANIFEST_SHA256_SIZE == 32u &&
          FWU_MANIFEST_COMMIT_HASH_SIZE == 20u && FWU_MANIFEST_SIGNATURE_SIZE == 64u);
    check("ofsetler: 0,4,5,6,8,12,16,18,22,54,74",
          FWU_MANIFEST_OFFSET_MAGIC == 0u && FWU_MANIFEST_OFFSET_FORMAT == 4u && FWU_MANIFEST_OFFSET_KEY_ID == 5u &&
          FWU_MANIFEST_OFFSET_RESERVED == 6u && FWU_MANIFEST_OFFSET_PRODUCT_ID == 8u &&
          FWU_MANIFEST_OFFSET_IMAGE_VERSION == 12u && FWU_MANIFEST_OFFSET_KEY_VERSION == 16u &&
          FWU_MANIFEST_OFFSET_IMAGE_SIZE == 18u && FWU_MANIFEST_OFFSET_SHA256 == 22u &&
          FWU_MANIFEST_OFFSET_COMMIT_HASH == 54u && FWU_MANIFEST_OFFSET_SIGNATURE == 74u);
    check("imzalanan bolge 74, toplam 138", FWU_MANIFEST_SIGNED_SIZE == 74u && FWU_MANIFEST_SIZE == 138u);
}

static void test_known_vector(void)
{
    fwu_byte_t m[TOTAL];
    fwu_manifest_t out;
    int sha_ok = 1;
    int commit_ok = 1;
    int sig_ok = 1;

    printf("\n--- bilinen ornek ---\n");
    build_valid(m);
    poison(&out);
    expect("gecerli manifest", fwu_manifest_parse(m, TOTAL, &out), FWU_OK);
    check("biclim surumu 1, key_id 2", out.format_version == 1u && out.key_id == 2u);
    check("product_id 12345 (buyuk endian)", out.product_id == 12345u);
    check("image_version 25", out.image_version == 25u);
    check("key_version 3 (2 bayt)", out.key_version == 3u);
    check("image_size 221184", out.image_size == 221184u);
    for(uint32_t i = 0u; i < 32u; i++) {
        if(out.sha256[i] != (0xA0u + i)) {
            sha_ok = 0;
        }
    }
    for(uint32_t i = 0u; i < 20u; i++) {
        if(out.commit_hash[i] != (0x10u + i)) {
            commit_ok = 0;
        }
    }
    for(uint32_t i = 0u; i < 64u; i++) {
        if(out.signature[i] != ((i * 3u + 1u) & 0xFFu)) {
            sig_ok = 0;
        }
    }
    check("sha256: 32 bayt, sirayla A0..BF", sha_ok);
    check("commit_hash: 20 bayt, sirayla 10..23", commit_ok);
    check("imza: 64 bayt, sirayla kopyalandi", sig_ok);

    /* buyuk endian ayrimi: her baytin agirligi farkli, 0x01020304 gibi simetrik olmayan degerler */
    build_valid(m);
    m[8] = 0x01u; m[9] = 0x02u; m[10] = 0x03u; m[11] = 0x04u;
    m[12] = 0xF0u; m[13] = 0xE1u; m[14] = 0xD2u; m[15] = 0xC3u;
    m[16] = 0xAAu; m[17] = 0x55u;
    m[18] = 0x80u; m[19] = 0x00u; m[20] = 0x00u; m[21] = 0x01u;
    expect("simetrik olmayan degerler", fwu_manifest_parse(m, TOTAL, &out), FWU_OK);
    check("product_id 0x01020304", out.product_id == 0x01020304u);
    check("image_version 0xF0E1D2C3", out.image_version == 0xF0E1D2C3u);
    check("key_version 0xAA55", out.key_version == 0xAA55u);
    check("image_size 0x80000001", out.image_size == 0x80000001u);
}

static void test_arguments_and_length(void)
{
    fwu_byte_t big[1000];
    fwu_manifest_t out;
    fwu_manifest_t ref;

    printf("\n--- argumanlar ve tampon uzunlugu ---\n");
    build_valid(big);
    poison(&out);
    expect("buffer NULL", fwu_manifest_parse(NULL, TOTAL, &out), FWU_ERR_INVALID_ARG);
    expect("manifest NULL", fwu_manifest_parse(big, TOTAL, NULL), FWU_ERR_INVALID_ARG);
    expect("ikisi de NULL", fwu_manifest_parse(NULL, TOTAL, NULL), FWU_ERR_INVALID_ARG);
    expect_rejected("uzunluk 0", big, 0u, FWU_ERR_NOT_ENOUGH_DATA);
    expect_rejected("uzunluk 1", big, 1u, FWU_ERR_NOT_ENOUGH_DATA);
    expect_rejected("uzunluk 73 (imza yok, imzalanan bolge bile eksik)", big, 73u, FWU_ERR_NOT_ENOUGH_DATA);
    expect_rejected("uzunluk 74 (yalniz imzalanan bolge)", big, 74u, FWU_ERR_NOT_ENOUGH_DATA);
    expect_rejected("uzunluk 137 (1 bayt eksik)", big, 137u, FWU_ERR_NOT_ENOUGH_DATA);
    expect("uzunluk 138 (tam)", fwu_manifest_parse(big, 138u, &ref), FWU_OK);
    expect("uzunluk 139 (fazlasi yok sayilir)", fwu_manifest_parse(big, 139u, &out), FWU_OK);
    check("... sonuc 138 ile ayni", out.product_id == ref.product_id && out.image_size == ref.image_size &&
          out.signature[63] == ref.signature[63] && out.sha256[0] == ref.sha256[0]);

    /* fazla baytlar (manifest'ten sonraki imaj baslangici) sonucu etkilemez */
    for(uint32_t i = 138u; i < 1000u; i++) {
        big[i] = (fwu_byte_t)((i * 7u) & 0xFFu);
    }
    expect("uzunluk 1000, sonrasi imaj verisi", fwu_manifest_parse(big, 1000u, &out), FWU_OK);
    check("... sonuc ayni", out.product_id == ref.product_id && out.image_size == ref.image_size &&
          out.signature[63] == ref.signature[63]);

    /* kisa tampon + bozuk icerik: once uzunluk (NOT_ENOUGH_DATA), icerik degil */
    build_valid(big);
    big[0] = 0x00u;
    expect_rejected("kisa VE bozuk magic: NOT_ENOUGH_DATA onceliklidir", big, 100u, FWU_ERR_NOT_ENOUGH_DATA);
}

static void test_structure_errors(void)
{
    fwu_byte_t m[TOTAL];
    fwu_manifest_t out;

    printf("\n--- yapi hatalari (her biri tek basina BAD_MANIFEST) ---\n");

    for(uint32_t i = 0u; i < 4u; i++) {
        build_valid(m);
        m[i] = (fwu_byte_t)(m[i] ^ 0x01u);
        expect_rejected("magic'in bir bayti bir bit bozuk", m, TOTAL, FWU_ERR_BAD_MANIFEST);
    }
    build_valid(m);
    m[0] = 0x00u; m[1] = 0x00u; m[2] = 0x00u; m[3] = 0x00u;
    expect_rejected("magic tamamen sifir", m, TOTAL, FWU_ERR_BAD_MANIFEST);
    build_valid(m);
    m[0] = 0x46u; m[1] = 0x4Du; m[2] = 0x57u; m[3] = 0x46u;
    expect_rejected("magic baytlari yer degistirmis (FMWF)", m, TOTAL, FWU_ERR_BAD_MANIFEST);

    build_valid(m);
    m[4] = 0x00u;
    expect_rejected("biclim surumu 0", m, TOTAL, FWU_ERR_BAD_MANIFEST);
    build_valid(m);
    m[4] = 0x02u;
    expect_rejected("biclim surumu 2 (desteklenmiyor)", m, TOTAL, FWU_ERR_BAD_MANIFEST);
    build_valid(m);
    m[4] = 0xFFu;
    expect_rejected("biclim surumu 255", m, TOTAL, FWU_ERR_BAD_MANIFEST);

    build_valid(m);
    m[6] = 0x01u;
    expect_rejected("ayrilmis alanin ilk bayti sifir degil", m, TOTAL, FWU_ERR_BAD_MANIFEST);
    build_valid(m);
    m[7] = 0x01u;
    expect_rejected("ayrilmis alanin ikinci bayti sifir degil", m, TOTAL, FWU_ERR_BAD_MANIFEST);
    build_valid(m);
    m[6] = 0x80u; m[7] = 0x80u;
    expect_rejected("ayrilmis alan yuksek bitler dolu", m, TOTAL, FWU_ERR_BAD_MANIFEST);

    build_valid(m);
    m[18] = 0x00u; m[19] = 0x00u; m[20] = 0x00u; m[21] = 0x00u;
    expect_rejected("image_size 0", m, TOTAL, FWU_ERR_BAD_MANIFEST);
    for(uint32_t i = 18u; i < 22u; i++) {
        build_valid(m);
        m[18] = 0x00u; m[19] = 0x00u; m[20] = 0x00u; m[21] = 0x00u;
        m[i] = 0x01u;
        expect("image_size yalniz tek bir baytta 1 (sifir degil)", fwu_manifest_parse(m, TOTAL, &out), FWU_OK);
    }
    build_valid(m);
    m[18] = 0xFFu; m[19] = 0xFFu; m[20] = 0xFFu; m[21] = 0xFFu;
    expect("image_size 0xFFFFFFFF (yapi gecerli, boyut kontrolu baska katmanda)", fwu_manifest_parse(m, TOTAL, &out), FWU_OK);
    check("... deger dogru okundu", out.image_size == 0xFFFFFFFFu);

    /* birden fazla hata birlikte: yine BAD_MANIFEST, tek kod */
    build_valid(m);
    m[0] = 0x00u; m[4] = 0x09u; m[6] = 0x01u; m[21] = 0x00u; m[20] = 0x00u; m[19] = 0x00u; m[18] = 0x00u;
    expect_rejected("hepsi birden bozuk", m, TOTAL, FWU_ERR_BAD_MANIFEST);
}

static void test_independence(void)
{
    fwu_byte_t m[TOTAL];
    fwu_byte_t copy[TOTAL];
    fwu_manifest_t out;
    int unchanged = 1;

    printf("\n--- girdi degismez, imza ve diger alanlar yapiyi etkilemez ---\n");
    build_valid(m);
    for(uint32_t i = 0u; i < TOTAL; i++) {
        copy[i] = m[i];
    }
    expect("okuma", fwu_manifest_parse(m, TOTAL, &out), FWU_OK);
    for(uint32_t i = 0u; i < TOTAL; i++) {
        if(copy[i] != m[i]) {
            unchanged = 0;
        }
    }
    check("girdi tamponu degismedi", unchanged);

    /* imza ve diger alanlar herhangi bir deger olabilir: yapi kontrolu bunlara bakmaz */
    build_valid(m);
    for(uint32_t i = 74u; i < TOTAL; i++) {
        m[i] = 0x00u;
    }
    expect("imza tamamen sifir (yapi gecerli, imza dogrulamasi G7b-G7d)", fwu_manifest_parse(m, TOTAL, &out), FWU_OK);
    for(uint32_t i = 74u; i < TOTAL; i++) {
        m[i] = 0xFFu;
    }
    expect("imza tamamen 0xFF", fwu_manifest_parse(m, TOTAL, &out), FWU_OK);
    build_valid(m);
    m[8] = 0x00u; m[9] = 0x00u; m[10] = 0x00u; m[11] = 0x00u;
    m[12] = 0x00u; m[13] = 0x00u; m[14] = 0x00u; m[15] = 0x00u;
    m[16] = 0x00u; m[17] = 0x00u;
    expect("product_id, image_version, key_version sifir (anlam kontrolu baska katmanda)", fwu_manifest_parse(m, TOTAL, &out), FWU_OK);
    check("... sifir okundu", out.product_id == 0u && out.image_version == 0u && out.key_version == 0u);
    build_valid(m);
    m[5] = 0xFFu;
    expect("key_id 255", fwu_manifest_parse(m, TOTAL, &out), FWU_OK);
    check("... key_id 255", out.key_id == 0xFFu);
    for(uint32_t i = 22u; i < 74u; i++) {
        m[i] = 0x00u;
    }
    expect("sha256 ve commit_hash tamamen sifir", fwu_manifest_parse(m, TOTAL, &out), FWU_OK);
}

static void test_dirty_upper_bits(void)
{
    fwu_byte_t clean[TOTAL];
    fwu_byte_t dirty[TOTAL];
    fwu_manifest_t a;
    fwu_manifest_t b;
    int arrays_equal = 1;

    printf("\n--- kirli ust bitler (C28x: elemanin ust 8 biti cop olabilir) ---\n");
    build_valid(clean);
    for(uint32_t i = 0u; i < TOTAL; i++) {
        dirty[i] = (fwu_byte_t)(clean[i] | 0xAB00u);
    }
    expect("temiz", fwu_manifest_parse(clean, TOTAL, &a), FWU_OK);
    expect("kirli (her elemana | 0xAB00)", fwu_manifest_parse(dirty, TOTAL, &b), FWU_OK);
    check("kirli ve temiz: sayisal alanlar ayni",
          a.format_version == b.format_version && a.key_id == b.key_id && a.product_id == b.product_id &&
          a.image_version == b.image_version && a.key_version == b.key_version && a.image_size == b.image_size);
    for(uint32_t i = 0u; i < 32u; i++) {
        if(a.sha256[i] != b.sha256[i]) {
            arrays_equal = 0;
        }
    }
    for(uint32_t i = 0u; i < 20u; i++) {
        if(a.commit_hash[i] != b.commit_hash[i]) {
            arrays_equal = 0;
        }
    }
    for(uint32_t i = 0u; i < 64u; i++) {
        if(a.signature[i] != b.signature[i]) {
            arrays_equal = 0;
        }
    }
    check("kirli ve temiz: sha256, commit_hash, imza ayni (elemanlar 0..255 ile sinirli)", arrays_equal);
    check("cikti elemanlarinin ust bitleri temiz (0..255 araliginda)",
          (b.sha256[0] & 0xFF00u) == 0u && (b.sha256[31] & 0xFF00u) == 0u && (b.commit_hash[19] & 0xFF00u) == 0u &&
          (b.signature[63] & 0xFF00u) == 0u && (b.format_version & 0xFF00u) == 0u && (b.key_id & 0xFF00u) == 0u);

    /* kirli bitler yapi kontrolunu da bozmamali: kirli ama GECERLI magic/surum/ayrilmis kabul, gercekten bozuk red */
    dirty[6] = (fwu_byte_t)(0x00u | 0xAB00u);
    dirty[7] = (fwu_byte_t)(0x00u | 0xAB00u);
    expect("kirli ama sifir olan ayrilmis alan: kabul", fwu_manifest_parse(dirty, TOTAL, &b), FWU_OK);
    dirty[6] = (fwu_byte_t)(0x01u | 0xAB00u);
    expect_rejected("kirli ve gercekten sifir olmayan ayrilmis alan: red", dirty, TOTAL, FWU_ERR_BAD_MANIFEST);
}

int main(void)
{
    test_constants();
    test_known_vector();
    test_arguments_and_length();
    test_structure_errors();
    test_independence();
    test_dirty_upper_bits();

    printf("\nResult: %s (%d failed)\n", g_failures == 0 ? "ALL TESTS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
