/**
 * @file    test_link.c
 * @brief   fwu_link (cihaz tarafı bağlantı katmanı) için kendi kendini kontrol eden test: GERÇEK ring buffer, parser, oturum,
 *          aktivasyon ve sahte flash/kriptografi üzerinde; taşıyıcı bir "mock"tur (meşgul, hata, kayıt).
 *
 * Kapsam: init doğrulaması; temel akış; poll başına çerçeve sınırı; meşgul taşıyıcıda yanıt BEKLETİLİR ve aynı baytlarla
 * yeniden denenir, bu sırada yeni istek işlenmez, sıra korunur; taşıyıcı hatasında yanıt düşürülür ve bağlantı devam eder;
 * taşıyıcı poll'u; oturum poll'undan çıkan yanıt (doğrulama sonucu); yeniden başlatma yalnızca son yanıt TESLİM edilince hazır;
 * çöp baytlar; zaman aşımı; fuzz.
 *
 * Derleme (transport klasöründen):
 *   gcc -Wall -Wextra -Wpedantic -Wconversion -std=c99 -I. -I.. -I../error -I../bytes -I../crc -I../ring_buffer -I../frame -I../flash -I../journal -I../activation -I../manifest -I../crypto -I../verify -I../session fwu_link.c ../session/fwu_session.c ../session/fwu_proto.c ../verify/fwu_verify.c ../manifest/fwu_manifest.c ../crypto/fwu_crypto.c ../crypto/fwu_crypto_sim.c ../activation/fwu_activate.c ../journal/fwu_journal_ab.c ../journal/fwu_journal.c ../flash/fwu_flash.c ../flash/fwu_flash_sim.c ../frame/fwu_frame.c ../frame/fwu_decode.c ../frame/fwu_parser.c ../ring_buffer/fwu_ringbuf.c test_link.c -o test_link && ./test_link
 */

#include "test_world.h"

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

/* ------------------------------------------------------------------ mock taşıyıcı */

#define MOCK_MAX 64u

typedef struct {
    uint32_t     busy_count;                    /* sonraki bu kadar send BUFFER_FULL doner */
    bool         fail_next;                     /* sonraki send fail_code doner (bir kez) */
    fwu_return_e fail_code;
    uint32_t     sends;                         /* send cagri sayisi (basarili + basarisiz) */
    uint32_t     polls;
    uint32_t     last_poll_now;
    uint32_t     count;                         /* kabul edilen cerceve sayisi */
    size_t       lengths[MOCK_MAX];
    fwu_byte_t   frames[MOCK_MAX][FWU_FRAME_MAX_SIZE];
} mock_t;

static mock_t g_mock;

static fwu_return_e mock_send(void *context, const fwu_byte_t *data, size_t length)
{
    mock_t *m = (mock_t *)context;

    m->sends++;
    if(m->busy_count > 0u) {
        m->busy_count--;
        return FWU_ERR_BUFFER_FULL;
    }
    if(m->fail_next) {
        m->fail_next = false;
        return m->fail_code;
    }
    if(m->count < MOCK_MAX) {
        for(size_t i = 0u; i < length; i++) {
            m->frames[m->count][i] = data[i];
        }
        m->lengths[m->count] = length;
    }
    m->count++;
    return FWU_OK;
}

static void mock_poll(void *context, uint32_t now_ms)
{
    mock_t *m = (mock_t *)context;

    m->polls++;
    m->last_poll_now = now_ms;
}

static void mock_reset(void)
{
    g_mock.busy_count = 0u;
    g_mock.fail_next = false;
    g_mock.fail_code = FWU_OK;
    g_mock.sends = 0u;
    g_mock.polls = 0u;
    g_mock.last_poll_now = 0u;
    g_mock.count = 0u;
}

static fwu_transport_t g_tr = { mock_send, mock_poll, &g_mock };
static fwu_transport_t g_tr_nopoll = { mock_send, NULL, &g_mock };
static uint32_t g_now;

static void setup(bool with_auth)
{
    tw_world(with_auth, FWU_ACTIVATE_MODE_COPY);
    mock_reset();
    (void)tw_link_setup(&g_tr);
    g_now = 1000u;
}

/** Host cercevesini bayt akisi olarak cihazin ring buffer'ina iter. */
static void host_push(fwu_byte_t type, fwu_byte_t seq, const fwu_byte_t *payload, size_t length)
{
    fwu_byte_t frame[FWU_FRAME_MAX_SIZE];
    size_t n = 0u;

    (void)fwu_frame_encode(type, seq, payload, length, frame, sizeof(frame), &n);
    (void)fwu_ringbuf_push(&tw_rx.base, frame, n);
}

static void host_push1(fwu_byte_t type, fwu_byte_t seq)
{
    const fwu_byte_t one[1] = { 1u };
    host_push(type, seq, one, 1u);
}

static void host_push_u32(fwu_byte_t type, fwu_byte_t seq, uint32_t value)
{
    fwu_byte_t p[4];
    size_t n = 0u;
    (void)fwu_proto_encode_u32(value, p, 4u, &n);
    host_push(type, seq, p, n);
}

/** Kabul edilen i. yaniti cozer. Gecerli degilse 0. */
static int resp(uint32_t i, fwu_frame_view_t *view)
{
    if((i >= g_mock.count) || (i >= MOCK_MAX)) {
        return 0;
    }
    return (fwu_frame_decode(g_mock.frames[i], g_mock.lengths[i], view) == FWU_OK) && (view->frame_length == g_mock.lengths[i]);
}

static int resp_is(uint32_t i, fwu_byte_t type, fwu_byte_t seq)
{
    fwu_frame_view_t v;
    return resp(i, &v) && (v.type == type) && (v.seq == seq);
}

static void poll_n(uint32_t n)
{
    for(uint32_t i = 0u; i < n; i++) {
        g_now += 1u;
        (void)fwu_link_poll(&tw_link, g_now);
    }
}

static void expect_poll_ok(const char *name)
{
    g_now += 1u;
    expect(name, fwu_link_poll(&tw_link, g_now), FWU_OK);
}

/* ------------------------------------------------------------------ 1. init */

static void test_init(void)
{
    fwu_link_t l;
    fwu_link_config_t c;

    printf("\n--- init dogrulamasi ---\n");
    setup(false);
    c.session = &tw_session;
    c.parser = &tw_parser;
    c.transport = &g_tr;
    expect("gecerli yapilandirma", fwu_link_init(&l, &c), FWU_OK);
    check("... bekleyen yanit yok, sayaclar sifir", !fwu_link_tx_pending(&l) && fwu_link_get_stats(&l)->frames_in == 0u &&
          fwu_link_get_stats(&l)->frames_out == 0u && fwu_link_get_stats(&l)->tx_busy == 0u && fwu_link_get_stats(&l)->tx_errors == 0u);
    expect("link NULL", fwu_link_init(NULL, &c), FWU_ERR_INVALID_ARG);
    expect("config NULL", fwu_link_init(&l, NULL), FWU_ERR_INVALID_ARG);
    c.session = NULL;
    expect("session NULL", fwu_link_init(&l, &c), FWU_ERR_INVALID_ARG);
    c.session = &tw_session; c.parser = NULL;
    expect("parser NULL", fwu_link_init(&l, &c), FWU_ERR_INVALID_ARG);
    c.parser = &tw_parser; c.transport = NULL;
    expect("transport NULL", fwu_link_init(&l, &c), FWU_ERR_INVALID_ARG);
    {
        fwu_transport_t bad = { NULL, NULL, NULL };
        c.transport = &bad;
        expect("transport.send NULL", fwu_link_init(&l, &c), FWU_ERR_INVALID_ARG);
    }
    c.transport = &g_tr_nopoll;
    expect("transport.poll NULL serbest", fwu_link_init(&l, &c), FWU_OK);
    expect("init edilmemis link ile poll", fwu_link_poll(&(fwu_link_t){ .initialized = false }, 0u), FWU_ERR_INVALID_ARG);
    expect("poll: link NULL", fwu_link_poll(NULL, 0u), FWU_ERR_INVALID_ARG);
    check("sorgular NULL'a dayanikli", !fwu_link_reboot_ready(NULL) && !fwu_link_tx_pending(NULL) && fwu_link_get_stats(NULL) == NULL);
    c.session = &tw_session; c.parser = &tw_parser; c.transport = &g_tr;
    (void)fwu_link_init(&l, &c);
    c.session = NULL;
    expect("basarisiz init", fwu_link_init(&l, &c), FWU_ERR_INVALID_ARG);
    expect("... basarisiz init sonrasi poll reddedilir (onceden hazir olan nesne bile)", fwu_link_poll(&l, 0u), FWU_ERR_INVALID_ARG);
}

/* ------------------------------------------------------------------ 2. temel akış */

static void test_basic(void)
{
    fwu_frame_view_t v;

    printf("\n--- temel akis: bayt akisi -> parser -> oturum -> tasiyici ---\n");
    setup(false);
    expect_poll_ok("bos poll");
    check("... hicbir sey gonderilmedi", g_mock.count == 0u && g_mock.sends == 0u);
    host_push1(FWU_MSG_HELLO, 7u);
    expect_poll_ok("HELLO bayt akisinda, poll");
    check("... tek yanit: INFO, SEQ 7", g_mock.count == 1u && resp_is(0u, FWU_MSG_INFO, 7u));
    check("... sayaclar: 1 cerceve girdi, 1 yanit cikti", fwu_link_get_stats(&tw_link)->frames_in == 1u && fwu_link_get_stats(&tw_link)->frames_out == 1u);
    check("... bekleyen yanit yok", !fwu_link_tx_pending(&tw_link));
    expect_poll_ok("yeni cerceve yokken poll");
    check("... yeni yanit uretilmedi", g_mock.count == 1u);

    /* bayt bayt gelen cerceve */
    {
        fwu_byte_t frame[FWU_FRAME_MAX_SIZE];
        size_t n = 0u;
        const fwu_byte_t one[1] = { 1u };
        (void)fwu_frame_encode(FWU_MSG_KEEPALIVE, 9u, one, 1u, frame, sizeof(frame), &n);
        for(size_t i = 0u; i < n; i++) {
            (void)fwu_ringbuf_push(&tw_rx.base, &frame[i], 1u);
            g_now += 1u;
            (void)fwu_link_poll(&tw_link, g_now);
            if((i + 1u) < n) {
                if(g_mock.count != 1u) {
                    break;
                }
            }
        }
        check("cerceve bayt bayt gelir: yalniz SON baytla yanit uretilir (KEEPALIVE_OK, SEQ 9)", g_mock.count == 2u && resp_is(1u, FWU_MSG_KEEPALIVE_OK, 9u));
    }
    check("yanit cercevesi tam (decode OK, uzunluk eslesiyor)", resp(1u, &v) && v.payload_length == 1u && v.payload[0] == FWU_PROTO_STATE_IDLE);
}

/* ------------------------------------------------------------------ 3. poll basina cerceve siniri */

static void test_frame_limit(void)
{
    printf("\n--- poll basina cerceve siniri ---\n");
    setup(false);
    for(uint8_t s = 1u; s <= 6u; s++) {
        host_push1(FWU_MSG_HELLO, s);
    }
    expect_poll_ok("6 cerceve birikmis, ilk poll");
    check("... en cok 4 cerceve islendi (FWU_LINK_MAX_FRAMES_PER_POLL), 4 yanit", fwu_link_get_stats(&tw_link)->frames_in == 4u && g_mock.count == 4u);
    expect_poll_ok("ikinci poll");
    check("... kalan 2 cerceve islendi, toplam 6 yanit, sirayla SEQ 1..6",
          fwu_link_get_stats(&tw_link)->frames_in == 6u && g_mock.count == 6u && resp_is(0u, FWU_MSG_INFO, 1u) && resp_is(3u, FWU_MSG_INFO, 4u) && resp_is(5u, FWU_MSG_INFO, 6u));
    check("... sinir 4", FWU_LINK_MAX_FRAMES_PER_POLL == 4u);
}

/* ------------------------------------------------------------------ 4. mesgul tasiyici */

static void test_busy(void)
{
    fwu_byte_t first_bytes[FWU_FRAME_MAX_SIZE];
    size_t first_length;

    printf("\n--- mesgul tasiyici: yanit BEKLETILIR, ayni baytlarla yeniden denenir, yeni istek islenmez ---\n");
    setup(false);
    g_mock.busy_count = 2u;
    host_push1(FWU_MSG_HELLO, 1u);
    host_push1(FWU_MSG_HELLO, 2u);
    expect_poll_ok("poll 1: ilk istek islenir, tasiyici meşgul");
    check("... yanit BEKLEMEDE, hic yanit kabul edilmedi, 1 meşgul denemesi, 1 cerceve girdi",
          fwu_link_tx_pending(&tw_link) && g_mock.count == 0u && fwu_link_get_stats(&tw_link)->tx_busy == 1u && fwu_link_get_stats(&tw_link)->frames_in == 1u);
    first_length = tw_link.pending_length;
    for(size_t i = 0u; i < first_length; i++) {
        first_bytes[i] = tw_link.pending[i];
    }
    expect_poll_ok("poll 2: hala meşgul");
    check("... yine beklemede, 2 meşgul denemesi, YENI ISTEK ISLENMEDI (cerceve sayisi hala 1)",
          fwu_link_tx_pending(&tw_link) && fwu_link_get_stats(&tw_link)->tx_busy == 2u && fwu_link_get_stats(&tw_link)->frames_in == 1u);
    check("... bekleyen yanit AYNI baytlar", tw_link.pending_length == first_length);
    expect_poll_ok("poll 3: tasiyici musait");
    check("... bekleyen yanit gitti (SEQ 1), SONRA ikinci istek islendi ve yaniti gitti (SEQ 2): sira korundu",
          g_mock.count == 2u && resp_is(0u, FWU_MSG_INFO, 1u) && resp_is(1u, FWU_MSG_INFO, 2u) && !fwu_link_tx_pending(&tw_link));
    {
        int same = (g_mock.lengths[0] == first_length);
        for(size_t i = 0u; same && (i < first_length); i++) {
            same = (g_mock.frames[0][i] == first_bytes[i]);
        }
        check("... giden yanit, bekletilen baytlarla birebir ayni", same);
    }
    check("... sayaclar: 2 cerceve girdi, 2 yanit cikti, 2 meşgul", fwu_link_get_stats(&tw_link)->frames_in == 2u && fwu_link_get_stats(&tw_link)->frames_out == 2u && fwu_link_get_stats(&tw_link)->tx_busy == 2u);

    /* oturum poll'undan cikan yanit da bekletilir */
    setup(false);
    host_push1(FWU_MSG_HELLO, 1u);
    poll_n(1u);
    {
        const uint32_t base = g_mock.count;
        fwu_activate_outcome_e dummy = FWU_ACT_OUTCOME_NONE;
        (void)dummy;
        host_push(FWU_MSG_REQUEST_DOWNLOAD, 2u, tw_manifest, 138u);
        poll_n(1u);
        for(uint32_t i = 0u; i < 11u; i++) {
            fwu_byte_t p[FWU_FRAME_MAX_PAYLOAD];
            size_t n = 0u;
            uint32_t len = (i < 10u) ? TW_CHUNK : (TW_IMG_SIZE - (10u * TW_CHUNK));
            (void)fwu_proto_encode_data(i * TW_CHUNK, &tw_image[i * TW_CHUNK], len, p, sizeof(p), &n);
            host_push(FWU_MSG_TRANSFER_DATA, 3u, p, n);
            poll_n(1u);
        }
        check("indirme tamam (12 yanit)", g_mock.count == base + 12u);
        g_mock.busy_count = 3u;
        host_push_u32(FWU_MSG_TRANSFER_EXIT, 4u, TW_IMG_SIZE);
        poll_n(1u);
        check("EXIT: EXIT_PENDING bekletildi; oturum poll'u (dogrulama) BEKLEYEN VARKEN CALISMAZ", fwu_link_tx_pending(&tw_link) &&
              fwu_session_get_state(&tw_session) == FWU_SESSION_VERIFYING && tw_cs.finish_calls == 0u);
        poll_n(2u);
        check("... hala meşgul (3 deneme): dogrulama hala calismadi", tw_cs.finish_calls == 0u);
        poll_n(1u);
        check("... tasiyici musait: once EXIT_PENDING gider", resp_is(base + 12u, FWU_MSG_EXIT_PENDING, 4u));
        check("... SONRA ayni poll'da oturum poll'u dogrulamayi calistirir ve VERIFY_RESULT(0) gider (SEQ 4)",
              resp_is(base + 13u, FWU_MSG_VERIFY_RESULT, 4u) && fwu_session_get_state(&tw_session) == FWU_SESSION_READY);
    }
}

/* ------------------------------------------------------------------ 5. tasiyici hatasi */

static void test_tx_error(void)
{
    printf("\n--- tasiyici hatasi: yanit dusurulur, baglanti devam eder ---\n");
    setup(false);
    g_mock.fail_next = true;
    g_mock.fail_code = FWU_ERR_FLASH_POWER_LOSS;           /* herhangi bir sert hata */
    host_push1(FWU_MSG_HELLO, 1u);
    host_push1(FWU_MSG_HELLO, 2u);
    expect_poll_ok("ilk yanit sert hatayla reddedilir");
    check("... yanit DUSURULDU (bekleyen yok), tx_errors 1, kabul edilen yok; ikinci istek ayni poll'da islendi ve gitti (SEQ 2)",
          !fwu_link_tx_pending(&tw_link) && fwu_link_get_stats(&tw_link)->tx_errors == 1u && g_mock.count == 1u && resp_is(0u, FWU_MSG_INFO, 2u));
    check("... sayaclar: 2 cerceve girdi, 1 yanit cikti", fwu_link_get_stats(&tw_link)->frames_in == 2u && fwu_link_get_stats(&tw_link)->frames_out == 1u);
    host_push1(FWU_MSG_HELLO, 3u);
    expect_poll_ok("hata sonrasi yeni istek");
    check("... normal yanit (SEQ 3)", resp_is(1u, FWU_MSG_INFO, 3u));
    /* hata kodu BUFFER_FULL degilse (ornek: OUT_OF_RANGE) de ayni */
    g_mock.fail_next = true;
    g_mock.fail_code = FWU_ERR_OUT_OF_RANGE;
    host_push1(FWU_MSG_HELLO, 4u);
    poll_n(1u);
    check("baska bir sert hata kodu da yaniti dusurur", fwu_link_get_stats(&tw_link)->tx_errors == 2u && !fwu_link_tx_pending(&tw_link));
}

/* ------------------------------------------------------------------ 6. tasiyici poll'u, cop, zaman asimi */

static void test_poll_garbage_timeout(void)
{
    static const fwu_byte_t junk[] = { 0x00u, 0xFFu, 0x12u, 0xA5u, 0xA5u, 0x5Au, 0x34u };

    printf("\n--- tasiyici poll'u, cop baytlar, zaman asimi ---\n");
    setup(false);
    g_now = 5000u;
    expect("poll, now_ms = 5001", fwu_link_poll(&tw_link, 5001u), FWU_OK);
    check("tasiyici poll'u cagrildi ve zamani aldi (5001)", g_mock.polls == 1u && g_mock.last_poll_now == 5001u);
    (void)fwu_link_poll(&tw_link, 5002u);
    check("her link poll'unda bir kez", g_mock.polls == 2u && g_mock.last_poll_now == 5002u);
    (void)tw_link_setup(&g_tr_nopoll);
    (void)fwu_link_poll(&tw_link, 5003u);
    check("tasiyici poll'u NULL: cagrilmaz, sorunsuz", g_mock.polls == 2u);
    (void)tw_link_setup(&g_tr);

    (void)fwu_ringbuf_push(&tw_rx.base, junk, sizeof(junk) / sizeof(junk[0]));
    expect_poll_ok("cop baytlar");
    check("... cop cerceve sayilmaz, yanit yok", fwu_link_get_stats(&tw_link)->frames_in == 0u && g_mock.count == 0u);
    host_push1(FWU_MSG_HELLO, 5u);
    expect_poll_ok("cop sonrasi gecerli cerceve");
    check("... islendi (INFO, SEQ 5)", g_mock.count == 1u && resp_is(0u, FWU_MSG_INFO, 5u));

    /* oturum zaman asimi link poll'u icinden isler */
    setup(false);
    g_now = 10000u;
    host_push(FWU_MSG_REQUEST_DOWNLOAD, 1u, tw_manifest, 138u);
    (void)fwu_link_poll(&tw_link, g_now);
    check("REQUEST_DOWNLOAD kabul, durum INDIRIYOR", fwu_session_get_state(&tw_session) == FWU_SESSION_DOWNLOADING);
    (void)fwu_link_poll(&tw_link, g_now + TW_TIMEOUT_MS - 1u);
    check("zaman asimindan 1 ms once: hala INDIRIYOR", fwu_session_get_state(&tw_session) == FWU_SESSION_DOWNLOADING);
    (void)fwu_link_poll(&tw_link, g_now + TW_TIMEOUT_MS);
    check("zaman asimi dolunca link poll'u oturumu BOSTA'ya dusurur", fwu_session_get_state(&tw_session) == FWU_SESSION_IDLE);
}

/* ------------------------------------------------------------------ 7. yeniden baslatma hazirligi */

static void test_reboot_ready(void)
{
    fwu_activate_outcome_e outcome = FWU_ACT_OUTCOME_NONE;

    printf("\n--- yeniden baslatma: yalniz son yanit TESLIM edilince hazir ---\n");
    setup(false);
    host_push(FWU_MSG_REQUEST_DOWNLOAD, 1u, tw_manifest, 138u);
    poll_n(1u);
    for(uint32_t i = 0u; i < 11u; i++) {
        fwu_byte_t p[FWU_FRAME_MAX_PAYLOAD];
        size_t n = 0u;
        uint32_t len = (i < 10u) ? TW_CHUNK : (TW_IMG_SIZE - (10u * TW_CHUNK));
        (void)fwu_proto_encode_data(i * TW_CHUNK, &tw_image[i * TW_CHUNK], len, p, sizeof(p), &n);
        host_push(FWU_MSG_TRANSFER_DATA, 2u, p, n);
        poll_n(1u);
    }
    host_push_u32(FWU_MSG_TRANSFER_EXIT, 3u, TW_IMG_SIZE);
    poll_n(1u);
    check("EXIT ve VERIFY_RESULT gitti, durum HAZIR", fwu_session_get_state(&tw_session) == FWU_SESSION_READY && !fwu_link_reboot_ready(&tw_link));
    g_mock.busy_count = 2u;
    host_push_u32(FWU_MSG_ACTIVATE, 4u, TW_IMG_SIZE);
    poll_n(1u);
    check("ACTIVATE islendi, COMMIT yazildi AMA ACTIVATE_OK mesgul tasiyicida bekliyor: yeniden baslatma HAZIR DEGIL",
          fwu_session_reboot_requested(&tw_session) && fwu_link_tx_pending(&tw_link) && !fwu_link_reboot_ready(&tw_link));
    poll_n(1u);
    check("... hala mesgul: hazir degil", !fwu_link_reboot_ready(&tw_link));
    poll_n(1u);
    check("... yanit teslim edildi: yeniden baslatma HAZIR", fwu_link_reboot_ready(&tw_link) && !fwu_link_tx_pending(&tw_link));
    {
        const uint32_t n = g_mock.count;
        check("... son yanit ACTIVATE_OK (SEQ 4)", resp_is(n - 1u, FWU_MSG_ACTIVATE_OK, 4u));
    }
    expect("bootloader", tw_bootloader_run(&outcome), FWU_OK);
    check("... uygulama yeni imaj (bayt akisi + link ile)", outcome == FWU_ACT_OUTCOME_ACTIVATED && tw_app_is_new());
}

/* ------------------------------------------------------------------ 8. fuzz */

static uint32_t g_rng = 987654321u;

static uint32_t rnd(void)
{
    g_rng = (g_rng * 1664525u) + 1013904223u;
    return g_rng >> 8;
}

static void test_fuzz(void)
{
    int ok = 1;
    int app_ok = 1;
    int commit_ok = 1;

    printf("\n--- fuzz: rastgele bayt akisi, rastgele meşgul/hata, araya gecerli cerceveler ---\n");
    setup(false);
    tw_make_package(TW_IMG_SIZE, 25u, 2u);                  /* SAHTE IMZALI: hicbir sirayla commit olmamali */
    for(uint32_t it = 0u; it < 20000u; it++) {
        fwu_byte_t buf[64];
        const uint32_t pick = rnd() % 10u;

        if(pick < 5u) {
            const size_t n = 1u + (size_t)(rnd() % 64u);
            for(size_t i = 0u; i < n; i++) {
                buf[i] = (fwu_byte_t)(rnd() & 0xFFu);
            }
            (void)fwu_ringbuf_push(&tw_rx.base, buf, n);
        } else if(pick < 8u) {
            host_push(FWU_MSG_REQUEST_DOWNLOAD, (fwu_byte_t)rnd(), tw_manifest, 138u);
        } else if(pick == 8u) {
            host_push1(FWU_MSG_HELLO, (fwu_byte_t)rnd());
        } else {
            host_push_u32(FWU_MSG_ACTIVATE, (fwu_byte_t)rnd(), TW_IMG_SIZE);
        }
        if((rnd() % 7u) == 0u) {
            g_mock.busy_count = rnd() % 4u;
        }
        if((rnd() % 31u) == 0u) {
            g_mock.fail_next = true;
            g_mock.fail_code = FWU_ERR_OUT_OF_RANGE;
        }
        g_now += rnd() % 50u;
        if(fwu_link_poll(&tw_link, g_now) != FWU_OK) {
            ok = 0;
        }
        if(g_mock.count >= (MOCK_MAX - 1u)) {
            g_mock.count = 0u;                              /* kayit tamponunu dondur */
        }
    }
    app_ok = tw_app_is_old() && tw_sim.base.erase_counts[0] == 0u && tw_sim.base.erase_counts[18] == 0u;
    commit_ok = (tw_act.last_state == 0u) && (fwu_session_get_state(&tw_session) != FWU_SESSION_COMMITTED) && !fwu_session_reboot_requested(&tw_session);
    check("20000 adim: poll her zaman FWU_OK (cop, meşgul, hata ne olursa)", ok);
    check("... sahte imzali paketle HICBIR sirayla commit olmadi, uygulama ve gecici sayfalara dokunulmadi", app_ok && commit_ok);
    check("... fuzz gercekten calisti: binlerce cerceve oturuma verildi ve yanitlari gonderildi", fwu_link_get_stats(&tw_link)->frames_in > 1000u && fwu_link_get_stats(&tw_link)->frames_out > 1000u);
}

/* ------------------------------------------------------------------ uzanti (fwu_link_set_extension) */

static uint32_t g_ext_calls;
static fwu_byte_t g_ext_last_type;
static void *g_ext_seen_context;

static bool ext_handles(fwu_byte_t type)
{
    return (type == FWU_MSG_KEEPALIVE);                     /* oturum yerine uzantiya giden bir tur */
}

static fwu_return_e ext_on_frame(void *context, const fwu_frame_view_t *view, uint32_t now_ms, fwu_byte_t *out, size_t out_capacity, size_t *out_length)
{
    const fwu_byte_t one[1] = { 0x5Au };

    (void)now_ms;
    g_ext_calls++;
    g_ext_last_type = view->type;
    g_ext_seen_context = context;
    return fwu_frame_encode((fwu_byte_t)(view->type | 0x80u), view->seq, one, 1u, out, out_capacity, out_length);
}

static void test_extension(void)
{
    static int marker;
    fwu_frame_view_t v;

    printf("\n--- uzanti: bazi cerceve turleri oturum yerine baska modulee gider ---\n");
    setup(false);
    expect("kanca ayarlanmadan: yalniz biri NULL", fwu_link_set_extension(&tw_link, ext_handles, NULL, NULL), FWU_ERR_INVALID_ARG);
    expect("link NULL", fwu_link_set_extension(NULL, ext_handles, ext_on_frame, NULL), FWU_ERR_INVALID_ARG);
    expect("gecerli kanca", fwu_link_set_extension(&tw_link, ext_handles, ext_on_frame, &marker), FWU_OK);
    g_ext_calls = 0u;

    host_push1(FWU_MSG_HELLO, 3u);
    expect_poll_ok("HELLO (uzantinin turu degil)");
    check("... oturum yanitladi (INFO), uzanti cagrilmadi", g_mock.count == 1u && resp_is(0u, FWU_MSG_INFO, 3u) && g_ext_calls == 0u);

    host_push1(FWU_MSG_KEEPALIVE, 4u);
    expect_poll_ok("KEEPALIVE (uzantinin turu)");
    check("... uzanti 1 kez cagrildi, baglam ve tur dogru", g_ext_calls == 1u && g_ext_last_type == FWU_MSG_KEEPALIVE && g_ext_seen_context == &marker);
    check("... yanit uzantidan geldi (tur 0x80 | KEEPALIVE, SEQ 4, yuk 0x5A), oturum yanitlamadi",
          g_mock.count == 2u && resp(1u, &v) && v.type == (fwu_byte_t)(FWU_MSG_KEEPALIVE | 0x80u) && v.seq == 4u && v.payload_length == 1u && v.payload[0] == 0x5Au);

    g_mock.busy_count = 2u;
    host_push1(FWU_MSG_KEEPALIVE, 5u);
    expect_poll_ok("uzanti yaniti, tasiyici meshgul");
    check("... yanit bekletildi (tx_pending)", fwu_link_tx_pending(&tw_link) && g_mock.count == 2u);
    expect_poll_ok("yeniden dene 1");
    expect_poll_ok("yeniden dene 2");
    check("... meshgul bitince AYNI yanit gonderildi", !fwu_link_tx_pending(&tw_link) && g_mock.count == 3u && resp_is(2u, (fwu_byte_t)(FWU_MSG_KEEPALIVE | 0x80u), 5u));

    expect("uzanti kaldir (ikisi de NULL)", fwu_link_set_extension(&tw_link, NULL, NULL, NULL), FWU_OK);
    host_push1(FWU_MSG_KEEPALIVE, 6u);
    expect_poll_ok("kanca kaldirildiktan sonra KEEPALIVE");
    check("... oturum yanitladi (KEEPALIVE_OK), uzanti cagrilmadi", g_mock.count == 4u && resp_is(3u, FWU_MSG_KEEPALIVE_OK, 6u) && g_ext_calls == 2u);

    /* yeniden init kancayi sifirlar */
    expect("kanca tekrar", fwu_link_set_extension(&tw_link, ext_handles, ext_on_frame, &marker), FWU_OK);
    setup(false);
    host_push1(FWU_MSG_KEEPALIVE, 8u);
    expect_poll_ok("yeni setup (init) sonrasi KEEPALIVE");
    check("... init uzantiyi sifirladi: oturum yanitladi", resp_is(0u, FWU_MSG_KEEPALIVE_OK, 8u));
}

int main(void)
{
    test_init();
    test_basic();
    test_frame_limit();
    test_busy();
    test_tx_error();
    test_poll_garbage_timeout();
    test_reboot_ready();
    test_fuzz();
    test_extension();

    printf("\nResult: %s (%d failed)\n", g_failures == 0 ? "ALL TESTS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
