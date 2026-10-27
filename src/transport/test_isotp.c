/**
 * @file    test_isotp.c
 * @brief   fwu_isotp (ISO-TP alt kümesi) için kendi kendini kontrol eden test. Beklenen CAN çerçeveleri ELLE yazılmıştır
 *          (gönderen modülün çıktısından türetilmez).
 *
 * Kapsam: init; bilinen çerçeveler (SF, FF, CF, SN sarması); 1..521 TÜM uzunluklar iki uç arasında; çerçeve ve akış kontrolü
 * sayıları elle türetildi; BS/STmin davranışı ve zamanlaması; FC(WAIT/OVFLW), zaman aşımları (sınır değerleri); alıcıda her
 * protokol ihlali (yanlış SN, beklenmeyen CF/FC, geçersiz uzunluk, kısa DLC, bilinmeyen PCI, yeni SF/FF birleştirmeyi iptal);
 * posta kutusu dolu (FF, CF, FC yeniden denenir); sert sürücü hatası; çift yönlü eşzamanlı trafik; fuzz.
 *
 * Derleme (transport klasöründen):
 *   gcc -Wall -Wextra -Wpedantic -Wconversion -std=c99 -I. -I.. -I../error -I../bytes -I../frame fwu_isotp.c test_isotp.c -o test_isotp && ./test_isotp
 */

#include <stdio.h>
#include "fwu_isotp.h"

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

/* ------------------------------------------------------------------ iki uclu CAN hatti */

#define LOG_MAX 2048u

typedef struct {
    fwu_byte_t f[8];
    uint32_t   t;
} can_frame_t;

typedef struct {
    can_frame_t  log[LOG_MAX];
    uint32_t     n;                 /* gonderilen cerceve sayisi */
    uint32_t     delivered;         /* karsi uca iletilenler */
    uint32_t     busy;              /* sonraki bu kadar can_send BUFFER_FULL doner */
    bool         fail_on;           /* fail_at'inci gonderim sert hata doner */
    uint32_t     fail_at;
    uint32_t     attempts;          /* can_send cagri sayisi */
} port_t;

typedef struct {
    fwu_isotp_t iso;
    port_t      port;
    fwu_byte_t  got[FWU_ISOTP_MAX_MESSAGE];     /* son teslim edilen mesaj */
    size_t      got_length;
    uint32_t    got_count;
    uint32_t    now;                /* gonderim zamani icin */
} end_t;

static end_t A;
static end_t B;
static uint32_t g_now;

static fwu_return_e can_send(void *context, const fwu_byte_t *data)
{
    end_t *e = (end_t *)context;

    e->port.attempts++;
    if(e->port.busy > 0u) {
        e->port.busy--;
        return FWU_ERR_BUFFER_FULL;
    }
    if(e->port.fail_on && (e->port.n == e->port.fail_at)) {
        e->port.fail_on = false;
        return FWU_ERR_FLASH_POWER_LOSS;                /* herhangi bir sert hata */
    }
    if(e->port.n < LOG_MAX) {
        for(uint32_t i = 0u; i < 8u; i++) {
            e->port.log[e->port.n].f[i] = data[i];
        }
        e->port.log[e->port.n].t = g_now;
    }
    e->port.n++;
    return FWU_OK;
}

static void deliver(void *context, const fwu_byte_t *message, size_t length)
{
    end_t *e = (end_t *)context;

    e->got_length = length;
    for(size_t i = 0u; i < length; i++) {
        e->got[i] = message[i];
    }
    e->got_count++;
}

static void end_init(end_t *e, uint32_t bs, uint32_t st_min, uint32_t timeout, uint32_t wft)
{
    fwu_isotp_config_t c;

    e->port.n = 0u;
    e->port.delivered = 0u;
    e->port.busy = 0u;
    e->port.fail_on = false;
    e->port.fail_at = 0u;
    e->port.attempts = 0u;
    e->got_length = 0u;
    e->got_count = 0u;
    c.can_send = can_send;
    c.deliver = deliver;
    c.context = e;
    c.block_size = bs;
    c.st_min_ms = st_min;
    c.timeout_ms = timeout;
    c.wft_max = wft;
    (void)fwu_isotp_init(&e->iso, &c);
}

static void setup_pair(uint32_t a_bs, uint32_t a_st, uint32_t b_bs, uint32_t b_st)
{
    end_init(&A, a_bs, a_st, 1000u, 10u);
    end_init(&B, b_bs, b_st, 1000u, 10u);
    g_now = 100u;
}

/** Bir uctan digerine bekleyen cerceveleri iletir; iletilen cerceve sayisi. */
static uint32_t pump_one(end_t *from, end_t *to)
{
    uint32_t moved = 0u;

    while(from->port.delivered < from->port.n) {
        if(from->port.delivered < LOG_MAX) {
            (void)fwu_isotp_on_can_frame(&to->iso, from->port.log[from->port.delivered].f, 8u, g_now);
        }
        from->port.delivered++;
        moved++;
    }
    return moved;
}

/** Zamani 1 ms adimlarla ilerletir; iki uc de bosalip hat sessizlesene kadar (en cok max_ms). */
static void run(uint32_t max_ms)
{
    for(uint32_t ms = 0u; ms < max_ms; ms++) {
        uint32_t moved;

        g_now++;
        (void)fwu_isotp_poll(&A.iso, g_now);
        (void)fwu_isotp_poll(&B.iso, g_now);
        do {
            moved = pump_one(&A, &B) + pump_one(&B, &A);
        } while(moved != 0u);
        if(!fwu_isotp_tx_busy(&A.iso) && !fwu_isotp_tx_busy(&B.iso) && (A.port.delivered == A.port.n) && (B.port.delivered == B.port.n) &&
           !A.iso.fc_pending && !B.iso.fc_pending) {
            return;
        }
    }
}

static void make_message(fwu_byte_t *m, size_t n, uint32_t seed)
{
    for(size_t i = 0u; i < n; i++) {
        m[i] = (fwu_byte_t)((i * 31u + seed * 7u + 5u) & 0xFFu);
    }
}

static int message_equals(const end_t *e, const fwu_byte_t *m, size_t n)
{
    if(e->got_length != n) {
        return 0;
    }
    for(size_t i = 0u; i < n; i++) {
        if(e->got[i] != m[i]) {
            return 0;
        }
    }
    return 1;
}

static int frame_is(const can_frame_t *cf, const unsigned *want)
{
    for(unsigned i = 0u; i < 8u; i++) {
        if(cf->f[i] != (fwu_byte_t)want[i]) {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ 1. init */

static void test_init(void)
{
    fwu_isotp_t iso;
    fwu_isotp_config_t c;

    printf("\n--- init dogrulamasi ---\n");
    end_init(&A, 8u, 2u, 1000u, 10u);
    c.can_send = can_send; c.deliver = deliver; c.context = &A; c.block_size = 8u; c.st_min_ms = 2u; c.timeout_ms = 1000u; c.wft_max = 10u;
    expect("gecerli", fwu_isotp_init(&iso, &c), FWU_OK);
    expect("iso NULL", fwu_isotp_init(NULL, &c), FWU_ERR_INVALID_ARG);
    expect("config NULL", fwu_isotp_init(&iso, NULL), FWU_ERR_INVALID_ARG);
    c.can_send = NULL;
    expect("can_send NULL", fwu_isotp_init(&iso, &c), FWU_ERR_INVALID_ARG);
    c.can_send = can_send; c.deliver = NULL;
    expect("deliver NULL", fwu_isotp_init(&iso, &c), FWU_ERR_INVALID_ARG);
    c.deliver = deliver; c.block_size = 256u;
    expect("block_size 256", fwu_isotp_init(&iso, &c), FWU_ERR_INVALID_ARG);
    c.block_size = 255u;
    expect("block_size 255 (sinir)", fwu_isotp_init(&iso, &c), FWU_OK);
    c.st_min_ms = 128u;
    expect("st_min 128", fwu_isotp_init(&iso, &c), FWU_ERR_INVALID_ARG);
    c.st_min_ms = 127u;
    expect("st_min 127 (sinir)", fwu_isotp_init(&iso, &c), FWU_OK);
    c.timeout_ms = 0u;
    expect("timeout 0", fwu_isotp_init(&iso, &c), FWU_ERR_INVALID_ARG);
    c.timeout_ms = 1u;
    expect("timeout 1 (sinir)", fwu_isotp_init(&iso, &c), FWU_OK);
    c.block_size = 0u; c.st_min_ms = 0u;
    expect("BS 0, STmin 0 (sinirsiz)", fwu_isotp_init(&iso, &c), FWU_OK);

    {
        const fwu_byte_t d[2] = { 1u, 2u };
        fwu_byte_t frame[8] = { 0x01u, 0xAAu, 0xCCu, 0xCCu, 0xCCu, 0xCCu, 0xCCu, 0xCCu };
        fwu_isotp_t bad;
        bad.initialized = false;
        expect("send: iso NULL", fwu_isotp_send(NULL, d, 2u, 0u), FWU_ERR_INVALID_ARG);
        expect("send: init edilmemis", fwu_isotp_send(&bad, d, 2u, 0u), FWU_ERR_INVALID_ARG);
        expect("send: veri NULL", fwu_isotp_send(&iso, NULL, 2u, 0u), FWU_ERR_INVALID_ARG);
        expect("send: uzunluk 0", fwu_isotp_send(&iso, d, 0u, 0u), FWU_ERR_INVALID_ARG);
        expect("on_can_frame: iso NULL", fwu_isotp_on_can_frame(NULL, frame, 8u, 0u), FWU_ERR_INVALID_ARG);
        expect("on_can_frame: init edilmemis", fwu_isotp_on_can_frame(&bad, frame, 8u, 0u), FWU_ERR_INVALID_ARG);
        expect("on_can_frame: veri NULL", fwu_isotp_on_can_frame(&iso, NULL, 8u, 0u), FWU_ERR_INVALID_ARG);
        expect("on_can_frame: uzunluk 0", fwu_isotp_on_can_frame(&iso, frame, 0u, 0u), FWU_ERR_INVALID_ARG);
        expect("on_can_frame: uzunluk 9", fwu_isotp_on_can_frame(&iso, frame, 9u, 0u), FWU_ERR_INVALID_ARG);
        expect("poll: iso NULL", fwu_isotp_poll(NULL, 0u), FWU_ERR_INVALID_ARG);
        expect("poll: init edilmemis", fwu_isotp_poll(&bad, 0u), FWU_ERR_INVALID_ARG);
        check("sorgular NULL'a dayanikli", !fwu_isotp_tx_busy(NULL) && fwu_isotp_get_stats(NULL) == NULL);
        expect("transport send: context NULL", fwu_isotp_transport_send(NULL, d, 2u), FWU_ERR_INVALID_ARG);
    }
    {
        static fwu_byte_t big[FWU_ISOTP_MAX_MESSAGE + 1u];
        fwu_isotp_t iso2;
        c.can_send = can_send; c.context = &A;
        (void)fwu_isotp_init(&iso2, &c);
        expect("send: uzunluk 522 (sinir + 1)", fwu_isotp_send(&iso2, big, 522u, 0u), FWU_ERR_INVALID_ARG);
        check("... en buyuk mesaj 521", FWU_ISOTP_MAX_MESSAGE == 521u);
    }
}

/* ------------------------------------------------------------------ 2. bilinen cerceveler */

static void test_known_frames(void)
{
    fwu_byte_t m[521];

    printf("\n--- bilinen cerceveler (ELLE yazilmis, ISO-TP SF/FF/CF/FC) ---\n");
    make_message(m, 521u, 1u);

    /* SF: 5 bayt */
    setup_pair(8u, 2u, 8u, 2u);
    expect("5 baytlik mesaj gonder", fwu_isotp_send(&A.iso, m, 5u, g_now), FWU_OK);
    {
        const unsigned want[8] = { 0x05u, m[0], m[1], m[2], m[3], m[4], 0xCCu, 0xCCu };
        check("SF: [0x05][5 bayt][CC CC] tek cerceve, gonderim hemen bitti", A.port.n == 1u && frame_is(&A.port.log[0], want) && !fwu_isotp_tx_busy(&A.iso));
    }
    /* SF: 7 bayt (en buyuk) */
    setup_pair(8u, 2u, 8u, 2u);
    (void)fwu_isotp_send(&A.iso, m, 7u, g_now);
    {
        const unsigned want[8] = { 0x07u, m[0], m[1], m[2], m[3], m[4], m[5], m[6] };
        check("SF: 7 bayt: [0x07][7 bayt] (dolgu yok)", A.port.n == 1u && frame_is(&A.port.log[0], want));
    }
    /* SF: 1 bayt */
    setup_pair(8u, 2u, 8u, 2u);
    (void)fwu_isotp_send(&A.iso, m, 1u, g_now);
    {
        const unsigned want[8] = { 0x01u, m[0], 0xCCu, 0xCCu, 0xCCu, 0xCCu, 0xCCu, 0xCCu };
        check("SF: 1 bayt: [0x01][bayt][CC x6]", A.port.n == 1u && frame_is(&A.port.log[0], want));
    }
    /* FF: 8 bayt */
    setup_pair(8u, 2u, 8u, 2u);
    (void)fwu_isotp_send(&A.iso, m, 8u, g_now);
    {
        const unsigned want[8] = { 0x10u, 0x08u, m[0], m[1], m[2], m[3], m[4], m[5] };
        check("8 baytlik mesaj FF ile baslar: [0x10][0x08][6 bayt], gonderim BITMEDI (FC bekliyor)",
              A.port.n == 1u && frame_is(&A.port.log[0], want) && fwu_isotp_tx_busy(&A.iso));
    }
    run(50u);
    {
        const unsigned want_fc[8] = { 0x30u, 0x08u, 0x02u, 0xCCu, 0xCCu, 0xCCu, 0xCCu, 0xCCu };
        const unsigned want_cf[8] = { 0x21u, m[6], m[7], 0xCCu, 0xCCu, 0xCCu, 0xCCu, 0xCCu };
        check("alici FC(CTS) yollar: [0x30][BS 8][STmin 2][CC x5]", B.port.n == 1u && frame_is(&B.port.log[0], want_fc));
        check("gonderen CF1: [0x21][2 bayt][CC x5]", A.port.n == 2u && frame_is(&A.port.log[1], want_cf));
        check("mesaj teslim edildi ve gonderim bitti", message_equals(&B, m, 8u) && !fwu_isotp_tx_busy(&A.iso));
    }
    /* 20 bayt: FF + 2 CF (6 + 7 + 7) */
    setup_pair(8u, 2u, 8u, 2u);
    (void)fwu_isotp_send(&A.iso, m, 20u, g_now);
    run(100u);
    {
        const unsigned ff[8] = { 0x10u, 0x14u, m[0], m[1], m[2], m[3], m[4], m[5] };
        const unsigned cf1[8] = { 0x21u, m[6], m[7], m[8], m[9], m[10], m[11], m[12] };
        const unsigned cf2[8] = { 0x22u, m[13], m[14], m[15], m[16], m[17], m[18], m[19] };
        check("20 bayt: FF [0x10][0x14] + CF1 [0x21] + CF2 [0x22] (6 + 7 + 7 = 20), tam 3 cerceve",
              A.port.n == 3u && frame_is(&A.port.log[0], ff) && frame_is(&A.port.log[1], cf1) && frame_is(&A.port.log[2], cf2));
        check("... teslim", message_equals(&B, m, 20u));
    }
    /* 521 bayt: FF uzunluk alani 0x209, SN sarmasi */
    setup_pair(0u, 0u, 0u, 0u);                              /* alici BS 0, STmin 0 */
    (void)fwu_isotp_send(&A.iso, m, 521u, g_now);
    run(200u);
    {
        const unsigned ff[8] = { 0x12u, 0x09u, m[0], m[1], m[2], m[3], m[4], m[5] };
        check("521 bayt: FF [0x12][0x09] (uzunluk 0x0209), CF sayisi (521-6)/7 yukari = 74, toplam 75 cerceve", A.port.n == 75u && frame_is(&A.port.log[0], ff));
        check("... SN: CF1 = 0x21, CF15 = 0x2F, CF16 = 0x20 (sarma), CF17 = 0x21, son CF (74.) SN = (74 mod 16) = 0x2A",
              (A.port.log[1].f[0] == 0x21u) && (A.port.log[15].f[0] == 0x2Fu) && (A.port.log[16].f[0] == 0x20u) &&
              (A.port.log[17].f[0] == 0x21u) && (A.port.log[74].f[0] == 0x2Au));
        check("... son CF 3 veri baytiyla (521 - 6 - 73 x 7 = 4)?", 521u - 6u - (73u * 7u) == 4u && A.port.log[74].f[5] == 0xCCu && A.port.log[74].f[4] != 0xCCu + 1u);
        check("... teslim: 521 bayt birebir", message_equals(&B, m, 521u));
        check("... BS 0, STmin 0: alici TEK FC yolladi", B.port.n == 1u && B.iso.stats.fc_sent == 1u);
    }
}

/* ------------------------------------------------------------------ 3. tum uzunluklar */

static void test_all_lengths(void)
{
    static fwu_byte_t m[FWU_ISOTP_MAX_MESSAGE];
    int deliver_ok = 1;
    int cf_ok = 1;
    int fc_ok = 1;
    int gap_ok = 1;
    int busy_ok = 1;
    const uint32_t bs = 8u;
    const uint32_t st = 2u;

    printf("\n--- 1..521 TUM uzunluklar (BS 8, STmin 2): teslim, cerceve sayisi, FC sayisi, STmin araligi ---\n");
    for(uint32_t len = 1u; len <= 521u; len++) {
        const uint32_t cfs = (len <= 7u) ? 0u : ((len - 6u) + 6u) / 7u;
        const uint32_t fcs = (len <= 7u) ? 0u : (1u + ((cfs - 1u) / bs));

        make_message(m, len, len);
        setup_pair(0u, 0u, bs, st);
        (void)fwu_isotp_send(&A.iso, m, len, g_now);
        run(2000u);
        if(!message_equals(&B, m, len) || (B.got_count != 1u)) {
            deliver_ok = 0;
        }
        if(A.iso.stats.cf_sent != cfs) {
            cf_ok = 0;
        }
        if(B.iso.stats.fc_sent != fcs) {
            fc_ok = 0;
        }
        if(fwu_isotp_tx_busy(&A.iso) || (A.iso.stats.tx_messages != 1u)) {
            busy_ok = 0;
        }
        for(uint32_t k = 2u; (k < A.port.n) && (k < LOG_MAX); k++) {
            const uint32_t gap = A.port.log[k].t - A.port.log[k - 1u].t;
            /* ayni blok icindeki ardisik CF'ler STmin'den sik olamaz */
            if(((k - 1u) % bs) != 0u) {
                if(gap < st) {
                    gap_ok = 0;
                }
            }
        }
    }
    check("her uzunlukta mesaj BIREBIR teslim edildi (tam 1 kez)", deliver_ok);
    check("... gonderilen CF sayisi = ceil((uzunluk - 6) / 7) (elle turetildi)", cf_ok);
    check("... alicinin FC sayisi = 1 + floor((CF - 1) / BS) (son CF'den sonra FC yok)", fc_ok);
    check("... gonderim bitti, tam 1 mesaj sayildi", busy_ok);
    check("... blok icinde ardisik CF'ler arasi >= STmin (2 ms)", gap_ok);
}

/* ------------------------------------------------------------------ 4. BS ve STmin */

static void test_flow_control(void)
{
    static fwu_byte_t m[200];
    static const uint32_t bss[] = { 0u, 1u, 2u, 3u, 7u, 8u, 28u, 29u, 255u };
    int ok = 1;
    int fc_ok = 1;

    printf("\n--- BS ve STmin ---\n");
    make_message(m, 200u, 3u);                               /* 200 bayt: FF + ceil(194/7) = 28 CF */
    for(unsigned k = 0u; k < sizeof(bss) / sizeof(bss[0]); k++) {
        const uint32_t bs = bss[k];
        const uint32_t cfs = 28u;
        const uint32_t fcs = (bs == 0u) ? 1u : (1u + ((cfs - 1u) / bs));

        setup_pair(0u, 0u, bs, 0u);
        (void)fwu_isotp_send(&A.iso, m, 200u, g_now);
        run(500u);
        ok = ok && message_equals(&B, m, 200u) && (A.iso.stats.cf_sent == cfs);
        fc_ok = fc_ok && (B.iso.stats.fc_sent == fcs);
        if(B.iso.stats.fc_sent != fcs) {
            printf("[FAIL] BS %u: FC sayisi %u (expected %u)\n", (unsigned)bs, (unsigned)B.iso.stats.fc_sent, (unsigned)fcs);
        }
    }
    check("200 bayt, BS = 0, 1, 2, 3, 7, 8, 28, 29, 255: hepsi teslim, 28 CF", ok);
    check("... FC sayisi = 1 + floor(27 / BS) (BS 0: 1); BS 28 ve ustu: 1", fc_ok);

    /* BS 3: gonderen her 3 CF'den sonra DURUR ve FC bekler */
    setup_pair(0u, 0u, 3u, 0u);
    (void)fwu_isotp_send(&A.iso, m, 200u, g_now);
    (void)pump_one(&A, &B);                                  /* FF -> alici FC yollar (B'nin kaydinda) */
    (void)pump_one(&B, &A);                                  /* FC -> gonderen CF'leri gonderir */
    check("BS 3: FC sonrasi gonderen TAM 3 CF yolladi (FF + 3 = 4 cerceve) ve DURDU (yeni FC bekliyor)",
          A.port.n == 4u && fwu_isotp_tx_busy(&A.iso) && A.iso.tx_state == 2u);
    (void)fwu_isotp_poll(&A.iso, g_now + 5u);
    check("... poll bunu degistirmez (FC gelmeden devam yok)", A.port.n == 4u);

    /* STmin: gonderen CF'ler arasinda en az STmin bekler */
    setup_pair(0u, 0u, 0u, 5u);
    (void)fwu_isotp_send(&A.iso, m, 30u, g_now);             /* FF + 4 CF */
    run(200u);
    {
        int gaps_ok = (A.port.n == 5u);
        for(uint32_t k = 2u; (k < A.port.n) && (k < LOG_MAX); k++) {
            if((A.port.log[k].t - A.port.log[k - 1u].t) < 5u) {
                gaps_ok = 0;
            }
        }
        check("STmin 5 ms: ardisik CF'ler arasi >= 5 ms, 30 bayt teslim", gaps_ok && message_equals(&B, m, 30u));
    }
    /* STmin 0 + poll basina sinir: bir adimda en cok 8 CF */
    setup_pair(0u, 0u, 0u, 0u);
    (void)fwu_isotp_send(&A.iso, m, 200u, g_now);
    (void)pump_one(&A, &B);
    (void)pump_one(&B, &A);
    check("STmin 0, BS 0: FC alininca ilk adimda en cok 8 CF (FF + 8 = 9 cerceve)", A.port.n == 9u);
    (void)fwu_isotp_poll(&A.iso, g_now + 1u);
    check("... sonraki poll 8 CF daha (17)", A.port.n == 17u);
    run(100u);
    check("... mesaj tamamlanir ve teslim edilir", message_equals(&B, m, 200u));

    /* STmin kodlari: 0xF1..0xF9 -> 1 ms, ayrilmis -> 127 ms */
    {
        static const unsigned codes[] = { 0x00u, 0x01u, 0x7Fu, 0xF1u, 0xF9u, 0x80u, 0xF0u, 0xFAu, 0xFFu };
        static const uint32_t gaps[] = { 0u, 1u, 127u, 1u, 1u, 127u, 127u, 127u, 127u };
        int codes_ok = 1;

        for(unsigned k = 0u; k < sizeof(codes) / sizeof(codes[0]); k++) {
            const unsigned fc[8] = { 0x30u, 0x00u, codes[k], 0xCCu, 0xCCu, 0xCCu, 0xCCu, 0xCCu };
            fwu_byte_t f[8];
            for(unsigned i = 0u; i < 8u; i++) {
                f[i] = (fwu_byte_t)fc[i];
            }
            setup_pair(0u, 0u, 0u, 0u);
            (void)fwu_isotp_send(&A.iso, m, 30u, g_now);     /* FF gitti, FC bekliyor */
            (void)fwu_isotp_on_can_frame(&A.iso, f, 8u, g_now);
            /* ilk CF hemen gitti; ikincisi STmin sonra */
            if(gaps[k] > 0u) {
                (void)fwu_isotp_poll(&A.iso, g_now + gaps[k] - 1u);
                codes_ok = codes_ok && (A.port.n == 2u);
            }
            (void)fwu_isotp_poll(&A.iso, g_now + gaps[k]);
            codes_ok = codes_ok && (A.port.n == ((gaps[k] > 0u) ? 3u : 5u));
        }
        check("STmin kodlari: 0 -> 0 ms, 1 -> 1, 0x7F -> 127, 0xF1..0xF9 -> 1 ms (yuvarlama), ayrilmis (0x80, 0xF0, 0xFA, 0xFF) -> 127 ms", codes_ok);
    }
}

/* ------------------------------------------------------------------ 5. gonderen: FC(WAIT/OVFLW), zaman asimi */

static void inject(end_t *e, unsigned b0, unsigned b1, unsigned b2, size_t len)
{
    fwu_byte_t f[8] = { (fwu_byte_t)b0, (fwu_byte_t)b1, (fwu_byte_t)b2, 0xCCu, 0xCCu, 0xCCu, 0xCCu, 0xCCu };
    (void)fwu_isotp_on_can_frame(&e->iso, f, len, g_now);
}

static void test_sender_edge(void)
{
    static fwu_byte_t m[100];

    printf("\n--- gonderen: FC(WAIT), FC(OVFLW), zaman asimi (sinir degerleri) ---\n");
    make_message(m, 100u, 4u);

    /* WAIT: wft_max = 3 */
    end_init(&A, 0u, 0u, 1000u, 3u);
    g_now = 100u;
    (void)fwu_isotp_send(&A.iso, m, 100u, g_now);
    inject(&A, 0x31u, 0u, 0u, 3u);
    inject(&A, 0x31u, 0u, 0u, 3u);
    inject(&A, 0x31u, 0u, 0u, 3u);
    check("3 WAIT (sinir) kabul: gonderim suruyor", fwu_isotp_tx_busy(&A.iso) && A.iso.stats.tx_aborts == 0u);
    inject(&A, 0x31u, 0u, 0u, 3u);
    check("4. WAIT: gonderim IPTAL (tx_aborts 1)", !fwu_isotp_tx_busy(&A.iso) && A.iso.stats.tx_aborts == 1u);
    /* WAIT sayaci CTS ile sifirlanir (yeni mesajda) */
    (void)fwu_isotp_send(&A.iso, m, 100u, g_now);
    inject(&A, 0x31u, 0u, 0u, 3u);
    inject(&A, 0x31u, 0u, 0u, 3u);
    inject(&A, 0x30u, 8u, 0u, 3u);
    check("2 WAIT + CTS: gonderim basladi (CF'ler gidiyor)", A.port.n > 2u);

    /* WAIT zaman asimini yeniler */
    end_init(&A, 0u, 0u, 1000u, 10u);
    g_now = 100u;
    (void)fwu_isotp_send(&A.iso, m, 100u, g_now);
    g_now = 900u;
    inject(&A, 0x31u, 0u, 0u, 3u);
    (void)fwu_isotp_poll(&A.iso, 1099u);
    check("WAIT zaman asimi sayacini yeniler (900'de WAIT, 1099'da hala bekliyor)", fwu_isotp_tx_busy(&A.iso));
    (void)fwu_isotp_poll(&A.iso, 1900u);
    check("... WAIT'ten 1000 ms sonra zaman asimi", !fwu_isotp_tx_busy(&A.iso) && A.iso.stats.tx_timeouts == 1u);

    /* OVFLW */
    end_init(&A, 0u, 0u, 1000u, 10u);
    g_now = 100u;
    (void)fwu_isotp_send(&A.iso, m, 100u, g_now);
    inject(&A, 0x32u, 0u, 0u, 3u);
    check("FC(OVFLW): gonderim IPTAL", !fwu_isotp_tx_busy(&A.iso) && A.iso.stats.tx_aborts == 1u);

    /* zaman asimi sinir */
    end_init(&A, 0u, 0u, 1000u, 10u);
    g_now = 100u;
    (void)fwu_isotp_send(&A.iso, m, 100u, g_now);
    (void)fwu_isotp_poll(&A.iso, 100u + 999u);
    check("FC gelmeden 999 ms: hala bekliyor", fwu_isotp_tx_busy(&A.iso));
    (void)fwu_isotp_poll(&A.iso, 100u + 1000u);
    check("1000 ms: zaman asimi, gonderim iptal (tx_timeouts 1, tx_aborts 1)", !fwu_isotp_tx_busy(&A.iso) && A.iso.stats.tx_timeouts == 1u && A.iso.stats.tx_aborts == 1u);
    expect("iptalden sonra yeni gonderim kabul", fwu_isotp_send(&A.iso, m, 5u, g_now), FWU_OK);

    /* zaman sarmasi */
    end_init(&A, 0u, 0u, 1000u, 10u);
    g_now = 0xFFFFFF00u;
    (void)fwu_isotp_send(&A.iso, m, 100u, g_now);
    (void)fwu_isotp_poll(&A.iso, 0xFFFFFF10u);
    check("sarmaya yakin: 16 ms sonra hala bekliyor (sarma-guvensiz karsilastirma erken iptal ederdi)", fwu_isotp_tx_busy(&A.iso));
    (void)fwu_isotp_poll(&A.iso, 0xFFFFFF00u + 999u);
    check("sarma sonrasi 999 ms: hala bekliyor", fwu_isotp_tx_busy(&A.iso));
    (void)fwu_isotp_poll(&A.iso, 0xFFFFFF00u + 1000u);
    check("sarma sonrasi 1000 ms: zaman asimi", !fwu_isotp_tx_busy(&A.iso));

    /* gonderim sirasinda yeni gonderim */
    end_init(&A, 0u, 0u, 1000u, 10u);
    g_now = 100u;
    (void)fwu_isotp_send(&A.iso, m, 100u, g_now);
    expect("gonderim surerken yeni send: BUFFER_FULL", fwu_isotp_send(&A.iso, m, 5u, g_now), FWU_ERR_BUFFER_FULL);
    check("... surmekte olan gonderim bozulmadi (hala bekliyor, 1 cerceve gitti)", fwu_isotp_tx_busy(&A.iso) && A.port.n == 1u);

    /* beklenmeyen FC (gonderim yokken) */
    end_init(&A, 0u, 0u, 1000u, 10u);
    inject(&A, 0x30u, 0u, 0u, 3u);
    check("gonderim yokken FC: yok sayilir, sayilir", A.iso.stats.rx_errors == 1u && A.port.n == 0u);
    expect("... BAD_STATE", fwu_isotp_on_can_frame(&A.iso, (const fwu_byte_t[8]){ 0x30u, 0u, 0u, 0xCCu, 0xCCu, 0xCCu, 0xCCu, 0xCCu }, 8u, 0u), FWU_ERR_BAD_STATE);
    /* FC kisa DLC / ayrilmis FS */
    end_init(&A, 0u, 0u, 1000u, 10u);
    g_now = 100u;
    (void)fwu_isotp_send(&A.iso, m, 100u, g_now);
    expect("FC DLC 2 (>= 3 gerekir): BAD_FRAME", fwu_isotp_on_can_frame(&A.iso, (const fwu_byte_t[2]){ 0x30u, 0u }, 2u, g_now), FWU_ERR_BAD_FRAME);
    check("... gonderim suruyor (gecersiz FC yok sayildi)", fwu_isotp_tx_busy(&A.iso));
    expect("FC FS = 3 (ayrilmis): BAD_FRAME", fwu_isotp_on_can_frame(&A.iso, (const fwu_byte_t[8]){ 0x33u, 0u, 0u, 0xCCu, 0xCCu, 0xCCu, 0xCCu, 0xCCu }, 8u, g_now), FWU_ERR_BAD_FRAME);
    check("... gonderim hala suruyor", fwu_isotp_tx_busy(&A.iso));
}

/* ------------------------------------------------------------------ 6. alici: protokol ihlalleri */

static void test_receiver_violations(void)
{
    static fwu_byte_t m[521];
    fwu_byte_t f[8];

    printf("\n--- alici: her protokol ihlali ---\n");
    make_message(m, 521u, 5u);

    /* CF, FF olmadan */
    end_init(&B, 8u, 2u, 1000u, 10u);
    inject(&B, 0x21u, 1u, 2u, 8u);
    check("CF birlestirme yokken: yok sayilir, BAD_STATE sayilir, teslim yok", B.iso.stats.rx_errors == 1u && B.got_count == 0u);
    expect("... donus BAD_STATE", fwu_isotp_on_can_frame(&B.iso, (const fwu_byte_t[8]){ 0x21u, 1u, 2u, 3u, 4u, 5u, 6u, 7u }, 8u, 0u), FWU_ERR_BAD_STATE);

    /* FF sonrasi yanlis SN */
    end_init(&B, 8u, 2u, 1000u, 10u);
    f[0] = 0x10u; f[1] = 20u; for(unsigned i = 2u; i < 8u; i++) { f[i] = (fwu_byte_t)i; }
    expect("FF 20 bayt", fwu_isotp_on_can_frame(&B.iso, f, 8u, 100u), FWU_OK);
    check("... FC(CTS) yollandi", B.port.n == 1u && B.port.log[0].f[0] == 0x30u);
    f[0] = 0x22u;                                            /* beklenen SN 1, gelen 2 */
    expect("yanlis SN (2 yerine 1): BAD_STATE", fwu_isotp_on_can_frame(&B.iso, f, 8u, 101u), FWU_ERR_BAD_STATE);
    check("... birlestirme iptal (rx_aborts 1), teslim yok", B.iso.stats.rx_aborts == 1u && B.got_count == 0u && !B.iso.rx_active);
    f[0] = 0x21u;
    expect("iptalden sonra dogru SN'li CF (birlestirme yok): BAD_STATE", fwu_isotp_on_can_frame(&B.iso, f, 8u, 102u), FWU_ERR_BAD_STATE);

    /* SN sarmasinda yanlis: 16 CF'den sonra SN 0 beklenir */
    end_init(&B, 0u, 0u, 1000u, 10u);
    f[0] = 0x12u; f[1] = 0x09u;
    (void)fwu_isotp_on_can_frame(&B.iso, f, 8u, 100u);
    for(unsigned sn = 1u; sn <= 15u; sn++) {
        f[0] = (fwu_byte_t)(0x20u | sn);
        (void)fwu_isotp_on_can_frame(&B.iso, f, 8u, 101u);
    }
    f[0] = 0x2Fu;                                            /* 16. CF icin 0 beklenir; 0xF (tekrar) yanlis */
    expect("15 CF'den sonra SN 0 yerine 15: BAD_STATE", fwu_isotp_on_can_frame(&B.iso, f, 8u, 102u), FWU_ERR_BAD_STATE);

    /* gecersiz uzunluklar */
    end_init(&B, 8u, 2u, 1000u, 10u);
    expect("SF uzunluk 0: BAD_LENGTH", fwu_isotp_on_can_frame(&B.iso, (const fwu_byte_t[8]){ 0x00u, 1u, 2u, 3u, 4u, 5u, 6u, 7u }, 8u, 0u), FWU_ERR_BAD_LENGTH);
    expect("SF uzunluk 8 (klasik CAN'da en cok 7): BAD_LENGTH", fwu_isotp_on_can_frame(&B.iso, (const fwu_byte_t[8]){ 0x08u, 1u, 2u, 3u, 4u, 5u, 6u, 7u }, 8u, 0u), FWU_ERR_BAD_LENGTH);
    expect("SF uzunluk 15: BAD_LENGTH", fwu_isotp_on_can_frame(&B.iso, (const fwu_byte_t[8]){ 0x0Fu, 1u, 2u, 3u, 4u, 5u, 6u, 7u }, 8u, 0u), FWU_ERR_BAD_LENGTH);
    expect("SF uzunluk 5 ama DLC 3 (veri sigmaz): BAD_FRAME", fwu_isotp_on_can_frame(&B.iso, (const fwu_byte_t[3]){ 0x05u, 1u, 2u }, 3u, 0u), FWU_ERR_BAD_FRAME);
    expect("SF uzunluk 5, DLC 6 (tam sigar): OK", fwu_isotp_on_can_frame(&B.iso, (const fwu_byte_t[6]){ 0x05u, 1u, 2u, 3u, 4u, 5u }, 6u, 0u), FWU_OK);
    check("... teslim edildi (5 bayt: 1 2 3 4 5)", B.got_count == 1u && B.got_length == 5u && B.got[0] == 1u && B.got[4] == 5u);
    expect("FF uzunluk 7 (SF ile gonderilmeli): BAD_LENGTH", fwu_isotp_on_can_frame(&B.iso, (const fwu_byte_t[8]){ 0x10u, 7u, 1u, 2u, 3u, 4u, 5u, 6u }, 8u, 0u), FWU_ERR_BAD_LENGTH);
    expect("FF uzunluk 0: BAD_LENGTH", fwu_isotp_on_can_frame(&B.iso, (const fwu_byte_t[8]){ 0x10u, 0u, 1u, 2u, 3u, 4u, 5u, 6u }, 8u, 0u), FWU_ERR_BAD_LENGTH);
    expect("FF DLC 7 (8 gerekir): BAD_FRAME", fwu_isotp_on_can_frame(&B.iso, (const fwu_byte_t[7]){ 0x10u, 20u, 1u, 2u, 3u, 4u, 5u }, 7u, 0u), FWU_ERR_BAD_FRAME);
    {
        const uint32_t before = B.port.n;
        expect("FF uzunluk 522 (> 521): BAD_LENGTH", fwu_isotp_on_can_frame(&B.iso, (const fwu_byte_t[8]){ 0x12u, 0x0Au, 1u, 2u, 3u, 4u, 5u, 6u }, 8u, 0u), FWU_ERR_BAD_LENGTH);
        check("... alici FC(OVFLW) yolladi: [0x32] ve birlestirme BASLAMADI", B.port.n == before + 1u && B.port.log[before].f[0] == 0x32u && !B.iso.rx_active);
        expect("FF uzunluk 4095 (12 bit en buyuk): BAD_LENGTH", fwu_isotp_on_can_frame(&B.iso, (const fwu_byte_t[8]){ 0x1Fu, 0xFFu, 1u, 2u, 3u, 4u, 5u, 6u }, 8u, 0u), FWU_ERR_BAD_LENGTH);
        expect("FF uzunluk 521 (sinir): OK", fwu_isotp_on_can_frame(&B.iso, (const fwu_byte_t[8]){ 0x12u, 0x09u, 1u, 2u, 3u, 4u, 5u, 6u }, 8u, 0u), FWU_OK);
        check("... birlestirme basladi", B.iso.rx_active);
    }

    /* bilinmeyen PCI turleri */
    {
        int ok = 1;
        for(unsigned pci = 0x40u; pci <= 0xF0u; pci += 0x10u) {
            const fwu_byte_t g[8] = { (fwu_byte_t)pci, 1u, 2u, 3u, 4u, 5u, 6u, 7u };
            if(fwu_isotp_on_can_frame(&B.iso, g, 8u, 0u) != FWU_ERR_BAD_FRAME) {
                ok = 0;
            }
        }
        check("bilinmeyen PCI turleri 0x4_..0xF_: hepsi BAD_FRAME", ok);
    }

    /* yeni SF birlestirmeyi iptal eder ve teslim edilir */
    end_init(&B, 8u, 2u, 1000u, 10u);
    f[0] = 0x10u; f[1] = 30u; for(unsigned i = 2u; i < 8u; i++) { f[i] = 0xEEu; }
    (void)fwu_isotp_on_can_frame(&B.iso, f, 8u, 100u);
    expect("birlestirme surerken SF", fwu_isotp_on_can_frame(&B.iso, (const fwu_byte_t[8]){ 0x03u, 9u, 8u, 7u, 0xCCu, 0xCCu, 0xCCu, 0xCCu }, 8u, 101u), FWU_OK);
    check("... SF teslim edildi (9 8 7), eski birlestirme IPTAL (rx_aborts 1)", B.got_count == 1u && B.got_length == 3u && B.got[0] == 9u && !B.iso.rx_active && B.iso.stats.rx_aborts == 1u);

    /* yeni FF birlestirmeyi yeniden baslatir */
    end_init(&B, 0u, 0u, 1000u, 10u);
    f[0] = 0x10u; f[1] = 14u; for(unsigned i = 2u; i < 8u; i++) { f[i] = (fwu_byte_t)(0x10u + i); }
    (void)fwu_isotp_on_can_frame(&B.iso, f, 8u, 100u);
    f[0] = 0x10u; f[1] = 14u; for(unsigned i = 2u; i < 8u; i++) { f[i] = (fwu_byte_t)(0x70u + i); }
    (void)fwu_isotp_on_can_frame(&B.iso, f, 8u, 101u);
    check("birlestirme surerken yeni FF: eskisi iptal, yenisi basladi, 2 FC gitti", B.iso.stats.rx_aborts == 1u && B.iso.rx_active && B.iso.stats.fc_sent == 2u);
    f[0] = 0x21u; for(unsigned i = 1u; i < 8u; i++) { f[i] = (fwu_byte_t)(0x80u + i); }
    (void)fwu_isotp_on_can_frame(&B.iso, f, 8u, 102u);
    f[0] = 0x22u;
    (void)fwu_isotp_on_can_frame(&B.iso, f, 8u, 103u);
    check("... 14 bayt yeni FF'in baytlariyla teslim (ilk bayt 0x72, 7. bayt 0x81)", B.got_count == 1u && B.got_length == 14u && B.got[0] == 0x72u && B.got[6] == 0x81u);

    /* CF kisa DLC: son CF'te yalnizca gereken bayt */
    end_init(&B, 0u, 0u, 1000u, 10u);
    (void)fwu_isotp_on_can_frame(&B.iso, (const fwu_byte_t[8]){ 0x10u, 9u, 1u, 2u, 3u, 4u, 5u, 6u }, 8u, 100u);
    expect("son CF (3 bayt gerekir) DLC 3 (1 + 3 = 4 gerekir): BAD_FRAME", fwu_isotp_on_can_frame(&B.iso, (const fwu_byte_t[3]){ 0x21u, 7u, 8u }, 3u, 101u), FWU_ERR_BAD_FRAME);
    check("... birlestirme iptal", !B.iso.rx_active && B.got_count == 0u);
    (void)fwu_isotp_on_can_frame(&B.iso, (const fwu_byte_t[8]){ 0x10u, 9u, 1u, 2u, 3u, 4u, 5u, 6u }, 8u, 110u);
    expect("son CF DLC 4 (tam gerektigi kadar): OK", fwu_isotp_on_can_frame(&B.iso, (const fwu_byte_t[4]){ 0x21u, 7u, 8u, 9u }, 4u, 111u), FWU_OK);
    check("... 9 bayt teslim (1..9)", B.got_count == 1u && B.got_length == 9u && B.got[8] == 9u);

    /* dolgu baytlari yok sayilir */
    end_init(&B, 8u, 2u, 1000u, 10u);
    (void)fwu_isotp_on_can_frame(&B.iso, (const fwu_byte_t[8]){ 0x02u, 0x11u, 0x22u, 0x00u, 0xFFu, 0x55u, 0xAAu, 0x12u }, 8u, 0u);
    check("SF dolgu baytlari (00 FF 55 AA 12) yok sayilir: teslim 2 bayt", B.got_count == 1u && B.got_length == 2u && B.got[0] == 0x11u && B.got[1] == 0x22u);

    /* kirli ust bitler (C28x) */
    end_init(&B, 8u, 2u, 1000u, 10u);
    {
        const fwu_byte_t dirty[8] = { (fwu_byte_t)(0x02u | 0xAB00u), (fwu_byte_t)(0x33u | 0xAB00u), (fwu_byte_t)(0x44u | 0xAB00u), 0u, 0u, 0u, 0u, 0u };
        (void)fwu_isotp_on_can_frame(&B.iso, dirty, 8u, 0u);
        check("kirli ust bitli SF (| 0xAB00, C28x): dogru ayristirilir ve temiz teslim edilir", B.got_count == 1u && B.got_length == 2u && B.got[0] == 0x33u && B.got[1] == 0x44u);
    }

    /* alici zaman asimi */
    end_init(&B, 8u, 2u, 1000u, 10u);
    f[0] = 0x10u; f[1] = 30u; for(unsigned i = 2u; i < 8u; i++) { f[i] = 1u; }
    (void)fwu_isotp_on_can_frame(&B.iso, f, 8u, 100u);
    (void)fwu_isotp_poll(&B.iso, 100u + 999u);
    check("CF gelmeden 999 ms: birlestirme suruyor", B.iso.rx_active);
    (void)fwu_isotp_poll(&B.iso, 100u + 1000u);
    check("1000 ms: zaman asimi, birlestirme iptal (rx_timeouts 1, rx_aborts 1)", !B.iso.rx_active && B.iso.stats.rx_timeouts == 1u && B.iso.stats.rx_aborts == 1u);
    /* her CF zamanlayiciyi yeniler */
    end_init(&B, 0u, 0u, 1000u, 10u);
    (void)fwu_isotp_on_can_frame(&B.iso, f, 8u, 100u);
    f[0] = 0x21u;
    (void)fwu_isotp_on_can_frame(&B.iso, f, 8u, 900u);
    (void)fwu_isotp_poll(&B.iso, 1899u);
    check("CF zamanlayiciyi yeniler (900'de CF, 1899'da hala suruyor)", B.iso.rx_active);
    (void)fwu_isotp_poll(&B.iso, 1900u);
    check("... CF'den 1000 ms sonra zaman asimi", !B.iso.rx_active);
    /* alici zaman sarmasi */
    end_init(&B, 0u, 0u, 1000u, 10u);
    f[0] = 0x10u;
    (void)fwu_isotp_on_can_frame(&B.iso, f, 8u, 0xFFFFFF00u);
    (void)fwu_isotp_poll(&B.iso, 0xFFFFFF10u);
    check("alici: sarmaya yakin 16 ms sonra birlestirme suruyor", B.iso.rx_active);
    (void)fwu_isotp_poll(&B.iso, 0xFFFFFF00u + 1000u);
    check("... sarma sonrasi 1000 ms: zaman asimi", !B.iso.rx_active);
}

/* ------------------------------------------------------------------ 7. posta kutusu dolu, sert hata */

static void test_busy_and_errors(void)
{
    static fwu_byte_t m[100];

    printf("\n--- posta kutusu dolu ve sert surucu hatasi ---\n");
    make_message(m, 100u, 6u);

    /* SF: kutu dolu -> poll'da gider */
    setup_pair(8u, 2u, 8u, 2u);
    A.port.busy = 2u;
    expect("SF, posta kutusu dolu", fwu_isotp_send(&A.iso, m, 5u, g_now), FWU_OK);
    check("... gonderim SURUYOR (cerceve gitmedi)", fwu_isotp_tx_busy(&A.iso) && A.port.n == 0u);
    (void)fwu_isotp_poll(&A.iso, g_now + 1u);
    check("... hala dolu (2. deneme)", A.port.n == 0u && fwu_isotp_tx_busy(&A.iso));
    (void)fwu_isotp_poll(&A.iso, g_now + 2u);
    check("... 3. denemede gitti, gonderim bitti", A.port.n == 1u && !fwu_isotp_tx_busy(&A.iso) && A.iso.stats.tx_messages == 1u);

    /* FF: kutu dolu */
    setup_pair(8u, 2u, 8u, 2u);
    A.port.busy = 3u;
    (void)fwu_isotp_send(&A.iso, m, 100u, g_now);
    run(300u);
    check("FF kutu dolu (3 deneme): sonunda mesaj BIREBIR teslim", message_equals(&B, m, 100u) && A.iso.stats.tx_aborts == 0u);

    /* CF: kutu dolu (ortada) */
    setup_pair(0u, 0u, 0u, 0u);
    (void)fwu_isotp_send(&A.iso, m, 100u, g_now);
    (void)pump_one(&A, &B);
    A.port.busy = 4u;
    (void)pump_one(&B, &A);                                  /* FC: gonderen CF'leri yollamaya calisir, kutu dolu */
    run(300u);
    check("CF kutu dolu (4 deneme): mesaj BIREBIR teslim, iptal yok", message_equals(&B, m, 100u) && A.iso.stats.tx_aborts == 0u);

    /* FC: alicinin kutusu dolu -> poll yeniden dener */
    setup_pair(0u, 0u, 8u, 2u);
    B.port.busy = 2u;
    (void)fwu_isotp_send(&A.iso, m, 100u, g_now);
    (void)pump_one(&A, &B);
    check("FC gonderilemedi: bekletiliyor", B.iso.fc_pending && B.port.n == 0u);
    run(300u);
    check("... poll yeniden dener, mesaj BIREBIR teslim", message_equals(&B, m, 100u) && !B.iso.fc_pending);

    /* FC bekletilirken birlestirme zaman asimina duserse FC iptal */
    setup_pair(0u, 0u, 8u, 2u);
    B.port.busy = 1000u;
    (void)fwu_isotp_send(&A.iso, m, 100u, g_now);
    (void)pump_one(&A, &B);
    check("FC surekli bekliyor", B.iso.fc_pending);
    (void)fwu_isotp_poll(&B.iso, g_now + 1000u);
    check("birlestirme zaman asimina dustu: bekleyen FC iptal (artik anlamsiz)", !B.iso.rx_active && !B.iso.fc_pending);
    {
        const uint32_t attempts = B.port.attempts;
        (void)fwu_isotp_poll(&B.iso, g_now + 1001u);
        check("... FC icin yeni deneme yapilmaz", B.port.attempts == attempts);
    }

    /* sert hata: FF */
    setup_pair(8u, 2u, 8u, 2u);
    A.port.fail_on = true; A.port.fail_at = 0u;
    expect("FF sert hata: send hatayi aynen dondurur", fwu_isotp_send(&A.iso, m, 100u, g_now), FWU_ERR_FLASH_POWER_LOSS);
    check("... gonderim iptal (tx_aborts 1), yeni gonderim kabul", !fwu_isotp_tx_busy(&A.iso) && A.iso.stats.tx_aborts == 1u && fwu_isotp_send(&A.iso, m, 5u, g_now) == FWU_OK);
    /* sert hata: CF ortasinda */
    setup_pair(0u, 0u, 0u, 0u);
    A.port.fail_on = true; A.port.fail_at = 3u;              /* FF, CF1, CF2 gider; 3. gonderim (CF3) hata */
    (void)fwu_isotp_send(&A.iso, m, 100u, g_now);
    run(300u);
    check("CF ortasinda sert hata: gonderim iptal, mesaj teslim EDILMEDI; alici birlestirmesi hala acik (CF gelmiyor)",
          A.iso.stats.tx_aborts == 1u && B.got_count == 0u && !fwu_isotp_tx_busy(&A.iso) && B.iso.rx_active);
    (void)fwu_isotp_poll(&B.iso, g_now + 1000u);
    check("... alici 1000 ms sonra zaman asimiyla temizler (rx_timeouts 1)", !B.iso.rx_active && B.iso.stats.rx_timeouts == 1u);
    /* sert hata: FC (hata dusurulur) */
    setup_pair(0u, 0u, 8u, 2u);
    B.port.fail_on = true; B.port.fail_at = 0u;
    (void)fwu_isotp_send(&A.iso, m, 100u, g_now);
    (void)pump_one(&A, &B);
    check("FC sert hatayla dustu: bekletilmez", !B.iso.fc_pending && B.iso.stats.fc_sent == 0u);
    run(1500u);
    check("... gonderen FC alamaz, zaman asimiyla vazgecer (tx_timeouts 1)", A.iso.stats.tx_timeouts == 1u && !fwu_isotp_tx_busy(&A.iso));
}

/* ------------------------------------------------------------------ 8. cift yonlu, tasiyici uyarlayicilari */

static void test_duplex_and_adapter(void)
{
    static fwu_byte_t ma[300];
    static fwu_byte_t mb[450];
    fwu_transport_t ta;

    printf("\n--- cift yonlu eszamanli trafik ve fwu_transport_t uyarlayicisi ---\n");
    make_message(ma, 300u, 7u);
    make_message(mb, 450u, 8u);
    setup_pair(4u, 1u, 3u, 2u);
    (void)fwu_isotp_send(&A.iso, ma, 300u, g_now);
    (void)fwu_isotp_send(&B.iso, mb, 450u, g_now);
    run(2000u);
    check("ayni anda iki yonde mesaj: ikisi de BIREBIR teslim", message_equals(&B, ma, 300u) && message_equals(&A, mb, 450u));
    check("... sayaclar: her uc 1 gonderdi, 1 aldi", A.iso.stats.tx_messages == 1u && A.iso.stats.rx_messages == 1u && B.iso.stats.tx_messages == 1u && B.iso.stats.rx_messages == 1u);

    /* art arda mesajlar */
    setup_pair(8u, 2u, 8u, 2u);
    {
        int ok = 1;
        static const size_t lens[] = { 5u, 40u, 7u, 8u, 200u, 1u, 521u };
        for(unsigned k = 0u; k < sizeof(lens) / sizeof(lens[0]); k++) {
            static fwu_byte_t mm[521];
            make_message(mm, lens[k], k);
            if(fwu_isotp_send(&A.iso, mm, lens[k], g_now) != FWU_OK) {
                ok = 0;
            }
            run(3000u);
            ok = ok && message_equals(&B, mm, lens[k]);
        }
        check("art arda 7 mesaj (SF, FF, sinirlar, 521): hepsi sirayla teslim", ok && B.got_count == 7u);
    }

    /* fwu_transport_t uyarlayicisi: link'in baktigi arayuz */
    setup_pair(8u, 2u, 8u, 2u);
    ta.send = fwu_isotp_transport_send;
    ta.poll = fwu_isotp_transport_poll;
    ta.context = &A.iso;
    A.iso.now_ms = g_now;
    expect("transport.send: 50 baytlik mesaj kabul", ta.send(ta.context, ma, 50u), FWU_OK);
    expect("... gonderim surerken ikinci send: BUFFER_FULL (link bekletir)", ta.send(ta.context, ma, 50u), FWU_ERR_BUFFER_FULL);
    ta.poll(ta.context, g_now + 1u);
    run(300u);
    check("... transport.poll ve hat ile mesaj teslim", message_equals(&B, ma, 50u));
    expect("... tamamlaninca yeni send kabul", ta.send(ta.context, ma, 50u), FWU_OK);
}

/* ------------------------------------------------------------------ 9. fuzz */

static uint32_t g_rng = 424242u;

static uint32_t rnd(void)
{
    g_rng = (g_rng * 1664525u) + 1013904223u;
    return g_rng >> 8;
}

static void test_fuzz(void)
{
    int ok = 1;
    int sane = 1;

    printf("\n--- fuzz: rastgele ve yari gecerli CAN cerceveleri ---\n");
    end_init(&B, 8u, 2u, 1000u, 10u);
    for(uint32_t it = 0u; it < 200000u; it++) {
        fwu_byte_t f[8];
        size_t dlc = 1u + (size_t)(rnd() % 8u);
        const uint32_t kind = rnd() % 10u;

        for(unsigned i = 0u; i < 8u; i++) {
            f[i] = (fwu_byte_t)(rnd() & 0xFFu);
        }
        if(kind < 3u) {
            f[0] = (fwu_byte_t)(0x10u | (rnd() & 0x03u));    /* FF benzeri, uzunluk 0..1023 */
            dlc = 8u;
        } else if(kind < 6u) {
            f[0] = (fwu_byte_t)(0x20u | (rnd() & 0x0Fu));    /* CF benzeri */
        } else if(kind == 6u) {
            f[0] = (fwu_byte_t)(rnd() & 0x07u);              /* SF benzeri */
        } else if(kind == 7u) {
            f[0] = (fwu_byte_t)(0x30u | (rnd() % 4u));
        }
        g_now += rnd() % 30u;
        {
            const fwu_return_e r = fwu_isotp_on_can_frame(&B.iso, f, dlc, g_now);
            if((r != FWU_OK) && (r != FWU_ERR_BAD_FRAME) && (r != FWU_ERR_BAD_LENGTH) && (r != FWU_ERR_BAD_STATE)) {
                ok = 0;                                      /* yalniz OK / BAD_FRAME / BAD_LENGTH / BAD_STATE beklenir */
            }
        }
        if((rnd() % 5u) == 0u) {
            (void)fwu_isotp_poll(&B.iso, g_now);
        }
        if(B.got_count > 0u) {
            if((B.got_length == 0u) || (B.got_length > FWU_ISOTP_MAX_MESSAGE)) {
                sane = 0;
            }
        }
        if(B.iso.rx_active && ((B.iso.rx_received > B.iso.rx_length) || (B.iso.rx_length > FWU_ISOTP_MAX_MESSAGE) || (B.iso.rx_length < 8u))) {
            sane = 0;
        }
        if(B.port.n >= (LOG_MAX - 1u)) {
            B.port.n = 0u;
            B.port.delivered = 0u;
        }
    }
    check("200000 rastgele cerceve: donus kodlari beklenen kumede (OK, BAD_FRAME, BAD_LENGTH, BAD_STATE)", ok);
    check("... teslim edilen her mesaj 1..521 bayt, birlestirme durumu tutarli (alinan <= uzunluk <= 521)", sane);
    check("... fuzz anlamli: yuzlerce mesaj teslim edildi ve cok sayida ihlal sayildi", B.iso.stats.rx_messages > 100u && B.iso.stats.rx_errors > 1000u);
}

/* ------------------------------------------------------------------ 10. mutasyon testinin buldugu bosluklar */

/** Tek bir hatali CAN cercevesi: donus kodu ve rx_errors tam 1 artar. */
static void error_frame(const char *name, end_t *e, const unsigned *bytes, size_t length, fwu_return_e want)
{
    fwu_byte_t f[8];
    const uint32_t before = e->iso.stats.rx_errors;
    fwu_return_e got;

    for(unsigned i = 0u; i < 8u; i++) {
        f[i] = (fwu_byte_t)((i < length) ? bytes[i] : 0xCCu);
    }
    got = fwu_isotp_on_can_frame(&e->iso, f, length, g_now);
    check(name, (got == want) && (e->iso.stats.rx_errors == (before + 1u)));
}

static void test_mutation_gaps(void)
{
    static fwu_byte_t m[100];
    static fwu_byte_t big[FWU_ISOTP_MAX_MESSAGE];
    fwu_isotp_t dirty;
    fwu_isotp_config_t c;

    printf("\n--- mutasyon testinin buldugu bosluklar ---\n");
    make_message(m, 100u, 11u);

    /* (a) kirli bellek: init kendisi bos durumu kurar (statik sifirli bellege guvenilmez) */
    {
        unsigned char *raw = (unsigned char *)&dirty;
        for(size_t i = 0u; i < sizeof(dirty); i++) {
            raw[i] = 0xA5u;
        }
    }
    end_init(&B, 4u, 0u, 1000u, 10u);
    c.can_send = can_send; c.deliver = deliver; c.context = &A; c.block_size = 4u; c.st_min_ms = 0u; c.timeout_ms = 1000u; c.wft_max = 10u;
    A.port.n = 0u; A.port.delivered = 0u; A.port.busy = 0u; A.port.fail_on = false; A.port.attempts = 0u; A.got_count = 0u;
    expect("kirli (0xA5 dolu) bellekte init", fwu_isotp_init(&dirty, &c), FWU_OK);
    {
        const fwu_isotp_stats_t *st = fwu_isotp_get_stats(&dirty);
        check("... tum sayaclar sifir, gonderim bos, FC bekleyen yok, birlestirme yok",
              st->rx_messages == 0u && st->tx_messages == 0u && st->rx_errors == 0u && st->rx_aborts == 0u && st->rx_timeouts == 0u &&
              st->tx_timeouts == 0u && st->tx_aborts == 0u && st->cf_sent == 0u && st->fc_sent == 0u &&
              !fwu_isotp_tx_busy(&dirty) && !dirty.fc_pending && !dirty.rx_active);
    }
    g_now = 100u;
    (void)fwu_isotp_poll(&dirty, g_now + 5000u);
    check("... kirli bellekten sonra uzun sure poll: hicbir sey gonderilmez (bekleyen FC/gonderim hayaleti yok)", A.port.n == 0u);

    /* (b) basarisiz init, onceki basarili durumu GECERSIZ kilar */
    {
        const fwu_byte_t d[2] = { 1u, 2u };
        c.block_size = 256u;
        expect("gecersiz yapilandirmayla yeniden init", fwu_isotp_init(&dirty, &c), FWU_ERR_INVALID_ARG);
        expect("... nesne artik KULLANILAMAZ (send INVALID_ARG)", fwu_isotp_send(&dirty, d, 2u, 0u), FWU_ERR_INVALID_ARG);
        expect("... poll da INVALID_ARG", fwu_isotp_poll(&dirty, 0u), FWU_ERR_INVALID_ARG);
    }

    /* (c) her alici hatasi rx_errors'u TAM 1 artirir */
    end_init(&A, 8u, 2u, 1000u, 10u);
    g_now = 100u;
    {
        const unsigned sf0[] = { 0x00u };
        const unsigned sf8[] = { 0x08u, 1u, 2u, 3u, 4u, 5u, 6u, 7u };
        const unsigned sf_short[] = { 0x05u, 1u, 2u };
        const unsigned ff_short[] = { 0x10u, 0x20u, 1u, 2u };
        const unsigned ff_small[] = { 0x10u, 0x07u, 1u, 2u, 3u, 4u, 5u, 6u };
        const unsigned ff_huge[] = { 0x12u, 0x0Au, 1u, 2u, 3u, 4u, 5u, 6u };
        const unsigned cf_alone[] = { 0x21u, 1u, 2u, 3u, 4u, 5u, 6u, 7u };
        const unsigned unknown[] = { 0x40u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
        const unsigned fc_alone[] = { 0x30u, 0u, 0u };
        error_frame("SF uzunluk 0: BAD_LENGTH, rx_errors +1", &A, sf0, 8u, FWU_ERR_BAD_LENGTH);
        error_frame("SF uzunluk 8: BAD_LENGTH, rx_errors +1", &A, sf8, 8u, FWU_ERR_BAD_LENGTH);
        error_frame("SF DLC cok kisa: BAD_FRAME, rx_errors +1", &A, sf_short, 3u, FWU_ERR_BAD_FRAME);
        error_frame("FF DLC 4: BAD_FRAME, rx_errors +1", &A, ff_short, 4u, FWU_ERR_BAD_FRAME);
        error_frame("FF toplam 7 (SF ile gitmeliydi): BAD_LENGTH, rx_errors +1", &A, ff_small, 8u, FWU_ERR_BAD_LENGTH);
        error_frame("FF toplam 522 (> 521): BAD_LENGTH, rx_errors +1", &A, ff_huge, 8u, FWU_ERR_BAD_LENGTH);
        check("... bu durumda alici FC(OVFLW) gonderdi: 0x32", A.port.n == 1u && A.port.log[0].f[0] == (fwu_byte_t)0x32u);
        error_frame("birlestirme yokken CF: BAD_STATE, rx_errors +1", &A, cf_alone, 8u, FWU_ERR_BAD_STATE);
        error_frame("bilinmeyen PCI (0x4X): BAD_FRAME, rx_errors +1", &A, unknown, 8u, FWU_ERR_BAD_FRAME);
        error_frame("gonderim yokken FC: BAD_STATE, rx_errors +1", &A, fc_alone, 3u, FWU_ERR_BAD_STATE);
    }
    {
        /* acik birlestirmede: yanlis SN ve kisa CF */
        const unsigned ff[] = { 0x10u, 0x14u, 1u, 2u, 3u, 4u, 5u, 6u };           /* toplam 20 */
        const unsigned cf_bad_sn[] = { 0x23u, 1u, 2u, 3u, 4u, 5u, 6u, 7u };       /* beklenen SN 1 */
        const unsigned cf_ok[] = { 0x21u, 1u, 2u, 3u, 4u, 5u, 6u, 7u };
        const unsigned cf_short[] = { 0x22u, 1u, 2u };                           /* son 7 bayt gerekir ama DLC 3 */
        fwu_byte_t f[8];
        end_init(&A, 8u, 2u, 1000u, 10u);
        for(unsigned i = 0u; i < 8u; i++) {
            f[i] = (fwu_byte_t)ff[i];
        }
        (void)fwu_isotp_on_can_frame(&A.iso, f, 8u, g_now);
        error_frame("CF yanlis SN: BAD_STATE, rx_errors +1", &A, cf_bad_sn, 8u, FWU_ERR_BAD_STATE);
        check("... birlestirme IPTAL (rx_aborts 1)", !A.iso.rx_active && A.iso.stats.rx_aborts == 1u);
        for(unsigned i = 0u; i < 8u; i++) {
            f[i] = (fwu_byte_t)ff[i];
        }
        (void)fwu_isotp_on_can_frame(&A.iso, f, 8u, g_now);
        for(unsigned i = 0u; i < 8u; i++) {
            f[i] = (fwu_byte_t)cf_ok[i];
        }
        (void)fwu_isotp_on_can_frame(&A.iso, f, 8u, g_now);
        error_frame("CF DLC yetersiz: BAD_FRAME, rx_errors +1", &A, cf_short, 3u, FWU_ERR_BAD_FRAME);
        check("... birlestirme IPTAL (rx_aborts 2)", !A.iso.rx_active && A.iso.stats.rx_aborts == 2u);
    }
    {
        /* gonderim sirasinda: FC DLC kisa ve ayrilmis FS sayilir */
        const unsigned fc_short[] = { 0x30u, 0u };
        const unsigned fc_reserved[] = { 0x33u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
        end_init(&A, 8u, 2u, 1000u, 10u);
        (void)fwu_isotp_send(&A.iso, m, 100u, g_now);
        error_frame("FC DLC 2: BAD_FRAME, rx_errors +1", &A, fc_short, 2u, FWU_ERR_BAD_FRAME);
        error_frame("FC FS = 3 (ayrilmis): BAD_FRAME, rx_errors +1", &A, fc_reserved, 8u, FWU_ERR_BAD_FRAME);
    }

    /* (d) ertelenmis FC durumunu KORUR ve tam bir kez gider (OVFLW, posta kutusu dolu) */
    end_init(&A, 8u, 2u, 1000u, 10u);
    g_now = 100u;
    A.port.busy = 2u;
    {
        const unsigned ff_huge[] = { 0x12u, 0x0Au, 1u, 2u, 3u, 4u, 5u, 6u };
        fwu_byte_t f[8];
        for(unsigned i = 0u; i < 8u; i++) {
            f[i] = (fwu_byte_t)ff_huge[i];
        }
        (void)fwu_isotp_on_can_frame(&A.iso, f, 8u, g_now);
    }
    check("FF toplam 522, posta kutusu dolu: FC(OVFLW) bekletiliyor, hicbir sey gitmedi", A.iso.fc_pending && A.port.n == 0u);
    (void)fwu_isotp_poll(&A.iso, g_now + 1u);
    check("... ikinci deneme de dolu", A.iso.fc_pending && A.port.n == 0u);
    (void)fwu_isotp_poll(&A.iso, g_now + 2u);
    check("... ucuncu denemede gitti: DURUM KORUNDU (0x32 = OVFLW, CTS degil), bekleyen kalmadi", A.port.n == 1u && A.port.log[0].f[0] == (fwu_byte_t)0x32u && !A.iso.fc_pending && A.iso.stats.fc_sent == 1u);
    for(uint32_t k = 3u; k < 50u; k++) {
        (void)fwu_isotp_poll(&A.iso, g_now + k);
    }
    check("... sonraki 47 poll'da TEKRAR gonderilmedi (FC tam 1 kez)", A.port.n == 1u && A.iso.stats.fc_sent == 1u);

    /* (e) WAIT zaman asimini YENILER (1100'de hala bekliyor; yenilemeyen kod burada vazgecerdi) */
    end_init(&A, 0u, 0u, 1000u, 10u);
    g_now = 100u;
    (void)fwu_isotp_send(&A.iso, m, 100u, g_now);
    g_now = 900u;
    inject(&A, 0x31u, 0u, 0u, 3u);
    (void)fwu_isotp_poll(&A.iso, 1100u);
    check("WAIT 900'de geldi: 1100'de HALA bekliyor (zamanlayici WAIT'ten itibaren)", fwu_isotp_tx_busy(&A.iso) && A.iso.stats.tx_timeouts == 0u);
    (void)fwu_isotp_poll(&A.iso, 1899u);
    check("... 1899'da hala bekliyor", fwu_isotp_tx_busy(&A.iso));
    (void)fwu_isotp_poll(&A.iso, 1900u);
    check("... 1900'de (WAIT + 1000) zaman asimi", !fwu_isotp_tx_busy(&A.iso) && A.iso.stats.tx_timeouts == 1u);

    /* (f) blok sonu: FC bekleme zamanlayicisi BLOGUN SON CF'sinden baslar; CTS zamani on_can_frame'in zamanidir */
    end_init(&A, 0u, 0u, 1000u, 10u);
    g_now = 100u;
    (void)fwu_isotp_send(&A.iso, m, 100u, g_now);
    g_now = 500u;
    inject(&A, 0x30u, 2u, 0u, 3u);                           /* CTS, BS 2, STmin 0: iki CF hemen gider */
    check("CTS BS=2 (500 ms'de): 2 CF gitti (toplam 3 cerceve), yeni FC bekleniyor", A.port.n == 3u && A.iso.stats.cf_sent == 2u && fwu_isotp_tx_busy(&A.iso));
    check("... CF'lerin zaman damgasi 500 (on_can_frame'in zamani, onceki poll'unki degil)", A.port.log[1].t == 500u && A.port.log[2].t == 500u);
    (void)fwu_isotp_poll(&A.iso, 1100u);
    check("blok 500'de bitti: 1100'de HALA bekliyor (zamanlayici blok sonundan, send'den degil)", fwu_isotp_tx_busy(&A.iso) && A.iso.stats.tx_timeouts == 0u);
    (void)fwu_isotp_poll(&A.iso, 1499u);
    check("... 1499'da hala bekliyor", fwu_isotp_tx_busy(&A.iso));
    (void)fwu_isotp_poll(&A.iso, 1500u);
    check("... 1500'de (blok sonu + 1000) zaman asimi", !fwu_isotp_tx_busy(&A.iso) && A.iso.stats.tx_timeouts == 1u);

    /* (g) WAIT sayaci blok sonunda sifirlanir: wft_max 2 ile her blokta 2 WAIT kabul */
    end_init(&A, 0u, 0u, 1000u, 2u);
    g_now = 100u;
    (void)fwu_isotp_send(&A.iso, m, 100u, g_now);
    inject(&A, 0x31u, 0u, 0u, 3u);
    inject(&A, 0x31u, 0u, 0u, 3u);
    inject(&A, 0x30u, 2u, 0u, 3u);                           /* CTS BS 2 */
    inject(&A, 0x31u, 0u, 0u, 3u);
    inject(&A, 0x31u, 0u, 0u, 3u);
    check("blok 1'de 2 WAIT + blok 2'de 2 WAIT: sayac blok sonunda sifirlandi, gonderim SURUYOR", fwu_isotp_tx_busy(&A.iso) && A.iso.stats.tx_aborts == 0u);
    inject(&A, 0x31u, 0u, 0u, 3u);
    check("... blok 2'de 3. WAIT: IPTAL", !fwu_isotp_tx_busy(&A.iso) && A.iso.stats.tx_aborts == 1u);

    /* (h) BS = 1: her CF'den sonra FC; FC sayisi elle: 14 CF (ceil((100-6)/7)) -> 1 + 13 = 14 */
    setup_pair(0u, 0u, 1u, 0u);                              /* alici B: BS 1 */
    (void)fwu_isotp_send(&A.iso, m, 100u, g_now);
    run(2000u);
    check("BS=1: mesaj BIREBIR teslim", message_equals(&B, m, 100u));
    check("... CF 14, alicinin FC'si 14 (FF sonrasi + 13 CF sonrasi), gonderende beklenmeyen FC YOK",
          A.iso.stats.cf_sent == 14u && B.iso.stats.fc_sent == 14u && A.iso.stats.rx_errors == 0u && B.iso.stats.rx_errors == 0u);
    /* ters yon: B alici BS=1 iken A'nin gonderici kuyrugu */
    setup_pair(1u, 0u, 0u, 0u);                              /* alici A: BS 1 */
    (void)fwu_isotp_send(&B.iso, m, 100u, g_now);
    run(2000u);
    check("BS=1 (karsi yon): mesaj teslim, FC sayisi 14", message_equals(&A, m, 100u) && A.iso.stats.fc_sent == 14u && B.iso.stats.cf_sent == 14u);

    /* (i) tamamlanan mesajdan sonra birlestirme KAPANIR: gec CF reddedilir, zaman asimi sayilmaz */
    setup_pair(8u, 0u, 8u, 0u);
    (void)fwu_isotp_send(&A.iso, m, 100u, g_now);
    run(500u);
    check("100 bayt teslim edildi", message_equals(&B, m, 100u) && !B.iso.rx_active);
    {
        const unsigned cf_late[] = { 0x2Fu, 1u, 2u, 3u, 4u, 5u, 6u, 7u };
        error_frame("tamamlanmis mesajdan sonra CF: BAD_STATE, rx_errors +1", &B, cf_late, 8u, FWU_ERR_BAD_STATE);
    }
    (void)fwu_isotp_poll(&B.iso, g_now + 5000u);
    check("... uzun sure sonra poll: rx_timeouts 0, rx_aborts 0 (tamamlanan mesaj zaman asimina dusmez)", B.iso.stats.rx_timeouts == 0u && B.iso.stats.rx_aborts == 0u);

    /* (j) 521 bayt (en buyuk) BIREBIR */
    make_message(big, FWU_ISOTP_MAX_MESSAGE, 12u);
    setup_pair(8u, 0u, 8u, 0u);
    (void)fwu_isotp_send(&A.iso, big, FWU_ISOTP_MAX_MESSAGE, g_now);
    run(4000u);
    check("521 bayt (sinir): BIREBIR teslim", message_equals(&B, big, FWU_ISOTP_MAX_MESSAGE));
}

int main(void)
{
    test_init();
    test_known_frames();
    test_all_lengths();
    test_flow_control();
    test_sender_edge();
    test_receiver_violations();
    test_busy_and_errors();
    test_duplex_and_adapter();
    test_mutation_gaps();
    test_fuzz();

    printf("\nResult: %s (%d failed)\n", g_failures == 0 ? "ALL TESTS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
