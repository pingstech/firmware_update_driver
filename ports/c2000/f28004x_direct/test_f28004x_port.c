/**
 * @file    test_f28004x_port.c
 * @brief   fwu_flash_f28004x portunun testi: TI Flash API KUKLASI (hal_stub/) üzerinde, PC'de. Hiçbir şey donanımda denenmedi.
 *
 * Kapsam:
 *   1. init / geometri / hw_init (Flash API başlatma sırası, prefetch kancası)
 *   2. oktet <-> kelime eşlemesi (düşük oktet önce) ve okuma
 *   3. silme: sektör, banka sınırı (0x08F000 / 0x090000), hata enjeksiyonu, yarım silme
 *   4. yazma: 16 oktetlik granül, bir silmede bir kez, silinmemiş hedef, hata enjeksiyonu, yarım yazma, üst bit maskeleme
 *   5. ÇEKİRDEKLE BİRLİKTE (gerçek fwu_layout F280049C adresleriyle): A/B journal dönüşü; aktivasyon COPY ve SWAP, HER
 *      silme/yazma işleminde güç kesintisi.
 *
 * Derleme (flash/port_c2000 klasöründen; -DC2000 ile de çalışır: fwu_byte_t 16 bit taklidi):
 *   gcc -Wall -Wextra -Wpedantic -Wconversion -std=c99 -DFWU_BOARD_F280049C -Ihal_stub -I. -I.. -I../.. -I../../error -I../../bytes
 *       -I../../crc -I../../journal -I../../activation -I../../config fwu_flash_f28004x.c hal_stub/hal_stub.c ../fwu_flash.c
 *       ../../journal/fwu_journal.c ../../journal/fwu_journal_ab.c ../../activation/fwu_activate.c test_f28004x_port.c -o test_f28004x_port
 */

#include <stdio.h>
#include <string.h>
#include "F021_F28004x_C28x.h"
#include "fwu_flash_f28004x.h"
#include "fwu_layout.h"
#include "fwu_journal_ab.h"
#include "fwu_activate.h"

#define PAGE        FWU_BOARD_PAGE_SIZE
#define FLASH_LEN   (FWU_BOARD_TOTAL_PAGES * FWU_BOARD_PAGE_SIZE)
#define WORD_BASE   0x00080000u

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

static fwu_flash_f28004x_t g_port;
static uint32_t g_keepalive_calls;

static void keepalive(void *context)
{
    (void)context;
    g_keepalive_calls++;
}

static void fresh(void)
{
    hal_stub_reset();
    g_keepalive_calls = 0u;
    (void)fwu_flash_f28004x_init(&g_port, FLASH_LEN, hal_stub_memory(), keepalive, NULL);
}

/* Oktet düzeyinde doğrudan bellek erişimi (portu ARADAN çıkarır: bağımsız beklenti). */
static uint8_t octet_at(uint32_t offset)
{
    const uint16_t word = hal_stub_memory()[offset / 2u];
    return (uint8_t)(((offset & 1u) == 0u) ? (word & 0xFFu) : (word >> 8));
}

static void set_octet(uint32_t offset, uint8_t value)
{
    uint16_t *word = &hal_stub_memory()[offset / 2u];
    *word = ((offset & 1u) == 0u) ? (uint16_t)((*word & 0xFF00u) | value) : (uint16_t)((*word & 0x00FFu) | ((uint16_t)value << 8));
}

static int page_is(uint32_t page, uint8_t value)
{
    for(uint32_t i = 0u; i < PAGE; i++) {
        if(octet_at((page * PAGE) + i) != value) {
            return 0;
        }
    }
    return 1;
}

static void fill_page(uint32_t page, uint8_t value)
{
    for(uint32_t i = 0u; i < PAGE; i++) {
        set_octet((page * PAGE) + i, value);
    }
}

static int g_prefetch_calls;
static int g_prefetch_order_ok;

static void prefetch_hook(bool enable)
{
    /* beklenen sıra: önce false, sonra true (setActiveFlashBank'ın etrafında) */
    if(g_prefetch_calls == 0 && !enable && hal_stub_stats()->bank_calls == 0u) {
        g_prefetch_order_ok = 1;
    }
    else if(g_prefetch_calls == 1 && enable && hal_stub_stats()->bank_calls == 1u && g_prefetch_order_ok) {
        g_prefetch_order_ok = 2;
    }
    else {
        g_prefetch_order_ok = 0;
    }
    g_prefetch_calls++;
}

/* ------------------------------------------------------------------ 1. init */

static void test_init(void)
{
    fwu_flash_f28004x_t p;

    printf("\n--- 1. init / geometri / hw_init ---\n");
    fresh();
    check("geometri: sayfa 8192 oktet, granul 16 oktet, silinmis 0xFF, uzunluk 262144",
          g_port.flash.geometry.page_length == 8192u && g_port.flash.geometry.write_granule == 16u &&
          g_port.flash.geometry.erased_value == 0xFFu && g_port.flash.geometry.flash_length == 262144u);
    check("port sabitleri layout_f280049c.h ile ayni (taban, sayfa, toplam)",
          FWU_F28004X_FLASH_WORD_BASE == FWU_C28_FLASH_WORD_BASE && FWU_F28004X_PAGE_OCTETS == FWU_BOARD_PAGE_SIZE &&
          FWU_F28004X_FLASH_OCTETS == (FWU_BOARD_PAGE_SIZE * FWU_BOARD_TOTAL_PAGES) &&
          FWU_F28004X_SECTOR_WORDS == FWU_C28_SECTOR_WORDS && (FWU_F28004X_PAGE_OCTETS / FWU_C28_OCTETS_PER_WORD) == FWU_C28_SECTOR_WORDS);
    check("context portun kendisi, tablo dolu", g_port.flash.context == &g_port && g_port.flash.callback == &g_port.callback &&
          g_port.callback.flash_erase != NULL && g_port.callback.flash_write != NULL && g_port.callback.flash_read != NULL);
    expect("yarim flash (128 KB) kabul", fwu_flash_f28004x_init(&p, 131072u, hal_stub_memory(), NULL, NULL), FWU_OK);
    expect("port NULL", fwu_flash_f28004x_init(NULL, FLASH_LEN, hal_stub_memory(), NULL, NULL), FWU_ERR_INVALID_ARG);
    expect("memory NULL", fwu_flash_f28004x_init(&p, FLASH_LEN, NULL, NULL, NULL), FWU_ERR_INVALID_ARG);
    expect("uzunluk 0", fwu_flash_f28004x_init(&p, 0u, hal_stub_memory(), NULL, NULL), FWU_ERR_FLASH_GEOMETRY);
    expect("uzunluk sayfa kati degil", fwu_flash_f28004x_init(&p, 8192u + 16u, hal_stub_memory(), NULL, NULL), FWU_ERR_FLASH_GEOMETRY);
    expect("uzunluk 256 KB'tan buyuk", fwu_flash_f28004x_init(&p, 262144u + 8192u, hal_stub_memory(), NULL, NULL), FWU_ERR_FLASH_GEOMETRY);

    hal_stub_reset();
    g_prefetch_calls = 0;
    g_prefetch_order_ok = 0;
    expect("hw_init (100 MHz, prefetch kancasi)", fwu_flash_f28004x_hw_init(100u, prefetch_hook), FWU_OK);
    check("Flash API bir kez baslatildi, 100 MHz; banka bir kez ayarlandi",
          hal_stub_stats()->init_calls == 1u && hal_stub_stats()->hclk_mhz == 100u && hal_stub_stats()->bank_calls == 1u);
    check("prefetch: setActiveFlashBank'tan ONCE kapali, SONRA acik (2 cagri, dogru sira)", g_prefetch_calls == 2 && g_prefetch_order_ok == 2);
    hal_stub_reset();
    expect("hw_init kancasiz", fwu_flash_f28004x_hw_init(100u, NULL), FWU_OK);
    expect("hw_init hata (Flash API reddetti: 0 MHz)", fwu_flash_f28004x_hw_init(0u, NULL), FWU_ERR_FLASH_GEOMETRY);
}

/* ------------------------------------------------------------------ 2. okuma ve oktet eşlemesi */

static void test_read(void)
{
    fwu_byte_t out[8];
    uint16_t *memory;

    printf("\n--- 2. okuma ve oktet <-> kelime eslemesi ---\n");
    fresh();
    memory = hal_stub_memory();
    memory[0] = 0x2211u;
    memory[1] = 0x4433u;
    memory[2] = 0x80FEu;
    memory[3] = 0xFF00u;
    expect("oku (0, 8)", fwu_flash_read(&g_port.flash, 0u, 8u, out), FWU_OK);
    check("oktetler: 11 22 33 44 FE 80 00 FF (duseni oktet kelimenin dusuk baytidir)",
          out[0] == 0x11u && out[1] == 0x22u && out[2] == 0x33u && out[3] == 0x44u && out[4] == 0xFEu && out[5] == 0x80u &&
          out[6] == 0x00u && out[7] == 0xFFu);
    expect("oku (1, 3)", fwu_flash_read(&g_port.flash, 1u, 3u, out), FWU_OK);
    check("tek ofsetten: 22 33 44", out[0] == 0x22u && out[1] == 0x33u && out[2] == 0x44u);
    expect("flash sonu: son 8 oktet", fwu_flash_read(&g_port.flash, FLASH_LEN - 8u, 8u, out), FWU_OK);
    check("... silinmis: hepsi 0xFF", out[0] == 0xFFu && out[7] == 0xFFu);
    expect("aralik disi (son oktetten sonra)", fwu_flash_read(&g_port.flash, FLASH_LEN - 4u, 8u, out), FWU_ERR_OUT_OF_RANGE);
    check("Flash API okuma icin cagrilmadi", hal_stub_stats()->erase_calls == 0u && hal_stub_stats()->program_calls == 0u);
}

/* ------------------------------------------------------------------ 3. silme */

static void test_erase(void)
{
    const hal_stub_stats_t *st;

    printf("\n--- 3. silme ---\n");
    fresh();
    st = hal_stub_stats();
    for(uint32_t p = 3u; p <= 7u; p++) {
        fill_page(p, 0x5Au);
    }
    expect("5. sayfayi sil", fwu_flash_erase(&g_port.flash, 5u * PAGE, PAGE), FWU_OK);
    check("5. sayfa silindi, komsular dokunulmadi", page_is(5u, 0xFFu) && page_is(4u, 0x5Au) && page_is(6u, 0x5Au));
    check("Flash API: 1 silme, kelime adresi 0x080000 + 5 * 0x1000", st->erase_calls == 1u && st->last_erase_address == (WORD_BASE + (5u * 0x1000u)));
    check("bekci kancasi sektor basina bir kez", g_keepalive_calls == 1u);

    fresh();
    st = hal_stub_stats();
    fill_page(15u, 0x11u);
    fill_page(16u, 0x22u);
    fill_page(14u, 0x33u);
    fill_page(17u, 0x44u);
    expect("banka siniri: 15. ve 16. sayfayi sil (0x08F000, 0x090000)", fwu_flash_erase(&g_port.flash, 15u * PAGE, 2u * PAGE), FWU_OK);
    check("iki sayfa da silindi, 14 ve 17 dokunulmadi", page_is(15u, 0xFFu) && page_is(16u, 0xFFu) && page_is(14u, 0x33u) && page_is(17u, 0x44u));
    check("Flash API adresleri 0x08F000 ve 0x090000 (banka 0 sonu, banka 1 basi)",
          st->erase_calls == 2u && st->erase_log[0] == 0x0008F000u && st->erase_log[1] == 0x00090000u);
    check("2 sayfa icin 2 bekci cagrisi", g_keepalive_calls == 2u);

    fresh();
    st = hal_stub_stats();
    for(uint32_t p = 0u; p < FWU_BOARD_TOTAL_PAGES; p++) {
        fill_page(p, 0x77u);
    }
    expect("tum flash'i sil (32 sayfa)", fwu_flash_erase(&g_port.flash, 0u, FLASH_LEN), FWU_OK);
    {
        int all = 1;
        for(uint32_t p = 0u; p < FWU_BOARD_TOTAL_PAGES; p++) {
            all = all && page_is(p, 0xFFu);
        }
        check("... 32 sayfanin hepsi silindi, 32 Flash API cagrisi, 32 bekci cagrisi", all && st->erase_calls == 32u && g_keepalive_calls == 32u);
    }

    fresh();
    expect("hizasiz silme (ofset sayfa kati degil) cekirdekte reddedilir", fwu_flash_erase(&g_port.flash, 100u, PAGE), FWU_ERR_ERASE_ALIGNMENT);
    expect("aralik disi silme", fwu_flash_erase(&g_port.flash, FLASH_LEN, PAGE), FWU_ERR_OUT_OF_RANGE);
    check("... Flash API hic cagrilmadi", hal_stub_stats()->erase_calls == 0u);

    /* hata enjeksiyonu: 3 sayfadan ikincisi hata verir */
    fresh();
    st = hal_stub_stats();
    for(uint32_t p = 0u; p <= 3u; p++) {
        fill_page(p, 0x5Au);
    }
    hal_stub_stats_rw()->fail_erase_at = 2u;
    expect("silme hatasi (FMSTAT) -> FWU_ERR_FLASH_ERASE", fwu_flash_erase(&g_port.flash, 0u, 3u * PAGE), FWU_ERR_FLASH_ERASE);
    check("1. sayfa silindi, 2. basarisiz (dokunulmadi), 3. DENENMEDI", page_is(0u, 0xFFu) && page_is(1u, 0x5Au) && page_is(2u, 0x5Au) &&
          st->erase_calls == 2u);

    /* FMSTAT hata biti: silme yapıldı (veri doğru) ama durum kötü: port yine de hata dönmeli */
    fresh();
    fill_page(1u, 0x5Au);
    hal_stub_stats_rw()->fmstat_erase_at = 1u;
    expect("FMSTAT hata biti (sektor bos ama durum kotu) -> FWU_ERR_FLASH_ERASE", fwu_flash_erase(&g_port.flash, PAGE, PAGE), FWU_ERR_FLASH_ERASE);
    check("... sektor gercekten silinmisti (hatayi FMSTAT verdi, bos kontrolu degil)", page_is(1u, 0xFFu));

    /* yarım silme (güç kesintisi): sektörün ilk yarısı silinir, ikinci yarısı eski içerikte kalır */
    fresh();
    fill_page(2u, 0x5Au);
    hal_stub_cut_power_after(0u);
    expect("guc kesintisinde silme hata verir (silme sonrasi bos degil)", fwu_flash_erase(&g_port.flash, 2u * PAGE, PAGE), FWU_ERR_FLASH_ERASE);
    check("sayfa YARIM silinmis (ilk yarisi 0xFF, son yarisi 0x5A)", octet_at((2u * PAGE)) == 0xFFu && octet_at((2u * PAGE) + PAGE - 1u) == 0x5Au);
}

/* ------------------------------------------------------------------ 4. yazma */

static void test_write(void)
{
    const hal_stub_stats_t *st;
    fwu_byte_t buf[64];
    fwu_byte_t back[64];

    printf("\n--- 4. yazma ---\n");
    fresh();
    st = hal_stub_stats();
    for(uint32_t i = 0u; i < 16u; i++) {
        buf[i] = (fwu_byte_t)(i + 1u);
    }
    expect("16 oktet yaz (ofset 0x100)", fwu_flash_write(&g_port.flash, 0x100u, 16u, buf), FWU_OK);
    check("bellekte kelimeler: 0x0201 0x0403 ... 0x100F (dusuk oktet once)",
          hal_stub_memory()[0x80] == 0x0201u && hal_stub_memory()[0x81] == 0x0403u && hal_stub_memory()[0x87] == 0x100Fu);
    check("Flash API: 1 programlama, 8 kelime, kelime adresi 0x080000 + 0x80", st->program_calls == 1u && st->last_program_words == 8u &&
          st->last_program_address == (WORD_BASE + 0x80u));
    check("bekci kancasi granul basina bir kez (1)", g_keepalive_calls == 1u);
    expect("geri oku", fwu_flash_read(&g_port.flash, 0x100u, 16u, back), FWU_OK);
    check("geri okunan 16 oktet yazilanla ayni", memcmp(buf, back, 16u * sizeof(fwu_byte_t)) == 0);

    /* uç değerler: 0x00, 0xFF, 0x80 */
    buf[0] = 0x00u; buf[1] = 0xFFu; buf[2] = 0x80u; buf[3] = 0x7Fu; buf[4] = 0xAAu; buf[5] = 0x55u;
    for(uint32_t i = 6u; i < 16u; i++) {
        buf[i] = (fwu_byte_t)0xFFu;
    }
    expect("uc degerler (00 FF 80 7F AA 55 ...)", fwu_flash_write(&g_port.flash, 0x110u, 16u, buf), FWU_OK);
    expect("geri oku", fwu_flash_read(&g_port.flash, 0x110u, 16u, back), FWU_OK);
    check("... ayni (kelimeler 0xFF00, 0x7F80, 0x55AA)", memcmp(buf, back, 16u * sizeof(fwu_byte_t)) == 0 && hal_stub_memory()[0x88] == 0xFF00u &&
          hal_stub_memory()[0x89] == 0x7F80u && hal_stub_memory()[0x8A] == 0x55AAu);

    /* çok granül */
    for(uint32_t i = 0u; i < 64u; i++) {
        buf[i] = (fwu_byte_t)(0xA0u + i);
    }
    expect("64 oktet yaz (4 granul, ofset 0x200)", fwu_flash_write(&g_port.flash, 0x200u, 64u, buf), FWU_OK);
    check("4 Flash API cagrisi daha (toplam 6), son adres 0x080000 + 0x100 + 24", st->program_calls == 6u &&
          st->last_program_address == (WORD_BASE + ((0x200u + 48u) / 2u)));
    expect("geri oku", fwu_flash_read(&g_port.flash, 0x200u, 64u, back), FWU_OK);
    check("geri okunan 64 oktet yazilanla ayni", memcmp(buf, back, 64u * sizeof(fwu_byte_t)) == 0);

    /* sektör sınırı ve banka 1 */
    fresh();
    st = hal_stub_stats();
    for(uint32_t i = 0u; i < 32u; i++) {
        buf[i] = (fwu_byte_t)(0x30u + i);
    }
    expect("sektor siniri (2 granul): 1. sektorun son 16 + 2. sektorun ilk 16", fwu_flash_write(&g_port.flash, PAGE - 16u, 32u, buf), FWU_OK);
    expect("geri oku", fwu_flash_read(&g_port.flash, PAGE - 16u, 32u, back), FWU_OK);
    check("... ayni", memcmp(buf, back, 32u * sizeof(fwu_byte_t)) == 0 && st->program_calls == 2u);
    expect("banka 1 (sektor 16, 0x090000)", fwu_flash_write(&g_port.flash, 16u * PAGE, 16u, buf), FWU_OK);
    check("... Flash API adresi 0x090000", st->last_program_address == 0x00090000u);

    /* aynı yere tekrar yazma */
    fresh();
    st = hal_stub_stats();
    for(uint32_t i = 0u; i < 16u; i++) {
        buf[i] = (fwu_byte_t)0x42u;
    }
    expect("ilk yazma", fwu_flash_write(&g_port.flash, 0x300u, 16u, buf), FWU_OK);
    expect("AYNI yere tekrar yazma -> FWU_ERR_FLASH_NOT_ERASED", fwu_flash_write(&g_port.flash, 0x300u, 16u, buf), FWU_ERR_FLASH_NOT_ERASED);
    check("... Flash API yeniden CAGRILMADI", st->program_calls == 1u);
    for(uint32_t i = 0u; i < 32u; i++) {
        buf[i] = (fwu_byte_t)0x11u;
    }
    expect("iki granul, ikincisi dolu -> NOT_ERASED", fwu_flash_write(&g_port.flash, 0x2F0u, 32u, buf), FWU_ERR_FLASH_NOT_ERASED);
    check("... ilk (silinmis) granul de YAZILMADI (once tum hedef denetlenir)", octet_at(0x2F0u) == 0xFFu && octet_at(0x2FFu) == 0xFFu &&
          st->program_calls == 1u);

    /* hizalama: çekirdek reddeder */
    fresh();
    st = hal_stub_stats();
    expect("hizasiz ofset (8)", fwu_flash_write(&g_port.flash, 8u, 16u, buf), FWU_ERR_WRITE_ALIGNMENT);
    expect("hizasiz uzunluk (8)", fwu_flash_write(&g_port.flash, 0u, 8u, buf), FWU_ERR_WRITE_ALIGNMENT);
    expect("aralik disi", fwu_flash_write(&g_port.flash, FLASH_LEN - 16u, 32u, buf), FWU_ERR_OUT_OF_RANGE);
    check("... Flash API hic cagrilmadi", st->program_calls == 0u);

    /* fwu_byte_t üst bitleri maskelenir (C28x'te 16 bitlik kap) */
    fresh();
    for(uint32_t i = 0u; i < 16u; i++) {
        buf[i] = (fwu_byte_t)(0x10u + i);
    }
#ifdef C2000
    buf[0] = (fwu_byte_t)0xAB10u;      /* üst 8 bit çöp */
    buf[1] = (fwu_byte_t)0xFF11u;
    expect("C28x: ust bitleri cop oktetler", fwu_flash_write(&g_port.flash, 0x400u, 16u, buf), FWU_OK);
    check("... maskelendi (kelime 0x1110, cop kelimeyi bozmadi)", hal_stub_memory()[0x200] == 0x1110u && hal_stub_memory()[0x201] == 0x1312u);
#endif

    /* hata enjeksiyonu */
    fresh();
    st = hal_stub_stats();
    for(uint32_t i = 0u; i < 48u; i++) {
        buf[i] = (fwu_byte_t)(0x60u + i);
    }
    hal_stub_stats_rw()->fail_program_at = 2u;
    expect("programlama hatasi (FMSTAT) -> FWU_ERR_FLASH_WRITE", fwu_flash_write(&g_port.flash, 0x500u, 48u, buf), FWU_ERR_FLASH_WRITE);
    check("1. granul yazildi, 2. yazilmadi, 3. DENENMEDI", octet_at(0x500u) == 0x60u && octet_at(0x510u) == 0xFFu && octet_at(0x520u) == 0xFFu &&
          st->program_calls == 2u);

    /* FMSTAT hata biti: yazma yapıldı (veri doğru) ama durum kötü */
    fresh();
    for(uint32_t i = 0u; i < 16u; i++) {
        buf[i] = (fwu_byte_t)(0x20u + i);
    }
    hal_stub_stats_rw()->fmstat_program_at = 1u;
    expect("FMSTAT hata biti (veri dogru ama durum kotu) -> FWU_ERR_FLASH_WRITE", fwu_flash_write(&g_port.flash, 0x700u, 16u, buf), FWU_ERR_FLASH_WRITE);
    check("... veri yazilmisti (hatayi FMSTAT verdi, dogrulama degil)", octet_at(0x700u) == 0x20u && octet_at(0x70Fu) == 0x2Fu);

    /* yarım yazma (güç kesintisi): ilk 4 kelime yazılır, sonrası boş; doğrulama yakalar */
    fresh();
    for(uint32_t i = 0u; i < 32u; i++) {
        buf[i] = (fwu_byte_t)(0x70u + i);
    }
    hal_stub_cut_power_after(1u);
    expect("guc kesintisinde yazma hata verir", fwu_flash_write(&g_port.flash, 0x600u, 32u, buf), FWU_ERR_FLASH_WRITE);
    check("1. granul tam, 2. granul YARIM (ilk 8 oktet), geri kalani bos",
          octet_at(0x600u) == 0x70u && octet_at(0x60Fu) == 0x7Fu && octet_at(0x610u) == 0x80u && octet_at(0x617u) == 0x87u &&
          octet_at(0x618u) == 0xFFu && octet_at(0x61Fu) == 0xFFu);
}

/* ------------------------------------------------------------------ 5. çekirdekle birlikte */

static fwu_byte_t g_work[512];

static fwu_byte_t old_byte(uint32_t i)
{
    return (fwu_byte_t)(((i * 5u) + 1u + ((i >> 8) * 11u)) & 0xFFu);
}

static fwu_byte_t new_byte(uint32_t i)
{
    return (fwu_byte_t)(((i * 3u) + 200u + ((i >> 8) * 7u)) & 0xFFu);
}

static void act_config(fwu_activate_config_t *c, fwu_activate_mode_e mode)
{
    c->flash = &g_port.flash;
    c->mode = mode;
    c->app_offset = FWU_LAYOUT_APP_OFFSET;
    c->staging_offset = FWU_LAYOUT_STAGING_IMAGE_OFFSET;
    c->region_length = FWU_LAYOUT_IMAGE_MAX_SIZE;
    c->journal_a_offset = FWU_LAYOUT_JOURNAL_A_OFFSET;
    c->journal_b_offset = FWU_LAYOUT_JOURNAL_B_OFFSET;
    c->journal_length = FWU_LAYOUT_JOURNAL_LENGTH;
    c->scratch_offset = FWU_LAYOUT_SCRATCH_OFFSET;
    c->scratch_pages = FWU_LAYOUT_SCRATCH_PAGES;
    c->work = g_work;
    c->work_length = 512u;
    c->max_trial_boots = 2u;
    c->require_verify = false;
    c->verify = NULL;
    c->verify_context = NULL;
    c->on_confirmed = NULL;
    c->on_confirmed_context = NULL;
}

/** "Fabrikada yazilmis" icerik: eski imaj uygulamada, yeni imaj staging'in imaj bolgesinde (n sayfa). */
static void load_fixture(uint32_t pages)
{
    for(uint32_t i = 0u; i < (pages * PAGE); i++) {
        set_octet(FWU_LAYOUT_APP_OFFSET + i, (uint8_t)old_byte(i));
        set_octet(FWU_LAYOUT_STAGING_IMAGE_OFFSET + i, (uint8_t)new_byte(i));
    }
}

static int app_has_new(uint32_t pages)
{
    for(uint32_t i = 0u; i < (pages * PAGE); i++) {
        if(octet_at(FWU_LAYOUT_APP_OFFSET + i) != (uint8_t)new_byte(i)) {
            return 0;
        }
    }
    return 1;
}

static int staging_has_old(uint32_t pages)
{
    for(uint32_t i = 0u; i < (pages * PAGE); i++) {
        if(octet_at(FWU_LAYOUT_STAGING_IMAGE_OFFSET + i) != (uint8_t)old_byte(i)) {
            return 0;
        }
    }
    return 1;
}

static fwu_return_e boot_until_ok(fwu_activate_t *act, const fwu_activate_config_t *c, uint32_t cut, bool cut_armed,
                                  fwu_activate_outcome_e *outcome)
{
    for(uint32_t k = 0u; k < 50u; k++) {
        fwu_return_e status;
        hal_stub_power_on();
        status = fwu_activate_init(act, c);
        if(status != FWU_OK) {
            return status;
        }
        if((k == 0u) && cut_armed) {
            hal_stub_cut_power_after(cut);
        }
        status = fwu_activate_run(act, outcome);
        if(status == FWU_OK) {
            return FWU_OK;
        }
    }
    return FWU_ERR_BAD_STATE;
}

static void test_with_core(void)
{
    static fwu_activate_t act;
    fwu_activate_config_t ac;
    fwu_activate_outcome_e outcome = FWU_ACT_OUTCOME_NONE;
    fwu_journal_ab_t jr;
    fwu_journal_record_t last;
    uint32_t total_ops;
    int copy_ok = 1;
    int swap_ok = 1;
    uint32_t copy_points = 0u;
    uint32_t swap_points = 0u;
    int journal_ok = 1;

    printf("\n--- 5. cekirdekle birlikte (gercek fwu_layout F280049C adresleri): journal ve aktivasyon, HER islemde kesinti ---\n");

    /* A/B journal: yedek bolgede, 3000 kayit (8 KB bolge dolar: bolge degisimleri) */
    fresh();
    expect("journal bolgelerini sil", fwu_flash_erase(&g_port.flash, FWU_LAYOUT_JOURNAL_A_OFFSET, 2u * PAGE), FWU_OK);
    expect("journal init", fwu_journal_ab_init(&jr, &g_port.flash, FWU_LAYOUT_JOURNAL_A_OFFSET, FWU_LAYOUT_JOURNAL_B_OFFSET, PAGE), FWU_OK);
    for(uint32_t i = 1u; i <= 3000u; i++) {
        if((fwu_journal_ab_append(&jr, (fwu_byte_t)(i & 0x7Fu), 0u, i) != FWU_OK) || !fwu_journal_ab_get_last(&jr, &last) ||
           (last.arg != i)) {
            journal_ok = 0;
        }
    }
    check("A/B journal: 3000 kayit (Flash API uzerinden, bolge degisimleri dahil), her adimda son kayit dogru", journal_ok);
    expect("yeniden baslat: journal ayni durumu verir", fwu_journal_ab_init(&jr, &g_port.flash, FWU_LAYOUT_JOURNAL_A_OFFSET, FWU_LAYOUT_JOURNAL_B_OFFSET, PAGE), FWU_OK);
    check("... son kayit arg 3000", fwu_journal_ab_get_last(&jr, &last) && last.arg == 3000u);
    check("journal bolgeleri banka 1'de: ilk silmeler 0x09D000 ve 0x09E000", hal_stub_stats()->erase_calls >= 2u &&
          hal_stub_stats()->erase_log[0] == 0x0009D000u && hal_stub_stats()->erase_log[1] == 0x0009E000u);

    /* COPY: 2 sayfa, kesintisiz */
    fresh();
    load_fixture(2u);
    act_config(&ac, FWU_ACTIVATE_MODE_COPY);
    expect("COPY: init", fwu_activate_init(&act, &ac), FWU_OK);
    expect("COPY: commit(2)", fwu_activate_commit(&act, 2u), FWU_OK);
    hal_stub_power_on();
    expect("COPY: run", boot_until_ok(&act, &ac, 0u, false, &outcome), FWU_OK);
    check("COPY: ACTIVATED, uygulama = yeni imaj (2 sayfa)", outcome == FWU_ACT_OUTCOME_ACTIVATED && app_has_new(2u));
    check("COPY: bekci kancasi cagrildi (silme + yazma)", g_keepalive_calls > 500u);
    total_ops = hal_stub_operations();

    for(uint32_t n = 0u; n < total_ops; n++) {
        fresh();
        load_fixture(2u);
        act_config(&ac, FWU_ACTIVATE_MODE_COPY);
        (void)fwu_activate_init(&act, &ac);
        (void)fwu_activate_commit(&act, 2u);
        if((boot_until_ok(&act, &ac, n, true, &outcome) != FWU_OK) || !app_has_new(2u) || (act.last_state != FWU_ACT_STATE_DONE)) {
            copy_ok = 0;
        }
        copy_points++;
    }
    check("COPY: HER silme/yazma isleminde guc kesintisi: kurtarma sonunda uygulama TAM YENI", copy_ok);
    printf("    (COPY: %u kesinti noktasi tarandi)\n", (unsigned)copy_points);

    /* SWAP: 2 sayfa, kesintisiz */
    fresh();
    load_fixture(2u);
    act_config(&ac, FWU_ACTIVATE_MODE_SWAP);
    (void)fwu_activate_init(&act, &ac);
    (void)fwu_activate_commit(&act, 2u);
    expect("SWAP: run", boot_until_ok(&act, &ac, 0u, false, &outcome), FWU_OK);
    check("SWAP: TRIAL, uygulama = yeni, staging = ESKI imaj", outcome == FWU_ACT_OUTCOME_TRIAL && app_has_new(2u) && staging_has_old(2u));
    total_ops = hal_stub_operations();

    for(uint32_t n = 0u; n < total_ops; n++) {
        fresh();
        load_fixture(2u);
        act_config(&ac, FWU_ACTIVATE_MODE_SWAP);
        (void)fwu_activate_init(&act, &ac);
        (void)fwu_activate_commit(&act, 2u);
        if((boot_until_ok(&act, &ac, n, true, &outcome) != FWU_OK) || (outcome != FWU_ACT_OUTCOME_TRIAL) || !app_has_new(2u) ||
           !staging_has_old(2u) || (act.phase != FWU_ACT_PHASE_TRIAL)) {
            swap_ok = 0;
        }
        swap_points++;
    }
    check("SWAP: HER silme/yazma isleminde guc kesintisi: kurtarma sonunda TRIAL, uygulama TAM YENI, staging TAM ESKI", swap_ok);
    printf("    (SWAP: %u kesinti noktasi tarandi)\n", (unsigned)swap_points);

    /* SWAP geri dönüş tam akış */
    fresh();
    load_fixture(2u);
    (void)fwu_activate_init(&act, &ac);
    (void)fwu_activate_commit(&act, 2u);
    (void)boot_until_ok(&act, &ac, 0u, false, &outcome);
    (void)fwu_activate_request_revert(&act);
    expect("SWAP geri donus", boot_until_ok(&act, &ac, 0u, false, &outcome), FWU_OK);
    {
        int old_back = 1;
        for(uint32_t i = 0u; i < (2u * PAGE); i++) {
            if(octet_at(FWU_LAYOUT_APP_OFFSET + i) != (uint8_t)old_byte(i)) {
                old_back = 0;
            }
        }
        check("... REVERTED, uygulama TAM ESKI", outcome == FWU_ACT_OUTCOME_REVERTED && old_back);
    }
}

int main(void)
{
    test_init();
    test_read();
    test_erase();
    test_write();
    test_with_core();

    printf("\nResult: %s (%d failed)\n", g_failures == 0 ? "ALL TESTS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
