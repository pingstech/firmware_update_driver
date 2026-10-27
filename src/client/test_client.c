/**
 * @file    test_client.c
 * @brief   fwu_client (host istemcisi) için kendi kendini kontrol eden test, üç katmanda:
 *   A. İZOLE MANTIK: cihaz yok; istemciye elle üretilmiş yanıtlar verilir (her durum, her hata kodu, SEQ/tür süzgeci,
 *      zaman aşımı sınırları, yeniden gönderim, iptal).
 *   B. GERÇEK CİHAZLA (kusursuz hat): istemci + linksim + fwu_link + fwu_session + aktivasyon + sahte flash; çerçeve sayısı
 *      ve süre elle türetildi; yetkilendirme, reddedilen paketler, bekleyen aktivasyon, devam, kayıp yanıt, iptal, kaybolan cihaz.
 *   C. KAYIPLI HAT TARAMALARI (G10d): düşen, tekrarlanan, bozulan, kesilen, geciken, çöp eklenen çerçevelerle YÜZLERCE tohum;
 *      ayrıca CAN üzerinden ISO-TP ile. Değişmezler: bitince uygulama ya tam eski ya tam yeni; DONE ise tam yeni; yarım commit yok;
 *      sahte imzalı/bozuk imaj hiçbir hatta commit edilmez.
 *
 * Derleme (client klasöründen):
 *   gcc -Wall -Wextra -Wpedantic -Wconversion -std=c99 -I. -I.. -I../error -I../bytes -I../crc -I../ring_buffer -I../frame -I../flash -I../journal -I../activation -I../manifest -I../crypto -I../verify -I../session -I../transport fwu_client.c ../transport/fwu_link.c ../transport/fwu_linksim.c ../transport/fwu_isotp.c ../session/fwu_session.c ../session/fwu_proto.c ../verify/fwu_verify.c ../manifest/fwu_manifest.c ../crypto/fwu_crypto.c ../crypto/fwu_crypto_sim.c ../activation/fwu_activate.c ../journal/fwu_journal_ab.c ../journal/fwu_journal.c ../flash/fwu_flash.c ../flash/fwu_flash_sim.c ../frame/fwu_frame.c ../frame/fwu_decode.c ../frame/fwu_parser.c ../ring_buffer/fwu_ringbuf.c test_client.c -o test_client && ./test_client
 */

#include "test_world.h"
#include "fwu_client.h"
#include "fwu_linksim.h"
#include "fwu_isotp.h"
#include <string.h>

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

FWU_RINGBUF_DEFINE(host_rb, 4096);
static host_rb_t g_host_rx;
static fwu_parser_t g_host_parser;

#define RESP_TIMEOUT   100u
#define MAX_RETRIES    8u
#define VERIFY_TIMEOUT 2000u
#define MAX_RESTARTS   2u

static void make_config(fwu_client_config_t *c)
{
    memset(c, 0, sizeof(*c));                       /* cekme modu alanlari (pull_target NULL = kapali) temiz baslar */
    c->manifest = tw_manifest;
    c->image = tw_image;
    c->image_size = TW_IMG_SIZE;
    c->auth = NULL;
    c->auth_context = NULL;
    c->expected_product = 0u;
    c->response_timeout_ms = RESP_TIMEOUT;
    c->max_retries = MAX_RETRIES;
    c->verify_timeout_ms = VERIFY_TIMEOUT;
    c->max_restarts = MAX_RESTARTS;
}

/* ================================================================== A. IZOLE MANTIK */

static fwu_client_t g_c;
static fwu_client_config_t g_cc;
static fwu_frame_view_t g_view;
static fwu_byte_t g_vbuf[FWU_FRAME_MAX_SIZE];
static uint32_t g_t;

/** Yanit cercevesini kurup istemciye verir (SEQ ve tur belirtilir). */
static void feed_seq(fwu_byte_t type, fwu_byte_t seq, const fwu_byte_t *payload, size_t length)
{
    size_t n = 0u;
    (void)fwu_frame_encode(type, seq, payload, length, g_vbuf, sizeof(g_vbuf), &n);
    (void)fwu_frame_decode(g_vbuf, n, &g_view);
    (void)fwu_client_on_frame(&g_c, &g_view, g_t);
}

/** Su an bekleyen istegin SEQ'iyle yanit verir. */
static void feed(fwu_byte_t type, const fwu_byte_t *payload, size_t length)
{
    feed_seq(type, g_c.req_seq, payload, length);
}

static void feed_info(fwu_byte_t version, fwu_byte_t flags, uint32_t product)
{
    fwu_proto_info_t info;
    fwu_byte_t p[FWU_PROTO_INFO_SIZE];
    size_t n = 0u;

    info.proto_version = version; info.flags = flags; info.product_id = product; info.running_image_version = 20u;
    info.max_chunk = TW_CHUNK; info.state = 0u;
    (void)fwu_proto_encode_info(&info, p, sizeof(p), &n);
    feed(FWU_MSG_INFO, p, n);
}

static void feed_accepted(uint16_t chunk, uint32_t next)
{
    fwu_proto_accepted_t a;
    fwu_byte_t p[FWU_PROTO_ACCEPTED_SIZE];
    size_t n = 0u;

    a.chunk_size = chunk; a.mode = 0u; a.timeout_s = 5u; a.next_offset = next;
    (void)fwu_proto_encode_accepted(&a, p, sizeof(p), &n);
    feed(FWU_MSG_DOWNLOAD_ACCEPTED, p, n);
}

static void feed_u32(fwu_byte_t type, uint32_t value)
{
    fwu_byte_t p[4];
    size_t n = 0u;
    (void)fwu_proto_encode_u32(value, p, sizeof(p), &n);
    feed(type, p, n);
}

static void feed_error(fwu_return_e code, fwu_byte_t request_type)
{
    fwu_byte_t p[2];
    size_t n = 0u;
    (void)fwu_proto_encode_error(code, request_type, p, sizeof(p), &n);
    feed(FWU_MSG_ERROR, p, n);
}

static void feed_byte(fwu_byte_t type, fwu_byte_t value)
{
    feed(type, &value, 1u);
}

/** poll: gonderilecek cerceve varsa coz ve (tur, seq) ver. Gonderilen bayt sayisini dondurur. */
static size_t poll_frame(fwu_byte_t *type, fwu_byte_t *seq, fwu_frame_view_t *view, fwu_byte_t *buf)
{
    size_t n = 0u;

    (void)fwu_client_poll(&g_c, g_t, buf, FWU_FRAME_MAX_SIZE, &n);
    if(n > 0u) {
        (void)fwu_frame_decode(buf, n, view);
        *type = view->type;
        *seq = view->seq;
    }
    return n;
}

static fwu_client_result_t result_of(const fwu_client_t *c)
{
    fwu_client_result_t r;
    (void)fwu_client_get_result(c, &r);
    return r;
}

static void fresh_client(void)
{
    tw_world(false, FWU_ACTIVATE_MODE_COPY);
    make_config(&g_cc);
    (void)fwu_client_init(&g_c, &g_cc);
    g_t = 1000u;
}

/** Istemciyi baslatir ve HELLO'yu gonderir (poll). */
static void started(void)
{
    fwu_byte_t buf[FWU_FRAME_MAX_SIZE];
    fwu_byte_t type;
    fwu_byte_t seq;
    fwu_frame_view_t v;

    fresh_client();
    (void)fwu_client_start(&g_c, g_t);
    (void)poll_frame(&type, &seq, &v, buf);
}

static fwu_return_e aut_ok(void *context, const fwu_byte_t *challenge, uint32_t challenge_length,
                           fwu_byte_t *response, uint32_t capacity, uint32_t *response_length)
{
    (void)context;
    if(capacity < challenge_length) {
        return FWU_ERR_BUFFER_TOO_SMALL;
    }
    tw_auth_response(challenge, challenge_length, response);
    *response_length = challenge_length;
    return FWU_OK;
}

static uint32_t g_auth_len_to_return;
static fwu_return_e aut_len(void *context, const fwu_byte_t *challenge, uint32_t challenge_length,
                            fwu_byte_t *response, uint32_t capacity, uint32_t *response_length)
{
    (void)context; (void)challenge; (void)challenge_length; (void)response; (void)capacity;
    *response_length = g_auth_len_to_return;
    return FWU_OK;
}

static fwu_return_e aut_fail(void *context, const fwu_byte_t *challenge, uint32_t challenge_length,
                             fwu_byte_t *response, uint32_t capacity, uint32_t *response_length)
{
    (void)context; (void)challenge; (void)challenge_length; (void)response; (void)capacity; (void)response_length;
    return FWU_ERR_CRYPTO_FAILURE;
}

static void unit_init_and_basic(void)
{
    fwu_client_config_t c;
    fwu_client_t cl;
    fwu_byte_t buf[FWU_FRAME_MAX_SIZE];
    size_t n = 99u;
    fwu_byte_t type = 0u;
    fwu_byte_t seq = 0u;
    fwu_frame_view_t v;

    printf("\n--- A1: init, baslatma, ilk cerceve ---\n");
    tw_world(false, FWU_ACTIVATE_MODE_COPY);
    make_config(&c);
    expect("gecerli yapilandirma", fwu_client_init(&cl, &c), FWU_OK);
    expect("client NULL", fwu_client_init(NULL, &c), FWU_ERR_INVALID_ARG);
    expect("config NULL", fwu_client_init(&cl, NULL), FWU_ERR_INVALID_ARG);
    c.manifest = NULL;
    expect("manifest NULL", fwu_client_init(&cl, &c), FWU_ERR_INVALID_ARG);
    make_config(&c); c.image = NULL;
    expect("imaj NULL", fwu_client_init(&cl, &c), FWU_ERR_INVALID_ARG);
    make_config(&c); c.image_size = 0u;
    expect("imaj boyutu 0", fwu_client_init(&cl, &c), FWU_ERR_INVALID_ARG);
    make_config(&c); c.image_size = TW_IMG_SIZE + 1u;
    expect("imaj boyutu manifest'tekiyle uyusmuyor (701)", fwu_client_init(&cl, &c), FWU_ERR_INVALID_ARG);
    make_config(&c); c.image_size = TW_IMG_SIZE - 1u;
    expect("imaj boyutu manifest'tekiyle uyusmuyor (699)", fwu_client_init(&cl, &c), FWU_ERR_INVALID_ARG);
    {
        fwu_byte_t bad[138];
        for(unsigned i = 0u; i < 138u; i++) {
            bad[i] = tw_manifest[i];
        }
        bad[0] = (fwu_byte_t)(bad[0] ^ 1u);
        make_config(&c); c.manifest = bad;
        expect("manifest yapisi bozuk (magic)", fwu_client_init(&cl, &c), FWU_ERR_INVALID_ARG);
    }
    make_config(&c); c.response_timeout_ms = 0u;
    expect("yanit zaman asimi 0", fwu_client_init(&cl, &c), FWU_ERR_INVALID_ARG);
    make_config(&c); c.response_timeout_ms = 1u; c.verify_timeout_ms = 1u;
    expect("zaman asimlari 1 ve 1 (dogrulama = yanit, sinir)", fwu_client_init(&cl, &c), FWU_OK);
    make_config(&c); c.verify_timeout_ms = RESP_TIMEOUT - 1u;
    expect("dogrulama beklemesi < yanit zaman asimi", fwu_client_init(&cl, &c), FWU_ERR_INVALID_ARG);
    make_config(&c); c.max_retries = 0u; c.max_restarts = 0u;
    expect("max_retries 0 ve max_restarts 0 serbest", fwu_client_init(&cl, &c), FWU_OK);

    make_config(&c);
    (void)fwu_client_init(&cl, &c);
    check("baslamadan: durum IDLE, bitmedi", fwu_client_get_state(&cl) == FWU_CLIENT_IDLE && !fwu_client_is_finished(&cl));
    expect("poll (baslamadan)", fwu_client_poll(&cl, 0u, buf, sizeof(buf), &n), FWU_OK);
    check("... gonderilecek cerceve yok (out_length 0)", n == 0u);
    expect("poll: tampon < en buyuk cerceve", fwu_client_poll(&cl, 0u, buf, FWU_FRAME_MAX_SIZE - 1u, &n), FWU_ERR_BUFFER_TOO_SMALL);
    expect("poll: client NULL", fwu_client_poll(NULL, 0u, buf, sizeof(buf), &n), FWU_ERR_INVALID_ARG);
    expect("poll: out NULL", fwu_client_poll(&cl, 0u, NULL, sizeof(buf), &n), FWU_ERR_INVALID_ARG);
    expect("poll: out_length NULL", fwu_client_poll(&cl, 0u, buf, sizeof(buf), NULL), FWU_ERR_INVALID_ARG);
    expect("start: client NULL", fwu_client_start(NULL, 0u), FWU_ERR_INVALID_ARG);
    expect("cancel: baslamadan", fwu_client_cancel(&cl), FWU_ERR_BAD_STATE);
    expect("on_frame: client NULL", fwu_client_on_frame(NULL, &g_view, 0u), FWU_ERR_INVALID_ARG);
    expect("on_frame: yanit NULL", fwu_client_on_frame(&cl, NULL, 0u), FWU_ERR_INVALID_ARG);
    expect("get_result: NULL", fwu_client_get_result(NULL, &(fwu_client_result_t){ FWU_CLIENT_IDLE, FWU_CLIENT_FAIL_NONE, FWU_OK, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u }), FWU_ERR_INVALID_ARG);
    check("sorgular NULL'a dayanikli", !fwu_client_is_finished(NULL) && fwu_client_get_state(NULL) == FWU_CLIENT_IDLE);
    expect("start", fwu_client_start(&cl, 0u), FWU_OK);
    expect("ikinci start: BAD_STATE", fwu_client_start(&cl, 0u), FWU_ERR_BAD_STATE);
    check("... durum HELLO", fwu_client_get_state(&cl) == FWU_CLIENT_HELLO);
    g_c = cl;
    g_t = 5000u;
    n = poll_frame(&type, &seq, &v, buf);
    check("ilk cerceve HELLO: tur 0x01, SEQ 1, yuk [protokol surumu = 1]", n > 0u && type == FWU_MSG_HELLO && seq == 1u && v.payload_length == 1u && v.payload[0] == 1u);
    check("... 12 bayt cerceve degil: yuk 1 bayt, toplam 10", n == 10u);
    n = poll_frame(&type, &seq, &v, buf);
    check("hemen tekrar poll: yeni cerceve yok (yanit bekleniyor)", n == 0u);
}

static void unit_hello(void)
{
    printf("\n--- A2: HELLO yaniti ---\n");
    started();
    feed_info(1u, 0x02u, TW_PRODUCT);
    check("INFO (yetkilendirme yok): REQUEST_DOWNLOAD'a gecti", fwu_client_get_state(&g_c) == FWU_CLIENT_REQUEST && g_c.req_type == FWU_MSG_REQUEST_DOWNLOAD);
    check("... SEQ 2 (yeni istek, yeni SEQ)", g_c.req_seq == 2u);

    started();
    feed_info(2u, 0x02u, TW_PRODUCT);
    check("protokol surumu 2: FAILED(PROTOCOL)", result_of(&g_c).state == FWU_CLIENT_FAILED && result_of(&g_c).failure == FWU_CLIENT_FAIL_PROTOCOL);
    started();
    feed_info(0u, 0x02u, TW_PRODUCT);
    check("protokol surumu 0: FAILED(PROTOCOL)", result_of(&g_c).failure == FWU_CLIENT_FAIL_PROTOCOL);
    fresh_client();
    g_cc.expected_product = 777u;
    (void)fwu_client_init(&g_c, &g_cc);
    (void)fwu_client_start(&g_c, g_t);
    feed_info(1u, 0x02u, TW_PRODUCT);
    check("beklenen urun 777, cihaz 12345: FAILED(PROTOCOL)", result_of(&g_c).failure == FWU_CLIENT_FAIL_PROTOCOL);
    fresh_client();
    g_cc.expected_product = TW_PRODUCT;
    (void)fwu_client_init(&g_c, &g_cc);
    (void)fwu_client_start(&g_c, g_t);
    feed_info(1u, 0x02u, TW_PRODUCT);
    check("beklenen urun eslesiyor: devam", fwu_client_get_state(&g_c) == FWU_CLIENT_REQUEST);
    started();
    feed(FWU_MSG_INFO, (const fwu_byte_t[5]){ 1u, 2u, 3u, 4u, 5u }, 5u);
    check("INFO yuku 5 bayt (13 gerekir): FAILED(PROTOCOL)", result_of(&g_c).failure == FWU_CLIENT_FAIL_PROTOCOL);

    /* yetkilendirme zorunlu */
    started();
    feed_info(1u, 0x03u, TW_PRODUCT);
    check("yetkilendirme zorunlu, istemcide kanca YOK: FAILED(AUTH_UNAVAILABLE)", result_of(&g_c).failure == FWU_CLIENT_FAIL_AUTH_UNAVAILABLE);
    fresh_client();
    g_cc.auth = aut_ok;
    (void)fwu_client_init(&g_c, &g_cc);
    (void)fwu_client_start(&g_c, g_t);
    feed_info(1u, 0x03u, TW_PRODUCT);
    check("yetkilendirme zorunlu, kanca var: AUTH_REQ gonderilir", fwu_client_get_state(&g_c) == FWU_CLIENT_AUTH_REQ && g_c.req_type == FWU_MSG_AUTH_REQ);
    {
        fwu_byte_t ch[16];
        for(unsigned i = 0u; i < 16u; i++) {
            ch[i] = (fwu_byte_t)(i + 1u);
        }
        feed(FWU_MSG_AUTH_CHALLENGE, ch, 16u);
        check("AUTH_CHALLENGE: kanca yaniti uretildi, AUTH_RESP gonderilir", fwu_client_get_state(&g_c) == FWU_CLIENT_AUTH_RESP && g_c.req_type == FWU_MSG_AUTH_RESP);
        {
            fwu_frame_view_t v;
            (void)fwu_frame_decode(g_c.req_frame, g_c.req_length, &v);
            check("... yanit = meydan okuma XOR 0x5A (16 bayt)", v.payload_length == 16u && v.payload[0] == (fwu_byte_t)(1u ^ 0x5Au) && v.payload[15] == (fwu_byte_t)(16u ^ 0x5Au));
        }
        feed_byte(FWU_MSG_AUTH_OK, 0u);
        check("AUTH_OK: REQUEST_DOWNLOAD", fwu_client_get_state(&g_c) == FWU_CLIENT_REQUEST);
    }
    fresh_client();
    g_cc.auth = aut_fail;
    (void)fwu_client_init(&g_c, &g_cc);
    (void)fwu_client_start(&g_c, g_t);
    feed_info(1u, 0x03u, TW_PRODUCT);
    feed(FWU_MSG_AUTH_CHALLENGE, (const fwu_byte_t[4]){ 1u, 2u, 3u, 4u }, 4u);
    check("kanca basarisiz: FAILED(AUTH_UNAVAILABLE)", result_of(&g_c).failure == FWU_CLIENT_FAIL_AUTH_UNAVAILABLE);
    {
        const uint32_t lens[] = { 0u, 129u };
        for(unsigned k = 0u; k < 2u; k++) {
            fresh_client();
            g_cc.auth = aut_len;
            g_auth_len_to_return = lens[k];
            (void)fwu_client_init(&g_c, &g_cc);
            (void)fwu_client_start(&g_c, g_t);
            feed_info(1u, 0x03u, TW_PRODUCT);
            feed(FWU_MSG_AUTH_CHALLENGE, (const fwu_byte_t[4]){ 1u, 2u, 3u, 4u }, 4u);
            check(k == 0u ? "kanca yanit uzunlugu 0: FAILED(AUTH_UNAVAILABLE)" : "kanca yanit uzunlugu 129: FAILED(AUTH_UNAVAILABLE)",
                  result_of(&g_c).failure == FWU_CLIENT_FAIL_AUTH_UNAVAILABLE);
        }
        fresh_client();
        g_cc.auth = aut_len;
        g_auth_len_to_return = 128u;
        (void)fwu_client_init(&g_c, &g_cc);
        (void)fwu_client_start(&g_c, g_t);
        feed_info(1u, 0x03u, TW_PRODUCT);
        feed(FWU_MSG_AUTH_CHALLENGE, (const fwu_byte_t[4]){ 1u, 2u, 3u, 4u }, 4u);
        check("kanca yanit uzunlugu 128 (sinir): AUTH_RESP gonderilir", fwu_client_get_state(&g_c) == FWU_CLIENT_AUTH_RESP);
    }
    {
        fwu_byte_t big[65];
        for(unsigned i = 0u; i < 65u; i++) {
            big[i] = (fwu_byte_t)i;
        }
        fresh_client();
        g_cc.auth = aut_ok;
        (void)fwu_client_init(&g_c, &g_cc);
        (void)fwu_client_start(&g_c, g_t);
        feed_info(1u, 0x03u, TW_PRODUCT);
        feed(FWU_MSG_AUTH_CHALLENGE, big, 65u);
        check("meydan okuma 65 bayt (> 64): FAILED(PROTOCOL)", result_of(&g_c).failure == FWU_CLIENT_FAIL_PROTOCOL);
        fresh_client();
        g_cc.auth = aut_ok;
        (void)fwu_client_init(&g_c, &g_cc);
        (void)fwu_client_start(&g_c, g_t);
        feed_info(1u, 0x03u, TW_PRODUCT);
        feed(FWU_MSG_AUTH_CHALLENGE, big, 64u);
        check("meydan okuma 64 bayt (sinir): kabul", fwu_client_get_state(&g_c) == FWU_CLIENT_AUTH_RESP);
    }
}

static void to_request(void)
{
    started();
    feed_info(1u, 0x02u, TW_PRODUCT);
}

static void to_transfer(uint32_t next)
{
    to_request();
    feed_accepted(TW_CHUNK, next);
}

static void unit_request_transfer(void)
{
    fwu_byte_t buf[FWU_FRAME_MAX_SIZE];
    fwu_byte_t type;
    fwu_byte_t seq;
    fwu_frame_view_t v;
    size_t n;

    printf("\n--- A3: REQUEST_DOWNLOAD ve TRANSFER ---\n");
    to_request();
    n = poll_frame(&type, &seq, &v, buf);
    check("REQUEST_DOWNLOAD cercevesi: tur 0x10, yuk = 138 bayt manifest (birebir)", n > 0u && type == FWU_MSG_REQUEST_DOWNLOAD && v.payload_length == 138u &&
          v.payload[0] == tw_manifest[0] && v.payload[137] == tw_manifest[137] && v.payload[74] == tw_manifest[74]);
    feed_accepted(TW_CHUNK, 0u);
    check("ACCEPTED (parca 64, ofset 0): TRANSFER, ilk parca ofset 0", fwu_client_get_state(&g_c) == FWU_CLIENT_TRANSFER && g_c.last_offset == 0u && g_c.last_length == 64u);
    n = poll_frame(&type, &seq, &v, buf);
    {
        fwu_proto_data_t d;
        check("ilk TRANSFER_DATA: ofset 0, 64 bayt, imajin ilk 64 baytiyla ayni",
              n > 0u && type == FWU_MSG_TRANSFER_DATA && (fwu_proto_parse_data(v.payload, v.payload_length, &d) == FWU_OK) && d.offset == 0u && d.data_length == 64u &&
              d.data[0] == tw_image[0] && d.data[63] == tw_image[63]);
    }
    feed_u32(FWU_MSG_DATA_ACK, 64u);
    check("DATA_ACK 64: acked 64, sonraki parca ofset 64", g_c.acked == 64u && g_c.last_offset == 64u && g_c.last_length == 64u);

    /* yeniden eslesme: cihaz daha eski bir ofset bildirir */
    feed_u32(FWU_MSG_DATA_ACK, 64u);                         /* beklenen 128, gelen 64 */
    check("DATA_ACK beklenenden farkli (64, beklenen 128): cihazin ofsetine yeniden eslesme (resyncs 1), parca ofset 64", result_of(&g_c).resyncs == 1u && g_c.last_offset == 64u);
    feed_u32(FWU_MSG_DATA_ACK, 640u);                        /* beklenen 128, gelen 640 (cihaz ilerde) */
    check("DATA_ACK ileri ofset (640): yeniden eslesme (resyncs 2), parca ofset 640, SON parca 60 bayt", result_of(&g_c).resyncs == 2u && g_c.last_offset == 640u && g_c.last_length == 60u);
    feed_u32(FWU_MSG_DATA_ACK, TW_IMG_SIZE);
    check("DATA_ACK 700 (imaj sonu): EXIT gonderilir", fwu_client_get_state(&g_c) == FWU_CLIENT_EXIT && g_c.req_type == FWU_MSG_TRANSFER_EXIT);
    n = poll_frame(&type, &seq, &v, buf);
    {
        uint32_t size = 0u;
        check("EXIT yuku = imaj boyutu 700", n > 0u && (fwu_proto_parse_u32(v.payload, v.payload_length, &size) == FWU_OK) && size == 700u);
    }

    /* gecersiz ACK */
    to_transfer(0u);
    feed_u32(FWU_MSG_DATA_ACK, TW_IMG_SIZE + 1u);
    check("DATA_ACK > imaj boyutu (701): FAILED(PROTOCOL)", result_of(&g_c).failure == FWU_CLIENT_FAIL_PROTOCOL);
    to_transfer(0u);
    feed(FWU_MSG_DATA_ACK, (const fwu_byte_t[3]){ 0u, 0u, 64u }, 3u);
    check("DATA_ACK yuku 3 bayt: FAILED(PROTOCOL)", result_of(&g_c).failure == FWU_CLIENT_FAIL_PROTOCOL);
    to_transfer(0u);
    feed_u32(FWU_MSG_DATA_ACK, TW_IMG_SIZE);
    check("DATA_ACK tam imaj boyutu (700): sinirda gecerli, EXIT", fwu_client_get_state(&g_c) == FWU_CLIENT_EXIT);

    /* ACCEPTED dogrulamasi */
    to_request();
    feed_accepted(0u, 0u);
    check("ACCEPTED parca 0: FAILED(PROTOCOL)", result_of(&g_c).failure == FWU_CLIENT_FAIL_PROTOCOL);
    to_request();
    feed_accepted(509u, 0u);
    check("ACCEPTED parca 509 (> 508): FAILED(PROTOCOL)", result_of(&g_c).failure == FWU_CLIENT_FAIL_PROTOCOL);
    to_request();
    feed_accepted(508u, 0u);
    check("ACCEPTED parca 508 (sinir): kabul, ilk parca 508 bayt", fwu_client_get_state(&g_c) == FWU_CLIENT_TRANSFER && g_c.last_length == 508u);
    to_request();
    feed_accepted(1u, 0u);
    check("ACCEPTED parca 1 (sinir): kabul, ilk parca 1 bayt", fwu_client_get_state(&g_c) == FWU_CLIENT_TRANSFER && g_c.last_length == 1u);
    to_request();
    feed_accepted(TW_CHUNK, TW_IMG_SIZE + 1u);
    check("ACCEPTED sonraki ofset > boyut (701): FAILED(PROTOCOL)", result_of(&g_c).failure == FWU_CLIENT_FAIL_PROTOCOL);
    to_request();
    feed_accepted(TW_CHUNK, TW_IMG_SIZE);
    check("ACCEPTED sonraki ofset = boyut (700): dogrudan EXIT (indirme zaten tamam)", fwu_client_get_state(&g_c) == FWU_CLIENT_EXIT);
    to_request();
    feed(FWU_MSG_DOWNLOAD_ACCEPTED, (const fwu_byte_t[3]){ 1u, 2u, 3u }, 3u);
    check("ACCEPTED yuku 3 bayt: FAILED(PROTOCOL)", result_of(&g_c).failure == FWU_CLIENT_FAIL_PROTOCOL);
    to_transfer(320u);
    check("ACCEPTED sonraki ofset 320 (devam): ilk parca ofset 320 (cihazin ofseti esastir)", result_of(&g_c).acked_bytes == 320u && g_c.last_offset == 320u);
}

static void to_exit(void)
{
    to_transfer(TW_IMG_SIZE);                                /* dogrudan EXIT durumu */
}

static void unit_verify_activate(void)
{
    fwu_byte_t buf[FWU_FRAME_MAX_SIZE];
    fwu_byte_t type;
    fwu_byte_t seq;
    fwu_frame_view_t v;
    fwu_byte_t first[FWU_FRAME_MAX_SIZE];
    size_t first_length;
    uint32_t t_exit;

    printf("\n--- A4: EXIT, dogrulama, ACTIVATE ---\n");
    to_exit();
    check("EXIT gonderilir", g_c.req_type == FWU_MSG_TRANSFER_EXIT);
    feed_byte(FWU_MSG_EXIT_PENDING, 1u);
    check("EXIT_PENDING: VERIFY_WAIT", fwu_client_get_state(&g_c) == FWU_CLIENT_VERIFY_WAIT);
    feed_byte(FWU_MSG_VERIFY_RESULT, 0u);
    check("VERIFY_RESULT(0): ACTIVATE gonderilir", fwu_client_get_state(&g_c) == FWU_CLIENT_ACTIVATE && g_c.req_type == FWU_MSG_ACTIVATE);
    (void)poll_frame(&type, &seq, &v, buf);
    {
        uint32_t size = 0u;
        check("ACTIVATE yuku = imaj boyutu 700", type == FWU_MSG_ACTIVATE && (fwu_proto_parse_u32(v.payload, v.payload_length, &size) == FWU_OK) && size == 700u);
    }
    feed_byte(FWU_MSG_ACTIVATE_OK, 0u);
    check("ACTIVATE_OK: DONE, bitti", fwu_client_get_state(&g_c) == FWU_CLIENT_DONE && fwu_client_is_finished(&g_c) && result_of(&g_c).failure == FWU_CLIENT_FAIL_NONE);
    {
        const uint32_t ignored = result_of(&g_c).ignored;
        feed_byte(FWU_MSG_ACTIVATE_OK, 0u);
        check("bittikten sonra gelen yanit: yok sayilir, durum DONE", result_of(&g_c).ignored == ignored + 1u && fwu_client_get_state(&g_c) == FWU_CLIENT_DONE);
    }

    to_exit();
    feed_byte(FWU_MSG_VERIFY_RESULT, 0u);
    check("EXIT'e dogrudan VERIFY_RESULT(0) (tekrar yaniti, EXIT_PENDING kayip): ACTIVATE", fwu_client_get_state(&g_c) == FWU_CLIENT_ACTIVATE);
    to_exit();
    feed_byte(FWU_MSG_EXIT_PENDING, 1u);
    feed_byte(FWU_MSG_VERIFY_RESULT, (fwu_byte_t)FWU_ERR_HASH_MISMATCH);
    check("VERIFY_RESULT(35): FAILED(REJECTED), kod 35", result_of(&g_c).failure == FWU_CLIENT_FAIL_REJECTED && result_of(&g_c).verify_code == 35u);
    to_exit();
    feed_byte(FWU_MSG_VERIFY_RESULT, 255u);
    check("VERIFY_RESULT(255): FAILED(REJECTED), kod 255", result_of(&g_c).failure == FWU_CLIENT_FAIL_REJECTED && result_of(&g_c).verify_code == 255u);
    to_exit();
    feed(FWU_MSG_VERIFY_RESULT, (const fwu_byte_t[2]){ 0u, 0u }, 2u);
    check("VERIFY_RESULT yuku 2 bayt: FAILED(PROTOCOL)", result_of(&g_c).failure == FWU_CLIENT_FAIL_PROTOCOL);
    to_exit();
    feed_byte(FWU_MSG_EXIT_PENDING, 1u);
    feed_byte(FWU_MSG_EXIT_PENDING, 1u);
    check("VERIFY_WAIT'te ikinci EXIT_PENDING: yok sayilir, VERIFY_WAIT", fwu_client_get_state(&g_c) == FWU_CLIENT_VERIFY_WAIT);

    /* dogrulama beklerken zaman asimi: EXIT TEKRARLANIR, toplam bekleme verify_timeout ile sinirli */
    to_exit();
    g_t = 10000u;
    (void)poll_frame(&type, &seq, &v, buf);
    first_length = 0u;
    {
        fwu_byte_t tmp[FWU_FRAME_MAX_SIZE];
        size_t k = poll_frame(&type, &seq, &v, tmp);
        (void)k;
    }
    to_exit();
    g_t = 10000u;
    first_length = poll_frame(&type, &seq, &v, first);
    t_exit = g_t;
    check("EXIT ilk gonderim", first_length > 0u && type == FWU_MSG_TRANSFER_EXIT);
    feed_byte(FWU_MSG_EXIT_PENDING, 1u);                      /* VERIFY_WAIT, verify_start = 10000 */
    g_t = t_exit + RESP_TIMEOUT - 1u;
    check("VERIFY_WAIT: yanit zaman asimindan 1 ms once: tekrar yok", poll_frame(&type, &seq, &v, buf) == 0u);
    g_t = t_exit + RESP_TIMEOUT;
    {
        const size_t n = poll_frame(&type, &seq, &v, buf);
        int same = (n == first_length);
        for(size_t i = 0u; same && (i < n); i++) {
            same = (buf[i] == first[i]);
        }
        check("zaman asimi dolunca EXIT AYNI baytlarla (ayni SEQ) tekrarlanir: sonucu sorar", same && type == FWU_MSG_TRANSFER_EXIT && result_of(&g_c).retransmits == 1u);
    }
    g_t = t_exit + VERIFY_TIMEOUT - 1u;
    (void)poll_frame(&type, &seq, &v, buf);
    check("toplam bekleme 1999 ms: hala VERIFY_WAIT (EXIT tekrarlari retry sinirina takilmaz)", fwu_client_get_state(&g_c) == FWU_CLIENT_VERIFY_WAIT);
    check("... 8'den fazla tekrar yapilabildi (retry siniri bekleme icin gecerli degil)", result_of(&g_c).retransmits >= 1u);
    g_t = t_exit + VERIFY_TIMEOUT + RESP_TIMEOUT;
    (void)poll_frame(&type, &seq, &v, buf);
    check("toplam bekleme dolunca: FAILED(TIMEOUT)", result_of(&g_c).state == FWU_CLIENT_FAILED && result_of(&g_c).failure == FWU_CLIENT_FAIL_TIMEOUT);

    /* VERIFY_WAIT sirasinda cihazin asenkron sonucu */
    to_exit();
    g_t = 20000u;
    (void)poll_frame(&type, &seq, &v, buf);
    feed_byte(FWU_MSG_EXIT_PENDING, 1u);
    g_t = 20000u + 1500u;
    feed_byte(FWU_MSG_VERIFY_RESULT, 0u);
    check("VERIFY_WAIT: 1500 ms sonra sonuc geldi (VERIFY_RESULT 0): ACTIVATE", fwu_client_get_state(&g_c) == FWU_CLIENT_ACTIVATE);
}

static void unit_errors_filters_timeouts(void)
{
    fwu_byte_t buf[FWU_FRAME_MAX_SIZE];
    fwu_byte_t type;
    fwu_byte_t seq;
    fwu_frame_view_t v;
    fwu_byte_t first[FWU_FRAME_MAX_SIZE];
    size_t first_length;

    printf("\n--- A5: ERROR yanitlari, SEQ/tur suzgeci, zaman asimi ve yeniden gonderim ---\n");

    /* ERROR: BAD_BLOCK TRANSFER'de -> REQUEST_DOWNLOAD (yeniden eslesme) */
    to_transfer(0u);
    feed_error(FWU_ERR_BAD_BLOCK, FWU_MSG_TRANSFER_DATA);
    check("TRANSFER'de ERROR(BAD_BLOCK): REQUEST_DOWNLOAD tekrarlanir (resyncs 1), kaldigi ofset ogrenilir",
          fwu_client_get_state(&g_c) == FWU_CLIENT_REQUEST && result_of(&g_c).resyncs == 1u && g_c.req_type == FWU_MSG_REQUEST_DOWNLOAD);
    feed_accepted(TW_CHUNK, 192u);
    check("... ACCEPTED sonraki ofset 192: oradan devam", g_c.last_offset == 192u);
    to_exit();
    feed_error(FWU_ERR_BAD_BLOCK, FWU_MSG_TRANSFER_EXIT);
    check("EXIT'te ERROR(BAD_BLOCK) (imaj eksik): REQUEST_DOWNLOAD ile yeniden eslesme", fwu_client_get_state(&g_c) == FWU_CLIENT_REQUEST && result_of(&g_c).resyncs == 1u);
    to_request();
    feed_error(FWU_ERR_BAD_BLOCK, FWU_MSG_REQUEST_DOWNLOAD);
    check("REQUEST'te ERROR(BAD_BLOCK): kesin, FAILED(DEVICE_ERROR, 61)", result_of(&g_c).failure == FWU_CLIENT_FAIL_DEVICE_ERROR && result_of(&g_c).device_error == FWU_ERR_BAD_BLOCK);

    /* ERROR: BAD_STATE -> baştan başla (sinirli) */
    to_transfer(0u);
    feed_error(FWU_ERR_BAD_STATE, FWU_MSG_TRANSFER_DATA);
    check("TRANSFER'de ERROR(BAD_STATE): oturum dusmus, bastan HELLO (restarts 1), ilerleme sifirlandi",
          fwu_client_get_state(&g_c) == FWU_CLIENT_HELLO && result_of(&g_c).restarts == 1u && result_of(&g_c).acked_bytes == 0u);
    feed_info(1u, 0x02u, TW_PRODUCT);
    feed_accepted(TW_CHUNK, 0u);
    feed_error(FWU_ERR_BAD_STATE, FWU_MSG_TRANSFER_DATA);
    check("... ikinci baştan başlama (restarts 2)", fwu_client_get_state(&g_c) == FWU_CLIENT_HELLO && result_of(&g_c).restarts == 2u);
    feed_info(1u, 0x02u, TW_PRODUCT);
    feed_accepted(TW_CHUNK, 0u);
    feed_error(FWU_ERR_BAD_STATE, FWU_MSG_TRANSFER_DATA);
    check("... uc kez: hak bitti (max_restarts 2), FAILED(DEVICE_ERROR, BAD_STATE)", result_of(&g_c).failure == FWU_CLIENT_FAIL_DEVICE_ERROR && result_of(&g_c).device_error == FWU_ERR_BAD_STATE && result_of(&g_c).restarts == 2u);
    to_request();
    feed_error(FWU_ERR_BAD_STATE, FWU_MSG_REQUEST_DOWNLOAD);
    check("REQUEST'te ERROR(BAD_STATE): baştan başla (HELLO)", fwu_client_get_state(&g_c) == FWU_CLIENT_HELLO && result_of(&g_c).restarts == 1u);
    started();
    feed_error(FWU_ERR_BAD_STATE, FWU_MSG_HELLO);
    check("HELLO'da ERROR(BAD_STATE): kesin, FAILED(DEVICE_ERROR, BAD_STATE) (baştan başlamanin anlami yok)", result_of(&g_c).failure == FWU_CLIENT_FAIL_DEVICE_ERROR && result_of(&g_c).restarts == 0u);
    fresh_client();
    g_cc.max_restarts = 0u;
    (void)fwu_client_init(&g_c, &g_cc);
    (void)fwu_client_start(&g_c, g_t);
    feed_info(1u, 0x02u, TW_PRODUCT);
    feed_error(FWU_ERR_BAD_STATE, FWU_MSG_REQUEST_DOWNLOAD);
    check("max_restarts 0: BAD_STATE hemen FAILED(DEVICE_ERROR)", result_of(&g_c).failure == FWU_CLIENT_FAIL_DEVICE_ERROR && result_of(&g_c).restarts == 0u);

    /* kesin hatalar */
    {
        static const fwu_return_e codes[] = { FWU_ERR_BAD_SIGNATURE, FWU_ERR_PRODUCT_MISMATCH, FWU_ERR_VERSION_REJECTED, FWU_ERR_KEY_REJECTED,
                                              FWU_ERR_IMAGE_TOO_LARGE, FWU_ERR_BAD_MANIFEST, FWU_ERR_LOCKED_OUT, FWU_ERR_UNKNOWN_MESSAGE,
                                              FWU_ERR_BAD_LENGTH, FWU_ERR_FLASH_WRITE, FWU_ERR_FLASH_ERASE, FWU_ERR_CRYPTO_FAILURE };
        int ok = 1;
        for(unsigned k = 0u; k < sizeof(codes) / sizeof(codes[0]); k++) {
            to_request();
            feed_error(codes[k], FWU_MSG_REQUEST_DOWNLOAD);
            ok = ok && (result_of(&g_c).failure == FWU_CLIENT_FAIL_DEVICE_ERROR) && (result_of(&g_c).device_error == codes[k]) && (result_of(&g_c).restarts == 0u);
        }
        check("kesin hatalar (50, 52, 53, 54, 34, 33, 62, 60, 31, 40, 47, 51): FAILED(DEVICE_ERROR, kod), yeniden denenmez", ok);
    }
    to_request();
    feed(FWU_MSG_ERROR, (const fwu_byte_t[1]){ 12u }, 1u);
    check("ERROR yuku 1 bayt (2 gerekir): FAILED(PROTOCOL)", result_of(&g_c).failure == FWU_CLIENT_FAIL_PROTOCOL);

    /* SEQ / tur suzgeci */
    to_request();
    {
        const uint32_t ig = result_of(&g_c).ignored;
        const fwu_byte_t wrong = (fwu_byte_t)(g_c.req_seq + 1u);
        fwu_proto_accepted_t a = { TW_CHUNK, 0u, 5u, 0u };
        fwu_byte_t p[FWU_PROTO_ACCEPTED_SIZE];
        size_t n = 0u;
        (void)fwu_proto_encode_accepted(&a, p, sizeof(p), &n);
        feed_seq(FWU_MSG_DOWNLOAD_ACCEPTED, wrong, p, n);
        check("yanit SEQ'i eslesmiyor: yok sayilir, durum REQUEST", result_of(&g_c).ignored == ig + 1u && fwu_client_get_state(&g_c) == FWU_CLIENT_REQUEST);
        feed_seq(FWU_MSG_DOWNLOAD_ACCEPTED, (fwu_byte_t)(g_c.req_seq - 1u), p, n);
        check("onceki istegin SEQ'i (eski yanit): yok sayilir", result_of(&g_c).ignored == ig + 2u && fwu_client_get_state(&g_c) == FWU_CLIENT_REQUEST);
        feed_u32(FWU_MSG_DATA_ACK, 64u);
        check("tur eslesmiyor (REQUEST beklerken DATA_ACK): yok sayilir", result_of(&g_c).ignored == ig + 3u && fwu_client_get_state(&g_c) == FWU_CLIENT_REQUEST);
        feed(FWU_MSG_INFO, p, n);
        check("tur eslesmiyor (INFO): yok sayilir", result_of(&g_c).ignored == ig + 4u);
        feed_seq(FWU_MSG_ERROR, wrong, (const fwu_byte_t[2]){ 50u, 0x10u }, 2u);
        check("ERROR bile yanlis SEQ ile gelirse yok sayilir (durum REQUEST kalir)", result_of(&g_c).ignored == ig + 5u && fwu_client_get_state(&g_c) == FWU_CLIENT_REQUEST);
        feed_seq(FWU_MSG_VERIFY_RESULT, g_c.req_seq, (const fwu_byte_t[1]){ 0u }, 1u);
        check("VERIFY_RESULT yalniz EXIT beklerken kabul: REQUEST'te yok sayilir", result_of(&g_c).ignored == ig + 6u);
    }
    {
        /* yeni istek yeni SEQ alir, SEQ 255'ten 0'a sarar */
        fwu_client_t cl;
        int wrap_ok = 1;
        fresh_client();
        g_cc.max_restarts = 1000u;
        (void)fwu_client_init(&cl, &g_cc);
        cl.next_seq = 254u;
        g_c = cl;
        (void)fwu_client_start(&g_c, g_t);
        wrap_ok = wrap_ok && (g_c.req_seq == 254u);
        feed_info(1u, 0x02u, TW_PRODUCT);
        wrap_ok = wrap_ok && (g_c.req_seq == 255u);
        feed_accepted(TW_CHUNK, 0u);
        wrap_ok = wrap_ok && (g_c.req_seq == 0u);
        feed_u32(FWU_MSG_DATA_ACK, 64u);
        wrap_ok = wrap_ok && (g_c.req_seq == 1u);
        check("SEQ 254, 255, 0, 1: her yeni istek bir artar ve 255'ten sonra 0'a sarar", wrap_ok);
    }

    /* zaman asimi ve yeniden gonderim */
    fresh_client();
    g_t = 7000u;
    (void)fwu_client_start(&g_c, g_t);
    first_length = poll_frame(&type, &seq, &v, first);
    check("HELLO ilk gonderim (frames_sent 1)", first_length > 0u && result_of(&g_c).frames_sent == 1u);
    g_t = 7000u + RESP_TIMEOUT - 1u;
    check("zaman asimindan 1 ms once: gonderim yok", poll_frame(&type, &seq, &v, buf) == 0u && result_of(&g_c).retransmits == 0u);
    g_t = 7000u + RESP_TIMEOUT;
    {
        const size_t n = poll_frame(&type, &seq, &v, buf);
        int same = (n == first_length);
        for(size_t i = 0u; same && (i < n); i++) {
            same = (buf[i] == first[i]);
        }
        check("tam zaman asiminda: AYNI baytlar (ayni SEQ) tekrarlanir, retransmits 1", same && result_of(&g_c).retransmits == 1u && result_of(&g_c).frames_sent == 2u);
    }
    g_t += RESP_TIMEOUT - 1u;
    check("tekrar gonderimden sonra zamanlayici yenilenir: 1 ms once yok", poll_frame(&type, &seq, &v, buf) == 0u);
    for(unsigned k = 2u; k <= MAX_RETRIES; k++) {
        g_t += RESP_TIMEOUT;
        (void)poll_frame(&type, &seq, &v, buf);
        g_t += 0u;
    }
    check("8 yeniden gonderim yapildi, hala FAILED degil", result_of(&g_c).retransmits == MAX_RETRIES && result_of(&g_c).state == FWU_CLIENT_HELLO);
    g_t += RESP_TIMEOUT;
    check("9. zaman asimi (retry siniri asildi): gonderim yok, FAILED(TIMEOUT)", poll_frame(&type, &seq, &v, buf) == 0u &&
          result_of(&g_c).failure == FWU_CLIENT_FAIL_TIMEOUT && result_of(&g_c).frames_sent == 1u + MAX_RETRIES);
    g_t += 10u * RESP_TIMEOUT;
    check("FAILED sonrasi poll: hicbir sey gonderilmez", poll_frame(&type, &seq, &v, buf) == 0u);

    fresh_client();
    g_cc.max_retries = 0u;
    (void)fwu_client_init(&g_c, &g_cc);
    (void)fwu_client_start(&g_c, g_t);
    (void)poll_frame(&type, &seq, &v, buf);
    g_t += RESP_TIMEOUT;
    (void)poll_frame(&type, &seq, &v, buf);
    check("max_retries 0: ilk zaman asiminda FAILED(TIMEOUT), tek cerceve gitti", result_of(&g_c).failure == FWU_CLIENT_FAIL_TIMEOUT && result_of(&g_c).frames_sent == 1u && result_of(&g_c).retransmits == 0u);

    /* zaman sarmasi */
    fresh_client();
    (void)fwu_client_start(&g_c, 0xFFFFFF00u);
    g_t = 0xFFFFFF00u;
    (void)poll_frame(&type, &seq, &v, buf);
    g_t = 0xFFFFFF10u;
    check("sarmaya yakin: 16 ms sonra tekrar yok (sarma-guvensiz karsilastirma erken tekrar ederdi)", poll_frame(&type, &seq, &v, buf) == 0u);
    g_t = 0xFFFFFF00u + RESP_TIMEOUT - 1u;
    check("sarma sonrasi 99 ms: tekrar yok", poll_frame(&type, &seq, &v, buf) == 0u);
    g_t = 0xFFFFFF00u + RESP_TIMEOUT;
    check("sarma sonrasi 100 ms: tekrar", poll_frame(&type, &seq, &v, buf) > 0u);
}

static void unit_cancel(void)
{
    fwu_byte_t buf[FWU_FRAME_MAX_SIZE];
    fwu_byte_t type;
    fwu_byte_t seq;
    fwu_frame_view_t v;

    printf("\n--- A6: iptal ---\n");
    to_transfer(0u);
    expect("cancel (TRANSFER)", fwu_client_cancel(&g_c), FWU_OK);
    check("... poll ABORT gonderir (tur 0x1F), durum ABORTING", poll_frame(&type, &seq, &v, buf) > 0u && type == FWU_MSG_ABORT && fwu_client_get_state(&g_c) == FWU_CLIENT_ABORTING);
    feed_byte(FWU_MSG_ABORT_OK, 0u);
    check("ABORT_OK: FAILED(CANCELLED)", result_of(&g_c).state == FWU_CLIENT_FAILED && result_of(&g_c).failure == FWU_CLIENT_FAIL_CANCELLED);
    expect("bittikten sonra cancel: BAD_STATE", fwu_client_cancel(&g_c), FWU_ERR_BAD_STATE);

    to_transfer(0u);
    (void)fwu_client_cancel(&g_c);
    (void)poll_frame(&type, &seq, &v, buf);
    feed_error(FWU_ERR_BAD_STATE, FWU_MSG_ABORT);
    check("ABORT'a ERROR yaniti: iptal yine de sonlanir (CANCELLED)", result_of(&g_c).failure == FWU_CLIENT_FAIL_CANCELLED);

    to_transfer(0u);
    (void)fwu_client_cancel(&g_c);
    (void)poll_frame(&type, &seq, &v, buf);
    for(unsigned k = 0u; k <= MAX_RETRIES; k++) {
        g_t += RESP_TIMEOUT;
        (void)poll_frame(&type, &seq, &v, buf);
    }
    check("ABORT_OK hic gelmez: yeniden gonderimler tukenince FAILED(CANCELLED) (TIMEOUT degil)", result_of(&g_c).failure == FWU_CLIENT_FAIL_CANCELLED);

    to_transfer(0u);
    expect("cancel (ilk)", fwu_client_cancel(&g_c), FWU_OK);
    expect("cancel (poll'dan once ikinci kez): serbest", fwu_client_cancel(&g_c), FWU_OK);
    (void)poll_frame(&type, &seq, &v, buf);
    expect("ABORTING'te cancel: BAD_STATE", fwu_client_cancel(&g_c), FWU_ERR_BAD_STATE);

    to_exit();
    feed_byte(FWU_MSG_VERIFY_RESULT, 0u);                    /* ACTIVATE durumu */
    expect("ACTIVATE asamasinda cancel: BAD_STATE (commit yazilmis olabilir)", fwu_client_cancel(&g_c), FWU_ERR_BAD_STATE);
    to_exit();
    feed_byte(FWU_MSG_EXIT_PENDING, 1u);
    expect("VERIFY_WAIT'te cancel serbest", fwu_client_cancel(&g_c), FWU_OK);
    {
        fwu_client_t idle;
        make_config(&g_cc);
        (void)fwu_client_init(&idle, &g_cc);
        expect("IDLE'da cancel: BAD_STATE", fwu_client_cancel(&idle), FWU_ERR_BAD_STATE);
    }
}

static void unit_mutation_gaps(void)
{
    fwu_byte_t buf[FWU_FRAME_MAX_SIZE];
    fwu_byte_t type;
    fwu_byte_t seq;
    fwu_frame_view_t v;

    printf("\n--- A7: mutasyon testinin buldugu bosluklar ---\n");

    /* yeniden baslatma ilerlemeyi SIFIRLAR (ilerleme varken) */
    to_transfer(320u);
    check("TRANSFER'de ilerleme 320", result_of(&g_c).acked_bytes == 320u);
    feed_error(FWU_ERR_BAD_STATE, FWU_MSG_TRANSFER_DATA);
    check("BAD_STATE ile bastan baslama: HELLO'ya donuldu ve ilerleme 320 -> 0", fwu_client_get_state(&g_c) == FWU_CLIENT_HELLO && result_of(&g_c).acked_bytes == 0u);

    /* 1 baytlik meydan okuma ve 1 baytlik yanit sinirda gecerli */
    fresh_client();
    g_cc.auth = aut_len;
    g_auth_len_to_return = 1u;
    (void)fwu_client_init(&g_c, &g_cc);
    (void)fwu_client_start(&g_c, g_t);
    feed_info(1u, 0x03u, TW_PRODUCT);
    feed(FWU_MSG_AUTH_CHALLENGE, (const fwu_byte_t[1]){ 9u }, 1u);
    check("meydan okuma 1 bayt + kanca yaniti 1 bayt (alt sinir): AUTH_RESP gonderilir", fwu_client_get_state(&g_c) == FWU_CLIENT_AUTH_RESP);
    (void)fwu_frame_decode(g_c.req_frame, g_c.req_length, &v);
    check("... AUTH_RESP yuku 1 bayt", v.payload_length == 1u);

    /* 1 baytlik imaj: init gecerli */
    {
        fwu_client_config_t c;
        fwu_client_t cl;
        tw_world(false, FWU_ACTIVATE_MODE_COPY);
        tw_make_package(1u, 25u, 0u);
        make_config(&c);
        c.image_size = 1u;
        expect("1 baytlik imaj (alt sinir): init gecerli", fwu_client_init(&cl, &c), FWU_OK);
    }

    /* basarisiz yeniden init nesneyi GECERSIZ kilar */
    fresh_client();
    expect("onceki init gecerli, start", fwu_client_start(&g_c, g_t), FWU_OK);
    make_config(&g_cc);
    g_cc.response_timeout_ms = 0u;
    expect("gecersiz yapilandirmayla yeniden init", fwu_client_init(&g_c, &g_cc), FWU_ERR_INVALID_ARG);
    expect("... nesne KULLANILAMAZ: start INVALID_ARG", fwu_client_start(&g_c, g_t), FWU_ERR_INVALID_ARG);
    expect("... poll INVALID_ARG", fwu_client_poll(&g_c, g_t, buf, sizeof(buf), &(size_t){ 0u }), FWU_ERR_INVALID_ARG);
    expect("... cancel INVALID_ARG", fwu_client_cancel(&g_c), FWU_ERR_INVALID_ARG);
    expect("cancel: client NULL", fwu_client_cancel(NULL), FWU_ERR_INVALID_ARG);
    expect("on_frame: init edilmemis", fwu_client_on_frame(&g_c, &v, g_t), FWU_ERR_INVALID_ARG);
    {
        fwu_client_result_t r;
        expect("get_result: init edilmemis nesne okunabilir (yalniz NULL reddedilir)", fwu_client_get_result(&g_c, &r), FWU_OK);
        expect("get_result: sonuc NULL", fwu_client_get_result(&g_c, NULL), FWU_ERR_INVALID_ARG);
    }

    /* kirli yeniden init: sonuc alanlarinin HEPSI sifirlanir */
    to_exit();
    feed_seq(FWU_MSG_EXIT_PENDING, (fwu_byte_t)(g_c.req_seq + 1u), (const fwu_byte_t[1]){ 1u }, 1u);   /* yok sayilan yanit */
    feed_byte(FWU_MSG_VERIFY_RESULT, (fwu_byte_t)FWU_ERR_HASH_MISMATCH);
    check("kirletme: REJECTED, verify_code 35, ilerleme 700, ignored 1", result_of(&g_c).failure == FWU_CLIENT_FAIL_REJECTED && result_of(&g_c).verify_code == 35u &&
          result_of(&g_c).acked_bytes == 700u && result_of(&g_c).ignored == 1u);
    make_config(&g_cc);
    (void)fwu_client_init(&g_c, &g_cc);
    {
        const fwu_client_result_t r = result_of(&g_c);
        check("yeniden init: durum IDLE, basarisizlik yok, verify_code 0, ilerleme 0, tum sayaclar 0",
              r.state == FWU_CLIENT_IDLE && r.failure == FWU_CLIENT_FAIL_NONE && r.verify_code == 0u && r.acked_bytes == 0u && r.total_bytes == 700u &&
              r.frames_sent == 0u && r.retransmits == 0u && r.restarts == 0u && r.resyncs == 0u && r.ignored == 0u);
    }
    to_request();
    feed_error(FWU_ERR_BAD_SIGNATURE, FWU_MSG_REQUEST_DOWNLOAD);
    check("kirletme: DEVICE_ERROR 50", result_of(&g_c).device_error == FWU_ERR_BAD_SIGNATURE);
    make_config(&g_cc);
    (void)fwu_client_init(&g_c, &g_cc);
    check("yeniden init: device_error sifirlandi (FWU_OK)", result_of(&g_c).device_error == FWU_OK);

    /* bekleyen iptal istegi yeniden init ile silinir */
    started();
    expect("iptal istendi (henuz poll edilmedi)", fwu_client_cancel(&g_c), FWU_OK);
    make_config(&g_cc);
    (void)fwu_client_init(&g_c, &g_cc);
    (void)fwu_client_start(&g_c, g_t);
    (void)poll_frame(&type, &seq, &v, buf);
    check("yeniden init + start: ilk cerceve HELLO (eski iptal istegi unutuldu), ABORT degil", type == FWU_MSG_HELLO && fwu_client_get_state(&g_c) == FWU_CLIENT_HELLO);

    /* dogrulama bekleme siniri: TAM verify_timeout_ms'de basarisiz */
    to_exit();
    g_t = 10000u;
    (void)poll_frame(&type, &seq, &v, buf);
    feed_byte(FWU_MSG_EXIT_PENDING, 1u);                      /* verify_start = 10000 */
    for(uint32_t t = 10100u; t <= 11900u; t += 100u) {
        g_t = t;
        (void)poll_frame(&type, &seq, &v, buf);
    }
    check("1900 ms bekledi: hala VERIFY_WAIT, 18 tekrar", fwu_client_get_state(&g_c) == FWU_CLIENT_VERIFY_WAIT && result_of(&g_c).retransmits == 19u);
    g_t = 11999u;
    check("11999 (son gonderimden 99 ms): gonderim yok", poll_frame(&type, &seq, &v, buf) == 0u && fwu_client_get_state(&g_c) == FWU_CLIENT_VERIFY_WAIT);
    g_t = 12000u;
    (void)poll_frame(&type, &seq, &v, buf);
    check("TAM 2000 ms (verify_timeout_ms): FAILED(TIMEOUT), sinir DAHIL", result_of(&g_c).failure == FWU_CLIENT_FAIL_TIMEOUT);
}

/* ================================================================== B. GERCEK CIHAZLA */

typedef struct {
    fwu_linksim_profile_t h2d;
    fwu_linksim_profile_t d2h;
    uint32_t              seed;
    uint32_t              max_ms;
    bool                  with_auth;
    bool                  use_can;
    uint32_t              busy_pm;              /* cihaz tasiyicisinin BUFFER_FULL olasiligi (binde) */
    bool                  client_auth;          /* istemcide yetkilendirme kancasi var mi */
    uint32_t              drop_resp_at;         /* betik: bu siradaki (0 tabanli) cihaz->host cerceveyi dusur (0xFFFFFFFF yok) */
    uint32_t              drop_resp_count;      /* ... ardisik bu kadar yaniti dusur */
    uint32_t              preseed_chunks;       /* istemci baslamadan once cihaza elle gonderilen parca sayisi (0 = yok) */
    bool                  pending_activation;   /* cihazda bekleyen commit var */
    uint32_t              image_flip_index;     /* host'un imajinda bu indeksteki bayt bozulur (0xFFFFFFFF yok) */
    uint32_t              signer;               /* manifest imzalayan anahtar (0 gecerli, 2 saldirgan) */
    uint32_t              version;              /* manifest surumu */
    uint32_t              product;              /* manifest urun (0 = varsayilan) */
    uint32_t              cancel_at_ms;         /* bu zamanda iptal iste (0xFFFFFFFF yok) */
    uint32_t              max_retries;
    uint32_t              max_restarts;
    uint32_t              response_timeout;
    bool                  can_loss;             /* CAN kanallarinda yalniz drop/dup/delay (corrupt/truncate/garbage yok) */
} run_opts_t;

typedef struct {
    fwu_client_result_t result;
    uint32_t            elapsed_ms;
    bool                finished;
    fwu_linksim_stats_t h2d;
    fwu_linksim_stats_t d2h;
    uint32_t            link_busy;
} run_out_t;

static fwu_linksim_t g_h2d;
static fwu_linksim_t g_d2h;
static uint32_t g_dev_rng;
static uint32_t g_busy_pm;
static uint32_t g_resp_index;
static uint32_t g_drop_at;
static uint32_t g_drop_count;
static uint32_t g_now;

/* --- dogrudan (UART benzeri) cihaz tasiyicisi: yaniti d2h hattina verir --- */
static fwu_return_e dev_send(void *context, const fwu_byte_t *data, size_t length)
{
    (void)context;
    g_dev_rng = (g_dev_rng * 1664525u) + 1013904223u;
    if((g_busy_pm > 0u) && (((g_dev_rng >> 8) % 1000u) < g_busy_pm)) {
        return FWU_ERR_BUFFER_FULL;
    }
    if((g_resp_index >= g_drop_at) && (g_resp_index < (g_drop_at + g_drop_count))) {
        g_resp_index++;                                      /* betikli kayip: yanit hic hatta cikmaz */
        return FWU_OK;
    }
    g_resp_index++;
    return fwu_linksim_send(&g_d2h, data, length, g_now);
}

/* --- CAN: iki ISO-TP ucu --- */
static fwu_linksim_t g_can_h2d;
static fwu_linksim_t g_can_d2h;
static fwu_isotp_t   g_iso_host;
static fwu_isotp_t   g_iso_dev;

static fwu_return_e iso_host_can_send(void *context, const fwu_byte_t *data)
{
    (void)context;
    return fwu_linksim_send(&g_can_h2d, data, 8u, g_now);
}

static fwu_return_e iso_dev_can_send(void *context, const fwu_byte_t *data)
{
    (void)context;
    return fwu_linksim_send(&g_can_d2h, data, 8u, g_now);
}

static void iso_host_deliver(void *context, const fwu_byte_t *message, size_t length)
{
    (void)context;
    (void)fwu_ringbuf_push(&g_host_rx.base, message, length);
}

static void iso_dev_deliver(void *context, const fwu_byte_t *message, size_t length)
{
    (void)context;
    (void)fwu_ringbuf_push(&tw_rx.base, message, length);
}

static fwu_transport_t g_dev_tr_direct = { dev_send, NULL, NULL };
static fwu_transport_t g_dev_tr_can = { fwu_isotp_transport_send, fwu_isotp_transport_poll, &g_iso_dev };

static fwu_client_config_t g_run_cfg;
static fwu_client_t g_run_client;

static void zero_profile(fwu_linksim_profile_t *p)
{
    p->drop_pm = 0u; p->duplicate_pm = 0u; p->corrupt_pm = 0u; p->truncate_pm = 0u; p->delay_pm = 0u; p->garbage_pm = 0u; p->delay_max_ms = 1u;
}

static void opts_default(run_opts_t *o)
{
    zero_profile(&o->h2d);
    zero_profile(&o->d2h);
    o->seed = 1u;
    o->max_ms = 20000u;
    o->with_auth = false;
    o->use_can = false;
    o->busy_pm = 0u;
    o->client_auth = false;
    o->drop_resp_at = 0xFFFFFFFFu;
    o->drop_resp_count = 0u;
    o->preseed_chunks = 0u;
    o->pending_activation = false;
    o->image_flip_index = 0xFFFFFFFFu;
    o->signer = 0u;
    o->version = 25u;
    o->product = 0u;
    o->cancel_at_ms = 0xFFFFFFFFu;
    o->max_retries = MAX_RETRIES;
    o->max_restarts = MAX_RESTARTS;
    o->response_timeout = RESP_TIMEOUT;
    o->can_loss = false;
}

/** Cihazin ring buffer'ina host cercevesini bayt akisi olarak iter (preseed icin). */
static void device_direct_push(fwu_byte_t type, fwu_byte_t seq, const fwu_byte_t *payload, size_t length)
{
    fwu_byte_t frame[FWU_FRAME_MAX_SIZE];
    size_t n = 0u;
    (void)fwu_frame_encode(type, seq, payload, length, frame, sizeof(frame), &n);
    (void)fwu_ringbuf_push(&tw_rx.base, frame, n);
}

/** Bir tam kosu: dunya + hat + istemci; sanal zaman 1 ms adimlarla. */
static void run_session(const run_opts_t *o, run_out_t *out)
{
    fwu_linksim_profile_t can_p;
    uint32_t t0;
    uint32_t host_pending_length = 0u;
    static fwu_byte_t host_pending[FWU_FRAME_MAX_SIZE];

    tw_world(o->with_auth, FWU_ACTIVATE_MODE_COPY);
    tw_make_package(TW_IMG_SIZE, o->version, o->signer);
    if(o->product != 0u) {
        tw_put_u32(tw_manifest, 8u, o->product);
        fwu_crypto_sim_sign(tw_pk[o->signer], tw_manifest, 74u, &tw_manifest[74]);
    }
    make_config(&g_run_cfg);
    g_run_cfg.auth = o->client_auth ? aut_ok : NULL;
    g_run_cfg.max_retries = o->max_retries;
    g_run_cfg.max_restarts = o->max_restarts;
    g_run_cfg.response_timeout_ms = o->response_timeout;
    g_run_cfg.verify_timeout_ms = (o->response_timeout * 20u > VERIFY_TIMEOUT) ? (o->response_timeout * 20u) : VERIFY_TIMEOUT;
    (void)fwu_client_init(&g_run_client, &g_run_cfg);
    if(o->image_flip_index != 0xFFFFFFFFu) {
        tw_image[o->image_flip_index] = (fwu_byte_t)(tw_image[o->image_flip_index] ^ 0x08u);   /* host imaji manifest'ten sonra bozulur */
    }

    (void)host_rb_init(&g_host_rx);
    (void)fwu_parser_init(&g_host_parser, &g_host_rx.base);
    (void)fwu_linksim_init(&g_h2d, &o->h2d, o->seed * 2u + 1u);
    (void)fwu_linksim_init(&g_d2h, &o->d2h, o->seed * 2u + 2u);
    g_dev_rng = o->seed * 7u + 3u;
    g_busy_pm = o->busy_pm;
    g_resp_index = 0u;
    g_drop_at = o->drop_resp_at;
    g_drop_count = o->drop_resp_count;
    t0 = 1000u;
    g_now = t0;

    if(o->use_can) {
        fwu_isotp_config_t ic;
        can_p = o->h2d;
        (void)fwu_linksim_init(&g_can_h2d, &can_p, o->seed * 2u + 11u);
        can_p = o->d2h;
        (void)fwu_linksim_init(&g_can_d2h, &can_p, o->seed * 2u + 12u);
        ic.can_send = iso_host_can_send; ic.deliver = iso_host_deliver; ic.context = NULL;
        ic.block_size = FWU_TRANSPORT_CAN_BLOCK_SIZE; ic.st_min_ms = FWU_TRANSPORT_CAN_ST_MIN_MS; ic.timeout_ms = 300u; ic.wft_max = FWU_ISOTP_DEFAULT_WFT_MAX;
        (void)fwu_isotp_init(&g_iso_host, &ic);
        ic.can_send = iso_dev_can_send; ic.deliver = iso_dev_deliver;
        (void)fwu_isotp_init(&g_iso_dev, &ic);
        (void)tw_link_setup(&g_dev_tr_can);
    } else {
        (void)tw_link_setup(&g_dev_tr_direct);
    }

    if(o->pending_activation) {
        (void)fwu_activate_commit(&tw_act, 3u);
    }
    if(o->preseed_chunks > 0u) {
        /* istemciden bagimsiz: cihaza elle REQUEST_DOWNLOAD + ilk parcalar (devam senaryosu) */
        (void)fwu_ringbuf_push(&tw_rx.base, (const fwu_byte_t *)"", 0u);
        device_direct_push(FWU_MSG_REQUEST_DOWNLOAD, 200u, tw_manifest, 138u);
        for(uint32_t i = 0u; i < o->preseed_chunks; i++) {
            fwu_byte_t p[FWU_FRAME_MAX_PAYLOAD];
            size_t n = 0u;
            (void)fwu_proto_encode_data(i * TW_CHUNK, &tw_image[i * TW_CHUNK], TW_CHUNK, p, sizeof(p), &n);
            device_direct_push(FWU_MSG_TRANSFER_DATA, 201u, p, n);
        }
        /* cihazin yanitlari hatta cikmasin: sanal tasiyici yok sayar */
        {
            const uint32_t saved_busy = g_busy_pm;
            uint32_t saved_at = g_drop_at;
            uint32_t saved_count = g_drop_count;
            g_drop_at = 0u;
            g_drop_count = 1000u;
            for(uint32_t k = 0u; k < 4u + o->preseed_chunks; k++) {
                g_now += 1u;
                (void)fwu_link_poll(&tw_link, g_now);
            }
            g_resp_index = 0u;
            g_drop_at = saved_at;
            g_drop_count = saved_count;
            g_busy_pm = saved_busy;
            t0 = g_now;
        }
    }

    (void)fwu_client_start(&g_run_client, t0);
    out->finished = false;
    out->elapsed_ms = 0u;
    for(uint32_t ms = 0u; ms < o->max_ms; ms++) {
        fwu_byte_t buf[FWU_LINKSIM_CHUNK_MAX];
        size_t n;

        g_now = t0 + ms;
        if((o->cancel_at_ms != 0xFFFFFFFFu) && (ms == o->cancel_at_ms)) {
            (void)fwu_client_cancel(&g_run_client);
        }

        /* (0) CAN: gelen CAN cerceveleri ve ISO-TP zamanlayicilari */
        if(o->use_can) {
            size_t k;
            (void)fwu_isotp_poll(&g_iso_host, g_now);
            while((k = fwu_linksim_receive(&g_can_d2h, g_now, buf, sizeof(buf))) > 0u) {
                (void)fwu_isotp_on_can_frame(&g_iso_host, buf, k, g_now);
            }
        }
        /* (1) cihaz -> host */
        if(!o->use_can) {
            size_t k;
            while((k = fwu_linksim_receive(&g_d2h, g_now, buf, sizeof(buf))) > 0u) {
                (void)fwu_ringbuf_push(&g_host_rx.base, buf, k);
            }
        }
        for(;;) {
            fwu_frame_view_t view;
            if(fwu_parser_poll(&g_host_parser, g_now, &view) != FWU_OK) {
                break;
            }
            (void)fwu_client_on_frame(&g_run_client, &view, g_now);
        }
        if(fwu_client_is_finished(&g_run_client)) {
            out->finished = true;
            out->elapsed_ms = ms;
            break;
        }
        /* (2) istemci -> host hatti */
        n = 0u;
        (void)fwu_client_poll(&g_run_client, g_now, buf, sizeof(buf), &n);
        if(n > 0u) {
            if(o->use_can) {
                if(host_pending_length == 0u) {
                    for(size_t i = 0u; i < n; i++) {
                        host_pending[i] = buf[i];
                    }
                    host_pending_length = (uint32_t)n;
                }
            } else {
                (void)fwu_linksim_send(&g_h2d, buf, n, g_now);
            }
        }
        if(o->use_can && (host_pending_length > 0u)) {
            if(fwu_isotp_send(&g_iso_host, host_pending, host_pending_length, g_now) == FWU_OK) {
                host_pending_length = 0u;
            }
        }
        /* (3) host -> cihaz */
        if(o->use_can) {
            size_t k;
            while((k = fwu_linksim_receive(&g_can_h2d, g_now, buf, sizeof(buf))) > 0u) {
                (void)fwu_isotp_on_can_frame(&g_iso_dev, buf, k, g_now);
            }
        } else {
            size_t k;
            while((k = fwu_linksim_receive(&g_h2d, g_now, buf, sizeof(buf))) > 0u) {
                (void)fwu_ringbuf_push(&tw_rx.base, buf, k);
            }
        }
        /* (4) cihaz */
        (void)fwu_link_poll(&tw_link, g_now);
    }
    (void)fwu_client_get_result(&g_run_client, &out->result);
    out->h2d = o->use_can ? g_can_h2d.stats : g_h2d.stats;
    out->d2h = o->use_can ? g_can_d2h.stats : g_d2h.stats;
    out->link_busy = fwu_link_get_stats(&tw_link)->tx_busy;
}

static void test_real_device_perfect(void)
{
    run_opts_t o;
    run_out_t r;
    fwu_activate_outcome_e outcome = FWU_ACT_OUTCOME_NONE;

    printf("\n--- B1: gercek cihaz, kusursuz hat: tam guncelleme ---\n");
    opts_default(&o);
    run_session(&o, &r);
    check("istemci DONE", r.finished && r.result.state == FWU_CLIENT_DONE && r.result.failure == FWU_CLIENT_FAIL_NONE);
    check("... gonderilen cerceve TAM 15 (HELLO + REQUEST + 11 DATA + EXIT + ACTIVATE), tekrar yok, yeniden baslama yok",
          r.result.frames_sent == 15u && r.result.retransmits == 0u && r.result.restarts == 0u && r.result.resyncs == 0u && r.result.ignored == 0u);
    check("... sure TAM 30 ms (15 degisim x 2 ms: hat 1 ms + 1 ms; EXIT yaniti ve dogrulama sonucu ayni degisimde)", r.elapsed_ms == 30u);
    check("... ilerleme 700/700", r.result.acked_bytes == 700u && r.result.total_bytes == 700u);
    check("... cihaz: oturum COMMIT, yeniden baslatma istendi ve yanit teslim edildi", fwu_session_get_state(&tw_session) == FWU_SESSION_COMMITTED && fwu_link_reboot_ready(&tw_link));
    check("... uygulama bu ana kadar ESKI (commit en son, uygulamaya dokunulmadi)", tw_app_is_old());
    expect("bootloader", tw_bootloader_run(&outcome), FWU_OK);
    check("... ACTIVATED, uygulama TAM YENI imaj", outcome == FWU_ACT_OUTCOME_ACTIVATED && tw_app_is_new());

    /* yetkilendirme */
    opts_default(&o);
    o.with_auth = true;
    o.client_auth = true;
    run_session(&o, &r);
    check("yetkilendirmeli cihaz + kancali istemci: DONE, 17 cerceve (15 + AUTH_REQ + AUTH_RESP), sure 34 ms",
          r.finished && r.result.state == FWU_CLIENT_DONE && r.result.frames_sent == 17u && r.elapsed_ms == 34u);
    check("... cihazin kancasi 1 meydan okuma uretti, 1 yanit denetledi", tw_auth_ctx.begin_calls == 1u && tw_auth_ctx.check_calls == 1u);
    opts_default(&o);
    o.with_auth = true;
    o.client_auth = false;
    run_session(&o, &r);
    check("yetkilendirmeli cihaz, istemcide kanca YOK: FAILED(AUTH_UNAVAILABLE), cihaza hicbir indirme gitmedi",
          r.finished && r.result.failure == FWU_CLIENT_FAIL_AUTH_UNAVAILABLE && r.result.frames_sent == 1u && tw_sim.base.operations_done == 0u);
}

static void test_real_device_rejections(void)
{
    run_opts_t o;
    run_out_t r;
    const uint32_t ops_before_check = 0u;

    printf("\n--- B2: gercek cihaz: reddedilen paketler, bekleyen aktivasyon, bozuk imaj ---\n");
    (void)ops_before_check;
    opts_default(&o);
    o.signer = 2u;
    run_session(&o, &r);
    check("saldirganin anahtariyla imzali manifest: FAILED(DEVICE_ERROR, 50 BAD_SIGNATURE), 2 cerceve (HELLO, REQUEST), flash'a dokunulmadi",
          r.finished && r.result.failure == FWU_CLIENT_FAIL_DEVICE_ERROR && r.result.device_error == FWU_ERR_BAD_SIGNATURE && r.result.frames_sent == 2u &&
          tw_sim.base.operations_done == 0u && tw_app_is_old());
    opts_default(&o);
    o.product = 999u;
    run_session(&o, &r);
    check("baska urun: FAILED(DEVICE_ERROR, 52)", r.result.failure == FWU_CLIENT_FAIL_DEVICE_ERROR && r.result.device_error == FWU_ERR_PRODUCT_MISMATCH && tw_sim.base.operations_done == 0u);
    opts_default(&o);
    o.version = 24u;
    run_session(&o, &r);
    check("eski surum (anti-rollback): FAILED(DEVICE_ERROR, 53)", r.result.failure == FWU_CLIENT_FAIL_DEVICE_ERROR && r.result.device_error == FWU_ERR_VERSION_REJECTED && tw_sim.base.operations_done == 0u);

    opts_default(&o);
    o.image_flip_index = 300u;
    run_session(&o, &r);
    check("host imaji bozuk (manifest ozetiyle uyusmuyor): FAILED(REJECTED, 35 HASH_MISMATCH)", r.finished && r.result.failure == FWU_CLIENT_FAIL_REJECTED && r.result.verify_code == 35u);
    check("... cihaz BOSTA, commit YOK, uygulama ESKI", fwu_session_get_state(&tw_session) == FWU_SESSION_IDLE && tw_act.last_state == 0u && tw_app_is_old());

    opts_default(&o);
    o.pending_activation = true;
    o.max_restarts = 0u;
    run_session(&o, &r);
    check("bekleyen aktivasyon var: REQUEST BAD_STATE, max_restarts 0: FAILED(DEVICE_ERROR, 12), flash'a dokunulmadi", r.result.failure == FWU_CLIENT_FAIL_DEVICE_ERROR &&
          r.result.device_error == FWU_ERR_BAD_STATE && r.result.frames_sent == 2u && tw_sim.base.erase_counts[8] == 0u);
    opts_default(&o);
    o.pending_activation = true;
    o.max_restarts = 2u;
    run_session(&o, &r);
    check("... max_restarts 2: HELLO + REQUEST uc kez denenir (6 cerceve), sonra FAILED(DEVICE_ERROR, 12): sonsuz dongu yok",
          r.finished && r.result.failure == FWU_CLIENT_FAIL_DEVICE_ERROR && r.result.restarts == 2u && r.result.frames_sent == 6u);

    opts_default(&o);
    o.product = 0u;
    run_session(&o, &r);
    check("referans: ayni kosu temiz DONE (parametreler baska testi etkilemiyor)", r.finished && r.result.state == FWU_CLIENT_DONE);
}

static void test_real_device_resume_and_loss(void)
{
    run_opts_t o;
    run_out_t r;
    fwu_activate_outcome_e outcome = FWU_ACT_OUTCOME_NONE;

    printf("\n--- B3: gercek cihaz: devam, kayip yanit (idempotent tekrar), kaybolan cihaz, iptal ---\n");
    /* devam: cihazda 5 parca zaten var */
    opts_default(&o);
    o.preseed_chunks = 5u;
    run_session(&o, &r);
    check("cihazda 5 parca (320 bayt) hazir: istemci DONE", r.finished && r.result.state == FWU_CLIENT_DONE);
    check("... gonderilen cerceve 10 (HELLO + REQUEST + 6 DATA (320..700) + EXIT + ACTIVATE): cihazin ofsetinden devam edildi", r.result.frames_sent == 10u && r.result.retransmits == 0u);
    expect("bootloader", tw_bootloader_run(&outcome), FWU_OK);
    check("... uygulama TAM YENI", outcome == FWU_ACT_OUTCOME_ACTIVATED && tw_app_is_new());

    /* kayip yanit: 6. yanit (HELLO, REQUEST, ACK0, ACK1, ACK2, ACK3 -> indeks 5 = 4. parcanin ACK'i) */
    opts_default(&o);
    o.drop_resp_at = 5u;
    o.drop_resp_count = 1u;
    run_session(&o, &r);
    check("bir DATA_ACK kayboldu: istemci zaman asimiyla AYNI parcayi tekrarladi, cihaz yeniden onayladi, DONE",
          r.finished && r.result.state == FWU_CLIENT_DONE && r.result.retransmits == 1u && r.result.frames_sent == 16u);
    check("... kayip yanit yuzunden yazma TEKRARLANMADI: cihazin flash islemi kusursuz kosudakiyle ayni (110)", tw_sim.base.operations_done == 110u + 2u);
    expect("bootloader", tw_bootloader_run(&outcome), FWU_OK);
    check("... uygulama TAM YENI", tw_app_is_new());

    /* kayip EXIT_PENDING + VERIFY_RESULT (ayni degisimde iki yanit) */
    opts_default(&o);
    o.drop_resp_at = 13u;                                    /* HELLO0 REQ1 ACK2..12 (11 parca) -> 13 EXIT_PENDING, 14 VERIFY_RESULT */
    o.drop_resp_count = 2u;
    run_session(&o, &r);
    check("EXIT_PENDING ve VERIFY_RESULT ikisi de kayboldu: istemci EXIT'i tekrarlar, cihaz VERIFY_RESULT(0) hemen yanitlar (HAZIR'da tekrar), DONE",
          r.finished && r.result.state == FWU_CLIENT_DONE && r.result.retransmits == 1u);
    check("... dogrulama TEKRAR hesaplanmadi (imaj ozeti 1 kez)", tw_cs.finish_calls == 1u);

    /* kayip ACTIVATE_OK */
    opts_default(&o);
    o.drop_resp_at = 15u;                                    /* ... 15 = ACTIVATE_OK */
    o.drop_resp_count = 1u;
    run_session(&o, &r);
    check("ACTIVATE_OK kayboldu: istemci ACTIVATE'i tekrarlar, cihaz ayni onayi verir (COMMIT'te tekrar), DONE, commit TEK",
          r.finished && r.result.state == FWU_CLIENT_DONE && r.result.retransmits == 1u && tw_act.page_count == 3u);
    expect("bootloader", tw_bootloader_run(&outcome), FWU_OK);
    check("... uygulama TAM YENI (tek commit)", outcome == FWU_ACT_OUTCOME_ACTIVATED && tw_app_is_new());

    /* kaybolan cihaz: hic yanit yok */
    opts_default(&o);
    o.drop_resp_at = 0u;
    o.drop_resp_count = 100000u;
    run_session(&o, &r);
    check("cihaz hic yanit vermez: FAILED(TIMEOUT), cerceve sayisi 1 + 8 tekrar", r.finished && r.result.failure == FWU_CLIENT_FAIL_TIMEOUT && r.result.frames_sent == 1u + MAX_RETRIES);
    check("... sure sinirli: (1 + 8 tekrar) x 100 ms = 900 ms'de basarisiz", r.elapsed_ms >= 900u && r.elapsed_ms < 1000u);
    check("... cihaz etkilenmedi", tw_app_is_old());

    /* tek yonlu kayip: istemci -> cihaz hatti %100 kayip */
    opts_default(&o);
    o.h2d.drop_pm = 1000u;
    run_session(&o, &r);
    check("istemci -> cihaz hatti %100 kayip: FAILED(TIMEOUT)", r.finished && r.result.failure == FWU_CLIENT_FAIL_TIMEOUT && r.h2d.dropped == r.h2d.sent);

    /* iptal: tam aktarimin ortasinda */
    opts_default(&o);
    o.cancel_at_ms = 15u;
    run_session(&o, &r);
    check("aktarim ortasinda iptal: ABORT -> ABORT_OK, FAILED(CANCELLED)", r.finished && r.result.failure == FWU_CLIENT_FAIL_CANCELLED);
    check("... cihaz BOSTA, commit yok, uygulama ESKI", fwu_session_get_state(&tw_session) == FWU_SESSION_IDLE && tw_act.last_state == 0u && tw_app_is_old());
    check("... iptal tamamlanmadan ilerleme tam degil", r.result.acked_bytes < 700u);

    /* cihaz tasiyicisi sik sik mesgul */
    opts_default(&o);
    o.busy_pm = 500u;
    run_session(&o, &r);
    check("cihaz tasiyicisi %50 mesgul: istemci yine DONE (link yaniti bekletir, istemci tekrarlar)", r.finished && r.result.state == FWU_CLIENT_DONE);
    check("... link mesgul denemesi sayildi", r.link_busy > 0u);
}

/* ================================================================== C. KAYIPLI HAT TARAMALARI */

static void profile_set(fwu_linksim_profile_t *p, uint32_t drop, uint32_t dup, uint32_t corrupt, uint32_t trunc, uint32_t delay, uint32_t garbage, uint32_t delay_max)
{
    p->drop_pm = drop; p->duplicate_pm = dup; p->corrupt_pm = corrupt; p->truncate_pm = trunc; p->delay_pm = delay; p->garbage_pm = garbage; p->delay_max_ms = delay_max;
}

typedef struct {
    uint32_t seeds;
    uint32_t done;
    uint32_t failed;
    uint32_t old_ok;
    uint32_t new_ok;
    uint32_t violations;
    uint32_t retransmits;
    uint32_t resyncs;
    uint32_t restarts;
    uint32_t max_elapsed;
} sweep_t;

/** Bir kosudan sonra degismezleri denetler (sahte imza/bozuk imaj: commit YOK). */
static void check_invariants(const run_opts_t *o, const run_out_t *r, sweep_t *s)
{
    fwu_activate_outcome_e outcome = FWU_ACT_OUTCOME_NONE;
    const bool bad_package = (o->signer != 0u) || (o->image_flip_index != 0xFFFFFFFFu);
    int old_before = tw_app_is_old();

    s->seeds++;
    s->retransmits += r->result.retransmits;
    s->resyncs += r->result.resyncs;
    s->restarts += r->result.restarts;
    if(r->elapsed_ms > s->max_elapsed) {
        s->max_elapsed = r->elapsed_ms;
    }
    if(!old_before) {
        s->violations++;                                     /* bootloader calismadan uygulama degisemez */
    }
    if(bad_package && (tw_act.last_state != 0u)) {
        s->violations++;                                     /* sahte imzali/bozuk imaj commit edilemez */
    }
    if(r->finished && (r->result.state == FWU_CLIENT_DONE)) {
        s->done++;
        if(!(tw_session.state == FWU_SESSION_COMMITTED)) {
            s->violations++;
        }
    } else {
        s->failed++;
    }
    if(tw_bootloader_run(&outcome) != FWU_OK) {
        s->violations++;
        return;
    }
    if(tw_app_is_old()) {
        s->old_ok++;
    } else if(tw_app_is_new()) {
        s->new_ok++;
        if(bad_package) {
            s->violations++;
        }
    } else {
        s->violations++;                                     /* karisik: yarim guncelleme */
    }
    if(r->finished && (r->result.state == FWU_CLIENT_DONE) && !tw_app_is_new()) {
        s->violations++;                                     /* istemci DONE dedi ama uygulama yeni degil */
    }
}

static void test_lossy_sweeps(void)
{
    sweep_t s;
    run_opts_t o;
    run_out_t r;

    printf("\n--- C1: kayipli hat taramalari: yuzlerce tohum, her kosu sonunda degismezler ---\n");

    /* hafif: %5 kayip */
    s = (sweep_t){ 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
    for(uint32_t seed = 1u; seed <= 150u; seed++) {
        opts_default(&o);
        o.seed = seed;
        profile_set(&o.h2d, 50u, 0u, 0u, 0u, 0u, 0u, 1u);
        profile_set(&o.d2h, 50u, 0u, 0u, 0u, 0u, 0u, 1u);
        o.max_ms = 60000u;
        run_session(&o, &r);
        check_invariants(&o, &r, &s);
    }
    check("hafif (%5 kayip, her iki yon), 150 tohum: HEPSI DONE, uygulama tam yeni, ihlal 0", s.done == 150u && s.new_ok == 150u && s.violations == 0u);
    printf("(bilgi: tekrar toplam %u, en uzun sure %u ms)\n", (unsigned)s.retransmits, (unsigned)s.max_elapsed);

    /* orta: her ariza turu */
    s = (sweep_t){ 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
    for(uint32_t seed = 1u; seed <= 200u; seed++) {
        opts_default(&o);
        o.seed = seed;
        profile_set(&o.h2d, 100u, 50u, 50u, 30u, 100u, 30u, 200u);
        profile_set(&o.d2h, 100u, 50u, 50u, 30u, 100u, 30u, 200u);
        o.busy_pm = 100u;
        o.max_ms = 120000u;
        o.max_retries = 30u;
        o.max_restarts = 3u;
        run_session(&o, &r);
        check_invariants(&o, &r, &s);
    }
    check("orta (%10 kayip, %5 tekrar, %5 bozulma, %3 kesilme, %10 gecikme + SIRA DEGISIKLIGI, %3 cop, %10 mesgul), 200 tohum: HEPSI DONE, ihlal 0",
          s.done == 200u && s.new_ok == 200u && s.violations == 0u);
    printf("(bilgi: tekrar %u, yeniden eslesme %u, bastan baslama %u, en uzun sure %u ms)\n", (unsigned)s.retransmits, (unsigned)s.resyncs, (unsigned)s.restarts, (unsigned)s.max_elapsed);
    check("... tarama anlamli: yuzlerce yeniden gonderim ve (gecikme/tekrar kaynakli) yok sayilan yanit goruldu", s.retransmits > 300u);

    /* agir: %30 kayip */
    s = (sweep_t){ 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
    for(uint32_t seed = 1u; seed <= 100u; seed++) {
        opts_default(&o);
        o.seed = seed;
        profile_set(&o.h2d, 300u, 100u, 100u, 50u, 200u, 50u, 400u);
        profile_set(&o.d2h, 300u, 100u, 100u, 50u, 200u, 50u, 400u);
        o.max_ms = 600000u;
        o.max_retries = 60u;
        o.max_restarts = 5u;
        run_session(&o, &r);
        check_invariants(&o, &r, &s);
    }
    check("agir (%30 kayip, %10 tekrar, %10 bozulma, %20 gecikme), 100 tohum: HEPSI DONE, ihlal 0", s.done == 100u && s.new_ok == 100u && s.violations == 0u);
    printf("(bilgi: tekrar %u, en uzun sure %u ms)\n", (unsigned)s.retransmits, (unsigned)s.max_elapsed);

    /* yalniz bozulma / yalniz kesilme / yalniz cop / yalniz tekrar / yalniz gecikme */
    {
        static const uint32_t kinds[5][6] = { { 0u, 0u, 300u, 0u, 0u, 0u }, { 0u, 0u, 0u, 300u, 0u, 0u }, { 0u, 0u, 0u, 0u, 0u, 500u }, { 0u, 500u, 0u, 0u, 0u, 0u }, { 0u, 0u, 0u, 0u, 500u, 0u } };
        static const char *const names[5] = { "yalniz bozulma %30", "yalniz kesilme %30", "yalniz cop %50", "yalniz tekrar %50", "yalniz gecikme/sira degisikligi %50" };
        for(unsigned k = 0u; k < 5u; k++) {
            s = (sweep_t){ 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
            for(uint32_t seed = 1u; seed <= 40u; seed++) {
                char text[120];
                opts_default(&o);
                o.seed = seed;
                profile_set(&o.h2d, kinds[k][0], kinds[k][1], kinds[k][2], kinds[k][3], kinds[k][4], kinds[k][5], 300u);
                profile_set(&o.d2h, kinds[k][0], kinds[k][1], kinds[k][2], kinds[k][3], kinds[k][4], kinds[k][5], 300u);
                o.max_ms = 300000u;
                o.max_retries = 40u;
                run_session(&o, &r);
                check_invariants(&o, &r, &s);
                (void)text;
            }
            {
                char text[120];
                (void)snprintf(text, sizeof(text), "%s, 40 tohum: HEPSI DONE, ihlal 0", names[k]);
                check(text, s.done == 40u && s.new_ok == 40u && s.violations == 0u);
            }
        }
    }

    /* saldiri: sahte imza / bozuk imaj, kayipli hatta bile commit YOK */
    s = (sweep_t){ 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
    for(uint32_t seed = 1u; seed <= 60u; seed++) {
        opts_default(&o);
        o.seed = seed;
        o.signer = 2u;
        profile_set(&o.h2d, 100u, 50u, 50u, 30u, 100u, 30u, 200u);
        profile_set(&o.d2h, 100u, 50u, 50u, 30u, 100u, 30u, 200u);
        o.max_ms = 60000u;
        o.max_retries = 30u;
        run_session(&o, &r);
        check_invariants(&o, &r, &s);
    }
    check("SAHTE imzali manifest, orta kayipli hat, 60 tohum: hicbirinde commit/yeni uygulama YOK, ihlal 0", s.new_ok == 0u && s.old_ok == 60u && s.violations == 0u && s.done == 0u);
    s = (sweep_t){ 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
    for(uint32_t seed = 1u; seed <= 60u; seed++) {
        opts_default(&o);
        o.seed = seed;
        o.image_flip_index = 123u;
        profile_set(&o.h2d, 100u, 50u, 50u, 30u, 100u, 30u, 200u);
        profile_set(&o.d2h, 100u, 50u, 50u, 30u, 100u, 30u, 200u);
        o.max_ms = 60000u;
        o.max_retries = 30u;
        run_session(&o, &r);
        check_invariants(&o, &r, &s);
    }
    check("BOZUK imaj (host baytlari ozetle uyusmuyor), orta kayipli hat, 60 tohum: hicbirinde commit YOK, hepsi REDDEDILDI/biter, ihlal 0",
          s.new_ok == 0u && s.violations == 0u && s.done == 0u);

    /* yetkilendirmeli, kayipli */
    s = (sweep_t){ 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
    for(uint32_t seed = 1u; seed <= 60u; seed++) {
        opts_default(&o);
        o.seed = seed;
        o.with_auth = true;
        o.client_auth = true;
        profile_set(&o.h2d, 100u, 50u, 50u, 30u, 100u, 30u, 200u);
        profile_set(&o.d2h, 100u, 50u, 50u, 30u, 100u, 30u, 200u);
        o.max_ms = 120000u;
        o.max_retries = 30u;
        o.max_restarts = 4u;
        run_session(&o, &r);
        if(!(r.finished && r.result.state == FWU_CLIENT_DONE)) {
            printf("(tani: tohum %u biter=%d durum=%d basarisizlik=%d cihaz_hatasi=%d kod=%d tekrar=%u bastan=%u gonderilen=%u)\n", (unsigned)seed, (int)r.finished,
                   (int)r.result.state, (int)r.result.failure, (int)r.result.device_error, (int)r.result.verify_code, (unsigned)r.result.retransmits,
                   (unsigned)r.result.restarts, (unsigned)r.result.frames_sent);
        }
        check_invariants(&o, &r, &s);
    }
    check("yetkilendirmeli cihaz, orta kayipli hat, 60 tohum: HEPSI DONE (kayip AUTH yanitlari tekrar ile telafi), ihlal 0", s.done == 60u && s.new_ok == 60u && s.violations == 0u);

    /* %100 kayip: sonsuz dongu yok, cihaza dokunulmaz */
    opts_default(&o);
    profile_set(&o.h2d, 1000u, 0u, 0u, 0u, 0u, 0u, 1u);
    run_session(&o, &r);
    check("%100 kayip: istemci FAILED(TIMEOUT) ile biter, uygulama ESKI", r.finished && r.result.failure == FWU_CLIENT_FAIL_TIMEOUT && tw_app_is_old());
}

static void test_can_sweeps(void)
{
    sweep_t s;
    run_opts_t o;
    run_out_t r;

    printf("\n--- C2: CAN uzerinden ISO-TP: kusursuz ve kayipli (yalniz kayip/tekrar/gecikme: CAN CRC bozulmayi onler) ---\n");
    opts_default(&o);
    o.use_can = true;
    o.response_timeout = 400u;
    o.max_ms = 60000u;
    run_session(&o, &r);
    check("CAN kusursuz: DONE, tekrar yok", r.finished && r.result.state == FWU_CLIENT_DONE && r.result.retransmits == 0u);
    check("... cihaz CAN'da COMMIT'e ulasti ve yanit teslim edildi", fwu_session_get_state(&tw_session) == FWU_SESSION_COMMITTED && fwu_link_reboot_ready(&tw_link));
    {
        fwu_activate_outcome_e outcome = FWU_ACT_OUTCOME_NONE;
        expect("bootloader", tw_bootloader_run(&outcome), FWU_OK);
        check("... uygulama TAM YENI", tw_app_is_new());
    }
    check("... CAN trafigi: istemci ve cihaz cok sayida 8 baytlik cerceve gonderdi (parcalama gercekten calisti)", r.h2d.sent > 100u && r.d2h.sent > 15u);

    s = (sweep_t){ 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
    for(uint32_t seed = 1u; seed <= 40u; seed++) {
        opts_default(&o);
        o.seed = seed;
        o.use_can = true;
        o.response_timeout = 400u;
        profile_set(&o.h2d, 20u, 10u, 0u, 0u, 50u, 0u, 20u);
        profile_set(&o.d2h, 20u, 10u, 0u, 0u, 50u, 0u, 20u);
        o.max_ms = 300000u;
        o.max_retries = 40u;
        o.max_restarts = 4u;
        run_session(&o, &r);
        check_invariants(&o, &r, &s);
    }
    check("CAN, %2 kayip + %1 tekrar + %5 gecikme (CF sirasi bozulabilir), 40 tohum: HEPSI DONE, ihlal 0", s.done == 40u && s.new_ok == 40u && s.violations == 0u);
    printf("(bilgi: tekrar %u, bastan baslama %u, en uzun sure %u ms)\n", (unsigned)s.retransmits, (unsigned)s.restarts, (unsigned)s.max_elapsed);

    s = (sweep_t){ 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
    for(uint32_t seed = 1u; seed <= 20u; seed++) {
        opts_default(&o);
        o.seed = seed;
        o.use_can = true;
        o.signer = 2u;
        o.response_timeout = 400u;
        profile_set(&o.h2d, 20u, 10u, 0u, 0u, 50u, 0u, 20u);
        profile_set(&o.d2h, 20u, 10u, 0u, 0u, 50u, 0u, 20u);
        o.max_ms = 60000u;
        o.max_retries = 40u;
        run_session(&o, &r);
        check_invariants(&o, &r, &s);
    }
    check("CAN, SAHTE imzali manifest, kayipli, 20 tohum: commit YOK, ihlal 0", s.new_ok == 0u && s.violations == 0u);
}

int main(void)
{
    unit_init_and_basic();
    unit_hello();
    unit_request_transfer();
    unit_verify_activate();
    unit_errors_filters_timeouts();
    unit_cancel();
    unit_mutation_gaps();
    test_real_device_perfect();
    test_real_device_rejections();
    test_real_device_resume_and_loss();
    test_lossy_sweeps();
    test_can_sweeps();

    printf("\nResult: %s (%d failed)\n", g_failures == 0 ? "ALL TESTS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
