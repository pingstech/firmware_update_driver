/**
 * @file    test_linksim.c
 * @brief   fwu_linksim (kayıplı bağlantı simülatörü) kendi kendini kontrol eden testi. Simülatör diğer testlerin ÖLÇÜ ALETİDİR;
 *          yanlış çalışırsa "hatta hata yokken geçti" ya da "hata vardı ama hiç uygulanmadı" yanılgısı doğar. Bu yüzden her arıza
 *          türünün gerçekten uygulandığı, oranların beklenen aralıkta kaldığı ve tohumla tekrarlanabilirlik elle kontrol edilir.
 *
 * Derleme (transport klasöründen):
 *   gcc -Wall -Wextra -Wpedantic -Wconversion -std=c99 -I. -I.. -I../error fwu_linksim.c test_linksim.c -o test_linksim && ./test_linksim
 */

#include <stdio.h>
#include "fwu_linksim.h"

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

/** Eleman eleman karsilastirma (C2000'de fwu_byte_t 16 bit: bayt cinsinden karsilastirma boyutu yanlis olurdu). */
static int same_bytes(const fwu_byte_t *a, const fwu_byte_t *b, size_t n)
{
    for(size_t i = 0u; i < n; i++) {
        if(a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static fwu_linksim_profile_t perfect(void)
{
    fwu_linksim_profile_t p = { 0u, 0u, 0u, 0u, 0u, 0u, 1u };
    return p;
}

static fwu_linksim_t g_a;
static fwu_linksim_t g_b;

static void test_validation(void)
{
    fwu_linksim_profile_t p = perfect();
    fwu_byte_t d[4] = { 1u, 2u, 3u, 4u };
    fwu_byte_t out[16];

    printf("\n--- dogrulama ---\n");
    expect("init gecerli", fwu_linksim_init(&g_a, &p, 1u), FWU_OK);
    expect("init sim NULL", fwu_linksim_init(NULL, &p, 1u), FWU_ERR_INVALID_ARG);
    expect("init profil NULL", fwu_linksim_init(&g_a, NULL, 1u), FWU_ERR_INVALID_ARG);
    p.drop_pm = 1001u;
    expect("drop 1001", fwu_linksim_init(&g_a, &p, 1u), FWU_ERR_INVALID_ARG);
    p = perfect(); p.duplicate_pm = 1001u;
    expect("duplicate 1001", fwu_linksim_init(&g_a, &p, 1u), FWU_ERR_INVALID_ARG);
    p = perfect(); p.corrupt_pm = 1001u;
    expect("corrupt 1001", fwu_linksim_init(&g_a, &p, 1u), FWU_ERR_INVALID_ARG);
    p = perfect(); p.truncate_pm = 1001u;
    expect("truncate 1001", fwu_linksim_init(&g_a, &p, 1u), FWU_ERR_INVALID_ARG);
    p = perfect(); p.delay_pm = 1001u;
    expect("delay 1001", fwu_linksim_init(&g_a, &p, 1u), FWU_ERR_INVALID_ARG);
    p = perfect(); p.garbage_pm = 1001u;
    expect("garbage 1001", fwu_linksim_init(&g_a, &p, 1u), FWU_ERR_INVALID_ARG);
    p = perfect(); p.drop_pm = 1000u; p.duplicate_pm = 1000u; p.corrupt_pm = 1000u; p.truncate_pm = 1000u; p.garbage_pm = 1000u;
    expect("hepsi 1000 (sinir) gecerli", fwu_linksim_init(&g_a, &p, 1u), FWU_OK);
    p = perfect(); p.delay_pm = 1u; p.delay_max_ms = 0u;
    expect("delay_pm > 0 iken delay_max_ms 0", fwu_linksim_init(&g_a, &p, 1u), FWU_ERR_INVALID_ARG);
    p.delay_pm = 0u;
    expect("delay_pm 0 iken delay_max_ms 0 serbest", fwu_linksim_init(&g_a, &p, 1u), FWU_OK);

    p = perfect();
    (void)fwu_linksim_init(&g_a, &p, 1u);
    expect("send sim NULL", fwu_linksim_send(NULL, d, 4u, 0u), FWU_ERR_INVALID_ARG);
    expect("send data NULL", fwu_linksim_send(&g_a, NULL, 4u, 0u), FWU_ERR_INVALID_ARG);
    expect("send uzunluk 0", fwu_linksim_send(&g_a, d, 0u, 0u), FWU_ERR_INVALID_ARG);
    {
        static fwu_byte_t big[FWU_LINKSIM_CHUNK_MAX];
        expect("send uzunluk CHUNK_MAX-8 (sinir)", fwu_linksim_send(&g_a, big, FWU_LINKSIM_CHUNK_MAX - 8u, 0u), FWU_OK);
        expect("send uzunluk CHUNK_MAX-7", fwu_linksim_send(&g_a, big, FWU_LINKSIM_CHUNK_MAX - 7u, 0u), FWU_ERR_INVALID_ARG);
    }
    check("receive NULL'lara dayanikli (0 dondurur)", fwu_linksim_receive(NULL, 0u, out, sizeof(out)) == 0u && fwu_linksim_receive(&g_a, 0u, NULL, sizeof(out)) == 0u);
    check("in_flight NULL = 0", fwu_linksim_in_flight(NULL) == 0u);
    expect("set_profile gecerli", fwu_linksim_set_profile(&g_a, &p), FWU_OK);
    expect("set_profile NULL profil", fwu_linksim_set_profile(&g_a, NULL), FWU_ERR_INVALID_ARG);
    expect("set_profile NULL sim", fwu_linksim_set_profile(NULL, &p), FWU_ERR_INVALID_ARG);
    p.drop_pm = 2000u;
    expect("set_profile gecersiz", fwu_linksim_set_profile(&g_a, &p), FWU_ERR_INVALID_ARG);
}

static void test_perfect_and_timing(void)
{
    fwu_linksim_profile_t p = perfect();
    fwu_byte_t d[3] = { 10u, 20u, 30u };
    fwu_byte_t e[2] = { 40u, 50u };
    fwu_byte_t out[16];

    printf("\n--- kusursuz hat ve teslim zamani ---\n");
    (void)fwu_linksim_init(&g_a, &p, 7u);
    expect("send", fwu_linksim_send(&g_a, d, 3u, 100u), FWU_OK);
    check("yolda 1 obek", fwu_linksim_in_flight(&g_a) == 1u);
    check("ayni ms'de teslim YOK (en az 1 ms gecikme)", fwu_linksim_receive(&g_a, 100u, out, sizeof(out)) == 0u);
    check("1 ms sonra teslim: 3 bayt, ayni icerik", fwu_linksim_receive(&g_a, 101u, out, sizeof(out)) == 3u && out[0] == 10u && out[1] == 20u && out[2] == 30u);
    check("teslimden sonra kuyruk bos", fwu_linksim_in_flight(&g_a) == 0u && fwu_linksim_receive(&g_a, 200u, out, sizeof(out)) == 0u);
    check("istatistik: gonderilen 1, teslim 1, kayip 0", g_a.stats.sent == 1u && g_a.stats.delivered == 1u && g_a.stats.dropped == 0u);

    /* ayni teslim zamaninda FIFO */
    (void)fwu_linksim_send(&g_a, d, 3u, 500u);
    (void)fwu_linksim_send(&g_a, e, 2u, 500u);
    check("ayni zamanda iki obek: ilk gonderilen once gelir (FIFO)", fwu_linksim_receive(&g_a, 501u, out, sizeof(out)) == 3u && out[0] == 10u);
    check("... sonra ikincisi", fwu_linksim_receive(&g_a, 501u, out, sizeof(out)) == 2u && out[0] == 40u);

    /* capacity yetmezse obek duser */
    (void)fwu_linksim_send(&g_a, d, 3u, 600u);
    check("capacity 2 < 3: obek DUSURULUR, 0 doner", fwu_linksim_receive(&g_a, 601u, out, 2u) == 0u && fwu_linksim_in_flight(&g_a) == 0u);
    check("... delivered artmadi", g_a.stats.delivered == 3u);

    /* sarma guvenli teslim */
    (void)fwu_linksim_send(&g_a, d, 3u, 0xFFFFFFFEu);                      /* teslim 0xFFFFFFFF */
    check("sarma: teslimden 1 ms once yok", fwu_linksim_receive(&g_a, 0xFFFFFFFEu, out, sizeof(out)) == 0u);
    check("sarma: tam zamaninda teslim (0xFFFFFFFF)", fwu_linksim_receive(&g_a, 0xFFFFFFFFu, out, sizeof(out)) == 3u);
    (void)fwu_linksim_send(&g_a, d, 3u, 0xFFFFFFFFu);                      /* teslim 0 (sarmis) */
    check("sarma: teslim zamani 0'a saran obek, now=0xFFFFFFFF'te yok", fwu_linksim_receive(&g_a, 0xFFFFFFFFu, out, sizeof(out)) == 0u);
    check("sarma: now=0'da teslim", fwu_linksim_receive(&g_a, 0u, out, sizeof(out)) == 3u);

#ifdef C2000
    /* 16 bitlik bayt taklidi: yalniz alt 8 bit gecerli */
    {
        fwu_byte_t m[2] = { 0x1AAu, 0x155u };
        (void)fwu_linksim_send(&g_a, m, 2u, 0u);
        check("veri ust bitleri maskelenir (bayt = 8 bit)", fwu_linksim_receive(&g_a, 1u, out, sizeof(out)) == 2u && out[0] == 0xAAu && out[1] == 0x55u);
    }
#endif
}

static void test_each_fault(void)
{
    fwu_linksim_profile_t p;
    fwu_byte_t d[20];
    fwu_byte_t out[FWU_LINKSIM_CHUNK_MAX];
    uint32_t differing;
    uint32_t shorter;
    uint32_t longer;
    uint32_t got;
    uint32_t t = 0u;

    printf("\n--- her ariza turu gercekten uygulaniyor ---\n");
    for(unsigned i = 0u; i < 20u; i++) {
        d[i] = (fwu_byte_t)(i + 1u);
    }

    p = perfect(); p.drop_pm = 1000u;
    (void)fwu_linksim_init(&g_a, &p, 1u);
    for(unsigned i = 0u; i < 50u; i++) {
        (void)fwu_linksim_send(&g_a, d, 20u, t);
    }
    check("drop %100: 50 gonderim, 50 kayip, yolda 0", g_a.stats.dropped == 50u && fwu_linksim_in_flight(&g_a) == 0u && g_a.stats.sent == 50u);

    p = perfect(); p.duplicate_pm = 1000u;
    (void)fwu_linksim_init(&g_a, &p, 1u);
    for(unsigned i = 0u; i < 10u; i++) {
        (void)fwu_linksim_send(&g_a, d, 20u, t);
    }
    got = 0u;
    while(fwu_linksim_receive(&g_a, t + 100u, out, sizeof(out)) == 20u) {
        got++;
    }
    check("duplicate %100: 10 gonderim -> 20 teslim (her biri iki kez, ayni icerik)", got == 20u && g_a.stats.duplicated == 10u);

    p = perfect(); p.corrupt_pm = 1000u;
    (void)fwu_linksim_init(&g_a, &p, 1u);
    differing = 0u;
    for(unsigned i = 0u; i < 100u; i++) {
        (void)fwu_linksim_send(&g_a, d, 20u, t);
        if(fwu_linksim_receive(&g_a, t + 10u, out, sizeof(out)) == 20u) {
            if(!same_bytes(out, d, 20u)) {
                differing++;
            }
        }
    }
    check("corrupt %100: 100 gonderimde cok buyuk cogunlukta icerik degisti (ayni uzunluk)", differing >= 90u && g_a.stats.corrupted == 100u);

    p = perfect(); p.truncate_pm = 1000u;
    (void)fwu_linksim_init(&g_a, &p, 1u);
    shorter = 0u;
    for(unsigned i = 0u; i < 100u; i++) {
        size_t n;
        (void)fwu_linksim_send(&g_a, d, 20u, t);
        n = fwu_linksim_receive(&g_a, t + 10u, out, sizeof(out));
        if((n >= 1u) && (n < 20u)) {
            shorter++;
        }
    }
    check("truncate %100: 100 teslimin HEPSI 1..19 bayt (kesik), hicbiri tam degil", shorter == 100u && g_a.stats.truncated == 100u);

    p = perfect(); p.garbage_pm = 1000u;
    (void)fwu_linksim_init(&g_a, &p, 1u);
    longer = 0u;
    for(unsigned i = 0u; i < 100u; i++) {
        size_t n;
        (void)fwu_linksim_send(&g_a, d, 20u, t);
        n = fwu_linksim_receive(&g_a, t + 10u, out, sizeof(out));
        if((n >= 21u) && (n <= 28u) && same_bytes(&out[n - 20u], d, 20u)) {
            longer++;                                        /* onune 1..8 cop bayt, veri sonda salim */
        }
    }
    check("garbage %100: 100 teslimin HEPSI 21..28 bayt, orijinal veri SONDA bozulmadan", longer == 100u && g_a.stats.garbage == 100u);

    p = perfect(); p.delay_pm = 1000u; p.delay_max_ms = 50u;
    (void)fwu_linksim_init(&g_a, &p, 1u);
    {
        uint32_t early = 0u;
        uint32_t late = 0u;
        uint32_t seen_max = 0u;
        for(unsigned i = 0u; i < 200u; i++) {
            uint32_t k;
            (void)fwu_linksim_init(&g_a, &p, 100u + i);
            (void)fwu_linksim_send(&g_a, d, 20u, 1000u);
            for(k = 1u; k <= 60u; k++) {
                if(fwu_linksim_receive(&g_a, 1000u + k, out, sizeof(out)) > 0u) {
                    break;
                }
            }
            if(k < 1u) {
                early++;
            }
            if(k > 50u) {
                late++;
            }
            if(k > seen_max) {
                seen_max = k;
            }
        }
        check("delay %100, max 50: teslim gecikmesi hep 1..50 ms (erken/gec yok)", early == 0u && late == 0u);
        check("... gecikmeler gercekten dagiliyor (en buyuk gozlenen >= 40 ms)", seen_max >= 40u);
    }

    /* gecikme sira degistirir */
    p = perfect(); p.delay_pm = 500u; p.delay_max_ms = 30u;
    (void)fwu_linksim_init(&g_a, &p, 3u);
    {
        fwu_byte_t m;
        uint32_t reordered = 0u;
        uint32_t last = 0u;
        for(fwu_byte_t i = 0u; i < 100u; i++) {
            m = i;
            (void)fwu_linksim_send(&g_a, &m, 1u, 0u);
        }
        for(uint32_t now = 1u; now < 100u; now++) {
            while(fwu_linksim_receive(&g_a, now, out, sizeof(out)) == 1u) {
                if(out[0] < last) {
                    reordered++;
                }
                last = out[0];
            }
        }
        check("%50 gecikmeli 100 obek: SIRA DEGISIKLIGI gozlendi", reordered > 0u);
    }
}

static void test_rates_and_determinism(void)
{
    fwu_linksim_profile_t p = { 100u, 50u, 0u, 0u, 0u, 0u, 1u };
    fwu_byte_t d[8] = { 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u };
    fwu_byte_t out[64];
    uint32_t sig_a = 0u;
    uint32_t sig_b = 0u;

    printf("\n--- oranlar ve tekrarlanabilirlik ---\n");
    (void)fwu_linksim_init(&g_a, &p, 42u);
    for(unsigned i = 0u; i < 20000u; i++) {
        (void)fwu_linksim_send(&g_a, d, 8u, 0u);
        while(fwu_linksim_receive(&g_a, 5u, out, sizeof(out)) > 0u) {
        }
    }
    check("drop %10, 20000 gonderim: kayip 1800..2200 araliginda", g_a.stats.dropped >= 1800u && g_a.stats.dropped <= 2200u);
    check("duplicate %5 (ulasanlar uzerinden): 700..1100 araliginda", g_a.stats.duplicated >= 700u && g_a.stats.duplicated <= 1100u);
    printf("(bilgi: kayip %u, tekrar %u)\n", (unsigned)g_a.stats.dropped, (unsigned)g_a.stats.duplicated);

    p.delay_pm = 300u; p.delay_max_ms = 20u; p.corrupt_pm = 50u; p.truncate_pm = 50u; p.garbage_pm = 50u;
    (void)fwu_linksim_init(&g_a, &p, 99u);
    (void)fwu_linksim_init(&g_b, &p, 99u);
    for(unsigned i = 0u; i < 2000u; i++) {
        size_t na;
        size_t nb;
        d[0] = (fwu_byte_t)i;
        (void)fwu_linksim_send(&g_a, d, 8u, i);
        (void)fwu_linksim_send(&g_b, d, 8u, i);
        na = fwu_linksim_receive(&g_a, i + 3u, out, sizeof(out));
        sig_a = (sig_a * 31u) + (uint32_t)na + (na > 0u ? out[0] : 0u);
        nb = fwu_linksim_receive(&g_b, i + 3u, out, sizeof(out));
        sig_b = (sig_b * 31u) + (uint32_t)nb + (nb > 0u ? out[0] : 0u);
    }
    check("ayni tohum + ayni girdi: iki simulator BIREBIR ayni teslim dizisi", sig_a == sig_b && g_a.stats.dropped == g_b.stats.dropped && g_a.stats.corrupted == g_b.stats.corrupted);
    (void)fwu_linksim_init(&g_b, &p, 100u);
    sig_b = 0u;
    for(unsigned i = 0u; i < 2000u; i++) {
        size_t nb;
        d[0] = (fwu_byte_t)i;
        (void)fwu_linksim_send(&g_b, d, 8u, i);
        nb = fwu_linksim_receive(&g_b, i + 3u, out, sizeof(out));
        sig_b = (sig_b * 31u) + (uint32_t)nb + (nb > 0u ? out[0] : 0u);
    }
    check("farkli tohum: farkli dizi (tohum gercekten etkili)", sig_a != sig_b);
}

static void test_queue_overflow(void)
{
    fwu_linksim_profile_t p = perfect();
    fwu_byte_t d[2] = { 1u, 2u };
    fwu_byte_t out[8];

    printf("\n--- kuyruk tasmasi ---\n");
    (void)fwu_linksim_init(&g_a, &p, 1u);
    for(unsigned i = 0u; i < FWU_LINKSIM_QUEUE; i++) {
        (void)fwu_linksim_send(&g_a, d, 2u, 0u);
    }
    check("128 obek yolda, tasma yok", fwu_linksim_in_flight(&g_a) == FWU_LINKSIM_QUEUE && g_a.stats.queue_overflow == 0u);
    (void)fwu_linksim_send(&g_a, d, 2u, 0u);
    check("129. obek: DUSER, tasma sayaci 1, kayip sayaci 1, yolda hala 128", g_a.stats.queue_overflow == 1u && g_a.stats.dropped == 1u && fwu_linksim_in_flight(&g_a) == FWU_LINKSIM_QUEUE);
    {
        unsigned n = 0u;
        while(fwu_linksim_receive(&g_a, 10u, out, sizeof(out)) == 2u) {
            n++;
        }
        check("128 obegin hepsi teslim edildi", n == FWU_LINKSIM_QUEUE && fwu_linksim_in_flight(&g_a) == 0u);
    }
    {
        unsigned n = 0u;
        p.duplicate_pm = 1000u;
        (void)fwu_linksim_init(&g_a, &p, 1u);
        for(unsigned i = 0u; i < 70u; i++) {
            (void)fwu_linksim_send(&g_a, d, 2u, 0u);
        }
        while(fwu_linksim_receive(&g_a, 10u, out, sizeof(out)) == 2u) {
            n++;
        }
        check("tekrarli 70 gonderim (140 kopya) 128'de kesilir; fazlasi tasma", n == FWU_LINKSIM_QUEUE && g_a.stats.queue_overflow == 12u);
    }
}

int main(void)
{
    test_validation();
    test_perfect_and_timing();
    test_each_fault();
    test_rates_and_determinism();
    test_queue_overflow();

    printf("\nResult: %s (%d failed)\n", g_failures == 0 ? "ALL TESTS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
