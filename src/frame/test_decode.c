/**
 * @file    test_decode.c
 * @brief   fwu_frame_decode (fwu_decode.h) için kendi kendini kontrol eden test.
 *
 * Beklentiler çerçeve biçiminden BAĞIMSIZ, elle yazılmış baytlardan ve bağımsız bir CRC
 * referansından gelir (başlıktaki sabitlere güvenilmez). Kapsam:
 *   1. Geçerli çerçeve ve view alanları; payload tampon içini gösterir (kopya yok).
 *   2. Kısmi veri: geçerli çerçevenin her önekı (0..11 bayt) NOT_ENOUGH_DATA; tampon uzunluğunun
 *      ötesi OKUNMAZ (öneklerin arkasında geçerli baytlar bilerek durur: fazla okuyan decode
 *      yanlışlıkla "geçerli" derdi).
 *   3. Erken ret: geçersizlik fark edilir edilmez (SOF, VER, LEN) bekletmeden bildirilir.
 *   4. LEN sınırı: 0, 513, 600, 65535 -> BAD_LENGTH; 512 geçerli; 512 bir eksik bayt -> bekle.
 *   5. Bozulma: TYPE, SEQ, payload ve CRC baytlarındaki her tek bit hatası -> CRC_MISMATCH.
 *   6. EOF hatası, EOF ile CRC birlikte hatalıysa EOF önce raporlanır.
 *   7. Fazla bayt (sonraki çerçeve) yok sayılır; hatada view değişmez; NULL argümanlar.
 *   8. encode -> decode gidiş-dönüş (300 rastgele çerçeve) ve 20000 rastgele çöp tampon.
 *
 * Derleme (frame klasöründen):
 *   gcc -Wall -Wextra -Wpedantic -Wconversion -std=c99 -I. -I.. -I../error -I../bytes -I../crc fwu_decode.c fwu_frame.c test_decode.c -o test_decode && ./test_decode
 */

#include <stdio.h>
#include <string.h>
#include "fwu_frame.h"
#include "fwu_decode.h"

#define BUFFER_SIZE   700u
#define SENTINEL_TYPE 0xEEu

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

/* ------------------------------------------------------------------ bağımsız yardımcılar */

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

/** Elle yazılmış sayılarla geçerli çerçeve üretir (başlık sabitleri KULLANILMAZ). */
static size_t build_frame(fwu_byte_t type, fwu_byte_t seq, const fwu_byte_t *payload, size_t n, fwu_byte_t *out)
{
    uint16_t crc;
    out[0] = 0xA5u; out[1] = 1u; out[2] = type; out[3] = seq;
    out[4] = (fwu_byte_t)((n >> 8) & 0xFFu); out[5] = (fwu_byte_t)(n & 0xFFu);
    for(size_t i = 0u; i < n; i++) {
        out[6u + i] = payload[i];
    }
    crc = ref_crc16(&out[1], 5u + n);
    out[6u + n] = (fwu_byte_t)((crc >> 8) & 0xFFu);
    out[7u + n] = (fwu_byte_t)(crc & 0xFFu);
    out[8u + n] = 0x5Au;
    return 9u + n;
}

static void copy_bytes(fwu_byte_t *dst, const fwu_byte_t *src, size_t n)
{
    for(size_t i = 0u; i < n; i++) {
        dst[i] = src[i];
    }
}

static void fill_bytes(fwu_byte_t *dst, size_t n, fwu_byte_t value)
{
    for(size_t i = 0u; i < n; i++) {
        dst[i] = value;
    }
}

static void clear_view(fwu_frame_view_t *view)
{
    view->type = (fwu_byte_t)SENTINEL_TYPE;
    view->seq = (fwu_byte_t)SENTINEL_TYPE;
    view->payload = NULL;
    view->payload_length = 12345u;
    view->frame_length = 12345u;
}

static int view_unchanged(const fwu_frame_view_t *view)
{
    return (view->type == (fwu_byte_t)SENTINEL_TYPE) && (view->seq == (fwu_byte_t)SENTINEL_TYPE) &&
           (view->payload == NULL) && (view->payload_length == 12345u) && (view->frame_length == 12345u);
}

/* Kurulum: tampon doldurmak için tek bir "decode et ve sonucu karşılaştır" yardımcısı. */
static void expect_decode(const char *name, const fwu_byte_t *buffer, size_t length, fwu_return_e want)
{
    fwu_frame_view_t view;
    clear_view(&view);
    expect(name, fwu_frame_decode(buffer, length, &view), want);
    if(want != FWU_OK) {
        char text[160];
        (void)snprintf(text, sizeof(text), "%s: hatada view degismedi", name);
        check(text, view_unchanged(&view));
    }
}

/* ------------------------------------------------------------------ testler */

static void test_valid(void)
{
    /* Elle yazılmış, bilinen çerçeve (CRC Python ile): payload 11 22 33, type 2, seq 7. */
    const fwu_byte_t frame[12] = { 0xA5u, 0x01u, 0x02u, 0x07u, 0x00u, 0x03u, 0x11u, 0x22u, 0x33u, 0x44u, 0xABu, 0x5Au };
    fwu_frame_view_t view;

    printf("\n--- gecerli cerceve ---\n");

    clear_view(&view);
    expect("12 baytlik gecerli cerceve", fwu_frame_decode(frame, 12u, &view), FWU_OK);
    check("type 2, seq 7", view.type == 2u && view.seq == 7u);
    check("payload_length 3, frame_length 12", view.payload_length == 3u && view.frame_length == 12u);
    check("payload tampon icini gosterir (kopya yok): frame + 6", view.payload == &frame[6]);
    check("payload icerigi 11 22 33", view.payload[0] == 0x11u && view.payload[1] == 0x22u && view.payload[2] == 0x33u);

    {
        /* Arkada fazladan baytlar (bir sonraki çerçevenin başı) yok sayılır. */
        fwu_byte_t longer[40];
        fill_bytes(longer, 40u, (fwu_byte_t)0xCCu);
        copy_bytes(longer, frame, 12u);
        clear_view(&view);
        expect("arkada fazladan bayt var: yine gecerli", fwu_frame_decode(longer, 40u, &view), FWU_OK);
        check("frame_length yine 12 (fazlasi tuketilmez)", view.frame_length == 12u);
    }
}

static void test_partial(void)
{
    /* Geçerli çerçeve, ARKASINDA da geçerli baytlar durur ki fazla okuyan decode yanılsın. */
    fwu_byte_t buffer[BUFFER_SIZE];
    fwu_byte_t payload[3] = { 0x11u, 0x22u, 0x33u };
    size_t frame_length = build_frame(0x02u, 0x07u, payload, 3u, buffer);
    int all_wait = 1;

    printf("\n--- kismi veri: her onek icin bekle ---\n");

    for(size_t k = 0u; k < frame_length; k++) {
        fwu_frame_view_t view;
        clear_view(&view);
        if(fwu_frame_decode(buffer, k, &view) != FWU_ERR_NOT_ENOUGH_DATA) {
            all_wait = 0;
            printf("   (onek %u bayt: beklenen NOT_ENOUGH_DATA)\n", (unsigned)k);
        }
        if(!view_unchanged(&view)) {
            all_wait = 0;
        }
    }
    check("0..11 bayt onek: hepsi NOT_ENOUGH_DATA, view degismedi", all_wait);
    expect_decode("tamami: 12 bayt gecerli", buffer, frame_length, FWU_OK);

    expect_decode("0 bayt (bos tampon) bekle", buffer, 0u, FWU_ERR_NOT_ENOUGH_DATA);
    expect_decode("5 bayt: LEN'in ikinci baytı eksik", buffer, 5u, FWU_ERR_NOT_ENOUGH_DATA);
    expect_decode("8 bayt: LEN=3 okunur ama 12 lazim", buffer, 8u, FWU_ERR_NOT_ENOUGH_DATA);
    expect_decode("11 bayt: yalniz EOF eksik", buffer, 11u, FWU_ERR_NOT_ENOUGH_DATA);
}

static void test_early_reject(void)
{
    fwu_byte_t buffer[BUFFER_SIZE];
    fwu_byte_t payload[3] = { 0x11u, 0x22u, 0x33u };
    (void)build_frame(0x02u, 0x07u, payload, 3u, buffer);

    printf("\n--- erken ret (bekleme, hemen bildir) ---\n");

    {
        fwu_byte_t b[BUFFER_SIZE];
        copy_bytes(b, buffer, 12u);
        b[0] = 0x7Fu;
        expect_decode("ilk bayt SOF degil, 12 bayt: BAD_FRAME", b, 12u, FWU_ERR_BAD_FRAME);
        expect_decode("ilk bayt SOF degil, YALNIZ 1 bayt: bekletmeden BAD_FRAME", b, 1u, FWU_ERR_BAD_FRAME);
        expect_decode("ilk bayt SOF degil, 3 bayt: BAD_FRAME", b, 3u, FWU_ERR_BAD_FRAME);
    }
    {
        fwu_byte_t b[BUFFER_SIZE];
        copy_bytes(b, buffer, 12u);
        b[1] = 0x02u;
        expect_decode("VER 2 (desteklenmiyor), 2 bayt: bekletmeden BAD_FRAME", b, 2u, FWU_ERR_BAD_FRAME);
        expect_decode("VER 2, 12 bayt: BAD_FRAME", b, 12u, FWU_ERR_BAD_FRAME);
        expect_decode("VER 0, 4 bayt: BAD_FRAME", (const fwu_byte_t[]){ 0xA5u, 0x00u, 0x02u, 0x07u }, 4u, FWU_ERR_BAD_FRAME);
        expect_decode("yalniz SOF (1 bayt, dogru): henuz bekle", b, 1u, FWU_ERR_NOT_ENOUGH_DATA);
    }
    {
        /* LEN sınırı: yalnızca BAŞLIK (6 bayt) bile gelse ret, çerçevenin kalanı beklenmez. */
        expect_decode("LEN = 600 (> 512), yalniz 6 bayt: BAD_LENGTH, beklemeden",
                      (const fwu_byte_t[]){ 0xA5u, 0x01u, 0x02u, 0x07u, 0x02u, 0x58u }, 6u, FWU_ERR_BAD_LENGTH);
        expect_decode("LEN = 513, yalniz 6 bayt: BAD_LENGTH",
                      (const fwu_byte_t[]){ 0xA5u, 0x01u, 0x02u, 0x07u, 0x02u, 0x01u }, 6u, FWU_ERR_BAD_LENGTH);
        expect_decode("LEN = 65535, yalniz 6 bayt: BAD_LENGTH (sonsuz bekletilemez)",
                      (const fwu_byte_t[]){ 0xA5u, 0x01u, 0x02u, 0x07u, 0xFFu, 0xFFu }, 6u, FWU_ERR_BAD_LENGTH);
        expect_decode("LEN = 0 (bos payload kabul edilmez): BAD_LENGTH",
                      (const fwu_byte_t[]){ 0xA5u, 0x01u, 0x02u, 0x07u, 0x00u, 0x00u, 0x5Au, 0x00u, 0x5Au }, 9u, FWU_ERR_BAD_LENGTH);
        expect_decode("baslik yarim (5 bayt) ama SOF/VER dogru: bekle",
                      (const fwu_byte_t[]){ 0xA5u, 0x01u, 0x02u, 0x07u, 0x02u }, 5u, FWU_ERR_NOT_ENOUGH_DATA);
    }
}

static void test_max_payload(void)
{
    fwu_byte_t payload[512];
    fwu_byte_t buffer[BUFFER_SIZE];
    size_t length;
    fwu_frame_view_t view;

    printf("\n--- LEN siniri (512) ---\n");

    for(size_t i = 0u; i < 512u; i++) {
        payload[i] = (fwu_byte_t)((i * 3u) & 0xFFu);
    }
    length = build_frame(0x01u, 0x00u, payload, 512u, buffer);
    clear_view(&view);
    expect("512 bayt payload (en buyuk cerceve, 521 bayt): gecerli", fwu_frame_decode(buffer, length, &view), FWU_OK);
    check("... payload_length 512, frame_length 521", view.payload_length == 512u && view.frame_length == 521u && length == 521u);
    expect_decode("512 bayt payload, tampon 520 bayt (1 eksik): bekle", buffer, 520u, FWU_ERR_NOT_ENOUGH_DATA);
    expect_decode("512 bayt payload, tampon 6 bayt: bekle", buffer, 6u, FWU_ERR_NOT_ENOUGH_DATA);

    length = build_frame(0x01u, 0x00u, payload, 1u, buffer);
    clear_view(&view);
    expect("1 bayt payload (en kucuk cerceve, 10 bayt): gecerli", fwu_frame_decode(buffer, length, &view), FWU_OK);
    check("... frame_length 10", view.frame_length == 10u && length == 10u);
}

static void test_corruption(void)
{
    fwu_byte_t payload[5] = { 0x10u, 0x20u, 0x30u, 0x40u, 0x50u };
    fwu_byte_t good[BUFFER_SIZE];
    fwu_byte_t bad[BUFFER_SIZE];
    size_t length = build_frame(0x04u, 0x09u, payload, 5u, good);   /* 14 bayt */
    int all_caught = 1;
    int count = 0;

    printf("\n--- bozulma: her tek bit hatasi ---\n");

    /* TYPE (2), SEQ (3), payload (6..10), CRC (11, 12): her bayt, her bit. LEN (4, 5) atlanir: LEN
     * bozulursa uzunluk degisir (BAD_LENGTH/bekle), CRC hatasi degil. EOF (13) ayri test. */
    for(size_t index = 2u; index <= 12u; index++) {
        if((index == 4u) || (index == 5u)) {
            continue;
        }
        for(unsigned bit = 0u; bit < 8u; bit++) {
            fwu_frame_view_t view;
            copy_bytes(bad, good, length);
            bad[index] = (fwu_byte_t)(bad[index] ^ (fwu_byte_t)(1u << bit));
            clear_view(&view);
            count++;
            if((fwu_frame_decode(bad, length, &view) != FWU_ERR_CRC_MISMATCH) || !view_unchanged(&view)) {
                all_caught = 0;
                printf("   (bayt %u, bit %u yakalanmadi)\n", (unsigned)index, bit);
            }
        }
    }
    check("TYPE, SEQ, payload ve CRC baytlarinda 72 tek-bit hatasi hep CRC_MISMATCH", all_caught && count == 72);

    copy_bytes(bad, good, length);
    bad[length - 1u] = 0x5Bu;
    expect_decode("EOF 5A yerine 5B (CRC dogru): BAD_FRAME", bad, length, FWU_ERR_BAD_FRAME);

    copy_bytes(bad, good, length);
    bad[length - 1u] = 0x00u;
    bad[7] = (fwu_byte_t)(bad[7] ^ 0x01u);
    expect_decode("EOF yanlis VE payload bozuk: EOF once raporlanir (BAD_FRAME)", bad, length, FWU_ERR_BAD_FRAME);

    copy_bytes(bad, good, length);
    bad[2] = (fwu_byte_t)0xEEu;     /* TYPE degisti, CRC eski */
    expect_decode("TYPE degisti, CRC eski: CRC_MISMATCH", bad, length, FWU_ERR_CRC_MISMATCH);
}

static void test_args(void)
{
    const fwu_byte_t frame[12] = { 0xA5u, 0x01u, 0x02u, 0x07u, 0x00u, 0x03u, 0x11u, 0x22u, 0x33u, 0x44u, 0xABu, 0x5Au };
    fwu_frame_view_t view;
    clear_view(&view);

    printf("\n--- argumanlar ---\n");

    expect("buffer NULL", fwu_frame_decode(NULL, 12u, &view), FWU_ERR_INVALID_ARG);
    check("... view degismedi", view_unchanged(&view));
    expect("view NULL", fwu_frame_decode(frame, 12u, NULL), FWU_ERR_INVALID_ARG);
    expect("buffer NULL, uzunluk 0", fwu_frame_decode(NULL, 0u, &view), FWU_ERR_INVALID_ARG);
}

static void test_round_trip_and_garbage(void)
{
    fwu_byte_t payload[512];
    fwu_byte_t buffer[BUFFER_SIZE];
    unsigned rng = 4242u;
    int ok = 1;
    int garbage_ok = 1;
    int garbage_valid = 0;

    printf("\n--- encode -> decode ve cop girdi ---\n");

    for(unsigned iteration = 0u; iteration < 300u; iteration++) {
        fwu_frame_view_t view;
        size_t n;
        size_t length = 0u;
        fwu_byte_t type;
        fwu_byte_t seq;

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
        if(fwu_frame_encode(type, seq, payload, n, buffer, BUFFER_SIZE, &length) != FWU_OK) {
            ok = 0;
            continue;
        }
        clear_view(&view);
        if(fwu_frame_decode(buffer, length, &view) != FWU_OK) {
            ok = 0;
            continue;
        }
        if((view.type != type) || (view.seq != seq) || (view.payload_length != n) ||
           (view.frame_length != length) || (memcmp(view.payload, payload, n * sizeof(fwu_byte_t)) != 0)) {
            ok = 0;
        }
    }
    check("300 rastgele cerceve: encode -> decode tum alanlar ayni", ok);

    /* Rastgele çöp: hiçbir şey çökmemeli; OK dönerse sonuç tutarlı olmalı (CRC bağımsız doğrulanır). */
    for(unsigned iteration = 0u; iteration < 20000u; iteration++) {
        fwu_frame_view_t view;
        size_t length;
        fwu_return_e status;

        rng = (rng * 1103515245u) + 12345u;
        length = (size_t)((rng >> 16) % 600u);
        for(size_t i = 0u; i < length; i++) {
            rng = (rng * 1103515245u) + 12345u;
            buffer[i] = (fwu_byte_t)((rng >> 16) & 0xFFu);
        }
        /* Bir kısmında SOF/VER doğru yapılır ki içeri girilsin. */
        if((iteration % 4u == 0u) && (length >= 2u)) {
            buffer[0] = 0xA5u;
            buffer[1] = 0x01u;
        }
        clear_view(&view);
        status = fwu_frame_decode(buffer, length, &view);
        if(status == FWU_OK) {
            garbage_valid++;
            if((view.frame_length > length) || (view.payload_length + 9u != view.frame_length) ||
               (ref_crc16(&buffer[1], 5u + view.payload_length) !=
                (uint16_t)(((uint16_t)buffer[6u + view.payload_length] << 8) | buffer[7u + view.payload_length]))) {
                garbage_ok = 0;
            }
        } else if(!view_unchanged(&view)) {
            garbage_ok = 0;
        }
        if((status != FWU_OK) && (status != FWU_ERR_NOT_ENOUGH_DATA) && (status != FWU_ERR_BAD_FRAME) &&
           (status != FWU_ERR_BAD_LENGTH) && (status != FWU_ERR_CRC_MISMATCH)) {
            garbage_ok = 0;   /* beklenmeyen hata kodu */
        }
    }
    check("20000 cop tampon: cokme yok, yalniz beklenen kodlar, OK dönerse tutarli, hatada view degismedi", garbage_ok);
    printf("   (cop girdiden tesadufen gecerli sayilan: %d)\n", garbage_valid);
}

/* ------------------------------------------------------------------ main */

int main(void)
{
    test_valid();
    test_partial();
    test_early_reject();
    test_max_payload();
    test_corruption();
    test_args();
    test_round_trip_and_garbage();

    printf("\nResult: %s (%d failed)\n", g_failures == 0 ? "ALL TESTS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
