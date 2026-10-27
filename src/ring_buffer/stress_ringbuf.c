/**
 * @file    stress_ringbuf.c
 * @brief   fwu_ringbuf için GERÇEK eşzamanlılık (iki iş parçacığı) stres testi. İsteğe bağlı.
 *
 * Bir ÜRETİCİ iş parçacığı (UART kesmesinin karşılığı) ve bir TÜKETİCİ iş parçacığı
 * (ana döngünün karşılığı) aynı tamponu aynı anda kullanır. Üretici sıra numarasından
 * türetilen baytları rastgele uzunlukta parçalarla yazar; tüketici rastgele uzunlukta
 * okur ve her baytın doğru sırada ve değerde olduğunu kontrol eder.
 * Aynı test eski (ortak sayaçlı) tasarımda sayacın bozulduğunu göstermişti.
 *
 * ISO C99'da iş parçacığı yoktur; bu test POSIX threads gerektirir, bu yüzden
 * modülün normal test paketinden AYRIDIR.
 *
 * Derleme (ring_buffer klasöründen):
 *   gcc -O2 -pthread -std=gnu99 -I. -I.. -I../error fwu_ringbuf.c stress_ringbuf.c -o stress_ringbuf && ./stress_ringbuf
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include "fwu_ringbuf.h"

#define TOTAL_BYTES 20000000u

FWU_RINGBUF_DEFINE(stress_rb, 1000);
static stress_rb_t g_stress;
static volatile int g_error = 0;

/** Sıra numarasından bayt üretir (komşu ve 256 öncesi baytlardan farklı çıkar). */
static fwu_byte_t pattern(unsigned long seq)
{
    return (fwu_byte_t)((seq * 37u + (seq >> 8) * 11u + (seq >> 16)) & 0xFFu);
}

static void *producer(void *arg)
{
    unsigned long seq = 0;
    unsigned rng = 1u;
    fwu_byte_t chunk[256];
    (void)arg;

    while(seq < TOTAL_BYTES) {
        rng = (rng * 1103515245u) + 12345u;
        size_t n = (size_t)(((rng >> 16) % 200u) + 1u);
        if(seq + n > TOTAL_BYTES) {
            n = (size_t)(TOTAL_BYTES - seq);
        }
        for(size_t k = 0; k < n; k++) {
            chunk[k] = pattern(seq + k);
        }
        if(fwu_ringbuf_push(&g_stress.base, chunk, n) == FWU_OK) {
            seq += n;
        }
    }
    return NULL;
}

static void *consumer(void *arg)
{
    unsigned long received = 0;
    unsigned rng = 7u;
    fwu_byte_t out[256];
    (void)arg;

    while(received < TOTAL_BYTES && g_error == 0) {
        rng = (rng * 1103515245u) + 12345u;
        size_t n = (size_t)(((rng >> 16) % 200u) + 1u);
        if(received + n > TOTAL_BYTES) {
            n = (size_t)(TOTAL_BYTES - received);
        }
        if(fwu_ringbuf_pull(&g_stress.base, out, n) == FWU_OK) {
            for(size_t k = 0; k < n; k++) {
                if(out[k] != pattern(received + k)) {
                    g_error = 1;
                }
            }
            received += n;
        }
    }
    printf("tuketici: %lu / %u bayt, veri hatasi=%d, kalan veri=%u\n",
           received, TOTAL_BYTES, g_error, (unsigned)fwu_ringbuf_used_size(&g_stress.base));
    return NULL;
}

int main(void)
{
    pthread_t prod, cons;

    if(stress_rb_init(&g_stress) != FWU_OK) {
        return 2;
    }
    pthread_create(&prod, NULL, producer, NULL);
    pthread_create(&cons, NULL, consumer, NULL);
    pthread_join(prod, NULL);
    pthread_join(cons, NULL);

    const int ok = (g_error == 0) && (fwu_ringbuf_used_size(&g_stress.base) == 0u);
    printf("Result: %s\n", ok ? "STRESS TEST PASSED" : "FAILURES");
    return ok ? 0 : 1;
}
