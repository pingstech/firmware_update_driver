/**
 * @file    test_flash_sim.c
 * @brief   Sahte flash (fwu_flash_sim.h) için kendi kendini kontrol eden test.
 *
 * Sahte flash, gerçek flash'ın kurallarını uygulamalı ve güç kesintisini doğru modellemeli.
 * Bu dosya iki şeyi sınar:
 *   1. KURALLAR: yeni flash silinmiş, yazma/okuma, silinmemiş alana yazma reddi, sayfa silme,
 *      silme sayaçları, kirli üst bitlerin maskelenmesi, sahte flash'ın kendi savunması.
 *   2. GÜÇ KESİNTİSİ: yazma/silme ortasında kesinti, kesinti sonrası davranış, reset, ve bir
 *      senaryoda HER kesinti noktasının bağımsız bir referans modele karşı taranması.
 *
 * Erişim çekirdeğin kullanacağı yoldan yapılır (fwu_flash_read/write/erase sarmalayıcıları).
 * Sahte flash'ın kendi savunması ise callback'ler doğrudan çağrılarak sınanır.
 *
 * Geometri (küçük, taramalar hızlı olsun diye): 4 sayfa x 64, granül 8 -> toplam 256.
 * Ayrıca gerçekçi bir geometri (4 sayfa x 2048, granül 8) ile 496 baytlık parça yazma testi vardır.
 *
 * Derleme (flash klasöründen):
 *   gcc -Wall -Wextra -Wpedantic -Wconversion -std=c99 -I. -I.. -I../error fwu_flash.c fwu_flash_sim.c test_flash_sim.c -o test_flash_sim && ./test_flash_sim
 */

#include <stdio.h>
#include <string.h>
#include "fwu_flash_sim.h"

FWU_FLASH_SIM_DEFINE(small_flash, 4, 64, 8);
FWU_FLASH_SIM_DEFINE(real_flash, 4, 2048, 8);

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

/** [offset, offset+length) aralığındaki her bayt `value` mu? (okuma sarmalayıcıdan) */
static int all_equal(const fwu_flash_t *flash, uint32_t offset, uint32_t length, fwu_byte_t value)
{
    fwu_byte_t buffer[256];
    if((length > 256u) || (fwu_flash_read(flash, offset, length, buffer) != FWU_OK)) {
        return 0;
    }
    for(uint32_t i = 0u; i < length; i++) {
        if(buffer[i] != value) {
            return 0;
        }
    }
    return 1;
}

/** Sahte flash'ı temiz kurar. */
static void fresh(small_flash_t *sim)
{
    (void)small_flash_init(sim);
}

/* ------------------------------------------------------------------ kurallar */

static void test_rules(small_flash_t *sim)
{
    const fwu_flash_t *flash = &sim->base.flash;
    fwu_byte_t data[64];
    fwu_byte_t back[64];

    printf("\n--- kurallar ---\n");

    fresh(sim);
    expect("init + validate", fwu_flash_validate(flash), FWU_OK);
    check("geometri: 256 / 64 / 8 / 0xFF",
          flash->geometry.flash_length == 256u && flash->geometry.page_length == 64u &&
          flash->geometry.write_granule == 8u && flash->geometry.erased_value == (fwu_byte_t)0xFFu);
    check("yeni flash tamamen silinmis (0xFF)", all_equal(flash, 0u, 256u, (fwu_byte_t)0xFFu));

    for(uint32_t i = 0u; i < 16u; i++) {
        data[i] = (fwu_byte_t)(i + 1u);
    }
    expect("yaz (0,16)", fwu_flash_write(flash, 0u, 16u, data), FWU_OK);
    check("geri oku: ayni veri", fwu_flash_read(flash, 0u, 16u, back) == FWU_OK && memcmp(data, back, 16u * sizeof(fwu_byte_t)) == 0);
    check("komsu alan hala 0xFF", all_equal(flash, 16u, 48u, (fwu_byte_t)0xFFu));
    check("okuma yan etkisiz (ikinci okuma ayni)", fwu_flash_read(flash, 0u, 16u, back) == FWU_OK && back[0] == 1u && back[15] == 16u);

    /* Silinmemiş alana yazma reddedilir ve hiçbir şey değişmez. */
    for(uint32_t i = 0u; i < 8u; i++) {
        data[i] = (fwu_byte_t)0xAAu;
    }
    expect("ustune farkli veri yazma -> NOT_ERASED", fwu_flash_write(flash, 8u, 8u, data), FWU_ERR_FLASH_NOT_ERASED);
    expect("ayni veriyi tekrar yazma da -> NOT_ERASED", fwu_flash_write(flash, 0u, 8u, back), FWU_ERR_FLASH_NOT_ERASED);
    check("ret sonrasi icerik bozulmadi", fwu_flash_read(flash, 0u, 16u, back) == FWU_OK && back[0] == 1u && back[8] == 9u && back[15] == 16u);
    {
        fwu_byte_t big[16];
        memset(big, 0x11, sizeof(big));
        expect("kismen dolu hedefe yazma reddedilir (8,16)", fwu_flash_write(flash, 8u, 16u, big), FWU_ERR_FLASH_NOT_ERASED);
        check("... ve hedefin bos kismi (16..24) degismedi", all_equal(flash, 16u, 8u, (fwu_byte_t)0xFFu));
    }

    /* Sayfa silme: yalnızca o sayfa. */
    memset(data, 0x00, sizeof(data));
    expect("sayfa 1'e yaz (64,8)", fwu_flash_write(flash, 64u, 8u, data), FWU_OK);
    expect("sayfa 1'i sil", fwu_flash_erase(flash, 64u, 64u), FWU_OK);
    check("sayfa 1 tamamen 0xFF", all_equal(flash, 64u, 64u, (fwu_byte_t)0xFFu));
    check("sayfa 0 etkilenmedi", fwu_flash_read(flash, 0u, 1u, back) == FWU_OK && back[0] == 1u);
    expect("silinen yere tekrar yazilabilir", fwu_flash_write(flash, 64u, 8u, data), FWU_OK);

    /* Silme sayaçları. */
    {
        uint32_t c0 = 0u, c1 = 0u, c2 = 0u, c3 = 0u, bad = 0u;
        (void)fwu_flash_sim_get_erase_count(&sim->base, 0u, &c0);
        (void)fwu_flash_sim_get_erase_count(&sim->base, 1u, &c1);
        (void)fwu_flash_sim_get_erase_count(&sim->base, 2u, &c2);
        check("silme sayaclari: p0=0, p1=1, p2=0", c0 == 0u && c1 == 1u && c2 == 0u);
        expect("cok sayfali silme (0,192)", fwu_flash_erase(flash, 0u, 192u), FWU_OK);
        (void)fwu_flash_sim_get_erase_count(&sim->base, 0u, &c0);
        (void)fwu_flash_sim_get_erase_count(&sim->base, 1u, &c1);
        (void)fwu_flash_sim_get_erase_count(&sim->base, 2u, &c2);
        (void)fwu_flash_sim_get_erase_count(&sim->base, 3u, &c3);
        check("sayaclar: 1, 2, 1, 0", c0 == 1u && c1 == 2u && c2 == 1u && c3 == 0u);
        expect("gecersiz sayfa sayaci -> INVALID_ARG", fwu_flash_sim_get_erase_count(&sim->base, 4u, &bad), FWU_ERR_INVALID_ARG);
    }

    /* Kirli üst bitler (C28x'te fwu_byte_t 16 bit) maskelenir. */
    fresh(sim);
    {
        fwu_byte_t dirty[8];
        for(uint32_t i = 0u; i < 8u; i++) {
            dirty[i] = (fwu_byte_t)(0xFF00u | 0xA5u);
        }
        expect("kirli ust bitli veri yaz", fwu_flash_write(flash, 0u, 8u, dirty), FWU_OK);
        check("saklanan deger maskeli 0xA5", fwu_flash_read(flash, 0u, 8u, back) == FWU_OK && back[0] == (fwu_byte_t)0xA5u && back[7] == (fwu_byte_t)0xA5u);
    }

    /* Sahte flash'in kendi savunmasi: callback'ler dogrudan cagrilir (sarmalayici atlanir). */
    fresh(sim);
    {
        const fwu_flash_callback_t *cb = flash->callback;
        void *ctx = flash->context;
        fwu_byte_t d8[8] = {0};
        expect("dogrudan: hizasiz yazma -> WRITE_ALIGNMENT", cb->flash_write(ctx, 4u, 8u, d8), FWU_ERR_WRITE_ALIGNMENT);
        expect("dogrudan: hizasiz silme -> ERASE_ALIGNMENT", cb->flash_erase(ctx, 8u, 64u), FWU_ERR_ERASE_ALIGNMENT);
        expect("dogrudan: aralik disi yazma -> OUT_OF_RANGE", cb->flash_write(ctx, 256u, 8u, d8), FWU_ERR_OUT_OF_RANGE);
        expect("dogrudan: aralik disi silme -> OUT_OF_RANGE", cb->flash_erase(ctx, 192u, 128u), FWU_ERR_OUT_OF_RANGE);
        expect("dogrudan: tasma okuma -> OUT_OF_RANGE", cb->flash_read(ctx, 0xFFFFFFFFu, 2u, back), FWU_ERR_OUT_OF_RANGE);
        expect("dogrudan: length 0 -> INVALID_ARG", cb->flash_read(ctx, 0u, 0u, back), FWU_ERR_INVALID_ARG);
        expect("dogrudan: veri NULL -> INVALID_ARG", cb->flash_write(ctx, 0u, 8u, NULL), FWU_ERR_INVALID_ARG);
        expect("dogrudan: context NULL -> INVALID_ARG", cb->flash_read(NULL, 0u, 8u, back), FWU_ERR_INVALID_ARG);
        check("savunma reddettikleri flash'i degistirmedi", all_equal(flash, 0u, 256u, (fwu_byte_t)0xFFu));
        check("savunma reddettikleri silme sayacini artirmadi", sim->base.erase_counts[0] == 0u && sim->base.erase_counts[1] == 0u);
    }

    /* init geometri hatalari. */
    {
        small_flash_t other;
        expect("init_raw: granul sayfaya bolunmuyor -> FLASH_GEOMETRY",
               fwu_flash_sim_init_raw(&other.base, other.memory, other.erase_counts, 4u, 64u, 12u), FWU_ERR_FLASH_GEOMETRY);
        expect("init_raw: NULL sim -> INVALID_ARG",
               fwu_flash_sim_init_raw(NULL, other.memory, other.erase_counts, 4u, 64u, 8u), FWU_ERR_INVALID_ARG);
        expect("init_raw: sifir sayfa -> INVALID_ARG",
               fwu_flash_sim_init_raw(&other.base, other.memory, other.erase_counts, 0u, 64u, 8u), FWU_ERR_INVALID_ARG);
        expect("init_raw: boyut tasmasi -> FLASH_GEOMETRY",
               fwu_flash_sim_init_raw(&other.base, other.memory, other.erase_counts, 0x10000u, 0x10000u, 8u), FWU_ERR_FLASH_GEOMETRY);
    }
}

/* ------------------------------------------------------------------ gerçekçi geometri */

static void test_realistic(void)
{
    static real_flash_t sim;
    const fwu_flash_t *flash = &sim.base.flash;
    fwu_byte_t chunk[496];
    fwu_byte_t back[496];
    int ok = 1;

    printf("\n--- gercekci geometri (4 x 2048, granul 8) ---\n");

    expect("init + validate", real_flash_init(&sim), FWU_OK);
    for(uint32_t i = 0u; i < 496u; i++) {
        chunk[i] = (fwu_byte_t)(i & 0xFFu);
    }
    /* Ardışık 496 baytlık parçalar: 496 granülün (8) katı ama sayfanın (2048) katı değil;
     * 5. parça (offset 1984) sayfa sınırını aşar. Yazma sayfa sınırıyla sınırlı değildir. */
    for(uint32_t n = 0u; n < 16u; n++) {
        if(fwu_flash_write(flash, n * 496u, 496u, chunk) != FWU_OK) {
            ok = 0;
        }
    }
    check("16 ardisik 496 baytlik parca yazildi (sayfa siniri asan parca dahil)", ok);
    ok = 1;
    for(uint32_t n = 0u; n < 16u; n++) {
        if((fwu_flash_read(flash, n * 496u, 496u, back) != FWU_OK) ||
           (memcmp(chunk, back, sizeof(chunk)) != 0)) {
            ok = 0;
        }
    }
    check("hepsi dogru geri okundu", ok);
    check("parcalarin bittigi yerden sonrasi (7936..8192) hala 0xFF", all_equal(flash, 7936u, 256u, (fwu_byte_t)0xFFu));
    expect("hizasiz silme (496 uzunlugu) reddedilir", fwu_flash_erase(flash, 0u, 496u), FWU_ERR_ERASE_ALIGNMENT);
    expect("sayfa 0'i sil", fwu_flash_erase(flash, 0u, 2048u), FWU_OK);
    check("sayfa 0 silindi, sayfa 1 veri kaldi",
          all_equal(flash, 0u, 256u, (fwu_byte_t)0xFFu) && fwu_flash_read(flash, 2048u, 8u, back) == FWU_OK && back[0] == chunk[2048u % 496u]);
}

/* ------------------------------------------------------------------ güç kesintisi */

/**
 * Referans model (sahte flash'tan BAĞIMSIZ yazıldı): senaryo
 *   önce sayfa 0 = 0x00; sonra erase(sayfa 0) ; write(offset 0, 24 bayt: i+1)
 * Toplam 4 temel işlem (1 silme + 3 granül yazma). k = kesinti noktası (k işlem biter).
 * Dönen: sayfa 0'ın i. baytının beklenen değeri.
 */
static fwu_byte_t model_byte(uint32_t k, uint32_t i)
{
    const fwu_byte_t new_value = (fwu_byte_t)(i + 1u);
    if(k == 0u) {
        return (i < 32u) ? (fwu_byte_t)0xFFu : (fwu_byte_t)0x00u;      /* silme kesildi: ilk yari silindi */
    }
    if(k >= 4u) {
        return (i < 24u) ? new_value : (fwu_byte_t)0xFFu;               /* kesinti yok */
    }
    {
        const uint32_t g = i / 8u;                                       /* k = 1..3: yazma granul k-1'de kesildi */
        if(g < (k - 1u)) {
            return new_value;
        }
        if(g == (k - 1u)) {
            return ((i % 8u) < 4u) ? new_value : (fwu_byte_t)0xFFu;
        }
        return (fwu_byte_t)0xFFu;
    }
}

static void test_power_cut(small_flash_t *sim)
{
    const fwu_flash_t *flash = &sim->base.flash;
    fwu_byte_t data[64];
    fwu_byte_t back[64];

    printf("\n--- guc kesintisi ---\n");

    /* Yazma ortasında kesinti: 3 granülden 2. kesilir. */
    fresh(sim);
    for(uint32_t i = 0u; i < 24u; i++) {
        data[i] = (fwu_byte_t)(i + 1u);
    }
    fwu_flash_sim_cut_power_after(&sim->base, 1u);
    expect("yazma kesildi -> POWER_LOSS", fwu_flash_write(flash, 0u, 24u, data), FWU_ERR_FLASH_POWER_LOSS);
    check("guc yok", !fwu_flash_sim_is_powered(&sim->base));
    expect("kesinti sonrasi okuma -> POWER_LOSS", fwu_flash_read(flash, 0u, 8u, back), FWU_ERR_FLASH_POWER_LOSS);
    expect("kesinti sonrasi silme -> POWER_LOSS", fwu_flash_erase(flash, 0u, 64u), FWU_ERR_FLASH_POWER_LOSS);
    expect("kesinti sonrasi yazma -> POWER_LOSS", fwu_flash_write(flash, 64u, 8u, data), FWU_ERR_FLASH_POWER_LOSS);
    fwu_flash_sim_reset(&sim->base);
    check("reset: guc var, islem sayaci 0", fwu_flash_sim_is_powered(&sim->base) && fwu_flash_sim_operations(&sim->base) == 0u);
    check("granul 0 tam, granul 1 yarim (ilk 4 bayt), granul 2 bos",
          fwu_flash_read(flash, 0u, 24u, back) == FWU_OK &&
          back[0] == 1u && back[7] == 8u && back[8] == 9u && back[11] == 12u &&
          back[12] == (fwu_byte_t)0xFFu && back[15] == (fwu_byte_t)0xFFu &&
          back[16] == (fwu_byte_t)0xFFu && back[23] == (fwu_byte_t)0xFFu);
    expect("yarim yazilmis granul artik silinmemis -> NOT_ERASED", fwu_flash_write(flash, 8u, 8u, data), FWU_ERR_FLASH_NOT_ERASED);
    expect("dokunulmamis bolgeye yazilabilir", fwu_flash_write(flash, 64u, 8u, data), FWU_OK);

    /* Silme ortasında kesinti: 2 sayfadan 2.'si kesilir. */
    fresh(sim);
    memset(data, 0x00, sizeof(data));
    for(uint32_t g = 0u; g < 16u; g++) {
        (void)fwu_flash_write(flash, g * 8u, 8u, data);   /* sayfa 0 ve 1 = 0x00 */
    }
    fwu_flash_sim_reset(&sim->base);
    fwu_flash_sim_cut_power_after(&sim->base, 1u);
    expect("silme kesildi -> POWER_LOSS", fwu_flash_erase(flash, 0u, 128u), FWU_ERR_FLASH_POWER_LOSS);
    fwu_flash_sim_reset(&sim->base);
    check("sayfa 0 tam silinmis", all_equal(flash, 0u, 64u, (fwu_byte_t)0xFFu));
    check("sayfa 1: alt yari silinmis", all_equal(flash, 64u, 32u, (fwu_byte_t)0xFFu));
    check("sayfa 1: ust yari eski veri (0x00)", all_equal(flash, 96u, 32u, (fwu_byte_t)0x00u));
    check("silme sayaclari: sayfa 0 = 1, kesilen sayfa 1 = 1 (asinma sayildi)",
          sim->base.erase_counts[0] == 1u && sim->base.erase_counts[1] == 1u);

    /* cut_power_after(0): ilk işlem hemen kesilir. */
    fresh(sim);
    fwu_flash_sim_cut_power_after(&sim->base, 0u);
    expect("cut_after(0): ilk yazma kesilir", fwu_flash_write(flash, 0u, 8u, data), FWU_ERR_FLASH_POWER_LOSS);
    fwu_flash_sim_reset(&sim->base);
    check("... granulun yalniz ilk yarisi yazildi", fwu_flash_read(flash, 0u, 8u, back) == FWU_OK && back[0] == 0x00u && back[3] == 0x00u && back[4] == (fwu_byte_t)0xFFu);

    /* Okumalar işlem sayılmaz; kesinti ayarı hiç tetiklenmezse hiçbir şey olmaz. */
    fresh(sim);
    fwu_flash_sim_cut_power_after(&sim->base, 2u);
    (void)fwu_flash_read(flash, 0u, 8u, back);
    (void)fwu_flash_read(flash, 0u, 8u, back);
    check("okumalar islem sayilmaz", fwu_flash_sim_operations(&sim->base) == 0u);
    expect("kesinti sinirina esit islem sayisi (2 granul, cut_after=2) kesilmez", fwu_flash_write(flash, 0u, 16u, data), FWU_OK);
    check("... ve islem sayaci 2", fwu_flash_sim_operations(&sim->base) == 2u && fwu_flash_sim_is_powered(&sim->base));
}

/** Kesinti taraması: her k için senaryoyu çalıştırır, sonucu bağımsız modelle bayt bayt karşılaştırır. */
static void test_power_cut_sweep(small_flash_t *sim)
{
    const fwu_flash_t *flash = &sim->base.flash;

    printf("\n--- kesinti taramasi (k = 0..5, referans model) ---\n");

    for(uint32_t k = 0u; k <= 5u; k++) {
        fwu_byte_t zero[8] = {0};
        fwu_byte_t data[24];
        fwu_byte_t page[64];
        fwu_return_e status;
        int ok = 1;
        char name[64];

        fresh(sim);
        for(uint32_t g = 0u; g < 8u; g++) {
            (void)fwu_flash_write(flash, g * 8u, 8u, zero);          /* sayfa 0 = 0x00 */
        }
        for(uint32_t i = 0u; i < 24u; i++) {
            data[i] = (fwu_byte_t)(i + 1u);
        }
        fwu_flash_sim_reset(&sim->base);
        fwu_flash_sim_cut_power_after(&sim->base, k);

        status = fwu_flash_erase(flash, 0u, 64u);
        if(status == FWU_OK) {
            status = fwu_flash_write(flash, 0u, 24u, data);
        }
        if(k < 4u) {
            ok = ok && (status == FWU_ERR_FLASH_POWER_LOSS) && !fwu_flash_sim_is_powered(&sim->base);
        } else {
            ok = ok && (status == FWU_OK) && (fwu_flash_sim_operations(&sim->base) == 4u);
        }
        fwu_flash_sim_reset(&sim->base);
        ok = ok && (fwu_flash_read(flash, 0u, 64u, page) == FWU_OK);
        for(uint32_t i = 0u; i < 64u; i++) {
            if(page[i] != model_byte((k > 4u) ? 4u : k, i)) {
                ok = 0;
            }
        }
        (void)snprintf(name, sizeof(name), "tarama k=%u: sonuc referans modele uyar", (unsigned)k);
        check(name, ok);
    }
}

/* ------------------------------------------------------------------ main */

int main(void)
{
    static small_flash_t sim;

    test_rules(&sim);
    test_realistic();
    test_power_cut(&sim);
    test_power_cut_sweep(&sim);

    printf("\nResult: %s (%d failed)\n", g_failures == 0 ? "ALL TESTS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
