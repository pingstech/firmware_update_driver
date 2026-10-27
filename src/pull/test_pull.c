/**
 * @file    test_pull.c
 * @brief   fwu_pull (imajı sunucudan çekme) için kendi kendini kontrol eden test. GERÇEK fwu_session (sahte flash, sahte
 *          kriptografi, gerçek aktivasyon) ve GERÇEK fwu_http üzerinde; sunucu, testin içinde yazılmış bir TAKLİTTİR (istek
 *          baytlarını gerçekten ayrıştırır, gerçek HTTP cevabı üretir, hatalar enjekte eder).
 *
 * Kapsam:
 *   1. init doğrulaması.
 *   2. PULL_START / PULL_QUERY protokolü: durum kontrolü, geçersiz yükler, idempotent tekrar, farklı hedef, bilinmeyen tür.
 *   3. Mutlu yol: rastgele ve bayt bayt parçalarla besleme; staging imajla BİREBİR aynı; EXIT -> VERIFY(0) -> ACTIVATE.
 *   4. KALICI hatalar: 404, 200 (Range yok sayıldı), yanlış toplam boyut: hiçbir bayt yazılmaz, doğru hata kodu.
 *   5. GEÇİCİ hatalar ve yeniden deneme: sessiz sunucu, bozuk cevap, eski/yinelenen cevap, yarım gövde; hak bitince vazgeçme.
 *   6. Bağlantı kopması (yarım parça silinir, yazılmış son parçadan sürer), Connection: close, taşıyıcı meşgul (BUFFER_FULL).
 *   7. Giriş halkası taşması (akış kontrolü ihlali) kurtarılır.
 *   8. Oturum etkileşimi: ABORT, boşta zaman aşımı çekme sürerken işlemez, flash hatası, devam (FAILED sonrası, oturum açıkken).
 *   9. Düşman akış: rastgele hata enjeksiyonu; ASLA yanlış bayt commit edilmez, sonsuz döngü yok.
 *  10. UÇTAN UCA: GERÇEK fwu_client (çekme modu, imaj baytı YOK) <-> çerçeve kaybı olan hat <-> oturum + fwu_pull + taklit sunucu:
         mutlu yol, TRANSFER_DATA HİÇ gönderilmez, kalıcı hatalar cihazdan istemciye taşınır, çerçeve kaybı, cihaz unutması
         (ABORT) sonrası yeniden başlama, toplam süre sınırı, iptal, çekme yapılandırması doğrulaması.
 *
 * Derleme (pull klasöründen):
 *   gcc -DFWU_PULL_INPUT_SIZE=256u -Wall -Wextra -Wpedantic -Wconversion -std=c99 -I. -I.. -I../error -I../bytes -I../crc -I../ring_buffer -I../frame -I../flash -I../journal -I../activation -I../manifest -I../crypto -I../verify -I../session -I../transport -I../client fwu_pull.c fwu_http.c ../client/fwu_client.c ../session/fwu_session.c ../session/fwu_proto.c ../verify/fwu_verify.c ../manifest/fwu_manifest.c ../crypto/fwu_crypto.c ../crypto/fwu_crypto_sim.c ../activation/fwu_activate.c ../journal/fwu_journal_ab.c ../journal/fwu_journal.c ../flash/fwu_flash.c ../flash/fwu_flash_sim.c ../frame/fwu_frame.c ../frame/fwu_decode.c ../ring_buffer/fwu_ringbuf.c test_pull.c -o test_pull && ./test_pull
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fwu_pull.h"
#include "fwu_client.h"
#include "fwu_bytes.h"
#include "fwu_crypto_sim.h"
#include "fwu_flash_sim.h"

#define ELEMENTS(a) (sizeof(a) / sizeof((a)[0]))

/* Giris halkasi bu testte KUCUK (256) tutulur ki akis kontrolu ve tasma gercekten zorlansin: bir cevap (~410 bayt) halkadan buyuktur.
 * fwu_pull.c ile ayni deger -DFWU_PULL_INPUT_SIZE=256u ile iki derleme biriminin ikisine de verilmelidir (yapi bicimi buna bagli). */
#if FWU_PULL_INPUT_SIZE != 256u
#error "test_pull.c must be compiled with -DFWU_PULL_INPUT_SIZE=256u"
#endif

/* 19 sayfa x 256, granül 8:  0..7 uygulama | 8 manifest | 9..15 imaj (7 sayfa) | 16,17 aktivasyon journal | 18 geçici */
FWU_FLASH_SIM_DEFINE(ps, 19, 256, 8);

#define PAGE            256u
#define MANIFEST_OFFSET 2048u
#define IMAGE_OFFSET    2304u
#define IMAGE_MAX       1792u
#define PRODUCT         12345u
#define CHUNK           64u
#define IMG_SIZE        1500u               /* 23 tam parça + 28 baytlık son parça (28 % 8 = 4: granüle dolgu) */
#define RANGE_BYTES     256u                /* 4 parça: 1500 bayt = 5 tam aralık + 220 baytlık son aralık = 6 GET */
#define SESSION_IDLE_MS 1000u
#define PULL_TIMEOUT_MS 400u
#define PULL_RETRIES    3u
#define STEP_MS         5u
#define ERASED          0xFFu
#define FAULT_WRITE     FWU_ERR_FLASH_NOT_ERASED

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

/* ------------------------------------------------------------------ paket (test_session.c ile aynı kurgu) */

static fwu_byte_t g_pk[3][32];
static fwu_verify_key_t g_keys[2];
static fwu_byte_t g_manifest[138];
static fwu_byte_t g_image[IMAGE_MAX + 64u];

static fwu_byte_t img_byte(uint32_t i)
{
    return (fwu_byte_t)((i * 13u + 29u) & 0xFFu);
}

static fwu_byte_t old_byte(uint32_t i)
{
    return (fwu_byte_t)((i * 7u + 3u) & 0xFFu);
}

static void init_keys(void)
{
    for(uint32_t i = 0u; i < 32u; i++) {
        g_pk[0][i] = (fwu_byte_t)(i + 1u);
        g_pk[1][i] = (fwu_byte_t)(i + 101u);
        g_pk[2][i] = (fwu_byte_t)(i + 201u);
        g_keys[0].public_key[i] = g_pk[0][i];
        g_keys[1].public_key[i] = g_pk[1][i];
    }
    g_keys[0].id = 1u;
    g_keys[1].id = 2u;
}

static void put_u32(fwu_byte_t *m, uint32_t offset, uint32_t value)
{
    m[offset] = (fwu_byte_t)((value >> 24) & 0xFFu);
    m[offset + 1u] = (fwu_byte_t)((value >> 16) & 0xFFu);
    m[offset + 2u] = (fwu_byte_t)((value >> 8) & 0xFFu);
    m[offset + 3u] = (fwu_byte_t)(value & 0xFFu);
}

static void make_package(uint32_t size)
{
    for(uint32_t i = 0u; i < ELEMENTS(g_image); i++) {
        g_image[i] = img_byte(i);
    }
    g_manifest[0] = 0x46u; g_manifest[1] = 0x57u; g_manifest[2] = 0x4Du; g_manifest[3] = 0x46u;
    g_manifest[4] = 0x01u;
    g_manifest[5] = 1u;
    g_manifest[6] = 0x00u; g_manifest[7] = 0x00u;
    put_u32(g_manifest, 8u, PRODUCT);
    put_u32(g_manifest, 12u, 25u);
    g_manifest[16] = 0x00u;
    g_manifest[17] = 1u;
    put_u32(g_manifest, 18u, size);
    fwu_crypto_sim_sha256(g_image, size, &g_manifest[22]);
    for(uint32_t i = 0u; i < 20u; i++) {
        g_manifest[54u + i] = (fwu_byte_t)(0x30u + i);
    }
    fwu_crypto_sim_sign(g_pk[0], g_manifest, 74u, &g_manifest[74]);
}

/* ------------------------------------------------------------------ flash'ı saran hata enjektörü */

typedef struct {
    fwu_flash_t          flash;
    fwu_flash_callback_t callback;
    ps_t                *sim;
    uint32_t             fail_write_offset;     /* bu ofseti kapsayan yazma YAZMADAN FAULT_WRITE döner */
} ff_t;

#define NO_OFFSET 0xFFFFFFFFu

static fwu_return_e ff_erase(void *context, uint32_t offset, uint32_t length)
{
    ff_t *f = (ff_t *)context;

    return fwu_flash_erase(&f->sim->base.flash, offset, length);
}

static fwu_return_e ff_write(void *context, uint32_t offset, uint32_t length, const fwu_byte_t *data)
{
    ff_t *f = (ff_t *)context;

    if((f->fail_write_offset != NO_OFFSET) && (f->fail_write_offset >= offset) && (f->fail_write_offset < (offset + length))) {
        return FAULT_WRITE;
    }
    return fwu_flash_write(&f->sim->base.flash, offset, length, data);
}

static fwu_return_e ff_read(void *context, uint32_t offset, uint32_t length, fwu_byte_t *data)
{
    ff_t *f = (ff_t *)context;

    return fwu_flash_read(&f->sim->base.flash, offset, length, data);
}

static void ff_init(ff_t *f, ps_t *sim)
{
    f->callback.flash_erase = ff_erase;
    f->callback.flash_write = ff_write;
    f->callback.flash_read = ff_read;
    f->flash.context = f;
    f->flash.callback = &f->callback;
    f->flash.geometry = sim->base.flash.geometry;
    f->sim = sim;
    f->fail_write_offset = NO_OFFSET;
}

/* ------------------------------------------------------------------ dünya: flash + aktivasyon + oturum + çekme */

static ps_t g_sim;
static ff_t g_ff;
static fwu_crypto_sim_t g_cs;
static fwu_verify_config_t g_vc;
static fwu_activate_config_t g_ac;
static fwu_activate_t g_act;
static fwu_session_config_t g_cfg;
static fwu_session_t g_s;
static fwu_byte_t g_work[CHUNK];
static fwu_byte_t g_vwork[64];
static fwu_byte_t g_awork[64];
static fwu_byte_t g_chunk_buffer[RANGE_BYTES];            /* ARALIK tamponu: cevap tümüyle gelmeden flash'a yazılmaz */
static fwu_pull_t g_p;
static fwu_pull_config_t g_pc;
static uint32_t g_now;

/* ---- taklit sunucu ---- */

typedef enum {
    SRV_DEFAULT = 0,        /* bu istek için özel mod yok: default_mode kullanılır */
    SRV_OK,                 /* doğru 206 */
    SRV_CLOSE,              /* doğru 206 + "Connection: close" */
    SRV_200_FULL,           /* Range yok sayılır: 200 + tüm dosya */
    SRV_404,                /* 404 + kısa gövde */
    SRV_BAD_TOTAL,          /* doğru 206 ama toplam boyut yanlış */
    SRV_WRONG_RANGE,        /* istenenden BAŞKA bir aralık (eski/yinelenen cevap gibi) */
    SRV_SILENT,             /* hiç cevap yok */
    SRV_HALF,               /* gövdenin yarısı gelir, sonra sessizlik */
    SRV_GARBAGE,            /* HTTP olmayan bayt çöpü */
    SRV_NO_LENGTH           /* Content-Length yok */
} srv_mode_e;

#define SRV_RESP_MAX 2600u
#define SRV_LOG_MAX  256u

typedef struct {
    srv_mode_e  policy[SRV_LOG_MAX];            /* i. istek için mod (SRV_DEFAULT = default_mode) */
    srv_mode_e  default_mode;
    uint32_t    requests;
    uint32_t    first_of[SRV_LOG_MAX];
    uint32_t    last_of[SRV_LOG_MAX];
    char        host[64];
    char        target[160];
    uint32_t    claimed_total;                  /* normalde IMG_SIZE */
    fwu_byte_t  resp[SRV_RESP_MAX];
    size_t      resp_length;
    size_t      resp_position;
    uint32_t    busy_returns;                   /* ilk bu kadar send çağrısı BUFFER_FULL döner */
    fwu_return_e hard_error;                    /* FWU_OK değilse send bu hatayı döner */
    uint32_t    bad_requests;                   /* biçimi bozuk (ayrıştırılamayan) istek sayısı */
} srv_t;

static srv_t g_srv;

static void srv_reset(void)
{
    memset(&g_srv, 0, sizeof(g_srv));
    g_srv.default_mode = SRV_OK;
    g_srv.claimed_total = IMG_SIZE;
    g_srv.hard_error = FWU_OK;
}

static void srv_put(const char *text)
{
    for(size_t i = 0u; text[i] != '\0'; i++) {
        if(g_srv.resp_length < SRV_RESP_MAX) {
            g_srv.resp[g_srv.resp_length] = (fwu_byte_t)(unsigned char)text[i];
            g_srv.resp_length++;
        }
    }
}

static void srv_put_body(uint32_t first, uint32_t count)
{
    for(uint32_t i = 0u; (i < count) && (g_srv.resp_length < SRV_RESP_MAX); i++) {
        g_srv.resp[g_srv.resp_length] = img_byte(first + i);
        g_srv.resp_length++;
    }
}

/** Gelen istek metninden "Range: bytes=a-b" ve hedefi/host'u çıkarır. false = biçim bozuk. */
static bool srv_parse(const fwu_byte_t *data, size_t length, uint32_t *first, uint32_t *last)
{
    char text[FWU_HTTP_REQUEST_MAX + 1u];
    const char *p;
    unsigned long a = 0u;
    unsigned long b = 0u;
    char *end;

    if(length >= sizeof(text)) {
        return false;
    }
    for(size_t i = 0u; i < length; i++) {
        text[i] = (char)(data[i] & 0xFFu);
    }
    text[length] = '\0';
    if(strncmp(text, "GET ", 4) != 0) {
        return false;
    }
    if(length < 4u || strstr(text, " HTTP/1.1\r\n") == NULL || strstr(text, "\r\n\r\n") == NULL) {
        return false;
    }
    {
        const char *sp = strchr(&text[4], ' ');
        if(sp == NULL || (size_t)(sp - &text[4]) >= sizeof(g_srv.target)) {
            return false;
        }
        memcpy(g_srv.target, &text[4], (size_t)(sp - &text[4]));
        g_srv.target[(size_t)(sp - &text[4])] = '\0';
    }
    p = strstr(text, "Host: ");
    if(p == NULL) {
        return false;
    }
    {
        const char *e = strstr(p, "\r\n");
        size_t n = (e != NULL) ? (size_t)(e - (p + 6)) : 0u;
        if(n == 0u || n >= sizeof(g_srv.host)) {
            return false;
        }
        memcpy(g_srv.host, p + 6, n);
        g_srv.host[n] = '\0';
    }
    p = strstr(text, "Range: bytes=");
    if(p == NULL) {
        return false;
    }
    a = strtoul(p + 13, &end, 10);
    if(*end != '-') {
        return false;
    }
    b = strtoul(end + 1, &end, 10);
    if(*end != '\r' || b < a) {
        return false;
    }
    *first = (uint32_t)a;
    *last = (uint32_t)b;
    return true;
}

static void srv_build(srv_mode_e mode, uint32_t first, uint32_t last)
{
    char header[400];
    const uint32_t count = (last - first) + 1u;

    g_srv.resp_length = 0u;
    g_srv.resp_position = 0u;
    switch(mode) {
    case SRV_SILENT:
        return;
    case SRV_GARBAGE:
        for(uint32_t i = 0u; i < 40u; i++) {
            g_srv.resp[i] = (fwu_byte_t)(0x80u + i);
        }
        g_srv.resp_length = 40u;
        return;
    case SRV_200_FULL:
        (void)snprintf(header, sizeof(header), "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: %u\r\n\r\n", (unsigned)IMG_SIZE);
        srv_put(header);
        srv_put_body(0u, IMG_SIZE);
        return;
    case SRV_404:
        srv_put("HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 9\r\n\r\nnot found");
        return;
    case SRV_NO_LENGTH:
        (void)snprintf(header, sizeof(header), "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes %u-%u/%u\r\n\r\n",
                       (unsigned)first, (unsigned)last, (unsigned)g_srv.claimed_total);
        srv_put(header);
        srv_put_body(first, count);
        return;
    case SRV_WRONG_RANGE: {
        const uint32_t f2 = (first >= 64u) ? (first - 64u) : (first + 64u);
        const uint32_t l2 = f2 + (count - 1u);
        (void)snprintf(header, sizeof(header), "HTTP/1.1 206 Partial Content\r\nContent-Length: %u\r\nContent-Range: bytes %u-%u/%u\r\n\r\n",
                       (unsigned)count, (unsigned)f2, (unsigned)l2, (unsigned)g_srv.claimed_total);
        srv_put(header);
        srv_put_body(f2, count);
        return;
    }
    case SRV_BAD_TOTAL:
        (void)snprintf(header, sizeof(header), "HTTP/1.1 206 Partial Content\r\nContent-Length: %u\r\nContent-Range: bytes %u-%u/%u\r\n\r\n",
                       (unsigned)count, (unsigned)first, (unsigned)last, (unsigned)(IMG_SIZE + 10u));
        srv_put(header);
        srv_put_body(first, count);
        return;
    case SRV_HALF:
        (void)snprintf(header, sizeof(header), "HTTP/1.1 206 Partial Content\r\nContent-Length: %u\r\nContent-Range: bytes %u-%u/%u\r\n\r\n",
                       (unsigned)count, (unsigned)first, (unsigned)last, (unsigned)g_srv.claimed_total);
        srv_put(header);
        srv_put_body(first, count / 2u);
        return;
    case SRV_CLOSE:
    case SRV_OK:
    case SRV_DEFAULT:
    default:
        (void)snprintf(header, sizeof(header), "HTTP/1.1 206 Partial Content\r\nServer: test\r\nContent-Length: %u\r\nContent-Range: bytes %u-%u/%u\r\n%s\r\n",
                       (unsigned)count, (unsigned)first, (unsigned)last, (unsigned)g_srv.claimed_total,
                       (mode == SRV_CLOSE) ? "Connection: close\r\n" : "Connection: keep-alive\r\n");
        srv_put(header);
        srv_put_body(first, count);
        return;
    }
}

/** fwu_pull'un send kancası: isteği GERÇEKTEN ayrıştırır, cevabı kurar. */
static fwu_return_e srv_send(void *context, const fwu_byte_t *data, size_t length)
{
    uint32_t first = 0u;
    uint32_t last = 0u;
    srv_mode_e mode;

    (void)context;
    if(g_srv.hard_error != FWU_OK) {
        return g_srv.hard_error;
    }
    if(g_srv.busy_returns > 0u) {
        g_srv.busy_returns--;
        return FWU_ERR_BUFFER_FULL;
    }
    if(!srv_parse(data, length, &first, &last)) {
        g_srv.bad_requests++;
        g_srv.resp_length = 0u;
        g_srv.resp_position = 0u;
        return FWU_OK;
    }
    mode = ((g_srv.requests < SRV_LOG_MAX) && (g_srv.policy[g_srv.requests] != SRV_DEFAULT)) ? g_srv.policy[g_srv.requests] : g_srv.default_mode;
    if(g_srv.requests < SRV_LOG_MAX) {
        g_srv.first_of[g_srv.requests] = first;
        g_srv.last_of[g_srv.requests] = last;
    }
    g_srv.requests++;
    srv_build(mode, first, last);
    return FWU_OK;
}

/** Çekme için temiz dünya: boş flash, eski uygulama, paket, oturum BOŞTA, çekme BOŞTA. */
static void world(void)
{
    (void)fwu_flash_sim_init_raw(&g_sim.base, g_sim.memory, g_sim.erase_counts, 19u, PAGE, 8u);
    init_keys();
    (void)fwu_crypto_sim_init(&g_cs);
    ff_init(&g_ff, &g_sim);
    for(uint32_t i = 0u; i < 8u * PAGE; i++) {
        g_sim.base.memory[i] = old_byte(i);
    }
    g_vc.crypto = &g_cs.crypto;
    g_vc.manifest_offset = MANIFEST_OFFSET;
    g_vc.image_offset = IMAGE_OFFSET;
    g_vc.max_image_size = IMAGE_MAX;
    g_vc.product_id = PRODUCT;
    g_vc.min_image_version = 25u;
    g_vc.min_key_version = 1u;
    g_vc.keys = g_keys;
    g_vc.key_count = 2u;
    g_vc.require_signature = true;
    g_vc.work = g_vwork;
    g_vc.work_length = 64u;
    g_ac.flash = &g_ff.flash;
    g_ac.mode = FWU_ACTIVATE_MODE_COPY;
    g_ac.app_offset = 0u;
    g_ac.staging_offset = IMAGE_OFFSET;
    g_ac.region_length = IMAGE_MAX;
    g_ac.journal_a_offset = 16u * PAGE;
    g_ac.journal_b_offset = 17u * PAGE;
    g_ac.journal_length = PAGE;
    g_ac.scratch_offset = 18u * PAGE;
    g_ac.scratch_pages = 1u;
    g_ac.work = g_awork;
    g_ac.work_length = 64u;
    g_ac.max_trial_boots = 2u;
    g_ac.require_verify = true;
    g_ac.verify = fwu_verify_activation_hook;
    g_ac.verify_context = &g_vc;
    g_ac.on_confirmed = NULL;
    g_ac.on_confirmed_context = NULL;
    (void)fwu_activate_init(&g_act, &g_ac);
    g_cfg.flash = &g_ff.flash;
    g_cfg.activate = &g_act;
    g_cfg.verify = &g_vc;
    g_cfg.auth = NULL;
    g_cfg.require_auth = false;
    g_cfg.running_image_version = 20u;
    g_cfg.chunk_size = CHUNK;
    g_cfg.idle_timeout_ms = SESSION_IDLE_MS;
    g_cfg.auth_max_failures = 3u;
    g_cfg.auth_lockout_ms = 5000u;
    g_cfg.work = g_work;
    g_cfg.work_length = CHUNK;
    (void)fwu_session_init(&g_s, &g_cfg);
    make_package(IMG_SIZE);

    srv_reset();
    g_pc.session = &g_s;
    g_pc.send = srv_send;
    g_pc.send_context = NULL;
    g_pc.chunk_buffer = g_chunk_buffer;
    g_pc.chunk_buffer_length = RANGE_BYTES;
    g_pc.range_bytes = RANGE_BYTES;
    g_pc.response_timeout_ms = PULL_TIMEOUT_MS;
    g_pc.max_retries = PULL_RETRIES;
    (void)fwu_pull_init(&g_p, &g_pc);
    g_now = 1000u;
    fwu_pull_set_link(&g_p, true, g_now);          /* soket bagli (test_link bunu ozellikle kapatir) */
}

/* ------------------------------------------------------------------ host: çerçeve gönder, yanıtı çöz */

typedef struct {
    fwu_return_e rc;
    size_t       length;
    int          decoded;
    fwu_byte_t   type;
    fwu_byte_t   seq;
    fwu_byte_t   payload[FWU_FRAME_MAX_PAYLOAD];
    size_t       payload_length;
} resp_t;

static resp_t g_r;

static void decode_response(const fwu_byte_t *buf, size_t n)
{
    fwu_frame_view_t view;

    g_r.length = n;
    g_r.decoded = 0;
    g_r.payload_length = 0u;
    if(n == 0u) {
        return;
    }
    if(fwu_frame_decode(buf, n, &view) == FWU_OK) {
        g_r.decoded = (view.frame_length == n) ? 1 : 0;
        g_r.type = view.type;
        g_r.seq = view.seq;
        g_r.payload_length = view.payload_length;
        for(size_t i = 0u; i < view.payload_length; i++) {
            g_r.payload[i] = view.payload[i];
        }
    }
}

/** to_pull = true: çerçeve fwu_pull'a, değilse oturuma verilir (cihaz yapıştırıcısının dağıtımı). */
static void host(fwu_byte_t type, fwu_byte_t seq, const fwu_byte_t *payload, size_t payload_length)
{
    fwu_byte_t frame[FWU_FRAME_MAX_SIZE];
    fwu_byte_t out[FWU_FRAME_MAX_SIZE];
    size_t frame_length = 0u;
    size_t out_length = 0u;
    fwu_frame_view_t view;

    (void)fwu_frame_encode(type, seq, payload, payload_length, frame, ELEMENTS(frame), &frame_length);
    (void)fwu_frame_decode(frame, frame_length, &view);
    if(fwu_pull_handles(type)) {
        g_r.rc = fwu_pull_on_frame(&g_p, &view, g_now, out, ELEMENTS(out), &out_length);
    }
    else {
        g_r.rc = fwu_session_on_frame(&g_s, &view, g_now, out, ELEMENTS(out), &out_length);
    }
    decode_response(out, (g_r.rc == FWU_OK) ? out_length : 0u);
}

static void host1(fwu_byte_t type, fwu_byte_t seq)
{
    const fwu_byte_t one[1] = { 0u };

    host(type, seq, one, 1u);
}

static int resp_is_error(fwu_return_e code)
{
    return (g_r.rc == FWU_OK) && g_r.decoded && (g_r.type == FWU_MSG_ERROR) && (g_r.payload_length == 2u) &&
           (g_r.payload[0] == (fwu_byte_t)code);
}

static const fwu_byte_t SERVER_IP[4] = { 192u, 168u, 1u, 20u };
#define SERVER_PORT 8080u
#define TARGET_TEXT "/api/com/tcp-config/getFwUpdateFile?deviceType=fan&version=12"

static void pull_start_frame(const fwu_byte_t *ip, uint16_t port, const char *target, fwu_byte_t seq)
{
    fwu_byte_t payload[FWU_FRAME_MAX_PAYLOAD];
    size_t length = 0u;
    size_t tl = strlen(target);
    fwu_byte_t t[FWU_PROTO_PULL_TARGET_MAX + 8u];

    for(size_t i = 0u; (i < tl) && (i < ELEMENTS(t)); i++) {
        t[i] = (fwu_byte_t)(unsigned char)target[i];
    }
    (void)fwu_proto_encode_pull_start(ip, port, t, tl, payload, ELEMENTS(payload), &length);
    host(FWU_MSG_PULL_START, seq, payload, length);
}

static void start_pull(void)
{
    pull_start_frame(SERVER_IP, SERVER_PORT, TARGET_TEXT, 50u);
}

static int status_is(fwu_byte_t reply_type, fwu_byte_t state)
{
    fwu_proto_pull_status_t st;

    return (g_r.rc == FWU_OK) && g_r.decoded && (g_r.type == reply_type) &&
           (fwu_proto_parse_pull_status(g_r.payload, g_r.payload_length, &st) == FWU_OK) && (st.state == state);
}

static fwu_proto_pull_status_t last_status(void)
{
    fwu_proto_pull_status_t st;

    st.state = 99u; st.next_offset = 0u; st.error = 0u; st.http_status = 0u;
    (void)fwu_proto_parse_pull_status(g_r.payload, g_r.payload_length, &st);
    return st;
}

/** REQUEST_DOWNLOAD (manifest) -> DOWNLOAD_ACCEPTED. */
static int accept_download(void)
{
    host(FWU_MSG_REQUEST_DOWNLOAD, 1u, g_manifest, 138u);
    return (g_r.rc == FWU_OK) && g_r.decoded && (g_r.type == FWU_MSG_DOWNLOAD_ACCEPTED);
}

static int staging_matches(uint32_t length)
{
    for(uint32_t i = 0u; i < length; i++) {
        if(g_sim.base.memory[IMAGE_OFFSET + i] != img_byte(i)) {
            return 0;
        }
    }
    return 1;
}

static int staging_untouched(uint32_t from, uint32_t length)
{
    for(uint32_t i = from; i < (from + length); i++) {
        if(g_sim.base.memory[IMAGE_OFFSET + i] != ERASED) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ sürücü döngü: zaman + sunucudan gelen baytlar */

static uint32_t g_rng = 4242u;

static uint32_t rnd(void)
{
    g_rng = (g_rng * 1664525u) + 1013904223u;
    return g_rng >> 8;
}

typedef struct {
    size_t   piece_min;                 /* sunucudan bir adımda iletilen en az bayt */
    size_t   piece_max;                 /* en çok bayt (rastgele) */
    int      random_pieces;
    uint32_t drop_at_body_position;     /* != 0: cevap bu konuma gelince bağlantı koparılır (bir kez) */
    int      drop_done;
    uint32_t reconnect_in_steps;        /* kopmadan kaç adım sonra yeniden bağlanılır */
    int      reconnecting;
    uint32_t overflow_burst;            /* != 0: akış kontrolü İHLAL edilir: bu kadar bayt bir adımda zorla verilir (bir kez) */
    int      overflow_done;
    int      close_cycles_link;         /* Connection: close cevabından sonra sunucu bağlantıyı kapatır (link düşer-kalkar) */
    uint32_t last_complete_responses;
} drive_t;

static void drive_default(drive_t *d)
{
    memset(d, 0, sizeof(*d));
    d->piece_min = 1u;
    d->piece_max = 40u;
    d->random_pieces = 1;
}

/** Bir adım: zaman ilerler, sunucunun bekleyen cevabından bir parça akış kontrolüyle iletilir, poll çalışır. */
static void step(drive_t *d)
{
    fwu_pull_stats_t stats;

    g_now += STEP_MS;
    if(d->reconnecting) {
        if(d->reconnect_in_steps > 0u) {
            d->reconnect_in_steps--;
        }
        if(d->reconnect_in_steps == 0u) {
            d->reconnecting = 0;
            fwu_pull_set_link(&g_p, true, g_now);
        }
    }
    if((g_srv.resp_position < g_srv.resp_length) && !d->reconnecting) {
        size_t piece = d->random_pieces ? (d->piece_min + (size_t)(rnd() % ((d->piece_max - d->piece_min) + 1u))) : d->piece_min;
        const size_t remaining = g_srv.resp_length - g_srv.resp_position;

        if(piece > remaining) {
            piece = remaining;
        }
        if(d->overflow_burst != 0u && !d->overflow_done && g_srv.resp_position > 0u) {
            /* akış kontrolü ihlali: halka boşalmadan çok bayt basılır */
            size_t burst = d->overflow_burst;
            if(burst > remaining) {
                burst = remaining;
            }
            d->overflow_done = 1;
            (void)fwu_pull_feed(&g_p, &g_srv.resp[g_srv.resp_position], burst, g_now);
            g_srv.resp_position += burst;
        }
        else if(fwu_pull_input_free(&g_p) >= piece) {            /* GERÇEK yapıştırıcının akış kontrolü */
            if(d->drop_at_body_position != 0u && !d->drop_done && (g_srv.resp_position + piece) > d->drop_at_body_position) {
                d->drop_done = 1;                               /* bağlantı koptu: cevabın kalanı hiç gelmez */
                g_srv.resp_length = 0u;
                g_srv.resp_position = 0u;
                fwu_pull_set_link(&g_p, false, g_now);
                d->reconnecting = 1;
            }
            else {
                (void)fwu_pull_feed(&g_p, &g_srv.resp[g_srv.resp_position], piece, g_now);
                g_srv.resp_position += piece;
            }
        }
    }
    fwu_pull_poll(&g_p, g_now);
    fwu_pull_get_stats(&g_p, &stats);
    if(d->close_cycles_link && (stats.responses != d->last_complete_responses)) {
        d->last_complete_responses = stats.responses;
        if(fwu_pull_active(&g_p) && g_srv.default_mode == SRV_CLOSE) {
            fwu_pull_set_link(&g_p, false, g_now);              /* sunucu kapattı */
            d->reconnecting = 1;
            d->reconnect_in_steps = 3u;
        }
    }
}

/** Çekme bitene (artık aktif değil) ya da max_steps'e kadar çalışır. Dönüş: harcanan adım. */
static uint32_t run(drive_t *d, uint32_t max_steps)
{
    uint32_t n = 0u;

    while((n < max_steps) && fwu_pull_active(&g_p)) {
        step(d);
        n++;
    }
    return n;
}

static fwu_proto_pull_status_t pull_status(void)
{
    fwu_proto_pull_status_t st;

    fwu_pull_get_status(&g_p, &st);
    return st;
}

/** EXIT -> poll -> VERIFY_RESULT(0) -> ACTIVATE -> ACTIVATE_OK. true = tamam. */
static int finish_update(void)
{
    fwu_byte_t out[FWU_FRAME_MAX_SIZE];
    size_t out_length = 0u;
    fwu_byte_t sz[4];
    size_t n = 0u;
    int ok = 1;

    (void)fwu_proto_encode_u32(IMG_SIZE, sz, 4u, &n);
    host(FWU_MSG_TRANSFER_EXIT, 60u, sz, 4u);
    ok = ok && (g_r.rc == FWU_OK) && g_r.decoded && (g_r.type == FWU_MSG_EXIT_PENDING);
    (void)fwu_session_poll(&g_s, g_now, out, ELEMENTS(out), &out_length);
    decode_response(out, out_length);
    ok = ok && g_r.decoded && (g_r.type == FWU_MSG_VERIFY_RESULT) && (g_r.payload_length == 1u) && (g_r.payload[0] == 0u);
    host(FWU_MSG_ACTIVATE, 61u, sz, 4u);
    ok = ok && (g_r.rc == FWU_OK) && g_r.decoded && (g_r.type == FWU_MSG_ACTIVATE_OK);
    return ok && fwu_session_reboot_requested(&g_s);
}

/* ------------------------------------------------------------------ testler */

static void test_init(void)
{
    fwu_pull_t p;
    fwu_pull_config_t c;

    world();
    c = g_pc;
    expect("init: geçerli yapılandırma", fwu_pull_init(&p, &c), FWU_OK);
    expect("init: pull NULL", fwu_pull_init(NULL, &c), FWU_ERR_INVALID_ARG);
    expect("init: config NULL", fwu_pull_init(&p, NULL), FWU_ERR_INVALID_ARG);
    c = g_pc; c.session = NULL;
    expect("init: session NULL", fwu_pull_init(&p, &c), FWU_ERR_INVALID_ARG);
    c = g_pc; c.send = NULL;
    expect("init: send NULL", fwu_pull_init(&p, &c), FWU_ERR_INVALID_ARG);
    c = g_pc; c.chunk_buffer = NULL;
    expect("init: chunk_buffer NULL", fwu_pull_init(&p, &c), FWU_ERR_INVALID_ARG);
    c = g_pc; c.chunk_buffer_length = RANGE_BYTES - 1u;
    expect("init: chunk_buffer, range_bytes'ten küçük", fwu_pull_init(&p, &c), FWU_ERR_INVALID_ARG);
    c = g_pc; c.response_timeout_ms = 0u;
    expect("init: zaman aşımı 0", fwu_pull_init(&p, &c), FWU_ERR_INVALID_ARG);
    c = g_pc; c.range_bytes = 0u;
    expect("init: range_bytes 0", fwu_pull_init(&p, &c), FWU_ERR_INVALID_ARG);
    c = g_pc; c.range_bytes = CHUNK + 1u;
    expect("init: range_bytes, parça boyutunun katı değil", fwu_pull_init(&p, &c), FWU_ERR_INVALID_ARG);
    c = g_pc; c.range_bytes = CHUNK;
    expect("init: range_bytes == chunk_size geçerli", fwu_pull_init(&p, &c), FWU_OK);
    {
        fwu_session_t cold;
        memset(&cold, 0, sizeof(cold));
        c = g_pc; c.session = &cold;
        expect("init: başlatılmamış oturum: BAD_STATE", fwu_pull_init(&p, &c), FWU_ERR_BAD_STATE);
    }
    check("başlatılmamış denetleyiciye sorgular/işlemler güvenli",
          !fwu_pull_active(NULL) && fwu_pull_input_free(NULL) == 0u && fwu_pull_input_used(NULL) == 0u);
    fwu_pull_poll(NULL, 0u);
    fwu_pull_set_link(NULL, true, 0u);
    fwu_pull_abort(NULL);
    check("NULL'a poll/set_link/abort çökmez", 1);
    check("başlatılmamış denetleyici feed reddeder", fwu_pull_feed(NULL, g_manifest, 1u, 0u) == FWU_ERR_INVALID_ARG);
}

static void test_protocol(void)
{
    fwu_byte_t payload[FWU_FRAME_MAX_PAYLOAD] = { 0u };
    size_t length = 0u;
    fwu_proto_pull_status_t st;

    world();
    check("fwu_pull_handles: PULL_START, PULL_QUERY evet", fwu_pull_handles(FWU_MSG_PULL_START) && fwu_pull_handles(FWU_MSG_PULL_QUERY));
    check("fwu_pull_handles: diğer türler hayır", !fwu_pull_handles(FWU_MSG_HELLO) && !fwu_pull_handles(FWU_MSG_TRANSFER_DATA) &&
                                                   !fwu_pull_handles(FWU_MSG_PULL_STARTED) && !fwu_pull_handles(0x00u));
    check("kirli üst bitli tür (C28x) tanınır", fwu_pull_handles((fwu_byte_t)(0x100u | FWU_MSG_PULL_QUERY)));

    /* oturum İNDİRİYOR değilken PULL_START: BAD_STATE */
    start_pull();
    check("oturum BOŞTA iken PULL_START: ERROR(BAD_STATE)", resp_is_error(FWU_ERR_BAD_STATE));
    host1(FWU_MSG_PULL_QUERY, 51u);
    check("PULL_QUERY (çekme hiç başlamadı): PULL_STATUS, durum IDLE", status_is(FWU_MSG_PULL_STATUS, FWU_PROTO_PULL_IDLE));
    check("yanıtın SEQ'i isteğinkiyle aynı", g_r.seq == 51u);

    check("REQUEST_DOWNLOAD kabul", accept_download());

    /* geçersiz yükler */
    host(FWU_MSG_PULL_START, 52u, payload, 5u);
    check("PULL_START çok kısa yük: ERROR(BAD_LENGTH)", resp_is_error(FWU_ERR_BAD_LENGTH));
    {
        /* gövdeyi bozulmuş kopyayla: uzunluk alanını artır */
        fwu_byte_t good[FWU_FRAME_MAX_PAYLOAD];
        const fwu_byte_t t[3] = { '/', 'a', 'b' };
        (void)fwu_proto_encode_pull_start(SERVER_IP, 80u, t, 3u, good, ELEMENTS(good), &length);
        good[6] = 9u;
        host(FWU_MSG_PULL_START, 54u, good, length);
        check("PULL_START uzunluk alanı yükle uyuşmuyor: ERROR(BAD_LENGTH)", resp_is_error(FWU_ERR_BAD_LENGTH));
        (void)fwu_proto_encode_pull_start(SERVER_IP, 80u, t, 3u, good, ELEMENTS(good), &length);
        good[4] = 0u; good[5] = 0u;
        host(FWU_MSG_PULL_START, 55u, good, length);
        check("PULL_START port 0: ERROR(INVALID_ARG)", resp_is_error(FWU_ERR_INVALID_ARG));
        (void)fwu_proto_encode_pull_start(SERVER_IP, 80u, t, 3u, good, ELEMENTS(good), &length);
        good[8] = 0x20u;
        host(FWU_MSG_PULL_START, 56u, good, length);
        check("PULL_START hedefte boşluk: ERROR(INVALID_ARG)", resp_is_error(FWU_ERR_INVALID_ARG));
    }
    check("geçersiz yükler çekmeyi başlatmaz", pull_status().state == FWU_PROTO_PULL_IDLE && !fwu_pull_active(&g_p));
    host(FWU_MSG_PULL_QUERY, 57u, payload, 2u);
    check("PULL_QUERY 2 baytlık yük: ERROR(BAD_LENGTH)", resp_is_error(FWU_ERR_BAD_LENGTH));

    /* geçerli başlatma */
    start_pull();
    check("PULL_START: PULL_STARTED, durum REQUESTING", status_is(FWU_MSG_PULL_STARTED, FWU_PROTO_PULL_REQUESTING) && g_r.seq == 50u);
    st = last_status();
    check("başlangıç durumu: ofset 0, hata 0, http 0", st.next_offset == 0u && st.error == 0u && st.http_status == 0u);
    check("çekme aktif", fwu_pull_active(&g_p));

    /* AYNI hedefle tekrar: idempotent */
    start_pull();
    check("AYNI PULL_START tekrarı: yeniden onay, durum değişmez", status_is(FWU_MSG_PULL_STARTED, FWU_PROTO_PULL_REQUESTING));
    /* FARKLI hedef: BAD_STATE */
    pull_start_frame(SERVER_IP, SERVER_PORT, "/baska/dosya.bin", 58u);
    check("çekme sürerken FARKLI hedef: ERROR(BAD_STATE)", resp_is_error(FWU_ERR_BAD_STATE));
    {
        const fwu_byte_t other_ip[4] = { 10u, 0u, 0u, 1u };
        pull_start_frame(other_ip, SERVER_PORT, TARGET_TEXT, 59u);
        check("çekme sürerken FARKLI ip: ERROR(BAD_STATE)", resp_is_error(FWU_ERR_BAD_STATE));
        pull_start_frame(SERVER_IP, SERVER_PORT + 1u, TARGET_TEXT, 59u);
        check("çekme sürerken FARKLI port: ERROR(BAD_STATE)", resp_is_error(FWU_ERR_BAD_STATE));
    }

    /* bilinmeyen tür fwu_pull'a verilirse */
    {
        fwu_byte_t frame[FWU_FRAME_MAX_SIZE];
        fwu_byte_t out[FWU_FRAME_MAX_SIZE];
        size_t fl = 0u;
        size_t ol = 777u;
        fwu_frame_view_t view;
        const fwu_byte_t one[1] = { 0u };

        (void)fwu_frame_encode(FWU_MSG_HELLO, 1u, one, 1u, frame, ELEMENTS(frame), &fl);
        (void)fwu_frame_decode(frame, fl, &view);
        expect("fwu_pull_on_frame: HELLO bu modüle ait değil -> UNKNOWN_MESSAGE", fwu_pull_on_frame(&g_p, &view, g_now, out, ELEMENTS(out), &ol), FWU_ERR_UNKNOWN_MESSAGE);
        check("yabancı türde yanıt yazılmaz (out_length değişmez)", ol == 777u);
        (void)fwu_frame_encode(FWU_MSG_PULL_QUERY, 1u, one, 1u, frame, ELEMENTS(frame), &fl);
        (void)fwu_frame_decode(frame, fl, &view);
        expect("çıkış tamponu çok küçük: BUFFER_TOO_SMALL", fwu_pull_on_frame(&g_p, &view, g_now, out, FWU_PULL_MIN_OUT - 1u, &ol), FWU_ERR_BUFFER_TOO_SMALL);
        expect("out NULL: INVALID_ARG", fwu_pull_on_frame(&g_p, &view, g_now, NULL, 100u, &ol), FWU_ERR_INVALID_ARG);
        expect("request NULL: INVALID_ARG", fwu_pull_on_frame(&g_p, NULL, g_now, out, ELEMENTS(out), &ol), FWU_ERR_INVALID_ARG);
    }
}

static void test_happy(void)
{
    static const size_t pieces[] = { 1u, 3u, 40u, 64u, 120u };
    drive_t d;
    fwu_pull_stats_t stats;
    fwu_proto_pull_status_t st;

    for(size_t k = 0u; k < ELEMENTS(pieces); k++) {
        char name[160];
        uint32_t steps;

        world();
        (void)accept_download();
        start_pull();
        drive_default(&d);
        d.random_pieces = 0;
        d.piece_min = pieces[k];
        d.piece_max = pieces[k];
        steps = run(&d, 200000u);
        st = pull_status();
        fwu_pull_get_stats(&g_p, &stats);
        (void)snprintf(name, sizeof(name), "mutlu yol (adımda %u bayt): DONE, ofset %u, staging imajla BİREBİR aynı", (unsigned)pieces[k], (unsigned)IMG_SIZE);
        check(name, st.state == FWU_PROTO_PULL_DONE && st.next_offset == IMG_SIZE && st.error == 0u && staging_matches(IMG_SIZE) && steps < 200000u);
        (void)snprintf(name, sizeof(name), "mutlu yol (adımda %u bayt): 6 istek, 6 cevap, 24 parça, 0 yeniden deneme, 0 taşma", (unsigned)pieces[k]);
        check(name, stats.requests == 6u && stats.responses == 6u && stats.chunks_written == 24u && stats.retries == 0u && stats.rx_overflows == 0u);
        (void)snprintf(name, sizeof(name), "mutlu yol (adımda %u bayt): sunucu 6 aralığı görür ve aralıklar bitişik", (unsigned)pieces[k]);
        check(name, g_srv.requests == 6u && g_srv.first_of[0] == 0u && g_srv.last_of[0] == 255u && g_srv.first_of[1] == 256u &&
                        g_srv.first_of[5] == 1280u && g_srv.last_of[5] == IMG_SIZE - 1u && g_srv.bad_requests == 0u);
    }

    /* istek metni: Host ve hedef sunucuya doğru ulaşır */
    world();
    (void)accept_download();
    start_pull();
    drive_default(&d);
    (void)run(&d, 200000u);
    check("sunucu istekten Host: 192.168.1.20:8080 ve hedefi okudu", strcmp(g_srv.host, "192.168.1.20:8080") == 0 && strcmp(g_srv.target, TARGET_TEXT) == 0);

    /* DONE'dan sonra: PULL_QUERY, idempotent PULL_START; sonra EXIT -> VERIFY -> ACTIVATE */
    host1(FWU_MSG_PULL_QUERY, 70u);
    st = last_status();
    check("DONE: PULL_QUERY durum DONE, ofset = imaj boyutu", status_is(FWU_MSG_PULL_STATUS, FWU_PROTO_PULL_DONE) && st.next_offset == IMG_SIZE && st.http_status == 206u);
    start_pull();
    check("DONE sonrası aynı PULL_START: DONE olarak yeniden onay (yeniden indirme yok)", status_is(FWU_MSG_PULL_STARTED, FWU_PROTO_PULL_DONE) && g_srv.requests == 6u);
    check("TRANSFER_EXIT -> VERIFY_RESULT(0) -> ACTIVATE_OK: gerçek imza/SHA-256 doğrulaması geçer", finish_update());

    /* rastgele parçalarla da aynı: 30 tohum */
    {
        int all_ok = 1;
        for(int seed = 0; seed < 30; seed++) {
            g_rng = 1000u + (uint32_t)seed;
            world();
            (void)accept_download();
            start_pull();
            drive_default(&d);
            d.piece_max = 1u + (size_t)(rnd() % 120u);
            (void)run(&d, 200000u);
            if(!(pull_status().state == FWU_PROTO_PULL_DONE && staging_matches(IMG_SIZE))) {
                all_ok = 0;
            }
        }
        check("30 rastgele parçalama: her seferinde DONE ve staging birebir aynı", all_ok);
    }
}

static void test_permanent_errors(void)
{
    drive_t d;
    fwu_proto_pull_status_t st;
    fwu_pull_stats_t stats;

    /* 404 */
    world();
    (void)accept_download();
    g_srv.default_mode = SRV_404;
    start_pull();
    drive_default(&d);
    (void)run(&d, 20000u);
    st = pull_status();
    fwu_pull_get_stats(&g_p, &stats);
    check("404: FAILED, hata PULL_HTTP_STATUS (81), http 404", st.state == FWU_PROTO_PULL_FAILED && st.error == (fwu_byte_t)FWU_ERR_PULL_HTTP_STATUS && st.http_status == 404u);
    check("404: kalıcı hata yeniden denenmez (1 istek), flash'a tek bayt yazılmadı", g_srv.requests == 1u && stats.retries == 0u && st.next_offset == 0u && staging_untouched(0u, 256u));
    host1(FWU_MSG_PULL_QUERY, 80u);
    check("404: PULL_QUERY hatayı host'a bildirir", status_is(FWU_MSG_PULL_STATUS, FWU_PROTO_PULL_FAILED) && last_status().error == 81u && last_status().http_status == 404u);
    check("404: oturum açık kalır (host aynı manifest ile yeniden deneyebilir)", fwu_session_get_state(&g_s) == FWU_SESSION_DOWNLOADING);

    /* 200: Range yok sayıldı */
    world();
    (void)accept_download();
    g_srv.default_mode = SRV_200_FULL;
    start_pull();
    drive_default(&d);
    d.piece_max = 120u;
    (void)run(&d, 20000u);
    st = pull_status();
    check("200 (Range yok sayıldı): FAILED, PULL_HTTP_STATUS, http 200, hiçbir bayt yazılmadı",
          st.state == FWU_PROTO_PULL_FAILED && st.error == 81u && st.http_status == 200u && st.next_offset == 0u && staging_untouched(0u, 512u));

    /* yanlış toplam boyut */
    world();
    (void)accept_download();
    g_srv.default_mode = SRV_BAD_TOTAL;
    start_pull();
    drive_default(&d);
    (void)run(&d, 20000u);
    st = pull_status();
    check("yanlış toplam boyut: FAILED, PULL_SIZE_MISMATCH (83), hiçbir bayt yazılmadı, yeniden denenmez",
          st.state == FWU_PROTO_PULL_FAILED && st.error == (fwu_byte_t)FWU_ERR_PULL_SIZE_MISMATCH && st.next_offset == 0u && staging_untouched(0u, 512u) && g_srv.requests == 1u);

    /* FAILED sonrası aynı PULL_START: yeniden başlatır (sunucu düzeldi) */
    g_srv.default_mode = SRV_OK;
    g_srv.requests = 0u;
    start_pull();
    check("FAILED sonrası PULL_START yeniden başlatır (REQUESTING)", status_is(FWU_MSG_PULL_STARTED, FWU_PROTO_PULL_REQUESTING));
    (void)run(&d, 200000u);
    check("sunucu düzelince tamamlanır: DONE, staging birebir aynı", pull_status().state == FWU_PROTO_PULL_DONE && staging_matches(IMG_SIZE) && g_srv.first_of[0] == 0u);
}

static void test_transient_errors(void)
{
    drive_t d;
    fwu_proto_pull_status_t st;
    fwu_pull_stats_t stats;
    uint32_t steps;

    /* ilk iki istek cevapsız: zaman aşımı + yeniden deneme, sonra başarı */
    world();
    (void)accept_download();
    g_srv.policy[0] = SRV_SILENT;
    g_srv.policy[1] = SRV_SILENT;
    start_pull();
    drive_default(&d);
    steps = run(&d, 200000u);
    st = pull_status();
    fwu_pull_get_stats(&g_p, &stats);
    check("2 sessiz cevap: kurtarılır, DONE, staging birebir aynı", st.state == FWU_PROTO_PULL_DONE && staging_matches(IMG_SIZE) && steps < 200000u);
    check("2 sessiz cevap: tam 2 yeniden deneme, ilk aralık 3 kez istendi", stats.retries == 2u && g_srv.requests == 8u && g_srv.first_of[0] == 0u && g_srv.first_of[1] == 0u && g_srv.first_of[2] == 0u && g_srv.first_of[3] == 256u);

    /* sunucu hiç cevap vermez: hak bitince vazgeç */
    world();
    (void)accept_download();
    g_srv.default_mode = SRV_SILENT;
    start_pull();
    drive_default(&d);
    steps = run(&d, 200000u);
    st = pull_status();
    fwu_pull_get_stats(&g_p, &stats);
    check("sunucu hep sessiz: FAILED, PULL_TIMEOUT (82)", st.state == FWU_PROTO_PULL_FAILED && st.error == (fwu_byte_t)FWU_ERR_PULL_TIMEOUT && steps < 200000u);
    check("hep sessiz: 1 + max_retries istek, flash'a dokunulmadı", g_srv.requests == (1u + PULL_RETRIES) && stats.retries == (PULL_RETRIES + 1u) && staging_untouched(0u, 256u));
    check("zaman aşımı süresi: toplam bekleme ~ (1+retries) x timeout + geri çekilme (500..3000 ms arası)",
          (steps * STEP_MS) >= (PULL_RETRIES * PULL_TIMEOUT_MS) && (steps * STEP_MS) < 3500u);

    /* bozuk (HTTP olmayan) cevap, sonra normal */
    world();
    (void)accept_download();
    g_srv.policy[0] = SRV_GARBAGE;
    g_srv.policy[1] = SRV_NO_LENGTH;
    g_srv.policy[2] = SRV_WRONG_RANGE;
    start_pull();
    drive_default(&d);
    (void)run(&d, 200000u);
    fwu_pull_get_stats(&g_p, &stats);
    check("çöp, Content-Length'siz ve yanlış aralıklı cevaplar yeniden denenir, sonra DONE ve staging birebir aynı",
          pull_status().state == FWU_PROTO_PULL_DONE && staging_matches(IMG_SIZE) && stats.retries == 3u);

    /* yarım gövde, sonra sessizlik: zaman aşımı; yarım parça flash'a ÇIKMAZ */
    world();
    (void)accept_download();
    g_srv.policy[0] = SRV_HALF;
    start_pull();
    drive_default(&d);
    (void)run(&d, 200000u);
    fwu_pull_get_stats(&g_p, &stats);
    check("yarım gövde: zaman aşımıyla yeniden istenir, DONE, staging birebir aynı", pull_status().state == FWU_PROTO_PULL_DONE && staging_matches(IMG_SIZE) && stats.retries == 1u);
    check("yarım gövde hiçbir şey yazmaz (aralık tümüyle gelmeden flash'a basılmaz): yeniden istek aynı aralığın BAŞINDAN (0'dan) başlar", g_srv.first_of[1] == 0u);

    /* hak bitince vazgeçme VE yeniden başlatma: ilerleme korunur */
    world();
    (void)accept_download();
    g_srv.policy[0] = SRV_OK;
    g_srv.policy[1] = SRV_OK;
    for(uint32_t i = 2u; i < SRV_LOG_MAX; i++) {
        g_srv.policy[i] = SRV_SILENT;
    }
    start_pull();
    drive_default(&d);
    (void)run(&d, 200000u);
    st = pull_status();
    check("2 aralık sonra sunucu kesilir: FAILED(82), 512 bayt yazılı", st.state == FWU_PROTO_PULL_FAILED && st.error == 82u && st.next_offset == 512u && staging_matches(512u));
    memset(g_srv.policy, 0, sizeof(g_srv.policy));
    g_srv.requests = 0u;
    start_pull();
    check("PULL_START kaldığı yerden yeniden başlatır (ofset 512 korunur)", status_is(FWU_MSG_PULL_STARTED, FWU_PROTO_PULL_REQUESTING) && last_status().next_offset == 512u);
    (void)run(&d, 200000u);
    check("devamdan tamamlanır: ilk istek 512'den, staging birebir aynı", pull_status().state == FWU_PROTO_PULL_DONE && staging_matches(IMG_SIZE) && g_srv.first_of[0] == 512u);
    check("devamlı indirme sonrası da EXIT -> VERIFY -> ACTIVATE geçer", finish_update());

    /* taşıyıcı meşgul: BUFFER_FULL sonra dener */
    world();
    (void)accept_download();
    g_srv.busy_returns = 7u;
    start_pull();
    drive_default(&d);
    (void)run(&d, 200000u);
    fwu_pull_get_stats(&g_p, &stats);
    check("send BUFFER_FULL: istek bekler, yeniden denenir, hata sayılmaz (retries 0), DONE", pull_status().state == FWU_PROTO_PULL_DONE && stats.retries == 0u && staging_matches(IMG_SIZE));

    /* taşıyıcı hatası (bağlantı yok): zaman aşımı gibi, hak bitince vazgeç */
    world();
    (void)accept_download();
    g_srv.hard_error = FWU_ERR_FLASH_POWER_LOSS;     /* herhangi bir "gönderilemedi" kodu */
    start_pull();
    drive_default(&d);
    (void)run(&d, 200000u);
    st = pull_status();
    check("send kalıcı hata veriyor: yeniden denemeler tükenince FAILED(PULL_TIMEOUT)", st.state == FWU_PROTO_PULL_FAILED && st.error == 82u);
}

static void test_link(void)
{
    drive_t d;
    fwu_pull_stats_t stats;

    /* bağlantı yarım gövdede kopar: yarım parça atılır, yeniden bağlanınca yazılmış son parçadan sürer, deneme hakkı harcanmaz */
    world();
    (void)accept_download();
    start_pull();
    drive_default(&d);
    d.drop_at_body_position = 300u;                 /* başlık(~150) + gövdenin ilk ~150 baytı geldi, yarım parça var */
    d.reconnect_in_steps = 5u;
    (void)run(&d, 200000u);
    fwu_pull_get_stats(&g_p, &stats);
    check("gövdede bağlantı kopması: DONE, staging birebir aynı", pull_status().state == FWU_PROTO_PULL_DONE && staging_matches(IMG_SIZE));
    check("kopma: link_drops 1, retries 0 (sunucunun suçu sayılmaz)", stats.link_drops == 1u && stats.retries == 0u);
    check("kopmadan sonra istek yazılmış son parçadan (ilk aralıkta 0 veya 128)", g_srv.requests >= 7u && (g_srv.first_of[1] % CHUNK) == 0u);

    /* link hiç kurulmadı: istek gönderilmez */
    world();
    fwu_pull_set_link(&g_p, false, g_now);
    (void)accept_download();
    start_pull();
    drive_default(&d);
    for(int i = 0; i < 300; i++) {
        step(&d);
    }
    check("link hiç kurulmadan istek gönderilmez, durum REQUESTING, hata yok", g_srv.requests == 0u && pull_status().state == FWU_PROTO_PULL_REQUESTING);
    fwu_pull_set_link(&g_p, true, g_now);
    (void)run(&d, 200000u);
    check("link kurulunca çekme başlar ve biter", pull_status().state == FWU_PROTO_PULL_DONE && staging_matches(IMG_SIZE));
}

static void test_close(void)
{
    drive_t d;
    fwu_pull_stats_t stats;

    /* Connection: close: sunucu her cevaptan sonra bağlantıyı kapatır; cihaz yeni bağlantıyı bekler ve sürdürür */
    world();
    (void)accept_download();
    g_srv.default_mode = SRV_CLOSE;
    start_pull();
    drive_default(&d);
    d.close_cycles_link = 1;
    (void)run(&d, 200000u);
    fwu_pull_get_stats(&g_p, &stats);
    check("Connection: close + gerçek yeniden bağlanma: DONE, staging birebir aynı", pull_status().state == FWU_PROTO_PULL_DONE && staging_matches(IMG_SIZE));
    check("Connection: close: 5 kopma (son cevaptan önce), yeniden deneme yok", stats.link_drops >= 5u && stats.retries == 0u);

    /* Connection: close ama bağlantı HİÇ kopmadı: bekleme süresi dolunca yine de devam eder */
    world();
    (void)accept_download();
    g_srv.default_mode = SRV_CLOSE;
    start_pull();
    drive_default(&d);
    (void)run(&d, 200000u);
    check("Connection: close ama link kopmadı: zaman aşımı sonrası devam eder, DONE", pull_status().state == FWU_PROTO_PULL_DONE && staging_matches(IMG_SIZE));
}

static void test_overflow(void)
{
    drive_t d;
    fwu_pull_stats_t stats;

    /* akış kontrolü ihlali: halkaya yer yokken bayt basılır -> düşer, sayaçta görünür, cevap yeniden istenir */
    world();
    (void)accept_download();
    start_pull();
    drive_default(&d);
    d.piece_min = 1u;
    d.piece_max = 3u;
    d.overflow_burst = FWU_PULL_INPUT_SIZE + 200u;
    (void)run(&d, 400000u);
    fwu_pull_get_stats(&g_p, &stats);
    check("halka taşması: sayaçta görünür (rx_overflows >= 1) ve bozuk cevap yeniden istenir (retries >= 1)", stats.rx_overflows >= 1u && stats.retries >= 1u);
    check("taşmadan sonra kurtarılır: DONE, staging birebir aynı", pull_status().state == FWU_PROTO_PULL_DONE && staging_matches(IMG_SIZE));

    /* feed davranışı */
    world();
    (void)accept_download();
    start_pull();
    {
        fwu_byte_t junk[FWU_PULL_INPUT_SIZE + 8u];
        for(size_t i = 0u; i < ELEMENTS(junk); i++) { junk[i] = 0x55u; }
        expect("feed: istek yokken gelen bayt sessizce atılır (hata değil)", fwu_pull_feed(&g_p, junk, 10u, g_now), FWU_OK);
        check("feed: istek yokken halkaya bir şey girmez", fwu_pull_input_used(&g_p) == 0u);
        fwu_pull_poll(&g_p, g_now);                       /* isteği gönderir (link açık) */
        check("poll isteği gönderdi", g_srv.requests == 1u);
        expect("feed: boş girdi (uzunluk 0) tamam", fwu_pull_feed(&g_p, junk, 0u, g_now), FWU_OK);
        expect("feed: NULL veri uzunluk > 0", fwu_pull_feed(&g_p, NULL, 5u, g_now), FWU_ERR_INVALID_ARG);
        expect("feed: halka kapasitesinden büyük grup reddedilir", fwu_pull_feed(&g_p, junk, FWU_PULL_INPUT_SIZE + 1u, g_now), FWU_ERR_BUFFER_FULL);
        check("büyük grup halkada hiçbir şey bırakmaz (hep ya da hiç)", fwu_pull_input_used(&g_p) == 0u);
        /* reddedilen grup cevapta delik açtı: yeniden istek gelene dek sonraki baytlar da reddedilir (test_hole_rejects_following_bytes);
         * bu yüzden aşağıdaki doldurma temiz bir istekle yapılır */
        world();
        (void)accept_download();
        start_pull();
        fwu_pull_poll(&g_p, g_now);
        expect("feed: halkayı tam doldur", fwu_pull_feed(&g_p, junk, FWU_PULL_INPUT_SIZE, g_now), FWU_OK);
        check("input_free 0, input_used = FWU_PULL_INPUT_SIZE", fwu_pull_input_free(&g_p) == 0u && fwu_pull_input_used(&g_p) == FWU_PULL_INPUT_SIZE);
        expect("feed: dolu halkaya bir bayt: BUFFER_FULL", fwu_pull_feed(&g_p, junk, 1u, g_now), FWU_ERR_BUFFER_FULL);
    }
}

static void test_session_interaction(void)
{
    drive_t d;
    fwu_pull_stats_t stats;
    fwu_proto_pull_status_t st;

    /* ABORT çekmeyi bitirir */
    world();
    (void)accept_download();
    start_pull();
    drive_default(&d);
    for(int i = 0; i < 60; i++) {
        step(&d);
    }
    check("ABORT'tan önce çekme sürüyor ve bir miktar ilerledi", fwu_pull_active(&g_p) && fwu_session_get_next_offset(&g_s) > 0u);
    host1(FWU_MSG_ABORT, 90u);
    check("ABORT_OK", g_r.decoded && g_r.type == FWU_MSG_ABORT_OK);
    fwu_pull_poll(&g_p, g_now);
    st = pull_status();
    check("ABORT sonrası çekme BOŞTA'ya döner (oturum sıfırlandı), hata yok", st.state == FWU_PROTO_PULL_IDLE && st.error == 0u && !fwu_pull_active(&g_p));
    start_pull();
    check("ABORT'tan sonra yeni REQUEST_DOWNLOAD olmadan PULL_START: BAD_STATE", resp_is_error(FWU_ERR_BAD_STATE));
    check("yeniden REQUEST_DOWNLOAD + PULL_START: ofset 0'dan temiz başlar", accept_download());
    start_pull();
    check("yeniden başlatıldı", status_is(FWU_MSG_PULL_STARTED, FWU_PROTO_PULL_REQUESTING) && last_status().next_offset == 0u);
    (void)run(&d, 200000u);
    check("ABORT sonrası yeniden çekme: DONE ve staging birebir aynı", pull_status().state == FWU_PROTO_PULL_DONE && staging_matches(IMG_SIZE));

    /* çekme sürerken oturum boşta zaman aşımına DÜŞMEZ (çok yavaş sunucu: toplam süre >> SESSION_IDLE_MS) */
    world();
    (void)accept_download();
    start_pull();
    drive_default(&d);
    d.random_pieces = 0;
    d.piece_min = 1u;                               /* adımda 1 bayt: ~2400 bayt x 5 ms = ~12 sn  >>  1 sn oturum zaman aşımı */
    {
        const uint32_t t0 = g_now;
        (void)run(&d, 400000u);
        check("yavaş çekme (>10 sn) oturum zaman aşımından (1 sn) uzun sürdü", (g_now - t0) > (SESSION_IDLE_MS * 5u));
    }
    fwu_pull_get_stats(&g_p, &stats);
    check("yavaş çekme: oturum düşmedi, DONE, staging birebir aynı, yeniden deneme yok", pull_status().state == FWU_PROTO_PULL_DONE && staging_matches(IMG_SIZE) && stats.retries == 0u);
    check("yavaş çekmeden sonra EXIT -> VERIFY -> ACTIVATE geçer", finish_update());

    /* flash yazma hatası: çekme FAILED, kod gerçek, oturum BOŞTA */
    world();
    (void)accept_download();
    g_ff.fail_write_offset = IMAGE_OFFSET + 700u;
    start_pull();
    drive_default(&d);
    (void)run(&d, 200000u);
    st = pull_status();
    check("flash yazma hatası: FAILED, hata = flash kodu (45), oturum BOŞTA'ya düştü", st.state == FWU_PROTO_PULL_FAILED && st.error == (fwu_byte_t)FAULT_WRITE && fwu_session_get_state(&g_s) == FWU_SESSION_IDLE);
    check("hata anına kadar yazılan parçalar doğru, sonrası yazılmadı", staging_matches(640u) && staging_untouched(704u, 64u));

    /* her şey zaten yazılmış: FARKLI hedefle PULL_START bile hemen DONE (yeniden indirme yok) */
    world();
    (void)accept_download();
    start_pull();
    drive_default(&d);
    (void)run(&d, 200000u);
    check("(hazırlık) çekme DONE", pull_status().state == FWU_PROTO_PULL_DONE);
    {
        const uint32_t before = g_srv.requests;
        pull_start_frame(SERVER_IP, SERVER_PORT, "/baska/dosya.bin", 91u);
        check("DONE iken FARKLI hedefle PULL_START: hemen DONE, sunucuya yeni istek yok",
              status_is(FWU_MSG_PULL_STARTED, FWU_PROTO_PULL_DONE) && g_srv.requests == before && fwu_pull_active(&g_p) == false);
    }
}

/* Eski oturumdan kalan DONE yeni oturumun çekmesini engellemez (donanımda görülen hata: doğrulaması başarısız olan güncellemeden
 * sonra aynı sunucuya yeni PULL_START "TAMAM, ofset 0" yanıtı alıp hiçbir şey indirmiyordu). */
static void test_new_session_after_done(void)
{
    drive_t d;
    uint32_t before;

    world();
    (void)accept_download();
    start_pull();
    drive_default(&d);
    (void)run(&d, 200000u);
    check("(hazırlık) ilk çekme DONE", pull_status().state == FWU_PROTO_PULL_DONE && staging_matches(IMG_SIZE));
    before = g_srv.requests;

    /* oturum düştü (ABORT ~ doğrulama hatası) ama poll henüz çalışmadı: pull hâlâ DONE */
    host1(FWU_MSG_ABORT, 90u);
    check("ABORT_OK", g_r.decoded && g_r.type == FWU_MSG_ABORT_OK);
    check("(hazırlık) oturum BOŞTA, pull hâlâ DONE", fwu_session_get_state(&g_s) == FWU_SESSION_IDLE && pull_status().state == FWU_PROTO_PULL_DONE);
    check("yeni REQUEST_DOWNLOAD kabul edildi", accept_download());
    start_pull();
    check("eski DONE'a rağmen AYNI hedefle PULL_START yeni çekmeyi başlatır: REQUESTING, ofset 0",
          status_is(FWU_MSG_PULL_STARTED, FWU_PROTO_PULL_REQUESTING) && last_status().next_offset == 0u);
    (void)run(&d, 200000u);
    check("yeni oturumda imaj gerçekten yeniden çekildi (sunucuya yeni istekler) ve DONE",
          pull_status().state == FWU_PROTO_PULL_DONE && g_srv.requests >= (before + 6u) && fwu_session_get_next_offset(&g_s) == IMG_SIZE);

    /* poll araya girerse: oturum BOŞTA iken DONE kendiliğinden BOŞTA'ya döner */
    host1(FWU_MSG_ABORT, 91u);
    fwu_pull_poll(&g_p, g_now);
    check("oturum düşünce poll DONE'ı BOŞTA'ya çevirir, hata yok", pull_status().state == FWU_PROTO_PULL_IDLE && pull_status().error == 0u);
    check("sonra yeni oturum + PULL_START normal başlar", accept_download());
    start_pull();
    check("REQUESTING, ofset 0", status_is(FWU_MSG_PULL_STARTED, FWU_PROTO_PULL_REQUESTING) && last_status().next_offset == 0u);

    /* gerçekten bitmiş çekmeye aynı hedefle tekrar PULL_START hâlâ idempotent (yanıt kayboldu senaryosu) */
    world();
    (void)accept_download();
    start_pull();
    (void)run(&d, 200000u);
    before = g_srv.requests;
    start_pull();
    check("aynı oturumda biten çekmeye aynı PULL_START: DONE, yeniden indirme yok",
          status_is(FWU_MSG_PULL_STARTED, FWU_PROTO_PULL_DONE) && g_srv.requests == before);
}

/* Halka taşması cevapta DELİK açar. Delikten SONRA gelen baytlar da reddedilmeli: yoksa poll onları (kaymış olarak) parça tamponuna
 * ve flash'a yazar, yeniden istek ise yazılmış ofsetten devam ettiği için bozukluk hiç düzelmez (donanımda HASH_MISMATCH). */
static void test_hole_rejects_following_bytes(void)
{
    drive_t d;
    size_t used;
    fwu_pull_stats_t stats;
    fwu_byte_t big[FWU_PULL_INPUT_SIZE + 8u];

    world();
    (void)accept_download();
    start_pull();
    drive_default(&d);
    fwu_pull_poll(&g_p, g_now);                       /* isteği gönderir */
    check("(hazırlık) istek gitti ve cevap yeterince uzun", g_srv.requests == 1u && g_srv.resp_length > 200u);
    expect("ilk parça alınır", fwu_pull_feed(&g_p, &g_srv.resp[0], 60u, g_now), FWU_OK);
    for(size_t i = 0u; i < ELEMENTS(big); i++) { big[i] = 0x55u; }
    expect("(delik) halka kapasitesinden büyük grup düşer", fwu_pull_feed(&g_p, big, ELEMENTS(big), g_now), FWU_ERR_BUFFER_FULL);
    used = fwu_pull_input_used(&g_p);
    expect("delikten SONRA gelen küçük parça da reddedilir", fwu_pull_feed(&g_p, &g_srv.resp[120], 40u, g_now), FWU_ERR_BUFFER_FULL);
    expect("... ve bir sonraki de", fwu_pull_feed(&g_p, &g_srv.resp[160], 40u, g_now), FWU_ERR_BUFFER_FULL);
    check("reddedilen baytlar halkaya girmedi", fwu_pull_input_used(&g_p) == used);
    (void)run(&d, 400000u);
    fwu_pull_get_stats(&g_p, &stats);
    check("delik algılandı ve yeniden istendi (retries >= 1, rx_overflows >= 1)", stats.retries >= 1u && stats.rx_overflows >= 1u);
    check("delikten sonra DONE ve staging imajla BİREBİR aynı", pull_status().state == FWU_PROTO_PULL_DONE && staging_matches(IMG_SIZE));
}

/** data'yı halka akış kontrolüne uyarak (yer varsa) besler; her adımda poll çalışır. */
static void feed_paced(const fwu_byte_t *data, size_t length, size_t piece)
{
    size_t pos = 0u;
    uint32_t guard = 0u;

    while((pos < length) && (guard++ < 100000u)) {
        size_t n = ((length - pos) < piece) ? (length - pos) : piece;

        g_now += STEP_MS;
        if(fwu_pull_input_free(&g_p) >= n) {
            (void)fwu_pull_feed(&g_p, &data[pos], n, g_now);
            pos += n;
        }
        fwu_pull_poll(&g_p, g_now);
    }
}

/* Taşıma katmanında SESSİZCE kaybolan baytlar (sayaç/bayrak yok): cevap kısa kalır. Eski tasarım kayıptan sonrasını kaymış olarak
 * flash'a yazıyordu (donanımda HASH_MISMATCH); aralık tamponu sayesinde eksik cevaptan flash'a TEK BAYT girmez. */
static void test_silent_hole_never_reaches_flash(void)
{
    drive_t d;
    fwu_pull_stats_t stats;
    size_t len;

    world();
    (void)accept_download();
    start_pull();
    drive_default(&d);
    fwu_pull_poll(&g_p, g_now);                       /* ilk aralığın isteğini gönderir */
    len = g_srv.resp_length;
    check("(hazırlık) istek gitti, cevap başlık + 256 bayt", g_srv.requests == 1u && len > RANGE_BYTES);
    feed_paced(&g_srv.resp[0], len - 100u, 17u);                       /* kayıptan ÖNCE */
    feed_paced(&g_srv.resp[len - 80u], 80u, 17u);                      /* 20 bayt kayıp, SONRASI gelir (kaymış) */
    g_srv.resp_position = g_srv.resp_length;                           /* sürücü bu cevabı bir daha beslemesin */
    check("kayıplı cevap: flash'a hiçbir şey yazılmadı, ofset 0, çekme sürüyor",
          staging_untouched(0u, RANGE_BYTES) && fwu_session_get_next_offset(&g_s) == 0u && fwu_pull_active(&g_p));
    (void)run(&d, 400000u);
    fwu_pull_get_stats(&g_p, &stats);
    check("zaman aşımıyla aynı aralık yeniden istenir (retries >= 1), sonunda DONE ve staging imajla BİREBİR aynı",
          stats.retries >= 1u && pull_status().state == FWU_PROTO_PULL_DONE && staging_matches(IMG_SIZE));
    check("yeniden istek 0'dan başladı", g_srv.first_of[1] == 0u);
}

/* Önceki isteğin geç gelen/yinelenen (başka aralığın) cevabı: gövdesi atılır, yeniden deneme hakkı HARCANMAZ. */
static void test_stale_response_is_skipped(void)
{
    drive_t d;
    fwu_pull_stats_t stats;
    fwu_byte_t first[SRV_RESP_MAX];
    size_t first_len;

    world();
    (void)accept_download();
    start_pull();
    drive_default(&d);
    fwu_pull_poll(&g_p, g_now);                       /* istek 1: aralık 0..255 */
    first_len = g_srv.resp_length;
    for(size_t i = 0u; i < first_len; i++) {
        first[i] = g_srv.resp[i];                 /* C2000 taklidinde fwu_byte_t 16 bit: memcpy bayt sayar, eleman değil */
    }
    feed_paced(first, first_len, 23u);                /* cevap 1 tamamlanır, yazılır; istek 2 (256..511) gönderilir */
    g_now += 5u * STEP_MS;
    fwu_pull_poll(&g_p, g_now);
    check("(hazırlık) ilk aralık yazıldı, ikinci istek gitti", fwu_session_get_next_offset(&g_s) == RANGE_BYTES && g_srv.requests == 2u);
    g_srv.resp_position = g_srv.resp_length;          /* ikinci cevabı sürücü şimdilik beslemesin */
    feed_paced(first, first_len, 23u);                /* bayat: ilk aralığın cevabı BİR KEZ DAHA gelir */
    fwu_pull_get_stats(&g_p, &stats);
    check("bayat cevap: yeniden deneme hakkı harcanmadı, çekme sürüyor, ofset bozulmadı",
          stats.retries == 0u && fwu_pull_active(&g_p) && fwu_session_get_next_offset(&g_s) == RANGE_BYTES);
    g_srv.resp_position = 0u;                         /* şimdi gerçek (ikinci) cevap gelsin */
    (void)run(&d, 200000u);
    fwu_pull_get_stats(&g_p, &stats);
    check("gerçek cevap gelince tamamlanır: DONE, staging birebir aynı, hiç yeniden deneme yok",
          pull_status().state == FWU_PROTO_PULL_DONE && staging_matches(IMG_SIZE) && stats.retries == 0u);
}

/* düşman akış: rastgele hata enjeksiyonu; ASLA yanlış bayt kabul edilmez, hep biter */
static void test_fuzz(void)
{
    int never_wrong = 1;
    int always_ends = 1;
    int done_count = 0;
    int failed_count = 0;
    int session_ok = 1;

    for(int seed = 0; seed < 250; seed++) {
        drive_t d;
        uint32_t steps;
        fwu_proto_pull_status_t st;

        g_rng = 7000u + (uint32_t)seed;
        world();
            (void)accept_download();
        for(uint32_t i = 0u; i < 24u; i++) {
            const uint32_t roll = rnd() % 100u;
            g_srv.policy[i] = (roll < 55u) ? SRV_OK : (roll < 62u) ? SRV_SILENT : (roll < 70u) ? SRV_GARBAGE : (roll < 78u) ? SRV_HALF :
                              (roll < 86u) ? SRV_WRONG_RANGE : (roll < 92u) ? SRV_NO_LENGTH : SRV_CLOSE;
        }
        g_srv.default_mode = SRV_OK;
        if((rnd() % 4u) == 0u) {
            g_srv.busy_returns = rnd() % 5u;
        }
        start_pull();
        drive_default(&d);
        d.piece_max = 1u + (size_t)(rnd() % 90u);
        if((rnd() % 3u) == 0u) {
            d.drop_at_body_position = 100u + (rnd() % 300u);
            d.reconnect_in_steps = rnd() % 8u;
        }
        d.close_cycles_link = 0;
        steps = run(&d, 300000u);
        st = pull_status();
        if(fwu_pull_active(&g_p) || (steps >= 300000u)) {
            always_ends = 0;
        }
        /* flash'ta yazılmış her bayt imajla aynı olmalı: ilerleme ne olursa olsun */
        if(!staging_matches(st.next_offset)) {
            never_wrong = 0;
        }
        if(st.state == FWU_PROTO_PULL_DONE) {
            done_count++;
            if(st.next_offset != IMG_SIZE || !staging_matches(IMG_SIZE)) {
                never_wrong = 0;
            }
            if(!finish_update()) {
                session_ok = 0;
            }
        }
        else {
            failed_count++;
        }
    }
    check("250 düşman akış: her biri sonlanır (sonsuz döngü / takılma yok)", always_ends);
    check("250 düşman akış: flash'a ASLA yanlış bayt yazılmadı (yazılan her bayt imajla aynı)", never_wrong);
    check("250 düşman akış: DONE olan her çekme gerçek imza/SHA-256 doğrulamasından geçip commit edilebildi", session_ok);
    check("düşman akışta hem başarı hem vazgeçme görüldü (test gerçekten zorluyor)", done_count > 20 && failed_count > 0);
    printf("        (DONE: %d, FAILED: %d)\n", done_count, failed_count);
}

/* ------------------------------------------------------------------ UÇTAN UCA: gerçek fwu_client (çekme modu) <-> cihaz */

static fwu_client_t g_cl;
static fwu_client_config_t g_cc;
static fwu_byte_t g_cl_target[FWU_PROTO_PULL_TARGET_MAX];
static size_t g_cl_target_length;
static uint32_t g_type_count[256];                  /* cihazın aldığı çerçeve türleri (TRANSFER_DATA hiç gelmemeli) */
static uint32_t g_loss_pct;                         /* hatta kayıp yüzdesi (her iki yön) */
static uint32_t g_frames_lost;

static void make_client_cfg(void)
{
    const char *t = TARGET_TEXT;

    g_cl_target_length = strlen(t);
    for(size_t i = 0u; i < g_cl_target_length; i++) {
        g_cl_target[i] = (fwu_byte_t)(unsigned char)t[i];
    }
    memset(&g_cc, 0, sizeof(g_cc));
    g_cc.manifest = g_manifest;
    g_cc.image = NULL;                              /* ÇEKME MODU: host imajın baytlarına sahip değil */
    g_cc.image_size = IMG_SIZE;
    g_cc.expected_product = PRODUCT;
    g_cc.response_timeout_ms = 200u;
    g_cc.max_retries = 6u;
    g_cc.verify_timeout_ms = 2000u;
    g_cc.max_restarts = 2u;
    g_cc.pull_target = g_cl_target;
    g_cc.pull_target_length = g_cl_target_length;
    g_cc.pull_ip[0] = SERVER_IP[0]; g_cc.pull_ip[1] = SERVER_IP[1]; g_cc.pull_ip[2] = SERVER_IP[2]; g_cc.pull_ip[3] = SERVER_IP[3];
    g_cc.pull_port = SERVER_PORT;
    g_cc.pull_poll_ms = 100u;
    g_cc.pull_timeout_ms = 0u;
}

static int lose_frame(void)
{
    if((g_loss_pct != 0u) && ((rnd() % 100u) < g_loss_pct)) {
        g_frames_lost++;
        return 1;
    }
    return 0;
}

/** Cihazdan istemciye bir yanıt çerçevesi (kayıp olabilir). */
static void to_client(const fwu_byte_t *frame, size_t length)
{
    fwu_frame_view_t view;

    if((length == 0u) || lose_frame()) {
        return;
    }
    if(fwu_frame_decode(frame, length, &view) == FWU_OK) {
        (void)fwu_client_on_frame(&g_cl, &view, g_now);
    }
}

/** İstemciden cihaza bir istek çerçevesi (kayıp olabilir): cihaz yapıştırıcısının dağıtımı. */
static void to_device(const fwu_byte_t *frame, size_t length)
{
    fwu_frame_view_t view;
    fwu_byte_t out[FWU_FRAME_MAX_SIZE];
    size_t out_length = 0u;
    fwu_return_e rc;

    if(lose_frame()) {
        return;
    }
    if(fwu_frame_decode(frame, length, &view) != FWU_OK) {
        return;
    }
    g_type_count[view.type & 0xFFu]++;
    if(fwu_pull_handles(view.type)) {
        rc = fwu_pull_on_frame(&g_p, &view, g_now, out, ELEMENTS(out), &out_length);
    }
    else {
        rc = fwu_session_on_frame(&g_s, &view, g_now, out, ELEMENTS(out), &out_length);
    }
    if(rc == FWU_OK) {
        to_client(out, out_length);
    }
}

/** Bir adım: zaman, sunucudan gelen baytlar + çekme poll'u (step), istemci gönderimi, oturum poll'u (doğrulama sonucu). */
static void client_step(drive_t *d)
{
    fwu_byte_t frame[FWU_FRAME_MAX_SIZE];
    fwu_byte_t out[FWU_FRAME_MAX_SIZE];
    size_t length = 0u;
    size_t out_length = 0u;

    step(d);
    if((fwu_client_poll(&g_cl, g_now, frame, ELEMENTS(frame), &length) == FWU_OK) && (length > 0u)) {
        to_device(frame, length);
    }
    if((fwu_session_poll(&g_s, g_now, out, ELEMENTS(out), &out_length) == FWU_OK) && (out_length > 0u)) {
        to_client(out, out_length);
    }
}

static uint32_t run_client(drive_t *d, uint32_t max_steps)
{
    uint32_t n = 0u;

    while((n < max_steps) && !fwu_client_is_finished(&g_cl)) {
        client_step(d);
        n++;
    }
    return n;
}

static void e2e_world(void)
{
    world();
    memset(g_type_count, 0, sizeof(g_type_count));
    g_loss_pct = 0u;
    g_frames_lost = 0u;
    make_client_cfg();
}

static void e2e_start(void)
{
    (void)fwu_client_init(&g_cl, &g_cc);
    (void)fwu_client_start(&g_cl, g_now);
}

static void test_client_e2e(void)
{
    drive_t d;
    fwu_client_result_t r;
    uint32_t steps;

    /* mutlu yol */
    e2e_world();
    expect("client init (cekme modu, image NULL)", fwu_client_init(&g_cl, &g_cc), FWU_OK);
    expect("client start", fwu_client_start(&g_cl, g_now), FWU_OK);
    drive_default(&d);
    steps = run_client(&d, 200000u);
    (void)fwu_client_get_result(&g_cl, &r);
    check("e2e: istemci DONE (imaj baytlari olmadan, cihaz sunucudan cekti)", r.state == FWU_CLIENT_DONE && r.failure == FWU_CLIENT_FAIL_NONE && steps < 200000u);
    check("e2e: ilerleme = imaj boyutu, cihaz commit istedi, staging imajla BIREBIR ayni",
          r.acked_bytes == IMG_SIZE && r.total_bytes == IMG_SIZE && fwu_session_reboot_requested(&g_s) && staging_matches(IMG_SIZE));
    check("e2e: cihaz TRANSFER_DATA HIC almadi (0), PULL_START tam 1 kez, PULL_QUERY en az 1 kez",
          g_type_count[FWU_MSG_TRANSFER_DATA] == 0u && g_type_count[FWU_MSG_PULL_START] == 1u && g_type_count[FWU_MSG_PULL_QUERY] >= 1u);
    check("e2e: istemci az cerceve gonderdi (< 40; imaj cerceveleri yok), http durumu 206 bildirildi",
          r.frames_sent < 40u && r.retransmits == 0u && r.pull_http_status == 206u);
    check("e2e: HELLO, REQUEST_DOWNLOAD, EXIT, ACTIVATE birer kez", g_type_count[FWU_MSG_HELLO] == 1u && g_type_count[FWU_MSG_REQUEST_DOWNLOAD] == 1u &&
                                                                    g_type_count[FWU_MSG_TRANSFER_EXIT] == 1u && g_type_count[FWU_MSG_ACTIVATE] == 1u);

    /* kalici hata cihazdan istemciye tasinir: 404 */
    e2e_world();
    g_srv.default_mode = SRV_404;
    e2e_start();
    drive_default(&d);
    (void)run_client(&d, 200000u);
    (void)fwu_client_get_result(&g_cl, &r);
    check("e2e 404: istemci FAILED(DEVICE_ERROR, 81), http 404 gorunur",
          r.state == FWU_CLIENT_FAILED && r.failure == FWU_CLIENT_FAIL_DEVICE_ERROR && r.device_error == FWU_ERR_PULL_HTTP_STATUS && r.pull_http_status == 404u);
    check("e2e 404: hicbir bayt yazilmadi, EXIT/ACTIVATE gonderilmedi", staging_untouched(0u, 256u) && g_type_count[FWU_MSG_TRANSFER_EXIT] == 0u && g_type_count[FWU_MSG_ACTIVATE] == 0u);

    /* yanlis dosya boyutu */
    e2e_world();
    g_srv.default_mode = SRV_BAD_TOTAL;
    e2e_start();
    drive_default(&d);
    (void)run_client(&d, 200000u);
    (void)fwu_client_get_result(&g_cl, &r);
    check("e2e yanlis dosya: istemci FAILED(DEVICE_ERROR, 83)", r.state == FWU_CLIENT_FAILED && r.device_error == FWU_ERR_PULL_SIZE_MISMATCH);

    /* cihaz sunucuya ulasamaz: cihazin vazgectigi hata (82) istemciye tasinir */
    e2e_world();
    g_srv.default_mode = SRV_SILENT;
    e2e_start();
    drive_default(&d);
    (void)run_client(&d, 200000u);
    (void)fwu_client_get_result(&g_cl, &r);
    check("e2e sunucu sessiz: istemci FAILED(DEVICE_ERROR, 82 PULL_TIMEOUT)", r.state == FWU_CLIENT_FAILED && r.device_error == FWU_ERR_PULL_TIMEOUT);

    /* cerceve kaybi: %20, 60 tohum: idempotent tekrarlar sayesinde hep tamamlanir, imaj dogru */
    {
        int all_done = 1;
        int all_correct = 1;
        uint32_t lost_total = 0u;

        for(int seed = 0; seed < 60; seed++) {
            g_rng = 31000u + (uint32_t)seed;
            e2e_world();
            g_loss_pct = 20u;
            g_cc.max_retries = 30u;                 /* kayip cok: yeniden gonderim hakki genis */
            g_cc.max_restarts = 4u;
            e2e_start();
            drive_default(&d);
            (void)run_client(&d, 400000u);
            (void)fwu_client_get_result(&g_cl, &r);
            lost_total += g_frames_lost;
            if(r.state != FWU_CLIENT_DONE) {
                all_done = 0;
            }
            else if(!(staging_matches(IMG_SIZE) && fwu_session_reboot_requested(&g_s) && g_type_count[FWU_MSG_TRANSFER_DATA] == 0u)) {
                all_correct = 0;
            }
        }
        check("e2e %20 cerceve kaybi (60 tohum): hepsi DONE", all_done);
        check("e2e %20 cerceve kaybi: her DONE'da staging birebir ayni, commit istendi, TRANSFER_DATA yok", all_correct);
        check("e2e kayip gercekten yasandi (toplam kayip cerceve > 100)", lost_total > 100u);
    }

    /* cihaz cekmeyi/oturumu unuttu (ABORT): istemci baştan baslar ve tamamlar */
    e2e_world();
    e2e_start();
    drive_default(&d);
    for(int i = 0; (i < 4000) && (fwu_session_get_next_offset(&g_s) < 400u) && !fwu_client_is_finished(&g_cl); i++) {
        client_step(&d);
    }
    check("(hazirlik) cekme basladi ve bir miktar ilerledi, istemci hala calisiyor",
          fwu_session_get_next_offset(&g_s) >= 400u && !fwu_client_is_finished(&g_cl));
    {
        const fwu_byte_t zero[1] = { 0u };
        fwu_byte_t frame[FWU_FRAME_MAX_SIZE];
        fwu_byte_t out[FWU_FRAME_MAX_SIZE];
        size_t fl = 0u;
        size_t ol = 0u;
        fwu_frame_view_t view;

        (void)fwu_frame_encode(FWU_MSG_ABORT, 200u, zero, 1u, frame, ELEMENTS(frame), &fl);
        (void)fwu_frame_decode(frame, fl, &view);
        (void)fwu_session_on_frame(&g_s, &view, g_now, out, ELEMENTS(out), &ol);     /* cihaz oturumu sifirlandi (ornegin yeniden basladi) */
    }
    (void)run_client(&d, 400000u);
    (void)fwu_client_get_result(&g_cl, &r);
    check("e2e cihaz unuttu: istemci basa dondu (restarts >= 1) ve DONE oldu", r.state == FWU_CLIENT_DONE && r.restarts >= 1u);
    check("e2e cihaz unuttu: staging imajla birebir ayni", staging_matches(IMG_SIZE) && fwu_session_reboot_requested(&g_s));

    /* toplam sure siniri: cihaz sonsuza dek deniyor (cok yuksek max_retries), istemci pull_timeout_ms'te vazgecer */
    e2e_world();
    g_srv.default_mode = SRV_SILENT;
    g_pc.max_retries = 100000u;
    (void)fwu_pull_init(&g_p, &g_pc);
    fwu_pull_set_link(&g_p, true, g_now);
    g_cc.pull_timeout_ms = 1500u;
    e2e_start();
    drive_default(&d);
    {
        const uint32_t t0 = g_now;
        (void)run_client(&d, 400000u);
        (void)fwu_client_get_result(&g_cl, &r);
        check("e2e toplam sure siniri: FAILED(TIMEOUT), ~1500 ms sonra (1500..2200 arasi)",
              r.state == FWU_CLIENT_FAILED && r.failure == FWU_CLIENT_FAIL_TIMEOUT && (g_now - t0) >= 1500u && (g_now - t0) < 2200u);
    }
    g_pc.max_retries = PULL_RETRIES;

    /* iptal: cekme sirasinda */
    e2e_world();
    e2e_start();
    drive_default(&d);
    for(int i = 0; (i < 4000) && (fwu_session_get_next_offset(&g_s) < 300u) && !fwu_client_is_finished(&g_cl); i++) {
        client_step(&d);
    }
    expect("cekme sirasinda cancel kabul edilir", fwu_client_cancel(&g_cl), FWU_OK);
    (void)run_client(&d, 400000u);
    (void)fwu_client_get_result(&g_cl, &r);
    check("e2e iptal: FAILED(CANCELLED), cihaz oturumu BOSTA, cekme durdu",
          r.state == FWU_CLIENT_FAILED && r.failure == FWU_CLIENT_FAIL_CANCELLED && fwu_session_get_state(&g_s) == FWU_SESSION_IDLE);
    for(int i = 0; i < 50; i++) {
        client_step(&d);
    }
    check("iptal sonrasi cekme BOSTA, sunucuya yeni istek gitmiyor", !fwu_pull_active(&g_p) && pull_status().state == FWU_PROTO_PULL_IDLE);

    /* cihaz zaten her seyi yazmis: PULL_START hic gonderilmez, dogrudan EXIT */
    e2e_world();
    (void)accept_download();
    start_pull();
    drive_default(&d);
    (void)run(&d, 200000u);
    check("(hazirlik) cihaz imaji cekti, DONE, EXIT beklemede", pull_status().state == FWU_PROTO_PULL_DONE && fwu_session_get_next_offset(&g_s) == IMG_SIZE);
    memset(g_type_count, 0, sizeof(g_type_count));
    e2e_start();
    (void)run_client(&d, 200000u);
    (void)fwu_client_get_result(&g_cl, &r);
    check("e2e: cihazda imaj tam ise istemci PULL_START gondermeden EXIT -> VERIFY -> ACTIVATE yapar",
          r.state == FWU_CLIENT_DONE && g_type_count[FWU_MSG_PULL_START] == 0u && g_type_count[FWU_MSG_TRANSFER_DATA] == 0u && fwu_session_reboot_requested(&g_s));

    /* cekme yapilandirmasi dogrulamasi */
    {
        fwu_client_t cl;
        fwu_client_config_t c;
        const fwu_byte_t bad_target[3] = { '/', 'a', ' ' };

        e2e_world();
        expect("config: gecerli cekme yapilandirmasi", fwu_client_init(&cl, &g_cc), FWU_OK);
        c = g_cc; c.image = g_image;
        expect("config: hem image hem cekme hedefi: cekme modu gecerli", fwu_client_init(&cl, &c), FWU_OK);
        c = g_cc; c.pull_target = NULL;
        expect("config: image NULL ve hedef NULL (veri kaynagi yok): INVALID_ARG", fwu_client_init(&cl, &c), FWU_ERR_INVALID_ARG);
        c = g_cc; c.pull_port = 0u;
        expect("config: port 0", fwu_client_init(&cl, &c), FWU_ERR_INVALID_ARG);
        c = g_cc; c.pull_poll_ms = 0u;
        expect("config: sorgu araligi 0", fwu_client_init(&cl, &c), FWU_ERR_INVALID_ARG);
        c = g_cc; c.pull_target_length = 0u;
        expect("config: hedef uzunlugu 0", fwu_client_init(&cl, &c), FWU_ERR_INVALID_ARG);
        c = g_cc; c.pull_target = bad_target; c.pull_target_length = 3u;
        expect("config: hedefte bosluk", fwu_client_init(&cl, &c), FWU_ERR_INVALID_ARG);
        c = g_cc; c.pull_target_length = FWU_PROTO_PULL_TARGET_MAX + 1u;
        expect("config: hedef 97 bayt", fwu_client_init(&cl, &c), FWU_ERR_INVALID_ARG);
    }
}

int main(void)
{
    test_init();
    test_protocol();
    test_happy();
    test_permanent_errors();
    test_transient_errors();
    test_link();
    test_close();
    test_overflow();
    test_session_interaction();
    test_new_session_after_done();
    test_hole_rejects_following_bytes();
    test_silent_hole_never_reaches_flash();
    test_stale_response_is_skipped();
    test_fuzz();
    test_client_e2e();

    printf("\nResult: %s (%d failed)\n", g_failures == 0 ? "ALL TESTS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
