/**
 * @file    test_parser.c
 * @brief   fwu_parser (fwu_parser.h) için kendi kendini kontrol eden test.
 *
 * Çerçeveler elle yazılmış sayılarla ve bağımsız bir CRC referansıyla üretilir (başlık
 * sabitlerine güvenilmez). Kapsam:
 *   1. Tek çerçeve, parça parça (5+4+4) ve bayt bayt gelen çerçeve.
 *   2. Başta çöp, arka arkaya iki çerçeve, payload içinde 0xA5 (yanlış alarm), bozuk çerçeve
 *      (CRC) ardından gelen sağlam çerçeve, LEN'i çok büyük başlık ardından gelen sağlam çerçeve.
 *   3. Zaman aşımı: sınırda bekler, aşınca atar, 32 bit ms sayacı sararken de çalışır, damla
 *      damla gelen veri (saldırı) başın zamanından sayıldığı için sonsuza kadar tutamaz.
 *   4. Teslim edilen çerçeve bir sonraki poll'a kadar geçerli, sonra silinir.
 *   5. Bir poll'da atılan bayt sınırı (kesme sürekli çöp doldursa bile poll geri döner).
 *   6. Rastgele akış (çöp ve çerçeveler karışık, rastgele parçalar) bağımsız modelle sıra ve
 *      içerik olarak karşılaştırılır.
 *   7. Argümanlar ve tanı sayaçları.
 *
 * Derleme (frame klasöründen):
 *   gcc -Wall -Wextra -Wpedantic -Wconversion -std=c99 -I. -I.. -I../error -I../bytes -I../crc -I../ring_buffer fwu_parser.c fwu_decode.c fwu_frame.c ../ring_buffer/fwu_ringbuf.c test_parser.c -o test_parser && ./test_parser
 */

#include <stdio.h>
#include <string.h>
#include "fwu_parser.h"

FWU_RINGBUF_DEFINE(small_rb, 1000);
FWU_RINGBUF_DEFINE(big_rb, 3000);

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

static void clear_view(fwu_frame_view_t *view)
{
    view->type = 0xEEu;
    view->seq = 0xEEu;
    view->payload = NULL;
    view->payload_length = 12345u;
    view->frame_length = 12345u;
}

/* Bir kurulum: ring buffer + parser. */
typedef struct {
    small_rb_t   rb;
    fwu_parser_t parser;
} rig_t;

static void rig_init(rig_t *rig)
{
    (void)small_rb_init(&rig->rb);
    (void)fwu_parser_init(&rig->parser, &rig->rb.base);
}

static void feed(rig_t *rig, const fwu_byte_t *data, size_t n)
{
    (void)fwu_ringbuf_push(&rig->rb.base, data, n);
}

/** Geçerli 12 baytlık örnek çerçeve: type 2, seq 7, payload 11 22 33. */
static const fwu_byte_t FRAME12[12] = { 0xA5u, 0x01u, 0x02u, 0x07u, 0x00u, 0x03u, 0x11u, 0x22u, 0x33u, 0x44u, 0xABu, 0x5Au };

static int view_is_frame12(const fwu_frame_view_t *view)
{
    return (view->type == 2u) && (view->seq == 7u) && (view->payload_length == 3u) && (view->frame_length == 12u) &&
           (view->payload[0] == 0x11u) && (view->payload[1] == 0x22u) && (view->payload[2] == 0x33u);
}

/* ------------------------------------------------------------------ testler */

static void test_basic(void)
{
    static rig_t rig;
    fwu_frame_view_t view;

    printf("\n--- temel akis ---\n");

    rig_init(&rig);
    clear_view(&view);
    expect("veri yokken poll: henuz yok", fwu_parser_poll(&rig.parser, 0u, &view), FWU_ERR_NOT_ENOUGH_DATA);

    feed(&rig, FRAME12, 12u);
    expect("tek seferde gelen cerceve: OK", fwu_parser_poll(&rig.parser, 1u, &view), FWU_OK);
    check("... alanlar dogru, payload parser tamponunda", view_is_frame12(&view));
    expect("sonraki poll: cerceve tuketildi, yeni veri yok", fwu_parser_poll(&rig.parser, 2u, &view), FWU_ERR_NOT_ENOUGH_DATA);
    check("frames_ok = 1, atilan bayt 0", rig.parser.stats.frames_ok == 1u && rig.parser.stats.bytes_dropped == 0u);

    /* Üç parça: 7F çöp + 4 bayt, sonra 4, sonra 4 bayt (örnek tablodaki akış). */
    rig_init(&rig);
    {
        const fwu_byte_t part1[5] = { 0x7Fu, 0xA5u, 0x01u, 0x02u, 0x07u };
        const fwu_byte_t part2[4] = { 0x00u, 0x03u, 0x11u, 0x22u };
        const fwu_byte_t part3[4] = { 0x33u, 0x44u, 0xABu, 0x5Au };
        feed(&rig, part1, 5u);
        expect("parca 1 (cop + 4 bayt): henuz yok", fwu_parser_poll(&rig.parser, 0u, &view), FWU_ERR_NOT_ENOUGH_DATA);
        check("... cop bayt atildi (1)", rig.parser.stats.bytes_dropped == 1u && rig.parser.count == 4u);
        feed(&rig, part2, 4u);
        expect("parca 2: henuz yok", fwu_parser_poll(&rig.parser, 1u, &view), FWU_ERR_NOT_ENOUGH_DATA);
        feed(&rig, part3, 4u);
        expect("parca 3: cerceve hazir", fwu_parser_poll(&rig.parser, 2u, &view), FWU_OK);
        check("... dogru cerceve", view_is_frame12(&view));
    }

    /* Bayt bayt: yalnızca son bayttan sonra OK. */
    rig_init(&rig);
    {
        int waits_ok = 1;
        int delivered = 0;
        for(size_t i = 0u; i < 12u; i++) {
            fwu_return_e status;
            feed(&rig, &FRAME12[i], 1u);
            status = fwu_parser_poll(&rig.parser, (uint32_t)i, &view);
            if(i < 11u) {
                if(status != FWU_ERR_NOT_ENOUGH_DATA) {
                    waits_ok = 0;
                }
            } else if(status == FWU_OK) {
                delivered = view_is_frame12(&view);
            }
        }
        check("bayt bayt gelen cerceve: ilk 11 bayttan sonra bekler", waits_ok);
        check("... 12. bayttan sonra dogru cerceve teslim edilir", delivered);
    }
}

static void test_resync(void)
{
    static rig_t rig;
    fwu_frame_view_t view;

    printf("\n--- yeniden senkronizasyon ---\n");

    /* Çerçeve + hemen ardından ikinci çerçeve (tek push). */
    rig_init(&rig);
    {
        fwu_byte_t two[24];
        copy_bytes(two, FRAME12, 12u);
        copy_bytes(&two[12], FRAME12, 12u);
        two[12 + 3] = 0x08u;           /* ikinci çerçevenin SEQ'i değişince CRC bozulur: bunu doğrusunu üreterek yap */
        {
            const fwu_byte_t payload[3] = { 0x11u, 0x22u, 0x33u };
            (void)build_frame(0x02u, 0x08u, payload, 3u, &two[12]);
        }
        feed(&rig, two, 24u);
        expect("iki ardisik cerceve: birinci OK", fwu_parser_poll(&rig.parser, 0u, &view), FWU_OK);
        check("... seq 7", view.seq == 7u);
        expect("ikinci OK", fwu_parser_poll(&rig.parser, 0u, &view), FWU_OK);
        check("... seq 8", view.seq == 8u);
        expect("sonra yok", fwu_parser_poll(&rig.parser, 0u, &view), FWU_ERR_NOT_ENOUGH_DATA);
        check("frames_ok = 2", rig.parser.stats.frames_ok == 2u);
    }

    /* Önünde çöp ve yanlış alarm (SOF görünümlü baytlar). */
    rig_init(&rig);
    {
        fwu_byte_t stream[40];
        const fwu_byte_t junk[6] = { 0x00u, 0xA5u, 0xA5u, 0x01u, 0x99u, 0x7Fu };
        copy_bytes(stream, junk, 6u);
        copy_bytes(&stream[6], FRAME12, 12u);
        feed(&rig, stream, 18u);
        expect("cop ve yanlis alarm (A5 A5 01 99 ...) ardindan cerceve: OK", fwu_parser_poll(&rig.parser, 0u, &view), FWU_OK);
        check("... dogru cerceve", view_is_frame12(&view));
        check("... atilan bayt 6", rig.parser.stats.bytes_dropped == 6u);
    }

    /* Payload içinde 0xA5 geçen gerçek çerçeve bozulmadan gelir. */
    rig_init(&rig);
    {
        const fwu_byte_t payload[5] = { 0xA5u, 0x01u, 0xA5u, 0x5Au, 0xA5u };
        fwu_byte_t frame[20];
        size_t n = build_frame(0x03u, 0x05u, payload, 5u, frame);
        feed(&rig, frame, n);
        expect("payload icinde 0xA5 var: OK", fwu_parser_poll(&rig.parser, 0u, &view), FWU_OK);
        check("... payload bozulmadi", view.payload_length == 5u && view.payload[0] == 0xA5u && view.payload[2] == 0xA5u && view.payload[4] == 0xA5u);
        check("... hic bayt atilmadi", rig.parser.stats.bytes_dropped == 0u);
    }

    /* Bozuk (CRC) çerçeve, ardından sağlam çerçeve. */
    rig_init(&rig);
    {
        fwu_byte_t stream[24];
        copy_bytes(stream, FRAME12, 12u);
        stream[7] = (fwu_byte_t)(stream[7] ^ 0x01u);      /* bozuk payload */
        copy_bytes(&stream[12], FRAME12, 12u);
        feed(&rig, stream, 24u);
        expect("bozuk cerceve atilir, ardindaki saglam cerceve gelir", fwu_parser_poll(&rig.parser, 0u, &view), FWU_OK);
        check("... saglam olan teslim edildi", view_is_frame12(&view));
        check("... crc_mismatch sayildi, 12 bayt atildi", rig.parser.stats.crc_mismatch == 1u && rig.parser.stats.bytes_dropped == 12u);
    }

    /* LEN'i çok büyük başlık (600), hemen ardından sağlam çerçeve: 600 bayt BEKLENMEZ. */
    rig_init(&rig);
    {
        fwu_byte_t stream[30];
        const fwu_byte_t head[6] = { 0xA5u, 0x01u, 0x02u, 0x07u, 0x02u, 0x58u };
        copy_bytes(stream, head, 6u);
        copy_bytes(&stream[6], FRAME12, 12u);
        feed(&rig, stream, 18u);
        expect("LEN=600 baslik ardindan saglam cerceve: beklemeden OK", fwu_parser_poll(&rig.parser, 0u, &view), FWU_OK);
        check("... dogru cerceve, bad_length 1, atilan 6", view_is_frame12(&view) && rig.parser.stats.bad_length == 1u && rig.parser.stats.bytes_dropped == 6u);
    }
}

static void test_timeout(void)
{
    static rig_t rig;
    fwu_frame_view_t view;

    printf("\n--- zaman asimi ---\n");

    check("FWU_PARSER_FRAME_TIMEOUT_MS == 500", FWU_PARSER_FRAME_TIMEOUT_MS == 500u);

    rig_init(&rig);
    feed(&rig, FRAME12, 5u);
    expect("yarim cerceve t=0: bekler", fwu_parser_poll(&rig.parser, 0u, &view), FWU_ERR_NOT_ENOUGH_DATA);
    expect("t=499: hala bekler", fwu_parser_poll(&rig.parser, 499u, &view), FWU_ERR_NOT_ENOUGH_DATA);
    expect("t=500 (tam sinir): hala bekler", fwu_parser_poll(&rig.parser, 500u, &view), FWU_ERR_NOT_ENOUGH_DATA);
    check("... henuz atilmadi", rig.parser.count == 5u && rig.parser.stats.timeouts == 0u);
    expect("t=501: zaman asimi, yarim cerceve atilir", fwu_parser_poll(&rig.parser, 501u, &view), FWU_ERR_NOT_ENOUGH_DATA);
    check("... tampon bos, timeouts 1, atilan 5", rig.parser.count == 0u && rig.parser.stats.timeouts == 1u && rig.parser.stats.bytes_dropped == 5u);
    feed(&rig, FRAME12, 12u);
    expect("sonra gelen saglam cerceve normal alinir", fwu_parser_poll(&rig.parser, 600u, &view), FWU_OK);
    check("... dogru", view_is_frame12(&view));

    /* 32 bit ms sayacı sarıyor: 0xFFFFFF00'da başlayan bekleme 0x00000200'de (768 ms sonra) bitmeli. */
    rig_init(&rig);
    feed(&rig, FRAME12, 5u);
    expect("sayac sarmasi: t=0xFFFFFF00 bekler", fwu_parser_poll(&rig.parser, 0xFFFFFF00u, &view), FWU_ERR_NOT_ENOUGH_DATA);
    expect("t=0x00000100 (sarmadan sonra, 512 ms): zaman asimi", fwu_parser_poll(&rig.parser, 0x00000100u, &view), FWU_ERR_NOT_ENOUGH_DATA);
    check("... yarim cerceve atildi", rig.parser.count == 0u && rig.parser.stats.timeouts == 1u);

    rig_init(&rig);
    feed(&rig, FRAME12, 5u);
    expect("sarma oncesi t=0xFFFFFFF0 bekler", fwu_parser_poll(&rig.parser, 0xFFFFFFF0u, &view), FWU_ERR_NOT_ENOUGH_DATA);
    expect("t=0x000000F0 (sarma sonrasi, 256 ms): HENUZ bekler", fwu_parser_poll(&rig.parser, 0x000000F0u, &view), FWU_ERR_NOT_ENOUGH_DATA);
    check("... atilmadi", rig.parser.count == 5u && rig.parser.stats.timeouts == 0u);

    /* Damla damla saldırı: LEN=496 ilan eden başlık, ardından 100 ms'de bir 1 bayt. Zaman başın
     * ilk geldiği andan sayılır; her yeni bayt zamanı sıfırlamaz. */
    rig_init(&rig);
    {
        const fwu_byte_t head[6] = { 0xA5u, 0x01u, 0x02u, 0x07u, 0x01u, 0xF0u };
        const fwu_byte_t filler = 0x00u;
        uint32_t t = 0u;
        int dropped_at = -1;
        feed(&rig, head, 6u);
        (void)fwu_parser_poll(&rig.parser, t, &view);
        for(int step = 1; step <= 10; step++) {
            t += 100u;
            feed(&rig, &filler, 1u);
            (void)fwu_parser_poll(&rig.parser, t, &view);
            if((dropped_at < 0) && (rig.parser.stats.timeouts > 0u)) {
                dropped_at = (int)t;
            }
        }
        check("damla damla saldiri: 600. ms'de (500'u asinca) cerceve atilir", dropped_at == 600);
    }
}

static void test_delivered_lifetime(void)
{
    static rig_t rig;
    fwu_frame_view_t view;
    fwu_frame_view_t view2;
    fwu_byte_t second[12];

    printf("\n--- teslim edilen cerceve bir sonraki poll'a kadar gecerli ---\n");

    rig_init(&rig);
    feed(&rig, FRAME12, 12u);
    expect("ilk poll OK", fwu_parser_poll(&rig.parser, 0u, &view), FWU_OK);
    check("teslimden sonra tampon hala tam cerceveyi tutar (payload gecerli)", rig.parser.count == 12u && view.payload == &rig.parser.buffer[6]);
    {
        const fwu_byte_t payload[3] = { 0x55u, 0x66u, 0x77u };
        (void)build_frame(0x09u, 0x01u, payload, 3u, second);
    }
    feed(&rig, second, 12u);
    expect("ikinci poll: onceki silindi, yenisi gelir", fwu_parser_poll(&rig.parser, 1u, &view2), FWU_OK);
    check("... ikinci cerceve dogru (type 9, payload 55 66 77)", view2.type == 9u && view2.payload[0] == 0x55u && view2.payload[2] == 0x77u);
    check("... tamponda yalniz bu cerceve var", rig.parser.count == 12u);
}

static void test_drop_budget(void)
{
    static big_rb_t rb;
    static fwu_parser_t parser;
    fwu_frame_view_t view;
    fwu_byte_t zeros[1000];
    int polls = 0;

    printf("\n--- bir poll'da atilan bayt siniri ---\n");

    (void)big_rb_init(&rb);
    (void)fwu_parser_init(&parser, &rb.base);
    for(size_t i = 0u; i < 1000u; i++) {
        zeros[i] = 0x00u;
    }
    for(int k = 0; k < 3; k++) {
        (void)fwu_ringbuf_push(&rb.base, zeros, 1000u);        /* toplam 3000 bayt cop, SOF yok */
    }
    expect("3000 bayt cop: ilk poll geri doner (sinirlanir)", fwu_parser_poll(&parser, 0u, &view), FWU_ERR_NOT_ENOUGH_DATA);
    check("... ilk poll'da tam 1042 bayt atildi (2 x 521, sinir 1024'u asinca birakir)", parser.stats.bytes_dropped == 1042u);
    check("... ring buffer'da hala bayt var", fwu_ringbuf_used_size(&rb.base) > 0u || parser.count > 0u);
    while((polls < 10) && ((fwu_ringbuf_used_size(&rb.base) > 0u) || (parser.count > 0u))) {
        (void)fwu_parser_poll(&parser, 0u, &view);
        polls++;
    }
    check("birkac poll sonra tum 3000 bayt tuketildi", parser.stats.bytes_dropped == 3000u);

    {
        fwu_byte_t frame12[12];
        copy_bytes(frame12, FRAME12, 12u);
        (void)fwu_ringbuf_push(&rb.base, frame12, 12u);
        expect("cop temizlendikten sonra gercek cerceve alinir", fwu_parser_poll(&parser, 0u, &view), FWU_OK);
    }
}

static void test_args(void)
{
    static rig_t rig;
    fwu_parser_t parser;
    fwu_frame_view_t view;

    printf("\n--- argumanlar ---\n");

    rig_init(&rig);
    expect("init: parser NULL", fwu_parser_init(NULL, &rig.rb.base), FWU_ERR_INVALID_ARG);
    expect("init: source NULL", fwu_parser_init(&parser, NULL), FWU_ERR_INVALID_ARG);
    expect("poll: parser NULL", fwu_parser_poll(NULL, 0u, &view), FWU_ERR_INVALID_ARG);
    expect("poll: view NULL", fwu_parser_poll(&rig.parser, 0u, NULL), FWU_ERR_INVALID_ARG);
}

/* ------------------------------------------------------------------ durum ve boşluk testleri */

static int view_untouched(const fwu_frame_view_t *view)
{
    return (view->type == 0xEEu) && (view->seq == 0xEEu) && (view->payload == NULL) &&
           (view->payload_length == 12345u) && (view->frame_length == 12345u);
}

static void test_state_and_edges(void)
{
    static rig_t rig;
    static fwu_parser_t zeroed;     /* static: sıfırlanmış, init ÇAĞRILMAMIŞ */
    fwu_frame_view_t view;

    printf("\n--- bosta bekleme, view korunmasi, tampon siniri, yeni basin zaman penceresi ---\n");

    /* 1) Uzun süre boşta bekleyen parser: poll hemen döner, sahte zaman aşımı sayılmaz. */
    rig_init(&rig);
    clear_view(&view);
    expect("bos parser, t=0", fwu_parser_poll(&rig.parser, 0u, &view), FWU_ERR_NOT_ENOUGH_DATA);
    expect("bos parser, t=1000000 (cok uzun bosta): yine hemen doner", fwu_parser_poll(&rig.parser, 1000000u, &view), FWU_ERR_NOT_ENOUGH_DATA);
    check("... sahte zaman asimi/atma yok", rig.parser.stats.timeouts == 0u && rig.parser.stats.bytes_dropped == 0u);
    check("... view degismedi", view_untouched(&view));

    /* 2) OK dışındaki poll'larda view değişmez: yarım çerçeve ve çöp. */
    rig_init(&rig);
    clear_view(&view);
    feed(&rig, FRAME12, 7u);
    expect("yarim cerceve: bekler", fwu_parser_poll(&rig.parser, 0u, &view), FWU_ERR_NOT_ENOUGH_DATA);
    check("... view degismedi (yarim cerceve)", view_untouched(&view));
    rig_init(&rig);
    clear_view(&view);
    {
        const fwu_byte_t junk[8] = { 0x00u, 0x11u, 0x22u, 0x33u, 0x44u, 0x55u, 0x66u, 0x77u };
        feed(&rig, junk, 8u);
    }
    expect("sadece cop: bekler", fwu_parser_poll(&rig.parser, 0u, &view), FWU_ERR_NOT_ENOUGH_DATA);
    check("... view degismedi (cop)", view_untouched(&view));

    /* 3) init edilmemiş (sıfırlanmış) parser: source NULL. */
    clear_view(&view);
    expect("init edilmemis parser: INVALID_ARG", fwu_parser_poll(&zeroed, 0u, &view), FWU_ERR_INVALID_ARG);

    /* 4) Tampon sınırı: LEN = 512 ilan eden başlık (10 bayt), ardından 990 bayt sıfır. Parser
     * tamponunu aşmamalı; sonuç kesin sayılarla doğrulanır (aşan bir parser tutarsız sayar). */
    rig_init(&rig);
    {
        const fwu_byte_t head[10] = { 0xA5u, 0x01u, 0x02u, 0x07u, 0x02u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u };
        fwu_byte_t zeros[990];
        for(size_t i = 0u; i < 990u; i++) {
            zeros[i] = 0x00u;
        }
        feed(&rig, head, 10u);
        expect("LEN=512 baslik (10 bayt): bekler", fwu_parser_poll(&rig.parser, 0u, &view), FWU_ERR_NOT_ENOUGH_DATA);
        feed(&rig, zeros, 990u);
        expect("ardindan 990 bayt sifir: hepsi islenir, cerceve yok", fwu_parser_poll(&rig.parser, 1u, &view), FWU_ERR_NOT_ENOUGH_DATA);
        check("... tampon bos, ring buffer bos", rig.parser.count == 0u && fwu_ringbuf_used_size(&rig.rb.base) == 0u);
        check("... tam 1000 bayt atildi (10 + 990), tampon siniri asilmadi", rig.parser.stats.bytes_dropped == 1000u);
        check("... bad_frame sayisi 2 (521 baytlik sahte cerceve + kalan 479 sifir)", rig.parser.stats.bad_frame == 2u);
    }

    /* 5) Yeni başın zaman penceresi: yeniden senkronizasyon sonrası kalan yarım çerçeve, KENDİ
     * başladığı andan sayılır (eski başın zamanından değil). */
    rig_init(&rig);
    {
        /* t=0: sahte çerçevenin ilk 11 baytı (EOF eksik). t=400: kalan EOF bayt (yanlış, 5B) +
         * gerçek çerçevenin yarım başı. Sahte çerçeve BAD_FRAME ile atılır; gerçek yarım baş t=400'de başlar. */
        const fwu_byte_t fake_part[11] = { 0xA5u, 0x01u, 0x02u, 0x07u, 0x00u, 0x03u, 0x11u, 0x22u, 0x33u, 0x44u, 0xABu };
        const fwu_byte_t at_400[6] = { 0x5Bu, 0xA5u, 0x01u, 0x02u, 0x07u, 0x00u };
        const fwu_byte_t at_700[7] = { 0x03u, 0x11u, 0x22u, 0x33u, 0x44u, 0xABu, 0x5Au };
        feed(&rig, fake_part, 11u);
        expect("t=0: sahte cerceve yarim", fwu_parser_poll(&rig.parser, 0u, &view), FWU_ERR_NOT_ENOUGH_DATA);
        feed(&rig, at_400, 6u);
        expect("t=400: sahte cerceve tamamlandi (EOF yanlis), atildi; gercek yarim bas kaldi", fwu_parser_poll(&rig.parser, 400u, &view), FWU_ERR_NOT_ENOUGH_DATA);
        check("... gercek yarim bas (5 bayt) tamponda", rig.parser.count == 5u);
        expect("t=600 (eski basa gore 600, yeni basa gore 200 ms): HALA bekler", fwu_parser_poll(&rig.parser, 600u, &view), FWU_ERR_NOT_ENOUGH_DATA);
        check("... zaman asimi olmadi", rig.parser.stats.timeouts == 0u && rig.parser.count == 5u);
        feed(&rig, at_700, 7u);
        expect("t=700: gercek cerceve tamamlanir", fwu_parser_poll(&rig.parser, 700u, &view), FWU_OK);
        check("... dogru cerceve", view_is_frame12(&view));
    }
}

/* Rastgele akış: çöp ve gerçek çerçeveler, rastgele parçalarla; modele karşı. */
typedef struct {
    fwu_byte_t type;
    fwu_byte_t seq;
    size_t     length;
    fwu_byte_t payload[100];
} expected_t;

static void test_random_stream(void)
{
    static rig_t rig;
    static expected_t expected[300];
    static fwu_byte_t stream[300 * 140];
    size_t stream_length = 0u;
    size_t expected_count = 0u;
    unsigned rng = 777u;
    size_t position = 0u;
    size_t delivered = 0u;
    int order_ok = 1;
    uint32_t now = 0u;

    printf("\n--- rastgele akis (300 cerceve + cop, rastgele parcalar) ---\n");

    for(unsigned f = 0u; f < 300u; f++) {
        /* 0..20 bayt çöp (SOF görünümlü baytlar da içerebilir) */
        unsigned junk;
        rng = (rng * 1103515245u) + 12345u;
        junk = (rng >> 16) % 21u;
        for(unsigned j = 0u; j < junk; j++) {
            rng = (rng * 1103515245u) + 12345u;
            stream[stream_length++] = ((rng >> 16) % 4u == 0u) ? (fwu_byte_t)0xA5u : (fwu_byte_t)((rng >> 20) & 0xFFu);
        }
        /* gerçek çerçeve: payload 1..100 */
        {
            expected_t *e = &expected[expected_count];
            fwu_byte_t frame[120];
            rng = (rng * 1103515245u) + 12345u;
            e->length = (size_t)(((rng >> 16) % 100u) + 1u);
            e->type = (fwu_byte_t)((rng >> 8) & 0xFFu);
            e->seq = (fwu_byte_t)(f & 0xFFu);   /* maske: C28x taklidinde 16 bitlik bayt 300'e kadar sayıyı kirli tutardı */
            for(size_t i = 0u; i < e->length; i++) {
                rng = (rng * 1103515245u) + 12345u;
                e->payload[i] = ((rng >> 16) % 5u == 0u) ? (fwu_byte_t)0xA5u : (fwu_byte_t)((rng >> 20) & 0xFFu);
            }
            {
                size_t n = build_frame(e->type, e->seq, e->payload, e->length, frame);
                copy_bytes(&stream[stream_length], frame, n);
                stream_length += n;
            }
            expected_count++;
        }
    }

    rig_init(&rig);
    while(position < stream_length) {
        size_t chunk;
        fwu_frame_view_t view;
        rng = (rng * 1103515245u) + 12345u;
        chunk = (size_t)(((rng >> 16) % 200u) + 1u);
        if(chunk > (stream_length - position)) {
            chunk = stream_length - position;
        }
        while(fwu_ringbuf_free_size(&rig.rb.base) < chunk) {
            /* ring buffer dolmasın: önce poll'la boşalt */
            while(fwu_parser_poll(&rig.parser, now, &view) == FWU_OK) {
                if((delivered >= expected_count) || (view.type != expected[delivered].type) || (view.seq != expected[delivered].seq) ||
                   (view.payload_length != expected[delivered].length) ||
                   (memcmp(view.payload, expected[delivered].payload, expected[delivered].length * sizeof(fwu_byte_t)) != 0)) {
                    order_ok = 0;
                }
                delivered++;
            }
            now += 1u;
        }
        feed(&rig, &stream[position], chunk);
        position += chunk;
        while(fwu_parser_poll(&rig.parser, now, &view) == FWU_OK) {
            if((delivered >= expected_count) || (view.type != expected[delivered].type) || (view.seq != expected[delivered].seq) ||
               (view.payload_length != expected[delivered].length) ||
               (memcmp(view.payload, expected[delivered].payload, expected[delivered].length * sizeof(fwu_byte_t)) != 0)) {
                order_ok = 0;
            }
            delivered++;
        }
        now += 1u;
    }
    /* Akış bitti: zamanı ileri sar, takılı kalmış sahte başlıklar zaman aşımıyla atılsın. */
    for(int k = 0; k < 20; k++) {
        fwu_frame_view_t view;
        now += 600u;
        while(fwu_parser_poll(&rig.parser, now, &view) == FWU_OK) {
            if((delivered >= expected_count) || (view.type != expected[delivered].type) || (view.seq != expected[delivered].seq) ||
               (view.payload_length != expected[delivered].length) ||
               (memcmp(view.payload, expected[delivered].payload, expected[delivered].length * sizeof(fwu_byte_t)) != 0)) {
                order_ok = 0;
            }
            delivered++;
        }
    }
    check("300 gercek cerceve, dogru sirada ve icerikte teslim edildi", order_ok && delivered == expected_count);
    printf("   (teslim: %u / %u, atilan bayt: %u, bad_frame %u, bad_length %u, crc %u, timeout %u)\n",
           (unsigned)delivered, (unsigned)expected_count, (unsigned)rig.parser.stats.bytes_dropped,
           (unsigned)rig.parser.stats.bad_frame, (unsigned)rig.parser.stats.bad_length,
           (unsigned)rig.parser.stats.crc_mismatch, (unsigned)rig.parser.stats.timeouts);
    check("hic sahte cerceve uretilmedi (frames_ok == 300)", rig.parser.stats.frames_ok == 300u);
}

/* ------------------------------------------------------------------ main */

int main(void)
{
    test_basic();
    test_resync();
    test_timeout();
    test_delivered_lifetime();
    test_drop_budget();
    test_args();
    test_state_and_edges();
    test_random_stream();

    printf("\nResult: %s (%d failed)\n", g_failures == 0 ? "ALL TESTS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
