/**
 * @file    test_stm32g0_port.c
 * @brief   STM32G0 flash portunun testi: port + HAL KUKLASI (hal_stub/) üzerinde. Gerçek çipte DEĞİL.
 *
 * Kuklanın doğruluğu çipin de öyle davrandığını göstermez (bkz. fwu_flash_stm32g0.h "DONANIMDA İLK DOĞRULANACAKLAR").
 * Bu test PORT KODUNUN kendi mantığını sınar: adres/banka/sayfa eşlemesi, 8 baytı çift kelimeye çevirme (bayt sırası),
 * silinmiş mi kontrolü, hata yolları, kilit eşleşmesi (açtığını HER durumda kilitler), bekçi kancası, ECC bayrakları.
 *
 * Kapsam:
 *   1. init: geçersiz argümanlar, çipten büyük düzen, geometri, init_mcu.
 *   2. erase: sayfa/banka eşlemesi (banka sınırı dahil), tek banka çip, tüm flash, hatalar, güç kesintisi, kilit.
 *   3. write: küçük endian çift kelime, çok granül, sayfa sınırı, silinmemiş hedef (HAL ÇAĞRILMAZ, hiçbir şey yazılmaz),
 *      HAL hataları, güç kesintisinde yarım yazma, OPTVERR temizleme.
 *   4. read, ECC bayrakları ve NMI yardımcısı.
 *   5. ÇEKİRDEKLE BİRLİKTE (gerçek fwu_layout adresleriyle): A/B journal dönüşü; aktivasyon COPY ve SWAP, HER HAL
 *      işleminde güç kesintisi taraması; kurtarma sonunda uygulama tam yeni imaj.
 *
 * Derleme (port_stm32g0 klasöründen):
 *   gcc -Wall -Wextra -Wpedantic -Wconversion -std=c99 -DFWU_BOARD_STM32G0B1 -Ihal_stub -I. -I.. -I../.. -I../../error -I../../bytes -I../../crc -I../../journal -I../../activation -I../../config fwu_flash_stm32g0.c hal_stub/hal_stub.c ../fwu_flash.c ../../journal/fwu_journal.c ../../journal/fwu_journal_ab.c ../../activation/fwu_activate.c test_stm32g0_port.c -o test_stm32g0_port && ./test_stm32g0_port
 */

#include <stdio.h>
#include "stm32g0xx_hal.h"
#include "fwu_flash_stm32g0.h"
#include "fwu_layout.h"
#include "fwu_journal_ab.h"
#include "fwu_activate.h"

#define PAGE        2048u
#define FLASH_LEN   (FWU_BOARD_TOTAL_PAGES * FWU_BOARD_PAGE_SIZE)

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

static fwu_flash_stm32g0_t g_port;
static uint32_t g_keepalive_calls;

static void keepalive(void *context)
{
    (void)context;
    g_keepalive_calls++;
}

/** HAL kuklasını sıfırlar ve portu 512 KB için başlatır. */
static void fresh(void)
{
    hal_stub_reset();
    g_keepalive_calls = 0u;
    (void)fwu_flash_stm32g0_init(&g_port, FLASH_LEN, hal_stub_memory(), keepalive, NULL);
}

static int page_is(uint32_t page, uint8_t value)
{
    const uint8_t *memory = hal_stub_memory();
    for(uint32_t i = 0u; i < PAGE; i++) {
        if(memory[(page * PAGE) + i] != value) {
            return 0;
        }
    }
    return 1;
}

static void fill_page(uint32_t page, uint8_t value)
{
    uint8_t *memory = hal_stub_memory();
    for(uint32_t i = 0u; i < PAGE; i++) {
        memory[(page * PAGE) + i] = value;
    }
}

/* ------------------------------------------------------------------ 1. init */

static void test_init(void)
{
    fwu_flash_stm32g0_t port;
    fwu_return_e status;

    printf("\n--- init ---\n");
    hal_stub_reset();
    expect("port NULL", fwu_flash_stm32g0_init(NULL, FLASH_LEN, hal_stub_memory(), NULL, NULL), FWU_ERR_INVALID_ARG);
    expect("memory NULL", fwu_flash_stm32g0_init(&port, FLASH_LEN, NULL, NULL, NULL), FWU_ERR_INVALID_ARG);
    expect("flash_length 0", fwu_flash_stm32g0_init(&port, 0u, hal_stub_memory(), NULL, NULL), FWU_ERR_FLASH_GEOMETRY);
    expect("flash_length sayfa kati degil (2047)", fwu_flash_stm32g0_init(&port, 2047u, hal_stub_memory(), NULL, NULL), FWU_ERR_FLASH_GEOMETRY);
    expect("flash_length sayfa kati degil (2049)", fwu_flash_stm32g0_init(&port, 2049u, hal_stub_memory(), NULL, NULL), FWU_ERR_FLASH_GEOMETRY);
    expect("flash_length 1 sayfa gecerli", fwu_flash_stm32g0_init(&port, PAGE, hal_stub_memory(), NULL, NULL), FWU_OK);

    port.ecc_corrected = 77u;
    port.ecc_detected = 77u;
    status = fwu_flash_stm32g0_init(&port, FLASH_LEN, hal_stub_memory(), keepalive, &port);
    expect("512 KB duzen, 512 KB cip", status, FWU_OK);
    check("geometri: sayfa 2048, granul 8, silinmis 0xFF, uzunluk 512 KB",
          port.flash.geometry.page_length == 2048u && port.flash.geometry.write_granule == 8u &&
          port.flash.geometry.erased_value == 0xFFu && port.flash.geometry.flash_length == 524288u);
    check("context portun kendisi, tablo dolu, kanca saklandi",
          port.flash.context == &port && port.flash.callback == &port.callback && port.keepalive == keepalive &&
          port.keepalive_context == &port);
    check("sayaclar sifirlandi", port.ecc_corrected == 0u && port.ecc_detected == 0u);
    expect("fwu_flash_validate gecer", fwu_flash_validate(&port.flash), FWU_OK);

    hal_stub_set_chip(262144u, true);
    expect("512 KB duzen, 256 KB'lik cip: duzen cipten buyuk", fwu_flash_stm32g0_init(&port, FLASH_LEN, hal_stub_memory(), NULL, NULL), FWU_ERR_FLASH_GEOMETRY);
    expect("256 KB duzen, 256 KB cip: tam sigar", fwu_flash_stm32g0_init(&port, 262144u, hal_stub_memory(), NULL, NULL), FWU_OK);
    expect("256 KB duzen + 2048 (cipten 1 sayfa buyuk)", fwu_flash_stm32g0_init(&port, 262144u + PAGE, hal_stub_memory(), NULL, NULL), FWU_ERR_FLASH_GEOMETRY);
    hal_stub_set_chip(131072u, true);
    expect("128 KB cip: 256 KB duzen reddedilir", fwu_flash_stm32g0_init(&port, 262144u, hal_stub_memory(), NULL, NULL), FWU_ERR_FLASH_GEOMETRY);
    expect("128 KB cip: 128 KB duzen gecerli", fwu_flash_stm32g0_init(&port, 131072u, hal_stub_memory(), NULL, NULL), FWU_OK);
    hal_stub_reset();

    expect("init_mcu: bellek penceresi FLASH_BASE", fwu_flash_stm32g0_init_mcu(&port, FLASH_LEN, NULL, NULL), FWU_OK);
    check("... memory = FLASH_BASE adresi", (uintptr_t)port.memory == (uintptr_t)FLASH_BASE);
    expect("init_mcu: port NULL", fwu_flash_stm32g0_init_mcu(NULL, FLASH_LEN, NULL, NULL), FWU_ERR_INVALID_ARG);
}

/* ------------------------------------------------------------------ 2. erase */

static void test_erase(void)
{
    const hal_stub_stats_t *st;
    int all_erased;

    printf("\n--- erase: sayfa ve banka eslemesi, hatalar, kilit ---\n");
    fresh();
    st = hal_stub_stats();

    for(uint32_t p = 0u; p < 256u; p++) {
        fill_page(p, 0x5Au);
    }
    expect("tek sayfa (5. sayfa)", fwu_flash_erase(&g_port.flash, 5u * PAGE, PAGE), FWU_OK);
    check("5. sayfa silindi, komsular dokunulmadi", page_is(5u, 0xFFu) && page_is(4u, 0x5Au) && page_is(6u, 0x5Au));
    check("HAL: 1 cagri, banka 1, sayfa 5, 1 sayfa", st->erase_calls == 1u && st->erase_log[0].banks == FLASH_BANK_1 &&
          st->erase_log[0].page == 5u && st->erase_log[0].nb_pages == 1u);
    check("kilit: 1 acma, 1 kilitleme, sonunda KILITLI", st->unlock_calls == 1u && st->lock_calls == 1u && hal_stub_is_locked());
    check("OPTVERR bayragi islem basina bir kez temizlendi", st->optverr_clears == 1u);
    check("bekci kancasi sayfa basina bir kez", g_keepalive_calls == 1u);

    /* banka siniri: 126, 127 banka 1; 128 banka 2 sayfa 0 */
    fresh();
    for(uint32_t p = 0u; p < 256u; p++) {
        fill_page(p, 0x5Au);
    }
    st = hal_stub_stats();
    expect("banka siniri: sayfa 126..128", fwu_flash_erase(&g_port.flash, 126u * PAGE, 3u * PAGE), FWU_OK);
    check("3 HAL cagrisi: (B1,126) (B1,127) (B2,0)",
          st->erase_calls == 3u && st->erase_log[0].banks == FLASH_BANK_1 && st->erase_log[0].page == 126u &&
          st->erase_log[1].banks == FLASH_BANK_1 && st->erase_log[1].page == 127u &&
          st->erase_log[2].banks == FLASH_BANK_2 && st->erase_log[2].page == 0u);
    check("126, 127, 128 silindi; 125 ve 129 dokunulmadi",
          page_is(126u, 0xFFu) && page_is(127u, 0xFFu) && page_is(128u, 0xFFu) && page_is(125u, 0x5Au) && page_is(129u, 0x5Au));
    check("3 sayfa icin 3 bekci cagrisi, tek acma/kilitleme", g_keepalive_calls == 3u && st->unlock_calls == 1u && st->lock_calls == 1u);
    expect("son sayfa (255)", fwu_flash_erase(&g_port.flash, 255u * PAGE, PAGE), FWU_OK);
    check("... (B2,127)", st->erase_log[3].banks == FLASH_BANK_2 && st->erase_log[3].page == 127u && page_is(255u, 0xFFu));
    expect("sayfa 128 tek basina", fwu_flash_erase(&g_port.flash, 128u * PAGE, PAGE), FWU_OK);
    check("... (B2,0)", st->erase_log[4].banks == FLASH_BANK_2 && st->erase_log[4].page == 0u);
    expect("sayfa 127 tek basina", fwu_flash_erase(&g_port.flash, 127u * PAGE, PAGE), FWU_OK);
    check("... (B1,127)", st->erase_log[5].banks == FLASH_BANK_1 && st->erase_log[5].page == 127u);

    /* tek banka cip */
    fresh();
    hal_stub_set_chip(524288u, false);
    st = hal_stub_stats();
    expect("tek banka yapilandirmasi: sayfa 200", fwu_flash_erase(&g_port.flash, 200u * PAGE, PAGE), FWU_OK);
    check("... (B1,200): banka 2 hic kullanilmaz", st->erase_log[0].banks == FLASH_BANK_1 && st->erase_log[0].page == 200u);
    expect("tek banka: sayfa 255", fwu_flash_erase(&g_port.flash, 255u * PAGE, PAGE), FWU_OK);
    check("... (B1,255)", st->erase_log[1].banks == FLASH_BANK_1 && st->erase_log[1].page == 255u);

    /* tum flash */
    fresh();
    for(uint32_t p = 0u; p < 256u; p++) {
        fill_page(p, 0x5Au);
    }
    expect("tum flash (256 sayfa)", fwu_flash_erase(&g_port.flash, 0u, FLASH_LEN), FWU_OK);
    all_erased = 1;
    for(uint32_t p = 0u; p < 256u; p++) {
        if(!page_is(p, 0xFFu)) {
            all_erased = 0;
        }
    }
    check("... 256 sayfanin hepsi silindi, 256 HAL cagrisi, 256 bekci cagrisi", all_erased &&
          hal_stub_stats()->erase_calls == 256u && g_keepalive_calls == 256u);

    /* sarmalayici hatalari: HAL cagrilmaz */
    fresh();
    st = hal_stub_stats();
    expect("hizasiz ofset", fwu_flash_erase(&g_port.flash, 100u, PAGE), FWU_ERR_ERASE_ALIGNMENT);
    expect("hizasiz uzunluk", fwu_flash_erase(&g_port.flash, 0u, 100u), FWU_ERR_ERASE_ALIGNMENT);
    expect("uzunluk 0", fwu_flash_erase(&g_port.flash, 0u, 0u), FWU_ERR_INVALID_ARG);
    expect("flash disi", fwu_flash_erase(&g_port.flash, 255u * PAGE, 2u * PAGE), FWU_ERR_OUT_OF_RANGE);
    check("... HAL hic cagrilmadi", st->erase_calls == 0u && st->unlock_calls == 0u);

    /* HAL hatalari */
    fresh();
    st = hal_stub_stats();
    hal_stub_fail_erase(true);
    expect("HAL silme hatasi", fwu_flash_erase(&g_port.flash, 3u * PAGE, PAGE), FWU_ERR_FLASH_ERASE);
    check("... HER durumda kilitlendi", hal_stub_is_locked() && st->lock_calls == 1u);
    hal_stub_fail_erase(false);
    hal_stub_fail_unlock(true);
    expect("HAL acma hatasi", fwu_flash_erase(&g_port.flash, 3u * PAGE, PAGE), FWU_ERR_FLASH_ERASE);
    check("... silme denenmedi, kilitli", st->erase_calls == 1u && hal_stub_is_locked());
    hal_stub_fail_unlock(false);

    /* guc kesintisi: ikinci sayfa yarim silinir */
    fresh();
    for(uint32_t p = 0u; p < 256u; p++) {
        fill_page(p, 0x5Au);
    }
    st = hal_stub_stats();
    hal_stub_cut_power_after(1u);
    expect("guc kesintisi 2. sayfada", fwu_flash_erase(&g_port.flash, 10u * PAGE, 3u * PAGE), FWU_ERR_FLASH_ERASE);
    check("1. sayfa tam, 2. sayfa YARIM (ilk yarisi silinmis), 3. sayfa dokunulmadi",
          page_is(10u, 0xFFu) && (hal_stub_memory()[(11u * PAGE) + 0u] == 0xFFu) &&
          (hal_stub_memory()[(11u * PAGE) + PAGE - 1u] == 0x5Au) && page_is(12u, 0x5Au));
    check("... kilitli (kesintiden sonra HAL_FLASH_Lock yine cagrildi)", hal_stub_is_locked());
    check("... 3. sayfa icin HAL cagrilmadi", st->erase_calls == 2u);
}

/* ------------------------------------------------------------------ 3. write */

static void test_write(void)
{
    const hal_stub_stats_t *st;
    fwu_byte_t data[64];
    fwu_byte_t back[64];
    const uint8_t *memory;
    int ok;

    printf("\n--- write: cift kelime, bayt sirasi, silinmis kontrolu, hatalar ---\n");
    fresh();
    st = hal_stub_stats();
    memory = hal_stub_memory();

    for(uint8_t i = 0u; i < 8u; i++) {
        data[i] = (fwu_byte_t)(i + 1u);
    }
    expect("8 bayt (1 granul) yaz", fwu_flash_write(&g_port.flash, 0x100u, 8u, data), FWU_OK);
    check("bellekte baytlar AYNI sirada (01..08)", memory[0x100] == 1u && memory[0x101] == 2u && memory[0x107] == 8u);
    check("HAL'a giden adres FLASH_BASE + 0x100, deger 0x0807060504030201 (kucuk endian)",
          st->program_calls == 1u && st->last_program_address == ((uint32_t)FLASH_BASE + 0x100u) &&
          st->last_program_data == 0x0807060504030201ULL);
    check("kilit: 1 acma, 1 kilitleme, kilitli; OPTVERR bir kez", st->unlock_calls == 1u && st->lock_calls == 1u &&
          hal_stub_is_locked() && st->optverr_clears == 1u);
    check("bekci kancasi granul basina bir kez (1)", g_keepalive_calls == 1u);

    /* yuksek bit ve isaret: 0xFF 0x00 0x80 0x7F 0x01 0xFE 0x55 0xAA */
    data[0] = 0xFFu; data[1] = 0x00u; data[2] = 0x80u; data[3] = 0x7Fu; data[4] = 0x01u; data[5] = 0xFEu; data[6] = 0x55u; data[7] = 0xAAu;
    expect("yuksek bitli baytlar", fwu_flash_write(&g_port.flash, 0x108u, 8u, data), FWU_OK);
    check("deger 0xAA55FE017F8000FF (bayt 0 = en dusuk bayt)", st->last_program_data == 0xAA55FE017F8000FFULL);
    check("bellek baytlari ayni sirada", memory[0x108] == 0xFFu && memory[0x10A] == 0x80u && memory[0x10F] == 0xAAu);

    /* cok granul */
    for(uint32_t i = 0u; i < 64u; i++) {
        data[i] = (fwu_byte_t)((i * 3u + 7u) & 0xFFu);
    }
    expect("64 bayt (8 granul)", fwu_flash_write(&g_port.flash, 0x200u, 64u, data), FWU_OK);
    check("8 HAL cagrisi daha (toplam 10), son adres 0x238", st->program_calls == 10u &&
          st->last_program_address == ((uint32_t)FLASH_BASE + 0x238u));
    expect("geri oku", fwu_flash_read(&g_port.flash, 0x200u, 64u, back), FWU_OK);
    ok = 1;
    for(uint32_t i = 0u; i < 64u; i++) {
        if(back[i] != data[i]) {
            ok = 0;
        }
    }
    check("geri okunan 64 bayt yazilanla ayni", ok);

    /* sayfa siniri: ikisi de silinmis */
    expect("sayfa 0 ve 1'i sil (zaten silinmis, yine de)", fwu_flash_erase(&g_port.flash, 0u, 2u * PAGE), FWU_OK);
    for(uint32_t i = 0u; i < 16u; i++) {
        data[i] = (fwu_byte_t)(0xA0u + i);
    }
    expect("sayfa sinirini asan yazma (2040, 16 bayt)", fwu_flash_write(&g_port.flash, 2040u, 16u, data), FWU_OK);
    check("... 2040..2055 yazildi", memory[2040] == 0xA0u && memory[2047] == 0xA7u && memory[2048] == 0xA8u && memory[2055] == 0xAFu);

    /* silinmemis hedef: HAL cagrilmaz, hicbir sey yazilmaz */
    fresh();
    st = hal_stub_stats();
    memory = hal_stub_memory();
    for(uint32_t i = 0u; i < 8u; i++) {
        data[i] = 0x11u;
    }
    (void)fwu_flash_write(&g_port.flash, 0x100u, 8u, data);
    expect("ayni yere ikinci yazma", fwu_flash_write(&g_port.flash, 0x100u, 8u, data), FWU_ERR_FLASH_NOT_ERASED);
    check("... HAL yeniden CAGRILMADI, acma yok", st->program_calls == 1u && st->unlock_calls == 1u);
    for(uint32_t i = 0u; i < 16u; i++) {
        data[i] = 0x22u;
    }
    expect("[silinmis][silinmemis] iki granul: yazma reddedilir", fwu_flash_write(&g_port.flash, 0xF8u, 16u, data), FWU_ERR_FLASH_NOT_ERASED);
    check("... ilk (silinmis) granul de YAZILMADI (once tum hedef denetlenir)", memory[0xF8] == 0xFFu && memory[0xFF] == 0xFFu &&
          st->program_calls == 1u);
    expect("[silinmemis][silinmis]", fwu_flash_write(&g_port.flash, 0x100u, 16u, data), FWU_ERR_FLASH_NOT_ERASED);
    check("... ikinci granul de yazilmadi", memory[0x108] == 0xFFu && st->program_calls == 1u);
    /* hedef aralikta YALNIZ SON bayt silinmemis: yine reddedilir (tum aralik denetlenir) */
    hal_stub_memory()[0x200 + 15] = 0x00u;
    expect("16 baytlik aralikta yalniz son bayt silinmemis", fwu_flash_write(&g_port.flash, 0x200u, 16u, data), FWU_ERR_FLASH_NOT_ERASED);
    check("... HAL cagrilmadi, aralik yazilmadi", st->program_calls == 1u && memory[0x200] == 0xFFu && memory[0x208] == 0xFFu);
    hal_stub_memory()[0x200 + 15] = 0xFFu;
    expect("tek bayti bile 0xFF degil (granulun son baytinda)", fwu_flash_write(&g_port.flash, 0x108u, 8u, data), FWU_OK);
    memory = hal_stub_memory();
    expect("... sonra o granul tekrar yazilamaz", fwu_flash_write(&g_port.flash, 0x108u, 8u, data), FWU_ERR_FLASH_NOT_ERASED);

    /* sarmalayici hatalari */
    fresh();
    st = hal_stub_stats();
    expect("hizasiz ofset", fwu_flash_write(&g_port.flash, 4u, 8u, data), FWU_ERR_WRITE_ALIGNMENT);
    expect("hizasiz uzunluk", fwu_flash_write(&g_port.flash, 0u, 4u, data), FWU_ERR_WRITE_ALIGNMENT);
    expect("data NULL", fwu_flash_write(&g_port.flash, 0u, 8u, NULL), FWU_ERR_INVALID_ARG);
    expect("flash disi", fwu_flash_write(&g_port.flash, FLASH_LEN - 8u, 16u, data), FWU_ERR_OUT_OF_RANGE);
    check("... HAL hic cagrilmadi", st->program_calls == 0u && st->unlock_calls == 0u);
    expect("flash'in son granulu gecerli", fwu_flash_write(&g_port.flash, FLASH_LEN - 8u, 8u, data), FWU_OK);
    check("... adres FLASH_BASE + 0x7FFF8", st->last_program_address == ((uint32_t)FLASH_BASE + 0x7FFF8u));

    /* HAL hatalari */
    fresh();
    st = hal_stub_stats();
    memory = hal_stub_memory();
    hal_stub_fail_program(true);
    expect("HAL yazma hatasi", fwu_flash_write(&g_port.flash, 0x40u, 16u, data), FWU_ERR_FLASH_WRITE);
    check("... HER durumda kilitlendi, ilk hatada durdu (1 cagri), bellek degismedi",
          hal_stub_is_locked() && st->program_calls == 1u && memory[0x40] == 0xFFu);
    hal_stub_fail_program(false);
    hal_stub_fail_unlock(true);
    expect("HAL acma hatasi", fwu_flash_write(&g_port.flash, 0x40u, 8u, data), FWU_ERR_FLASH_WRITE);
    check("... yazma denenmedi, kilitli", st->program_calls == 1u && hal_stub_is_locked());
    hal_stub_fail_unlock(false);

    /* guc kesintisi: 3 granulun ikincisi yarim yazilir */
    fresh();
    st = hal_stub_stats();
    memory = hal_stub_memory();
    for(uint32_t i = 0u; i < 24u; i++) {
        data[i] = (fwu_byte_t)(0x30u + i);
    }
    hal_stub_cut_power_after(1u);
    expect("guc kesintisi 2. granulda", fwu_flash_write(&g_port.flash, 0x300u, 24u, data), FWU_ERR_FLASH_WRITE);
    check("1. granul tam, 2. granul YARIM (ilk 4 bayt), 3. granul silinmis",
          memory[0x300] == 0x30u && memory[0x307] == 0x37u && memory[0x308] == 0x38u && memory[0x30B] == 0x3Bu &&
          memory[0x30C] == 0xFFu && memory[0x30F] == 0xFFu && memory[0x310] == 0xFFu);
    check("... kilitli, 3. granul icin HAL cagrilmadi", hal_stub_is_locked() && st->program_calls == 2u);
}

/* ------------------------------------------------------------------ 4. read ve ECC */

static void test_read_and_ecc(void)
{
    fwu_byte_t buffer[32];
    uint8_t *memory;
    int ok = 1;

    printf("\n--- read, ECC bayraklari, NMI yardimcisi ---\n");
    fresh();
    memory = hal_stub_memory();
    for(uint32_t i = 0u; i < 32u; i++) {
        memory[0x500 + i] = (uint8_t)(0x80u + i);
    }
    expect("okuma", fwu_flash_read(&g_port.flash, 0x500u, 32u, buffer), FWU_OK);
    for(uint32_t i = 0u; i < 32u; i++) {
        if(buffer[i] != (0x80u + i)) {
            ok = 0;
        }
    }
    check("32 bayt dogru okundu (0x80.. yuksek bit dahil)", ok);
    expect("tek bayt, flash'in son baytindan", fwu_flash_read(&g_port.flash, FLASH_LEN - 1u, 1u, buffer), FWU_OK);
    expect("flash disi okuma", fwu_flash_read(&g_port.flash, FLASH_LEN - 1u, 2u, buffer), FWU_ERR_OUT_OF_RANGE);
    expect("okuma, hizasiz ofset ve uzunluk serbest", fwu_flash_read(&g_port.flash, 0x501u, 3u, buffer), FWU_OK);
    check("HAL okuma icin cagrilmadi", hal_stub_stats()->unlock_calls == 0u && hal_stub_stats()->program_calls == 0u);

    /* okuma, ESKI ECC bayraklarini temizler ve sayaci artirmaz (sayac yalniz okuma SIRASINDA olusan bayragi sayar) */
    hal_stub_set_flag(FLASH_FLAG_ECCD1);
    hal_stub_set_flag(FLASH_FLAG_ECCC2);
    (void)fwu_flash_read(&g_port.flash, 0x500u, 8u, buffer);
    check("eski bayraklar okumadan once temizlendi, sayaclar degismedi",
          !hal_stub_get_flag(FLASH_FLAG_ECCD1) && !hal_stub_get_flag(FLASH_FLAG_ECCC2) &&
          g_port.ecc_detected == 0u && g_port.ecc_corrected == 0u);

    /* NMI yardimcisi */
    fresh();
    check("bayrak yok: false", !fwu_flash_stm32g0_handle_ecc_nmi(&g_port));
    hal_stub_set_flag(FLASH_FLAG_ECCD1);
    check("ECCD1 (cift hata, banka 1): true", fwu_flash_stm32g0_handle_ecc_nmi(&g_port));
    check("... bayrak temizlendi, ecc_detected 1", !hal_stub_get_flag(FLASH_FLAG_ECCD1) && g_port.ecc_detected == 1u && g_port.ecc_corrected == 0u);
    hal_stub_set_flag(FLASH_FLAG_ECCD2);
    check("ECCD2 (cift hata, banka 2): true", fwu_flash_stm32g0_handle_ecc_nmi(&g_port));
    check("... ecc_detected 2", g_port.ecc_detected == 2u && !hal_stub_get_flag(FLASH_FLAG_ECCD2));
    hal_stub_set_flag(FLASH_FLAG_ECCC1);
    check("yalniz ECCC1 (tek hata DUZELTILDI): false (NMI nedeni degil)", !fwu_flash_stm32g0_handle_ecc_nmi(&g_port));
    check("... ama duzeltme sayildi ve bayrak temizlendi", g_port.ecc_corrected == 1u && !hal_stub_get_flag(FLASH_FLAG_ECCC1));
    hal_stub_set_flag(FLASH_FLAG_ECCC2);
    hal_stub_set_flag(FLASH_FLAG_ECCD1);
    check("ECCC2 + ECCD1 birlikte: true", fwu_flash_stm32g0_handle_ecc_nmi(&g_port));
    check("... ikisi de sayildi", g_port.ecc_corrected == 2u && g_port.ecc_detected == 3u);
    hal_stub_set_flag(FLASH_FLAG_ECCD1);
    check("port NULL: yine bayragi temizler ve true doner", fwu_flash_stm32g0_handle_ecc_nmi(NULL) && !hal_stub_get_flag(FLASH_FLAG_ECCD1));
    check("... sayaclar degismedi", g_port.ecc_detected == 3u);
}

/* ------------------------------------------------------------------ 5. cekirdekle birlikte */

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
    uint8_t *memory = hal_stub_memory();
    for(uint32_t i = 0u; i < (pages * PAGE); i++) {
        memory[FWU_LAYOUT_APP_OFFSET + i] = old_byte(i);
        memory[FWU_LAYOUT_STAGING_IMAGE_OFFSET + i] = new_byte(i);
    }
}

static int app_has_new(uint32_t pages)
{
    const uint8_t *memory = hal_stub_memory();
    for(uint32_t i = 0u; i < (pages * PAGE); i++) {
        if(memory[FWU_LAYOUT_APP_OFFSET + i] != new_byte(i)) {
            return 0;
        }
    }
    return 1;
}

static int staging_has_old(uint32_t pages)
{
    const uint8_t *memory = hal_stub_memory();
    for(uint32_t i = 0u; i < (pages * PAGE); i++) {
        if(memory[FWU_LAYOUT_STAGING_IMAGE_OFFSET + i] != old_byte(i)) {
            return 0;
        }
    }
    return 1;
}

/** Her acilista [guc ver, init, run]; ilk acilislarda kesinti uygulanir. */
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

    printf("\n--- cekirdekle birlikte (gercek fwu_layout adresleri): journal ve aktivasyon, HER HAL isleminde kesinti ---\n");

    /* A/B journal: bolgeler yedek bolgede (banka 2): 300 kayit, birden cok bolge degisimi */
    fresh();
    expect("journal bolgelerini sil", fwu_flash_erase(&g_port.flash, FWU_LAYOUT_JOURNAL_A_OFFSET, 2u * PAGE), FWU_OK);
    expect("journal init", fwu_journal_ab_init(&jr, &g_port.flash, FWU_LAYOUT_JOURNAL_A_OFFSET, FWU_LAYOUT_JOURNAL_B_OFFSET, PAGE), FWU_OK);
    for(uint32_t i = 1u; i <= 300u; i++) {
        if((fwu_journal_ab_append(&jr, (fwu_byte_t)(i & 0x7Fu), 0u, i) != FWU_OK) || !fwu_journal_ab_get_last(&jr, &last) ||
           (last.arg != i)) {
            journal_ok = 0;
        }
    }
    check("A/B journal: 300 kayit (HAL uzerinden, bolge degisimleri dahil), her adimda son kayit dogru", journal_ok);
    expect("yeniden baslat: journal ayni durumu verir", fwu_journal_ab_init(&jr, &g_port.flash, FWU_LAYOUT_JOURNAL_A_OFFSET, FWU_LAYOUT_JOURNAL_B_OFFSET, PAGE), FWU_OK);
    check("... son kayit arg 300", fwu_journal_ab_get_last(&jr, &last) && last.arg == 300u);
    check("journal bolgeleri banka 2'de: HAL silmeleri (B2,124) ve (B2,125) gordu",
          hal_stub_stats()->erase_calls >= 3u && hal_stub_stats()->erase_log[0].banks == FLASH_BANK_2 &&
          hal_stub_stats()->erase_log[0].page == 124u);

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

    /* COPY: HER HAL isleminde kesinti */
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
    check("COPY: HER HAL isleminde (silme veya cift kelime yazma) guc kesintisi: kurtarma sonunda uygulama TAM YENI", copy_ok);
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
    check("SWAP: HER HAL isleminde guc kesintisi: kurtarma sonunda TRIAL, uygulama TAM YENI, staging TAM ESKI", swap_ok);
    printf("    (SWAP: %u kesinti noktasi tarandi)\n", (unsigned)swap_points);

    /* SWAP geri donus tam akis */
    fresh();
    load_fixture(2u);
    (void)fwu_activate_init(&act, &ac);
    (void)fwu_activate_commit(&act, 2u);
    (void)boot_until_ok(&act, &ac, 0u, false, &outcome);
    (void)fwu_activate_request_revert(&act);
    expect("SWAP geri donus", boot_until_ok(&act, &ac, 0u, false, &outcome), FWU_OK);
    {
        const uint8_t *memory = hal_stub_memory();
        int old_back = 1;
        for(uint32_t i = 0u; i < (2u * PAGE); i++) {
            if(memory[FWU_LAYOUT_APP_OFFSET + i] != old_byte(i)) {
                old_back = 0;
            }
        }
        check("... REVERTED, uygulama TAM ESKI", outcome == FWU_ACT_OUTCOME_REVERTED && old_back);
    }
}

int main(void)
{
    test_init();
    test_erase();
    test_write();
    test_read_and_ecc();
    test_with_core();

    printf("\nResult: %s (%d failed)\n", g_failures == 0 ? "ALL TESTS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
