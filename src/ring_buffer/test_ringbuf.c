/**
 * @file    test_ringbuf.c
 * @brief   fwu_ringbuf için kendi kendini kontrol eden test programı.
 *
 * Aynı test paketi, 1 .. 32767 arasında birçok farklı KAPASİTEDE çalıştırılır
 * (tek eleman, 2'nin kuvveti olmayan, büyük, uç değer). Her kapasitede:
 *   - başlatma, boş/dolu sorguları, FIFO sırası,
 *   - dolu tampona yazma reddi (veri korunur), sarma,
 *   - taşma tespiti: depolamanın SONUNA konan nöbetçi baytlar değişmemeli,
 *   - rastgele işlemlerde basit bir REFERANS MODELLE birebir aynı sonuç,
 *   - her adımda değişmezler (göstergeler aralıkta, kullanılan + boş = kapasite).
 *
 * Kapsam DIŞI: gerçek eşzamanlılık (bkz. stress_ringbuf.c) ve gerçek C28x derleyicisi.
 *
 * Derleme (ring_buffer klasöründen):
 *   gcc -Wall -Wextra -Wpedantic -Wconversion -std=c99 -I. -I.. -I../error \
 *       fwu_ringbuf.c test_ringbuf.c -o test_ringbuf && ./test_ringbuf
 *   gcc -DC2000 ...  (aynı komut, C28x'in 16 bitlik bayt tipini PC'de taklit eder)
 */

#include <stdio.h>
#include <string.h>
#include "fwu_ringbuf.h"

/** Başarısız kontrol sayısı. */
static int g_failures = 0;

/** Bir kontrolün sonucunu yazdırır ve başarısızlıkları sayar. */
static void check(const char *name, int condition)
{
    printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    if(!condition) {
        g_failures++;
    }
}

/** Depolama + nöbetçi bayt alanı. Nöbetçiler kapasitenin hemen ardına konur. */
#define GUARD_COUNT 8u
#define GUARD_VALUE 0xA5u
static fwu_byte_t g_storage[FWU_RINGBUF_MAX_CAPACITY + GUARD_COUNT];

/** Referans model: büyük bir dizi ve iki sayaç (basit ve açıkça doğru). */
#define MODEL_SIZE 200000u
static unsigned char g_model[MODEL_SIZE];

/** Deterministik sözde-rastgele sayı (platformdan bağımsız). */
static uint32_t g_rng = 12345u;
static uint32_t rng_next(void)
{
    g_rng = (g_rng * 1664525u) + 1013904223u;
    return g_rng >> 8;
}

static void set_guards(size_t capacity)
{
    for(size_t i = 0; i < GUARD_COUNT; i++) {
        g_storage[capacity + i] = (fwu_byte_t)GUARD_VALUE;
    }
}

static int guards_intact(size_t capacity)
{
    int ok = 1;
    for(size_t i = 0; i < GUARD_COUNT; i++) {
        if(g_storage[capacity + i] != (fwu_byte_t)GUARD_VALUE) {
            ok = 0;
        }
    }
    return ok;
}

/* Makro ile üretilen tipler: dizi ve boyut aynı sayıdan türer. */
FWU_RINGBUF_DEFINE(test_rb_one, 1);
FWU_RINGBUF_DEFINE(test_rb_seven, 7);
FWU_RINGBUF_DEFINE(test_rb_hundred, 100);

/** FWU_RINGBUF_DEFINE ile üretilen tiplerin kontrolü. */
static void run_macro_type_tests(void)
{
    static test_rb_one_t one;
    static test_rb_seven_t seven;
    static test_rb_hundred_t hundred;
    fwu_byte_t data[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    fwu_byte_t back[8] = { 0 };

    check("makro tipi: gomulu dizi boyutu = verilen sayi (1, 7, 100)",
          sizeof(one.storage) / sizeof(one.storage[0]) == 1u &&
          sizeof(seven.storage) / sizeof(seven.storage[0]) == 7u &&
          sizeof(hundred.storage) / sizeof(hundred.storage[0]) == 100u);
    check("makro tipi: init OK ve kapasite = gomulu dizi boyutu",
          test_rb_one_init(&one) == FWU_OK && fwu_ringbuf_free_size(&one.base) == 1u &&
          test_rb_seven_init(&seven) == FWU_OK && fwu_ringbuf_free_size(&seven.base) == 7u &&
          test_rb_hundred_init(&hundred) == FWU_OK && fwu_ringbuf_free_size(&hundred.base) == 100u);
    check("makro tipi: init(NULL) -> INVALID_ARG", test_rb_seven_init(NULL) == FWU_ERR_INVALID_ARG);
    check("makro tipi: tam kapasite (7) yazilir, 1 fazlasi reddedilir, okununca ayni veri",
          fwu_ringbuf_push(&seven.base, data, 7) == FWU_OK &&
          fwu_ringbuf_push(&seven.base, data, 1) == FWU_ERR_BUFFER_FULL &&
          fwu_ringbuf_pull(&seven.base, back, 7) == FWU_OK &&
          memcmp(back, data, 7u * sizeof(back[0])) == 0);
}

/** Tüm testleri verilen kapasitede çalıştırır. */
static void run_capacity_suite(size_t capacity, unsigned operations)
{
    static fwu_ringbuf_t rb;
    static fwu_byte_t in[FWU_RINGBUF_MAX_CAPACITY + 8u];
    static fwu_byte_t out[FWU_RINGBUF_MAX_CAPACITY + 8u];
    char label[96];

    set_guards(capacity);
    for(size_t i = 0; i < sizeof(in) / sizeof(in[0]); i++) {
        in[i] = (fwu_byte_t)((i * 31u + 7u) & 0xFFu);
    }

    /* A) Başlatma ve boş tampon. */
    (void)snprintf(label, sizeof(label), "cap=%u: init OK, bos, dolu degil, 0 veri, tam bos yer", (unsigned)capacity);
    check(label, fwu_ringbuf_init_raw(&rb, g_storage, capacity) == FWU_OK &&
                 fwu_ringbuf_is_empty(&rb) && !fwu_ringbuf_is_full(&rb) &&
                 fwu_ringbuf_used_size(&rb) == 0u && fwu_ringbuf_free_size(&rb) == capacity);

    (void)snprintf(label, sizeof(label), "cap=%u: bos tampondan 1 bayt iste -> NOT_ENOUGH_DATA", (unsigned)capacity);
    check(label, fwu_ringbuf_pull(&rb, out, 1) == FWU_ERR_NOT_ENOUGH_DATA);

    /* B) Doldur: bir fazlası reddedilir, veri bozulmaz, sıra korunur. */
    (void)snprintf(label, sizeof(label), "cap=%u: kapasite kadar yaz -> OK ve dolu", (unsigned)capacity);
    check(label, fwu_ringbuf_push(&rb, in, capacity) == FWU_OK && fwu_ringbuf_is_full(&rb) &&
                 fwu_ringbuf_free_size(&rb) == 0u);

    (void)snprintf(label, sizeof(label), "cap=%u: dolu tampona 1 bayt -> BUFFER_FULL, durum degismez", (unsigned)capacity);
    check(label, fwu_ringbuf_push(&rb, in, 1) == FWU_ERR_BUFFER_FULL && fwu_ringbuf_used_size(&rb) == capacity);

    int order_ok = (fwu_ringbuf_pull(&rb, out, capacity) == FWU_OK);
    for(size_t i = 0; i < capacity; i++) {
        if(out[i] != in[i]) {
            order_ok = 0;
        }
    }
    (void)snprintf(label, sizeof(label), "cap=%u: doldur-bosalt, sira ve degerler korunur", (unsigned)capacity);
    check(label, order_ok && fwu_ringbuf_is_empty(&rb));

    /* C) Geçersiz istekler. */
    (void)snprintf(label, sizeof(label), "cap=%u: kapasiteden uzun istek / NULL veri -> INVALID_ARG", (unsigned)capacity);
    check(label, fwu_ringbuf_push(&rb, in, capacity + 1u) == FWU_ERR_INVALID_ARG &&
                 fwu_ringbuf_pull(&rb, out, capacity + 1u) == FWU_ERR_INVALID_ARG &&
                 fwu_ringbuf_push(&rb, NULL, 1) == FWU_ERR_INVALID_ARG &&
                 fwu_ringbuf_pull(&rb, NULL, 1) == FWU_ERR_INVALID_ARG);

    (void)snprintf(label, sizeof(label), "cap=%u: uzunluk 0: push/pull OK, durum degismez", (unsigned)capacity);
    check(label, fwu_ringbuf_push(&rb, in, 0) == FWU_OK && fwu_ringbuf_pull(&rb, out, 0) == FWU_OK &&
                 fwu_ringbuf_used_size(&rb) == 0u);

    /* D) clear. */
    fwu_ringbuf_push(&rb, in, (capacity > 3u) ? 3u : capacity);
    (void)snprintf(label, sizeof(label), "cap=%u: clear sonrasi bos ve tekrar kullanilir", (unsigned)capacity);
    check(label, fwu_ringbuf_clear(&rb) == FWU_OK && fwu_ringbuf_is_empty(&rb) &&
                 fwu_ringbuf_push(&rb, in, 1) == FWU_OK && fwu_ringbuf_used_size(&rb) == 1u);
    fwu_ringbuf_clear(&rb);

    /* E) Rastgele işlemler: referans modelle birebir aynı olmalı. */
    int diff = 0;
    size_t model_head = 0u;
    size_t model_tail = 0u;
    size_t pushed_total = 0u;
    for(unsigned step = 0; step < operations && diff == 0; step++) {
        const uint32_t op = rng_next() % 100u;
        const size_t n = (size_t)(rng_next() % (uint32_t)(capacity + 4u));
        size_t used = model_head - model_tail;

        if(op < 50u) {                                          /* push */
            for(size_t k = 0; k < n && k < sizeof(in) / sizeof(in[0]); k++) {
                in[k] = (fwu_byte_t)(rng_next() & 0xFFu);
            }
            const fwu_return_e e = fwu_ringbuf_push(&rb, in, n);
            if(n > capacity) {
                diff |= (e != FWU_ERR_INVALID_ARG);
            }
            else if((capacity - used) < n) {
                diff |= (e != FWU_ERR_BUFFER_FULL);
            }
            else {
                diff |= (e != FWU_OK);
                for(size_t k = 0; k < n; k++) {
                    g_model[(model_head + k) % MODEL_SIZE] = (unsigned char)(in[k] & FWU_BYTE_MASK);
                }
                model_head += n;
                pushed_total += n;
            }
        }
        else if(op < 95u) {                                     /* pull */
            const fwu_return_e e = fwu_ringbuf_pull(&rb, out, n);
            if(n > capacity) {
                diff |= (e != FWU_ERR_INVALID_ARG);
            }
            else if(used < n) {
                diff |= (e != FWU_ERR_NOT_ENOUGH_DATA);
            }
            else {
                diff |= (e != FWU_OK);
                for(size_t k = 0; k < n; k++) {
                    if((unsigned char)(out[k] & FWU_BYTE_MASK) != g_model[(model_tail + k) % MODEL_SIZE]) {
                        diff = 1;
                    }
                }
                model_tail += n;
            }
        }
        else {                                                  /* clear (ara sıra) */
            diff |= (fwu_ringbuf_clear(&rb) != FWU_OK);
            model_tail = model_head;
        }

        /* Her adımdan sonra değişmezler. */
        used = model_head - model_tail;
        diff |= (fwu_ringbuf_used_size(&rb) != used);
        diff |= ((size_t)fwu_ringbuf_used_size(&rb) + (size_t)fwu_ringbuf_free_size(&rb) != capacity);
        diff |= (fwu_ringbuf_is_empty(&rb) != (used == 0u));
        diff |= (fwu_ringbuf_is_full(&rb) != (used == capacity));
        diff |= ((size_t)rb.head >= 2u * capacity) || ((size_t)rb.tail >= 2u * capacity);
    }
    (void)snprintf(label, sizeof(label), "cap=%u: %u rastgele islem, referans modelle ayni (%u bayt akti)",
                   (unsigned)capacity, operations, (unsigned)pushed_total);
    check(label, diff == 0 && pushed_total > 2u * capacity);

    /* F) Taşma tespiti: depolamanın dışına yazılmamış olmalı. */
    (void)snprintf(label, sizeof(label), "cap=%u: depolama disina yazilmadi (nobetci baytlar saglam)", (unsigned)capacity);
    check(label, guards_intact(capacity));
}

int main(void)
{
    static fwu_ringbuf_t rb;
    fwu_byte_t tmp[4] = { 1, 2, 3, 4 };

    /* 1) Başlatma: geçersiz parametreler. */
    check("init(NULL, ...) -> INVALID_ARG", fwu_ringbuf_init_raw(NULL, g_storage, 8) == FWU_ERR_INVALID_ARG);
    check("init(NULL depolama) -> INVALID_ARG", fwu_ringbuf_init_raw(&rb, NULL, 8) == FWU_ERR_INVALID_ARG);
    check("init(kapasite 0) -> INVALID_ARG", fwu_ringbuf_init_raw(&rb, g_storage, 0) == FWU_ERR_INVALID_ARG);
    check("init(kapasite MAX+1) -> INVALID_ARG",
          fwu_ringbuf_init_raw(&rb, g_storage, (size_t)FWU_RINGBUF_MAX_CAPACITY + 1u) == FWU_ERR_INVALID_ARG);
    check("init(kapasite MAX) -> OK", fwu_ringbuf_init_raw(&rb, g_storage, FWU_RINGBUF_MAX_CAPACITY) == FWU_OK);

    /* 2) Başlatılmamış (sıfırlanmış) tampon güvenle reddedilir. */
    memset(&rb, 0, sizeof(rb));
    check("baslatilmamis tampon: islemler INVALID_ARG",
          fwu_ringbuf_push(&rb, tmp, 1) == FWU_ERR_INVALID_ARG &&
          fwu_ringbuf_pull(&rb, tmp, 1) == FWU_ERR_INVALID_ARG &&
          fwu_ringbuf_clear(&rb) == FWU_ERR_INVALID_ARG);
    check("baslatilmamis tampon ve NULL: sorgular false/0",
          !fwu_ringbuf_is_empty(&rb) && !fwu_ringbuf_is_full(&rb) &&
          fwu_ringbuf_used_size(&rb) == 0u && fwu_ringbuf_free_size(&rb) == 0u &&
          !fwu_ringbuf_is_empty(NULL) && !fwu_ringbuf_is_full(NULL) &&
          fwu_ringbuf_used_size(NULL) == 0u && fwu_ringbuf_free_size(NULL) == 0u);
    check("NULL tampon: push/pull/clear -> INVALID_ARG",
          fwu_ringbuf_push(NULL, tmp, 1) == FWU_ERR_INVALID_ARG &&
          fwu_ringbuf_pull(NULL, tmp, 1) == FWU_ERR_INVALID_ARG &&
          fwu_ringbuf_clear(NULL) == FWU_ERR_INVALID_ARG);

    /* 3) Makro ile üretilen tipler. */
    run_macro_type_tests();

    /* 4) Çeşitli kapasitelerde tüm paket. */
    static const size_t capacities[] = { 1u, 2u, 3u, 7u, 8u, 100u, 255u, 256u, 1000u, FWU_RINGBUF_MAX_CAPACITY };
    for(size_t i = 0; i < sizeof(capacities) / sizeof(capacities[0]); i++) {
        run_capacity_suite(capacities[i], (capacities[i] > 1000u) ? 600u : 20000u);
    }

    printf("\nResult: %s (%d failed)\n", g_failures == 0 ? "ALL TESTS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
