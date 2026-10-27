/**
 * @file    test_frame.c
 * @brief   fwu_frame.h için kendi kendini kontrol eden test (şimdilik: fwu_frame_encode ve sabitler).
 *
 * Testler çerçevenin biçimini BAĞIMSIZ olarak bilir: beklenen değerler başlıktaki sabitlerden
 * DEĞİL, bu dosyada elle yazılmış sayılardan (0xA5, 6, 9, ...) ve bağımsız bir CRC referansından
 * gelir. Böylece başlıktaki bir sabit yanlışsa da test yakalar (aynı hata hem koda hem beklentiye
 * girmesin diye).
 *
 * Kapsam:
 *   1. Sabitler: boyutlar, ofsetler, SOF/EOF, sınırlar.
 *   2. Bilinen çerçeve vektörleri (CRC değerleri Python ile ayrıca hesaplandı).
 *   3. Rastgele payload'larla (1..512 bayt) bağımsız modele karşı bayt bayt karşılaştırma.
 *   4. Hata durumları: doğru kod ve çıkış tamponunun / out_length'in DEĞİŞMEMESİ.
 *   5. Kapasite sınırı (tam yetmesi / bir eksik), nöbetçi baytlarla taşma kontrolü.
 *   6. C28x taklidi (-DC2000): üst bitleri kirli girdiler maskelenir.
 *
 * Derleme (frame klasöründen):
 *   gcc -Wall -Wextra -Wpedantic -Wconversion -std=c99 -I. -I.. -I../error -I../bytes -I../crc fwu_frame.c test_frame.c -o test_frame && ./test_frame
 */

#include <stdio.h>
#include <string.h>
#include "fwu_frame.h"

#define SENTINEL        0x77u      /* tampondaki "dokunulmadı" işareti */
#define BUFFER_SIZE     600u       /* en büyük çerçeve (521) + nöbetçi alan */
#define LENGTH_SENTINEL 12345u

static int g_failures = 0;

static void check(const char *name, int condition)
{
    printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    if(!condition) {
        g_failures++;
    }
}

/* ------------------------------------------------------------------ bağımsız referans */

/** CRC-16/MODBUS bit bit (fwu_crc'den BAĞIMSIZ yazıldı; polinom 0xA001, başlangıç 0xFFFF). */
static uint16_t ref_crc16(const fwu_byte_t *data, size_t length)
{
    uint16_t crc = 0xFFFFu;
    for(size_t i = 0u; i < length; i++) {
        crc = (uint16_t)(crc ^ (uint16_t)(data[i] & 0xFFu));
        for(unsigned bit = 0u; bit < 8u; bit++) {
            crc = ((crc & 1u) != 0u) ? (uint16_t)((crc >> 1) ^ 0xA001u) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

/** Beklenen çerçeveyi ELLE yazılmış sayılarla üretir (başlık sabitlerini KULLANMAZ). */
static size_t model_frame(fwu_byte_t type, fwu_byte_t seq, const fwu_byte_t *payload, size_t n, fwu_byte_t *out)
{
    uint16_t crc;
    out[0] = (fwu_byte_t)0xA5u;
    out[1] = (fwu_byte_t)1u;
    out[2] = (fwu_byte_t)(type & 0xFFu);
    out[3] = (fwu_byte_t)(seq & 0xFFu);
    out[4] = (fwu_byte_t)((n >> 8) & 0xFFu);
    out[5] = (fwu_byte_t)(n & 0xFFu);
    for(size_t i = 0u; i < n; i++) {
        out[6u + i] = (fwu_byte_t)(payload[i] & 0xFFu);
    }
    crc = ref_crc16(&out[1], 5u + n);
    out[6u + n] = (fwu_byte_t)((crc >> 8) & 0xFFu);
    out[7u + n] = (fwu_byte_t)(crc & 0xFFu);
    out[8u + n] = (fwu_byte_t)0x5Au;
    return 9u + n;
}

static void fill(fwu_byte_t *buffer, size_t n, fwu_byte_t value)
{
    for(size_t i = 0u; i < n; i++) {
        buffer[i] = value;
    }
}

static int untouched(const fwu_byte_t *buffer, size_t from, size_t to)
{
    for(size_t i = from; i < to; i++) {
        if(buffer[i] != (fwu_byte_t)SENTINEL) {
            return 0;
        }
    }
    return 1;
}

static int equal_bytes(const fwu_byte_t *a, const fwu_byte_t *b, size_t n)
{
    for(size_t i = 0u; i < n; i++) {
        if((a[i] & 0xFFu) != (b[i] & 0xFFu)) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ sabitler */

static void test_constants(void)
{
    printf("\n--- sabitler (elle yazilmis beklentilere karsi) ---\n");
    check("SOF 0xA5, EOF 0x5A, SOF != EOF", FWU_FRAME_SOF == 0xA5u && FWU_FRAME_EOF == 0x5Au && FWU_FRAME_SOF != FWU_FRAME_EOF);
    check("VERSION == 1", FWU_FRAME_VERSION == 1u);
    check("alan boyutlari 1,1,1,1,2,2,1",
          FWU_FRAME_SOF_SIZE == 1u && FWU_FRAME_VER_SIZE == 1u && FWU_FRAME_TYPE_SIZE == 1u &&
          FWU_FRAME_SEQ_SIZE == 1u && FWU_FRAME_LEN_SIZE == 2u && FWU_FRAME_CRC_SIZE == 2u &&
          FWU_FRAME_EOF_SIZE == 1u);
    check("ofsetler SOF0 VER1 TYPE2 SEQ3 LEN4 PAYLOAD6",
          FWU_FRAME_OFFSET_SOF == 0u && FWU_FRAME_OFFSET_VER == 1u && FWU_FRAME_OFFSET_TYPE == 2u &&
          FWU_FRAME_OFFSET_SEQ == 3u && FWU_FRAME_OFFSET_LEN == 4u && FWU_FRAME_OFFSET_PAYLOAD == 6u);
    check("baslik 6, bitis 3, ek yuk 9", FWU_FRAME_HEADER_SIZE == 6u && FWU_FRAME_TRAILER_SIZE == 3u && FWU_FRAME_OVERHEAD == 9u);
    check("CRC kapsami VER'den (ofset 1) baslar", FWU_FRAME_CRC_START_OFFSET == 1u);
    check("MAX_PAYLOAD 512, MAX_SIZE 521", FWU_FRAME_MAX_PAYLOAD == 512u && FWU_FRAME_MAX_SIZE == 521u);
}

/* ------------------------------------------------------------------ bilinen vektörler */

static void test_vectors(void)
{
    fwu_byte_t out[BUFFER_SIZE];
    size_t length = LENGTH_SENTINEL;

    printf("\n--- bilinen vektorler ---\n");

    {
        /* type 0x02, seq 0x07, payload 11 22 33; CRC(01 02 07 00 03 11 22 33) = 0x44AB (Python) */
        const fwu_byte_t payload[3] = { 0x11u, 0x22u, 0x33u };
        const fwu_byte_t want[12] = { 0xA5u, 0x01u, 0x02u, 0x07u, 0x00u, 0x03u, 0x11u, 0x22u, 0x33u, 0x44u, 0xABu, 0x5Au };
        fill(out, BUFFER_SIZE, (fwu_byte_t)SENTINEL);
        check("vektor 1: FWU_OK", fwu_frame_encode(0x02u, 0x07u, payload, 3u, out, 12u, &length) == FWU_OK);
        check("vektor 1: out_length == 12", length == 12u);
        check("vektor 1: 12 bayt birebir", equal_bytes(out, want, 12u));
        check("vektor 1: sonrasi dokunulmadi", untouched(out, 12u, BUFFER_SIZE));
    }
    {
        /* tek bayt 0x00 payload, type 0, seq 0; CRC(01 00 00 00 01 00) = 0x5A00 (Python) */
        const fwu_byte_t payload[1] = { 0x00u };
        const fwu_byte_t want[10] = { 0xA5u, 0x01u, 0x00u, 0x00u, 0x00u, 0x01u, 0x00u, 0x5Au, 0x00u, 0x5Au };
        fill(out, BUFFER_SIZE, (fwu_byte_t)SENTINEL);
        length = LENGTH_SENTINEL;
        check("vektor 2: FWU_OK", fwu_frame_encode(0x00u, 0x00u, payload, 1u, out, 10u, &length) == FWU_OK);
        check("vektor 2: out_length == 10 (en kucuk cerceve)", length == 10u);
        check("vektor 2: 10 bayt birebir", equal_bytes(out, want, 10u));
    }
}

/* ------------------------------------------------------------------ rastgele tarama */

static void test_random_sweep(void)
{
    fwu_byte_t payload[512];
    fwu_byte_t out[BUFFER_SIZE];
    fwu_byte_t want[BUFFER_SIZE];
    unsigned rng = 12345u;
    int ok = 1;
    int guard_ok = 1;

    printf("\n--- rastgele tarama (300 cerceve, uzunluk 1..512) ---\n");

    for(unsigned iteration = 0u; iteration < 300u; iteration++) {
        size_t n;
        fwu_byte_t type;
        fwu_byte_t seq;
        size_t length = LENGTH_SENTINEL;
        size_t want_length;

        rng = (rng * 1103515245u) + 12345u;
        n = (iteration == 0u) ? 1u : (iteration == 1u) ? 512u : (size_t)(((rng >> 16) % 512u) + 1u);
        rng = (rng * 1103515245u) + 12345u;
        type = (fwu_byte_t)((rng >> 16) & 0xFFu);
        rng = (rng * 1103515245u) + 12345u;
        seq = (fwu_byte_t)((rng >> 16) & 0xFFu);
        for(size_t i = 0u; i < n; i++) {
            rng = (rng * 1103515245u) + 12345u;
            payload[i] = (fwu_byte_t)((rng >> 16) & 0xFFu);
        }

        want_length = model_frame(type, seq, payload, n, want);
        fill(out, BUFFER_SIZE, (fwu_byte_t)SENTINEL);
        if(fwu_frame_encode(type, seq, payload, n, out, want_length, &length) != FWU_OK) {
            ok = 0;
            continue;
        }
        if((length != want_length) || !equal_bytes(out, want, want_length)) {
            ok = 0;
        }
        if(!untouched(out, want_length, BUFFER_SIZE)) {
            guard_ok = 0;
        }
    }
    check("300 cerceve bagimsiz modelle birebir ayni (kapasite tam yeterli)", ok);
    check("hicbirinde cerceve sinirinin otesine yazilmadi (notbetci bayt)", guard_ok);
}

/* ------------------------------------------------------------------ hata durumları */

static void test_errors(void)
{
    fwu_byte_t payload[600];
    fwu_byte_t out[BUFFER_SIZE];
    size_t length;

    printf("\n--- hata durumlari (tampon ve out_length degismemeli) ---\n");

    for(size_t i = 0u; i < sizeof(payload) / sizeof(payload[0]); i++) {
        payload[i] = (fwu_byte_t)(i & 0xFFu);
    }

#define EXPECT_FAIL(name, want_code, call)                                                         \
    do {                                                                                          \
        fill(out, BUFFER_SIZE, (fwu_byte_t)SENTINEL);                                              \
        length = LENGTH_SENTINEL;                                                                 \
        check(name ": kod", (call) == (want_code));                                               \
        check(name ": tampon degismedi", untouched(out, 0u, BUFFER_SIZE));                        \
        check(name ": out_length degismedi", length == LENGTH_SENTINEL);                          \
    } while(0)

    EXPECT_FAIL("payload 0 bayt", FWU_ERR_INVALID_ARG, fwu_frame_encode(1u, 1u, payload, 0u, out, BUFFER_SIZE, &length));
    EXPECT_FAIL("payload 513 bayt (sinirin 1 uzeri)", FWU_ERR_INVALID_ARG, fwu_frame_encode(1u, 1u, payload, 513u, out, BUFFER_SIZE, &length));
    EXPECT_FAIL("payload cok buyuk (60000)", FWU_ERR_INVALID_ARG, fwu_frame_encode(1u, 1u, payload, 60000u, out, BUFFER_SIZE, &length));
    EXPECT_FAIL("payload NULL", FWU_ERR_INVALID_ARG, fwu_frame_encode(1u, 1u, NULL, 8u, out, BUFFER_SIZE, &length));
    EXPECT_FAIL("kapasite 1 eksik (payload 10, kapasite 18)", FWU_ERR_BUFFER_TOO_SMALL, fwu_frame_encode(1u, 1u, payload, 10u, out, 18u, &length));
    EXPECT_FAIL("kapasite 0", FWU_ERR_BUFFER_TOO_SMALL, fwu_frame_encode(1u, 1u, payload, 1u, out, 0u, &length));
    EXPECT_FAIL("kapasite sadece payload kadar (kapasite 496, payload 496 -> 505 gerekir)", FWU_ERR_BUFFER_TOO_SMALL, fwu_frame_encode(1u, 1u, payload, 496u, out, 496u, &length));

    /* out ve out_length NULL durumlari: yalnizca dönen kod ve diger tamponun degismemesi. */
    fill(out, BUFFER_SIZE, (fwu_byte_t)SENTINEL);
    length = LENGTH_SENTINEL;
    check("out NULL", fwu_frame_encode(1u, 1u, payload, 8u, NULL, BUFFER_SIZE, &length) == FWU_ERR_INVALID_ARG);
    check("out NULL: out_length degismedi", length == LENGTH_SENTINEL);
    check("out_length NULL", fwu_frame_encode(1u, 1u, payload, 8u, out, BUFFER_SIZE, NULL) == FWU_ERR_INVALID_ARG);
    check("out_length NULL: tampon degismedi", untouched(out, 0u, BUFFER_SIZE));

#undef EXPECT_FAIL
}

/* ------------------------------------------------------------------ kapasite sınırı */

static void test_capacity_boundary(void)
{
    fwu_byte_t payload[512];
    fwu_byte_t out[BUFFER_SIZE];
    fwu_byte_t want[BUFFER_SIZE];
    size_t length;
    size_t want_length;

    printf("\n--- kapasite siniri ve nobetci ---\n");

    for(size_t i = 0u; i < sizeof(payload) / sizeof(payload[0]); i++) {
        payload[i] = (fwu_byte_t)((i * 7u) & 0xFFu);
    }

    /* Tam sığan kapasite (496 bayt payload -> 505) */
    want_length = model_frame(0x10u, 0xFFu, payload, 496u, want);
    fill(out, BUFFER_SIZE, (fwu_byte_t)SENTINEL);
    length = LENGTH_SENTINEL;
    check("496 bayt payload, kapasite tam 505: FWU_OK", fwu_frame_encode(0x10u, 0xFFu, payload, 496u, out, 505u, &length) == FWU_OK);
    check("... out_length 505, icerik modelle ayni", length == 505u && want_length == 505u && equal_bytes(out, want, 505u));
    check("... 505. bayttan sonrasi dokunulmadi (tasma yok)", untouched(out, 505u, BUFFER_SIZE));

    /* Bir eksik kapasite */
    fill(out, BUFFER_SIZE, (fwu_byte_t)SENTINEL);
    length = LENGTH_SENTINEL;
    check("kapasite 504 (bir eksik): BUFFER_TOO_SMALL", fwu_frame_encode(0x10u, 0xFFu, payload, 496u, out, 504u, &length) == FWU_ERR_BUFFER_TOO_SMALL);
    check("... tampon dokunulmadi", untouched(out, 0u, BUFFER_SIZE));

    /* En büyük çerçeve */
    want_length = model_frame(0x01u, 0x00u, payload, 512u, want);
    fill(out, BUFFER_SIZE, (fwu_byte_t)SENTINEL);
    length = LENGTH_SENTINEL;
    check("512 bayt payload, kapasite 521: FWU_OK", fwu_frame_encode(0x01u, 0x00u, payload, 512u, out, 521u, &length) == FWU_OK);
    check("... out_length 521, LEN alani 02 00, son bayt EOF",
          length == 521u && want_length == 521u && out[4] == 0x02u && out[5] == 0x00u && out[520] == 0x5Au);
    check("... icerik modelle birebir ayni", equal_bytes(out, want, 521u));
    check("... 521. bayttan sonrasi dokunulmadi", untouched(out, 521u, BUFFER_SIZE));

    /* Kapasite gereğinden FAZLA: yalnızca gereken kadar yazılır, out_length gerçek boyuttur. */
    fill(out, BUFFER_SIZE, (fwu_byte_t)SENTINEL);
    length = LENGTH_SENTINEL;
    check("kapasite fazla (600) ama payload 4: FWU_OK", fwu_frame_encode(0x03u, 0x01u, payload, 4u, out, 600u, &length) == FWU_OK);
    check("... out_length 13 ve sonrasi dokunulmadi", length == 13u && untouched(out, 13u, BUFFER_SIZE));
}

/* ------------------------------------------------------------------ C28x kirli bitler */

static void test_dirty_bits(void)
{
    fwu_byte_t out[BUFFER_SIZE];
    size_t length = 0u;
    /* Normal modda (fwu_byte_t 8 bit) bu degerler zaten kirpilir; -DC2000'de ust bitler gercekten kirlidir. */
    const fwu_byte_t payload[2] = { (fwu_byte_t)0x1FFu, (fwu_byte_t)0x1A5u };

    printf("\n--- ust biti kirli girdiler (C28x taklidinde anlamli) ---\n");

    fill(out, BUFFER_SIZE, (fwu_byte_t)SENTINEL);
    check("kirli type/seq/payload ile FWU_OK", fwu_frame_encode((fwu_byte_t)0x1A5u, (fwu_byte_t)0x1FEu, payload, 2u, out, 11u, &length) == FWU_OK);
    check("TYPE ve SEQ alt 8 bite maskelendi", (out[2] == 0xA5u) && (out[3] == 0xFEu));
    check("payload baytlari maskelendi (FF, A5)", (out[6] == 0xFFu) && (out[7] == 0xA5u));
    {
        int clean = 1;
        for(size_t i = 0u; i < 11u; i++) {
            if((((unsigned)out[i]) & 0xFF00u) != 0u) {
                clean = 0;
            }
        }
        check("cikis baytlarinin ust bitleri temiz (hepsi 0..255)", clean);
    }
}

/* ------------------------------------------------------------------ main */

int main(void)
{
    test_constants();
    test_vectors();
    test_random_sweep();
    test_errors();
    test_capacity_boundary();
    test_dirty_bits();

    printf("\nResult: %s (%d failed)\n", g_failures == 0 ? "ALL TESTS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
