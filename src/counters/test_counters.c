/**
 * @file    test_counters.c
 * @brief   fwu_counters (sürüm sayaçları) için kendi kendini kontrol eden test: sahte flash + sahte kriptografi
 *          üzerinde; en sonda GERÇEK aktivasyonla (fwu_activate, SWAP + onay kancası) uçtan uca.
 *
 * Kapsam:
 *   1. init / get / hata durumları: argümanlar, geçersiz bölge, "hazır değil" kuralı (güvenilmez sayaç sıfır gibi
 *      davranmaz), yeni cihazda (0, 0).
 *   2. raise: kalıcılık (yeniden başlatma), KAYIT BİÇİMİ (elle yazılmış beklentiyle: sürüm arg'da, anahtar sürümü
 *      state+flags'te), yalnızca artar (alan başına max), değişiklik yoksa flash'a YAZMAZ, sınır değerleri.
 *   3. Rastgele artış/düşüş dizisi bir modele karşı; bölge dönüşünde değerler kaybolmaz.
 *   4. Güç kesintisi taraması: her kayıt konumu (iki bölge dönüşü dahil) x her kesinti noktası: sayaç ya eski ya
 *      yeni ikilidir (iki alan ayrılmaz), düşmez; hatada nesne "hazır değil"; yeniden deneme tamamlar.
 *   5. Flash hataları: yazma yapıldı ama hata döndü (flash gerçeği kazanır), yazma yapılmadı, okuma hataları.
 *   6. Onay kancası: yalnızca İMZALI manifest'ten yükseltir; sahte/bozuk/başka ürün/bilinmeyen anahtar/iptal edilmiş
 *      anahtar sürümü sayacı DEĞİŞTİRMEZ; tekrar çağrı yazmaz; DEV kipi.
 *   7. Aktivasyonla uçtan uca (SWAP): onaydan ÖNCE sayaç artmaz, onaydan sonra artar, geri dönüşte artmaz, onay ile
 *      sayaç yazımı arasında güç kesilirse tekrar çağrı tamamlar, sayaçla doğrulama eski sürümü reddeder.
 *
 * Derleme (counters klasöründen):
 *   gcc -Wall -Wextra -Wpedantic -Wconversion -std=c99 -I. -I.. -I../error -I../bytes -I../crc -I../flash -I../journal -I../activation -I../manifest -I../crypto -I../verify fwu_counters.c ../verify/fwu_verify.c ../manifest/fwu_manifest.c ../crypto/fwu_crypto.c ../crypto/fwu_crypto_sim.c ../activation/fwu_activate.c ../journal/fwu_journal_ab.c ../journal/fwu_journal.c ../flash/fwu_flash.c ../flash/fwu_flash_sim.c test_counters.c -o test_counters && ./test_counters
 */

#include <stdio.h>
#include "fwu_counters.h"
#include "fwu_activate.h"
#include "fwu_crypto_sim.h"
#include "fwu_flash_sim.h"

/* 21 sayfa x 256, granül 8:
 *   0..7 uygulama | 8 manifest | 9..15 imaj (7 sayfa) | 16,17 aktivasyon journal | 18 geçici | 19,20 sayaç A/B
 * Sayaç sayfası 256 / 16 = 16 yuva: bölge dönüşü az kayıtla denenebilir. */
FWU_FLASH_SIM_DEFINE(cn, 21, 256, 8);

#define PAGE            256u
#define MANIFEST_OFFSET (8u * PAGE)
#define IMAGE_OFFSET    (9u * PAGE)
#define IMAGE_MAX       (7u * PAGE)
#define COUNTER_A       (19u * PAGE)
#define COUNTER_B       (20u * PAGE)
#define SLOTS_PER_PAGE  16u
#define PRODUCT         12345u

#define FAULT_WRITE     FWU_ERR_FLASH_WRITE              /* enjekte yazma hatası: sahte flash bunu hiç üretmez */
#define FAULT_READ      FWU_ERR_ERASE_ALIGNMENT          /* enjekte okuma hatası: bu akışta başka yoldan üretilemez */

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

/** Sayaç nesnesi hazır ve değerleri beklenen mi? */
static int counters_are(const fwu_counters_t *c, uint32_t image, uint16_t key)
{
    uint32_t i = 0xA5A5A5A5u;
    uint16_t k = 0x5A5Au;

    return (fwu_counters_get(c, &i, &k) == FWU_OK) && (i == image) && (k == key);
}

static fwu_return_e counters_open(fwu_counters_t *c, const fwu_flash_t *flash)
{
    return fwu_counters_init(c, flash, COUNTER_A, COUNTER_B, PAGE);
}

/** Flash'tan YENİDEN okunan değerler beklenen mi (yeniden başlatma simülasyonu)? */
static int persisted_is(const fwu_flash_t *flash, uint32_t image, uint16_t key)
{
    fwu_counters_t again;

    return (counters_open(&again, flash) == FWU_OK) && counters_are(&again, image, key);
}

/* ------------------------------------------------------------------ 1. init / get */

static void test_init_and_get(void)
{
    static cn_t sim;
    fwu_counters_t c;
    uint32_t image = 0x11111111u;
    uint16_t key = 0x2222u;

    printf("\n--- init / get: argumanlar, hazir degil kurali, yeni cihaz ---\n");
    (void)cn_init(&sim);

    expect("counters NULL", fwu_counters_init(NULL, &sim.base.flash, COUNTER_A, COUNTER_B, PAGE), FWU_ERR_INVALID_ARG);
    c.ready = true;                                     /* onceden "hazir" gorunen cop nesne */
    c.min_image_version = 77u;
    c.min_key_version = 7u;
    expect("flash NULL", fwu_counters_init(&c, NULL, COUNTER_A, COUNTER_B, PAGE), FWU_ERR_INVALID_ARG);
    check("... hata sonrasi nesne HAZIR DEGIL (eski cop deger kullanilmaz)", !c.ready);
    c.ready = true;
    expect("offset A hizasiz", fwu_counters_init(&c, &sim.base.flash, COUNTER_A + 8u, COUNTER_B, PAGE), FWU_ERR_INVALID_ARG);
    check("... hazir degil", !c.ready);
    c.ready = true;
    expect("uzunluk 0", fwu_counters_init(&c, &sim.base.flash, COUNTER_A, COUNTER_B, 0u), FWU_ERR_INVALID_ARG);
    check("... hazir degil", !c.ready);
    c.ready = true;
    expect("A ve B ayni bolge (cakisir)", fwu_counters_init(&c, &sim.base.flash, COUNTER_A, COUNTER_A, PAGE), FWU_ERR_INVALID_ARG);
    check("... hazir degil", !c.ready);
    c.ready = true;
    expect("bolge flash disinda", fwu_counters_init(&c, &sim.base.flash, 40u * PAGE, 41u * PAGE, PAGE), FWU_ERR_OUT_OF_RANGE);
    check("... hazir degil", !c.ready);

    expect("get: hazir degil -> BAD_STATE", fwu_counters_get(&c, &image, &key), FWU_ERR_BAD_STATE);
    check("... cikislar DEGISMEDI (guvenilmez sifir 'her surumu kabul et' dedirmesin)", image == 0x11111111u && key == 0x2222u);

    expect("yeni cihaz: init", counters_open(&c, &sim.base.flash), FWU_OK);
    check("... hazir ve degerler (0, 0)", c.ready && counters_are(&c, 0u, 0u));
    expect("get: counters NULL", fwu_counters_get(NULL, &image, &key), FWU_ERR_INVALID_ARG);
    expect("get: image NULL", fwu_counters_get(&c, NULL, &key), FWU_ERR_INVALID_ARG);
    expect("get: key NULL", fwu_counters_get(&c, &image, NULL), FWU_ERR_INVALID_ARG);

    expect("reload: NULL", fwu_counters_reload(NULL), FWU_ERR_INVALID_ARG);
    expect("raise: NULL", fwu_counters_raise(NULL, 1u, 1u), FWU_ERR_INVALID_ARG);
    expect("reload: normal", fwu_counters_reload(&c), FWU_OK);
    check("... degerler ayni (0, 0)", counters_are(&c, 0u, 0u));
}

/* ------------------------------------------------------------------ 2. raise: kalicilik, biçim, yalniz artar */

static int slot_is_erased(const cn_t *sim, uint32_t offset)
{
    for(uint32_t i = 0u; i < 16u; i++) {
        if((sim->base.memory[offset + i] & 0xFFu) != 0xFFu) {
            return 0;
        }
    }
    return 1;
}

static void test_raise(void)
{
    static cn_t sim;
    fwu_counters_t c;
    uint32_t ops;

    printf("\n--- raise: kalicilik, kayit bicimi, yalniz artar, degisiklik yoksa yazmaz ---\n");
    (void)cn_init(&sim);
    (void)counters_open(&c, &sim.base.flash);

    expect("yeni cihazda raise(0, 0)", fwu_counters_raise(&c, 0u, 0u), FWU_OK);
    check("... HIC flash islemi yok, ilk yuva silinmis kaldi", sim.base.operations_done == 0u && slot_is_erased(&sim, COUNTER_A));

    /* KAYIT BICIMI: elle yazilmis beklenti (fwu_journal.h: magic 0, state 2, flags 3, seq 4, arg 8) */
    expect("raise(0x01020304, 0x0A0B)", fwu_counters_raise(&c, 0x01020304u, 0x0A0Bu), FWU_OK);
    check("... bellek: magic 'J' 'R'", (sim.base.memory[COUNTER_A + 0u] & 0xFFu) == 0x4Au && (sim.base.memory[COUNTER_A + 1u] & 0xFFu) == 0x52u);
    check("... state = anahtar surumu YUKSEK bayti (0x0A), flags = DUSUK bayti (0x0B)",
          (sim.base.memory[COUNTER_A + 2u] & 0xFFu) == 0x0Au && (sim.base.memory[COUNTER_A + 3u] & 0xFFu) == 0x0Bu);
    check("... arg = imaj surumu, buyuk endian (01 02 03 04)",
          (sim.base.memory[COUNTER_A + 8u] & 0xFFu) == 0x01u && (sim.base.memory[COUNTER_A + 9u] & 0xFFu) == 0x02u &&
          (sim.base.memory[COUNTER_A + 10u] & 0xFFu) == 0x03u && (sim.base.memory[COUNTER_A + 11u] & 0xFFu) == 0x04u);
    check("... ikinci yuva hala silinmis (tek kayit yazildi)", slot_is_erased(&sim, COUNTER_A + 16u));
    check("... RAM degerleri", counters_are(&c, 0x01020304u, 0x0A0Bu));
    check("... yeniden baslatinca ayni degerler", persisted_is(&sim.base.flash, 0x01020304u, 0x0A0Bu));

    /* yeni, temiz bir cihazda ince noktalar */
    (void)cn_init(&sim);
    (void)counters_open(&c, &sim.base.flash);
    expect("raise(5, 1)", fwu_counters_raise(&c, 5u, 1u), FWU_OK);
    check("... degerler (5, 1), kalici", counters_are(&c, 5u, 1u) && persisted_is(&sim.base.flash, 5u, 1u));

    ops = sim.base.operations_done;
    expect("ayni deger raise(5, 1)", fwu_counters_raise(&c, 5u, 1u), FWU_OK);
    expect("daha dusuk raise(4, 0)", fwu_counters_raise(&c, 4u, 0u), FWU_OK);
    expect("karisik dusuk raise(0, 1)", fwu_counters_raise(&c, 0u, 1u), FWU_OK);
    check("... hicbiri flash'a yazmadi (aşinma yok)", sim.base.operations_done == ops);
    check("... degerler hala (5, 1)", counters_are(&c, 5u, 1u));

    expect("yalniz imaj yukselir raise(9, 0)", fwu_counters_raise(&c, 9u, 0u), FWU_OK);
    check("... (9, 1): anahtar DUSMEDI", counters_are(&c, 9u, 1u) && persisted_is(&sim.base.flash, 9u, 1u));
    expect("yalniz anahtar yukselir raise(0, 3)", fwu_counters_raise(&c, 0u, 3u), FWU_OK);
    check("... (9, 3): imaj DUSMEDI", counters_are(&c, 9u, 3u) && persisted_is(&sim.base.flash, 9u, 3u));
    expect("imaj yukselir, anahtar dusuk raise(10, 2)", fwu_counters_raise(&c, 10u, 2u), FWU_OK);
    check("... (10, 3): alan basina max", counters_are(&c, 10u, 3u) && persisted_is(&sim.base.flash, 10u, 3u));
    expect("anahtar yukselir, imaj dusuk raise(7, 4)", fwu_counters_raise(&c, 7u, 4u), FWU_OK);
    check("... (10, 4)", counters_are(&c, 10u, 4u) && persisted_is(&sim.base.flash, 10u, 4u));

    /* anahtar surumu bayt sinirlari: 0x00FF -> 0x0100 (bayt sirasi/tasma hatasi yakalanir) */
    expect("raise(10, 0x00FF)", fwu_counters_raise(&c, 10u, 0x00FFu), FWU_OK);
    check("... (10, 0x00FF) kalici", counters_are(&c, 10u, 0x00FFu) && persisted_is(&sim.base.flash, 10u, 0x00FFu));
    expect("raise(10, 0x0100)", fwu_counters_raise(&c, 10u, 0x0100u), FWU_OK);
    check("... (10, 0x0100) kalici", counters_are(&c, 10u, 0x0100u) && persisted_is(&sim.base.flash, 10u, 0x0100u));
    expect("raise(10, 0xFF00)", fwu_counters_raise(&c, 10u, 0xFF00u), FWU_OK);
    check("... (10, 0xFF00) kalici", counters_are(&c, 10u, 0xFF00u) && persisted_is(&sim.base.flash, 10u, 0xFF00u));

    /* tavan degerleri */
    expect("raise(0xFFFFFFFF, 0xFFFF)", fwu_counters_raise(&c, 0xFFFFFFFFu, 0xFFFFu), FWU_OK);
    check("... tavan (0xFFFFFFFF, 0xFFFF) kalici", counters_are(&c, 0xFFFFFFFFu, 0xFFFFu) && persisted_is(&sim.base.flash, 0xFFFFFFFFu, 0xFFFFu));
    ops = sim.base.operations_done;
    expect("tavandan sonra raise(0, 0)", fwu_counters_raise(&c, 0u, 0u), FWU_OK);
    expect("tavandan sonra raise(0xFFFFFFFF, 0xFFFF)", fwu_counters_raise(&c, 0xFFFFFFFFu, 0xFFFFu), FWU_OK);
    check("... tavanda hic yazma yok, deger ayni", sim.base.operations_done == ops && counters_are(&c, 0xFFFFFFFFu, 0xFFFFu));
}

/* ------------------------------------------------------------------ 3. rastgele dizi ve bolge donusu */

static void test_random_and_rotation(void)
{
    static cn_t sim;
    fwu_counters_t c;
    uint32_t seed = 12345u;
    uint32_t model_image = 0u;
    uint16_t model_key = 0u;
    int values_ok = 1;
    int no_write_ok = 1;
    int persisted_ok = 1;
    int monotonic_ok = 1;
    unsigned writes = 0u;

    printf("\n--- rastgele artis/dusus dizisi modele karsi; bolge donusunde degerler kaybolmaz ---\n");
    (void)cn_init(&sim);
    (void)counters_open(&c, &sim.base.flash);

    for(unsigned step = 0u; step < 300u; step++) {
        uint32_t want_image = model_image;
        uint16_t want_key = model_key;
        const uint32_t before_ops = sim.base.operations_done;
        uint32_t prev_image = model_image;
        uint16_t prev_key = model_key;
        uint32_t got_image = 0u;
        uint16_t got_key = 0u;
        const uint32_t action = (seed >> 28) & 3u;
        int changed;

        seed = (seed * 1664525u) + 1013904223u;
        switch(action) {
        case 0u:                                        /* dusuk degerler: hicbir sey degismez */
            want_image = model_image / 2u;
            want_key = (uint16_t)(model_key / 2u);
            break;
        case 1u:                                        /* yalniz imaj yukselir */
            want_image = model_image + 1u + ((seed >> 8) & 0xFFu);
            want_key = 0u;
            break;
        case 2u:                                        /* yalniz anahtar yukselir */
            want_image = 0u;
            want_key = (uint16_t)(model_key + 1u + ((seed >> 8) & 0x3u));
            break;
        default:                                        /* ikisi birden */
            want_image = model_image + 1u + ((seed >> 8) & 0xFFu);
            want_key = (uint16_t)(model_key + 1u + ((seed >> 4) & 0x3u));
            break;
        }
        changed = (want_image > model_image) || (want_key > model_key);
        if(want_image > model_image) {
            model_image = want_image;
        }
        if(want_key > model_key) {
            model_key = want_key;
        }

        if(fwu_counters_raise(&c, want_image, want_key) != FWU_OK) {
            values_ok = 0;
            break;
        }
        if(changed) {
            writes++;
        } else if(sim.base.operations_done != before_ops) {
            no_write_ok = 0;
        }
        if((fwu_counters_get(&c, &got_image, &got_key) != FWU_OK) || (got_image != model_image) || (got_key != model_key)) {
            values_ok = 0;
        }
        if((got_image < prev_image) || (got_key < prev_key)) {
            monotonic_ok = 0;
        }
        if(((step % 20u) == 19u) && !persisted_is(&sim.base.flash, model_image, model_key)) {
            persisted_ok = 0;
        }
    }
    check("300 raise: her adimda degerler = model (alan basina max)", values_ok);
    check("... hicbir adimda deger DUSMEDI", monotonic_ok);
    check("... degisiklik yoksa flash'a yazilmadi", no_write_ok);
    check("... her 20 adimda yeniden baslatinca flash'taki deger = model", persisted_ok);
    check("... yeterince yazma yapildi (bolge donusu icin > 2 sayfa dolusu)", writes > (2u * SLOTS_PER_PAGE));
    check("... iki sayac sayfasi da silindi (bolge donusu gerceklesti)",
          sim.base.erase_counts[COUNTER_A / PAGE] > 0u && sim.base.erase_counts[COUNTER_B / PAGE] > 0u);
    check("... diger bolgelere dokunulmadi (uygulama, manifest, imaj, aktivasyon journal, gecici)",
          sim.base.erase_counts[0] == 0u && sim.base.erase_counts[8] == 0u && sim.base.erase_counts[9] == 0u &&
          sim.base.erase_counts[16] == 0u && sim.base.erase_counts[18] == 0u);
}

/* ------------------------------------------------------------------ 4. guc kesintisi taramasi */

static void test_power_cut_sweep(void)
{
    static cn_t sim;
    int pair_ok = 1;
    int failsafe_ok = 1;
    int status_ok = 1;
    int retry_ok = 1;
    int reinit_matches_ok = 1;
    unsigned errors = 0u;
    unsigned successes = 0u;

    printf("\n--- guc kesintisi: her kayit konumu (iki bolge donusu dahil) x her kesinti noktasi ---\n");
    for(uint32_t prefill = 0u; prefill <= 36u; prefill++) {
        for(uint32_t cut = 0u; cut <= 4u; cut++) {
            fwu_counters_t c;
            fwu_counters_t reopened;
            uint32_t old_image = 0u;
            uint16_t old_key = 0u;
            uint32_t new_image;
            uint16_t new_key;
            fwu_return_e status;
            uint32_t got_image = 0u;
            uint16_t got_key = 0u;

            (void)cn_init(&sim);
            (void)counters_open(&c, &sim.base.flash);
            for(uint32_t i = 0u; i < prefill; i++) {
                const uint16_t k = (uint16_t)(i % 4u);
                (void)fwu_counters_raise(&c, i + 1u, k);
                old_image = i + 1u;
                if(k > old_key) {
                    old_key = k;
                }
            }
            new_image = old_image + 100u;
            new_key = (uint16_t)(old_key + 1u);

            fwu_flash_sim_reset(&sim.base);                     /* islem sayaci sifirlanir: kesinti raise'in kendi islemlerine gore */
            fwu_flash_sim_cut_power_after(&sim.base, cut);
            status = fwu_counters_raise(&c, new_image, new_key);
            if(status == FWU_OK) {
                successes++;
                if(!counters_are(&c, new_image, new_key)) {
                    pair_ok = 0;
                }
            } else {
                errors++;
                if(status != FWU_ERR_FLASH_POWER_LOSS) {
                    status_ok = 0;
                }
                /* flash okunamiyor: nesne 'hazir degil' olmali, sifir gibi davranmamali */
                if((fwu_counters_get(&c, &got_image, &got_key) != FWU_ERR_BAD_STATE) ||
                   (fwu_counters_raise(&c, new_image, new_key) != FWU_ERR_BAD_STATE)) {
                    failsafe_ok = 0;
                }
            }

            fwu_flash_sim_reset(&sim.base);                     /* guc geri geldi */
            if(fwu_counters_reload(&c) != FWU_OK) {
                pair_ok = 0;
            }
            got_image = 0u;
            got_key = 0u;
            if((fwu_counters_get(&c, &got_image, &got_key) != FWU_OK) ||
               !(((got_image == old_image) && (got_key == old_key)) || ((got_image == new_image) && (got_key == new_key)))) {
                pair_ok = 0;                                    /* ne eski ne yeni: alanlar ayrildi veya dustu */
            }
            if(status == FWU_OK && !((got_image == new_image) && (got_key == new_key))) {
                pair_ok = 0;
            }
            if((counters_open(&reopened, &sim.base.flash) != FWU_OK) || !counters_are(&reopened, got_image, got_key)) {
                reinit_matches_ok = 0;
            }

            if((fwu_counters_raise(&c, new_image, new_key) != FWU_OK) || !counters_are(&c, new_image, new_key) ||
               !persisted_is(&sim.base.flash, new_image, new_key)) {
                retry_ok = 0;
            }
        }
    }
    check("185 senaryo: sayac HER ZAMAN ya eski ya yeni ikili (alanlar ayrilmaz, dusmez)", pair_ok);
    check("... kesinti hatasi FLASH_POWER_LOSS olarak iletilir", status_ok);
    check("... hatada nesne 'hazir degil': get ve raise BAD_STATE", failsafe_ok);
    check("... reload ile yeniden acma ayni degeri verir", reinit_matches_ok);
    check("... yeniden deneme (raise) her durumda yeni degere tamamlar ve kalici yazar", retry_ok);
    /* Elle turetilmis dagilim. Temel islem = bir 8 baytlik granulun yazilmasi veya bir sayfanin silinmesi; kayit 16 bayt
     * = 2 granul. Normal raise = 2 islem. Bolge donusu olan raise = 5 islem (silme 1 + anlik goruntu 2 + kayit 2); donus
     * 2 konumda gerekir (16 ve 31 kayit dolu iken). 37 konum x 5 kesinti noktasi = 185 senaryo.
     *   kesinti 0 ve 1: 37 + 37 hata (en az 2 islem gerekir); kesinti 2, 3, 4: yalniz donusu olan 2 konumda hata (5 islem).
     *   hata = 74 + 3 x 2 = 80; basarili = 185 - 80 = 105. */
    check("... kesinti dagilimi: 80 hata, 105 basarili (donus konumlarinin 5 islemi de kesildi)", errors == 80u && successes == 105u);
}

/* ------------------------------------------------------------------ 5. flash hatalari */

typedef struct {
    fwu_flash_t          flash;
    fwu_flash_callback_t callback;
    cn_t                *sim;
    bool                 write_then_fail;       /* yazma gercekten yapilir ama FAULT_WRITE doner */
    bool                 fail_before_write;     /* yazma yapilmadan FAULT_WRITE doner */
    bool                 fail_reads;            /* tum okumalar FAULT_READ doner */
} faulty_t;

static fwu_return_e faulty_erase(void *context, uint32_t offset, uint32_t length)
{
    faulty_t *f = (faulty_t *)context;
    return fwu_flash_erase(&f->sim->base.flash, offset, length);
}

static fwu_return_e faulty_write(void *context, uint32_t offset, uint32_t length, const fwu_byte_t *data)
{
    faulty_t *f = (faulty_t *)context;
    fwu_return_e status;

    if(f->fail_before_write) {
        return FAULT_WRITE;
    }
    status = fwu_flash_write(&f->sim->base.flash, offset, length, data);
    if((status == FWU_OK) && f->write_then_fail) {
        return FAULT_WRITE;
    }
    return status;
}

static fwu_return_e faulty_read(void *context, uint32_t offset, uint32_t length, fwu_byte_t *data)
{
    faulty_t *f = (faulty_t *)context;
    if(f->fail_reads) {
        return FAULT_READ;
    }
    return fwu_flash_read(&f->sim->base.flash, offset, length, data);
}

static void faulty_init(faulty_t *f, cn_t *sim)
{
    f->callback.flash_erase = faulty_erase;
    f->callback.flash_write = faulty_write;
    f->callback.flash_read = faulty_read;
    f->flash.context = f;
    f->flash.callback = &f->callback;
    f->flash.geometry = sim->base.flash.geometry;
    f->sim = sim;
    f->write_then_fail = false;
    f->fail_before_write = false;
    f->fail_reads = false;
}

static void test_flash_faults(void)
{
    static cn_t sim;
    static faulty_t faulty;
    fwu_counters_t c;
    uint32_t image = 0u;
    uint16_t key = 0u;

    printf("\n--- flash hatalari: yazildi ama hata dondu, yazilmadi, okuma hatalari ---\n");
    (void)cn_init(&sim);
    faulty_init(&faulty, &sim);
    (void)counters_open(&c, &faulty.flash);
    (void)fwu_counters_raise(&c, 10u, 2u);

    faulty.write_then_fail = true;
    expect("yazma YAPILDI ama hata dondu", fwu_counters_raise(&c, 20u, 4u), FAULT_WRITE);
    check("... nesne hazir ve degerler FLASH'TAKI gercek (20, 4): hata donse de kayit gecerli",
          c.ready && counters_are(&c, 20u, 4u));
    check("... yeniden baslatinca da (20, 4)", persisted_is(&sim.base.flash, 20u, 4u));
    faulty.write_then_fail = false;

    faulty.fail_before_write = true;
    expect("yazma YAPILMADI, hata dondu", fwu_counters_raise(&c, 30u, 5u), FAULT_WRITE);
    check("... nesne hazir ve degerler hala (20, 4): flash'taki gercek", c.ready && counters_are(&c, 20u, 4u));
    check("... yeniden baslatinca (20, 4)", persisted_is(&sim.base.flash, 20u, 4u));
    faulty.fail_before_write = false;
    expect("hata giderilince ayni raise basarili", fwu_counters_raise(&c, 30u, 5u), FWU_OK);
    check("... (30, 5)", counters_are(&c, 30u, 5u) && persisted_is(&sim.base.flash, 30u, 5u));

    faulty.write_then_fail = true;
    faulty.fail_reads = true;
    expect("yazma yapildi, hata dondu VE flash okunamiyor", fwu_counters_raise(&c, 40u, 6u), FAULT_WRITE);
    check("... nesne HAZIR DEGIL (RAM'e guvenilmez)", !c.ready);
    expect("... get: BAD_STATE", fwu_counters_get(&c, &image, &key), FWU_ERR_BAD_STATE);
    expect("... raise: BAD_STATE", fwu_counters_raise(&c, 50u, 7u), FWU_ERR_BAD_STATE);
    expect("... reload da okuyamaz: okuma hatasi iletilir", fwu_counters_reload(&c), FAULT_READ);
    check("... hazir degil kalir", !c.ready);
    faulty.write_then_fail = false;
    faulty.fail_reads = false;
    expect("okuma duzelince reload", fwu_counters_reload(&c), FWU_OK);
    check("... degerler flash'taki gercek (40, 6)", counters_are(&c, 40u, 6u));

    faulty.fail_reads = true;
    expect("init: okuma hatasi iletilir", counters_open(&c, &faulty.flash), FAULT_READ);
    check("... hazir degil", !c.ready);
    faulty.fail_reads = false;
    expect("okuma duzelince init", counters_open(&c, &faulty.flash), FWU_OK);
    check("... (40, 6)", counters_are(&c, 40u, 6u));
}

/* ------------------------------------------------------------------ paket kurma (dogrulamadan BAGIMSIZ) */

static fwu_byte_t g_pk[3][32];
static fwu_verify_key_t g_keys[2];
static fwu_byte_t g_work[64];
static fwu_byte_t g_act_work[64];

static void init_keys(void)
{
    for(uint32_t i = 0u; i < 32u; i++) {
        g_pk[0][i] = (fwu_byte_t)(i + 1u);              /* anahtar 1 */
        g_pk[1][i] = (fwu_byte_t)(i + 101u);            /* anahtar 2 */
        g_pk[2][i] = (fwu_byte_t)(i + 201u);            /* SALDIRGANIN anahtari: cihazda YOK */
        g_keys[0].public_key[i] = g_pk[0][i];
        g_keys[1].public_key[i] = g_pk[1][i];
    }
    g_keys[0].id = 1u;
    g_keys[1].id = 2u;
}

static fwu_byte_t img_byte(uint32_t i)
{
    return (fwu_byte_t)((i * 13u + 29u) & 0xFFu);
}

static fwu_byte_t old_byte(uint32_t i)
{
    return (fwu_byte_t)((i * 7u + 3u) & 0xFFu);
}

typedef struct {
    uint32_t   product;
    uint32_t   version;
    uint32_t   size;
    fwu_byte_t key_id;
    uint32_t   key_version;
} pkg_t;

static pkg_t good_pkg(uint32_t version, uint32_t key_version)
{
    pkg_t p;
    p.product = PRODUCT;
    p.version = version;
    p.size = 700u;
    p.key_id = 1u;
    p.key_version = key_version;
    return p;
}

static void put_u32(fwu_byte_t *m, uint32_t offset, uint32_t value)
{
    m[offset] = (fwu_byte_t)((value >> 24) & 0xFFu);
    m[offset + 1u] = (fwu_byte_t)((value >> 16) & 0xFFu);
    m[offset + 2u] = (fwu_byte_t)((value >> 8) & 0xFFu);
    m[offset + 3u] = (fwu_byte_t)(value & 0xFFu);
}

/** Imaji (7 sayfa deseni) ve manifest'i flash belligine DOGRUDAN yazar; `signer` imzalayan anahtarin indeksi. */
static void write_package(cn_t *sim, const pkg_t *p, uint32_t signer)
{
    fwu_byte_t raw[138];

    for(uint32_t i = 0u; i < IMAGE_MAX; i++) {
        sim->base.memory[IMAGE_OFFSET + i] = img_byte(i);
    }
    raw[0] = 0x46u; raw[1] = 0x57u; raw[2] = 0x4Du; raw[3] = 0x46u;
    raw[4] = 0x01u;
    raw[5] = p->key_id;
    raw[6] = 0x00u; raw[7] = 0x00u;
    put_u32(raw, 8u, p->product);
    put_u32(raw, 12u, p->version);
    raw[16] = (fwu_byte_t)((p->key_version >> 8) & 0xFFu);
    raw[17] = (fwu_byte_t)(p->key_version & 0xFFu);
    put_u32(raw, 18u, p->size);
    fwu_crypto_sim_sha256(&sim->base.memory[IMAGE_OFFSET], p->size, &raw[22]);
    for(uint32_t i = 0u; i < 20u; i++) {
        raw[54u + i] = (fwu_byte_t)(0x30u + i);
    }
    fwu_crypto_sim_sign(g_pk[signer], raw, 74u, &raw[74]);
    for(uint32_t i = 0u; i < 138u; i++) {
        sim->base.memory[MANIFEST_OFFSET + i] = raw[i];
    }
}

static void make_verify_cfg(fwu_verify_config_t *v, fwu_crypto_sim_t *cs, uint32_t min_image, uint16_t min_key)
{
    v->crypto = &cs->crypto;
    v->manifest_offset = MANIFEST_OFFSET;
    v->image_offset = IMAGE_OFFSET;
    v->max_image_size = IMAGE_MAX;
    v->product_id = PRODUCT;
    v->min_image_version = min_image;
    v->min_key_version = min_key;
    v->keys = g_keys;
    v->key_count = 2u;
    v->require_signature = true;
    v->work = g_work;
    v->work_length = 64u;
}

/* ------------------------------------------------------------------ 6. onay kancasi */

static void test_confirm_hook(void)
{
    static cn_t sim;
    static fwu_crypto_sim_t cs;
    static faulty_t faulty;
    fwu_counters_t c;
    fwu_verify_config_t vc;
    fwu_counters_confirm_t ctx;
    pkg_t p;
    uint32_t ops;

    printf("\n--- onay kancasi: yalniz IMZALI manifest'ten yukseltir ---\n");
    init_keys();
    (void)cn_init(&sim);
    (void)counters_open(&c, &sim.base.flash);
    (void)fwu_crypto_sim_init(&cs);
    make_verify_cfg(&vc, &cs, 0u, 0u);
    ctx.counters = &c;
    ctx.verify = &vc;
    ctx.flash = &sim.base.flash;

    p = good_pkg(30u, 2u);
    write_package(&sim, &p, 0u);
    expect("gecerli imzali manifest (surum 30, anahtar surumu 2)", fwu_counters_on_confirmed(&ctx), FWU_OK);
    check("... sayac (30, 2) ve kalici", counters_are(&c, 30u, 2u) && persisted_is(&sim.base.flash, 30u, 2u));
    check("... imza dogrulandi, imajin HASH'i hesaplanmadi", cs.verify_calls == 1u && cs.begin_calls == 0u);

    ops = sim.base.operations_done;
    expect("kanca tekrar cagrilir", fwu_counters_on_confirmed(&ctx), FWU_OK);
    check("... flash'a yazilmadi, sayac ayni", sim.base.operations_done == ops && counters_are(&c, 30u, 2u));

    p = good_pkg(10u, 1u);
    write_package(&sim, &p, 0u);
    expect("daha dusuk surumlu gecerli manifest", fwu_counters_on_confirmed(&ctx), FWU_OK);
    check("... sayac DUSMEDI, yazma yok", sim.base.operations_done == ops && counters_are(&c, 30u, 2u));

    /* SALDIRI: surum 0xFFFFFFFF'li manifest (sayaci tavana cikarip tum guncellemeleri kilitlemek icin) */
    p = good_pkg(0xFFFFFFFFu, 0xFFFFu);
    write_package(&sim, &p, 2u);                        /* saldirganin anahtariyla imzali, kimlik 1 */
    expect("SALDIRI: saldirganin anahtariyla imzali manifest", fwu_counters_on_confirmed(&ctx), FWU_ERR_BAD_SIGNATURE);
    check("... sayac DEGISMEDI ve flash'a dokunulmadi", counters_are(&c, 30u, 2u) && sim.base.operations_done == ops);
    p = good_pkg(31u, 2u);
    write_package(&sim, &p, 0u);
    sim.base.memory[MANIFEST_OFFSET + 15u] = (fwu_byte_t)(sim.base.memory[MANIFEST_OFFSET + 15u] ^ 0x80u);   /* surum biti, imza eski */
    expect("SALDIRI: imzali alan degistirilmis (imza eski)", fwu_counters_on_confirmed(&ctx), FWU_ERR_BAD_SIGNATURE);
    check("... sayac DEGISMEDI", counters_are(&c, 30u, 2u) && sim.base.operations_done == ops);

    p = good_pkg(40u, 2u);
    p.product = PRODUCT + 1u;
    write_package(&sim, &p, 0u);
    expect("baska urunun manifest'i", fwu_counters_on_confirmed(&ctx), FWU_ERR_PRODUCT_MISMATCH);
    check("... sayac DEGISMEDI", counters_are(&c, 30u, 2u) && sim.base.operations_done == ops);
    p = good_pkg(40u, 2u);
    write_package(&sim, &p, 0u);
    sim.base.memory[MANIFEST_OFFSET] = (fwu_byte_t)(sim.base.memory[MANIFEST_OFFSET] ^ 0x01u);
    expect("manifest yapisi bozuk (magic)", fwu_counters_on_confirmed(&ctx), FWU_ERR_BAD_MANIFEST);
    check("... sayac DEGISMEDI", counters_are(&c, 30u, 2u) && sim.base.operations_done == ops);
    p = good_pkg(40u, 2u);
    p.key_id = 9u;
    write_package(&sim, &p, 0u);
    expect("bilinmeyen anahtar kimligi", fwu_counters_on_confirmed(&ctx), FWU_ERR_KEY_REJECTED);
    check("... sayac DEGISMEDI", counters_are(&c, 30u, 2u) && sim.base.operations_done == ops);

    /* iptal: dogrulama yapilandirmasindaki asgari anahtar surumu sayactan gelir */
    {
        uint32_t min_image = 0u;
        uint16_t min_key = 0u;
        (void)fwu_counters_get(&c, &min_image, &min_key);
        make_verify_cfg(&vc, &cs, min_image, min_key);
    }
    p = good_pkg(40u, 1u);
    write_package(&sim, &p, 0u);
    expect("iptal: anahtar surumu 1 < sayac 2", fwu_counters_on_confirmed(&ctx), FWU_ERR_KEY_REJECTED);
    check("... sayac DEGISMEDI", counters_are(&c, 30u, 2u) && sim.base.operations_done == ops);
    p = good_pkg(40u, 3u);
    write_package(&sim, &p, 0u);
    expect("yeni anahtar surumu 3", fwu_counters_on_confirmed(&ctx), FWU_OK);
    check("... sayac (40, 3)", counters_are(&c, 40u, 3u) && persisted_is(&sim.base.flash, 40u, 3u));

    /* kripto hatasi: yapilamadi */
    p = good_pkg(50u, 3u);
    write_package(&sim, &p, 0u);
    (void)fwu_crypto_sim_init(&cs);
    cs.fail_verify = FWU_ERR_CRYPTO_FAILURE;
    ops = sim.base.operations_done;
    expect("kripto: imza dogrulama yapilamadi", fwu_counters_on_confirmed(&ctx), FWU_ERR_CRYPTO_FAILURE);
    check("... sayac DEGISMEDI", counters_are(&c, 40u, 3u) && sim.base.operations_done == ops);
    cs.fail_verify = FWU_OK;
    expect("kripto duzelince ayni manifest", fwu_counters_on_confirmed(&ctx), FWU_OK);
    check("... sayac (50, 3)", counters_are(&c, 50u, 3u));

    /* sayac flash'i yazma hatasi verirse hata iletilir */
    faulty_init(&faulty, &sim);
    (void)counters_open(&c, &faulty.flash);
    ctx.counters = &c;
    p = good_pkg(60u, 3u);
    write_package(&sim, &p, 0u);
    faulty.fail_before_write = true;
    expect("sayac yazimi basarisiz: hata iletilir", fwu_counters_on_confirmed(&ctx), FAULT_WRITE);
    check("... sayac hala (50, 3)", counters_are(&c, 50u, 3u));
    faulty.fail_before_write = false;
    expect("hata giderilince kanca tamamlar", fwu_counters_on_confirmed(&ctx), FWU_OK);
    check("... sayac (60, 3)", counters_are(&c, 60u, 3u) && persisted_is(&sim.base.flash, 60u, 3u));

    /* hazir olmayan sayac */
    faulty.fail_reads = true;
    (void)fwu_counters_reload(&c);
    faulty.fail_reads = false;
    p = good_pkg(70u, 3u);
    write_package(&sim, &p, 0u);
    expect("sayac hazir degil: kanca BAD_STATE", fwu_counters_on_confirmed(&ctx), FWU_ERR_BAD_STATE);
    (void)fwu_counters_reload(&c);
    check("... reload sonrasi sayac hala (60, 3)", counters_are(&c, 60u, 3u));

    /* argumanlar */
    expect("context NULL", fwu_counters_on_confirmed(NULL), FWU_ERR_INVALID_ARG);
    ctx.counters = NULL;
    expect("context.counters NULL", fwu_counters_on_confirmed(&ctx), FWU_ERR_INVALID_ARG);
    ctx.counters = &c;
    ctx.verify = NULL;
    expect("context.verify NULL", fwu_counters_on_confirmed(&ctx), FWU_ERR_INVALID_ARG);
    ctx.verify = &vc;
    ctx.flash = NULL;
    expect("context.flash NULL", fwu_counters_on_confirmed(&ctx), FWU_ERR_INVALID_ARG);

    /* DEV kipi: imza kontrol edilmez (belgeli), urun yine denetlenir */
    (void)cn_init(&sim);
    (void)counters_open(&c, &sim.base.flash);
    ctx.flash = &sim.base.flash;
    vc.require_signature = false;
    p = good_pkg(80u, 4u);
    write_package(&sim, &p, 2u);
    expect("DEV: saldirganin anahtariyla imzali manifest kabul (imza kontrol edilmez)", fwu_counters_on_confirmed(&ctx), FWU_OK);
    check("... sayac (80, 4)", counters_are(&c, 80u, 4u));
    p = good_pkg(90u, 4u);
    p.product = PRODUCT + 1u;
    write_package(&sim, &p, 0u);
    expect("DEV: baska urun yine reddedilir", fwu_counters_on_confirmed(&ctx), FWU_ERR_PRODUCT_MISMATCH);
    check("... sayac (80, 4)", counters_are(&c, 80u, 4u));
}

/* ------------------------------------------------------------------ 7. aktivasyonla uctan uca (SWAP) */

static void act_config(fwu_activate_config_t *a, cn_t *sim, fwu_verify_config_t *vc, fwu_counters_confirm_t *ctx)
{
    a->flash = &sim->base.flash;
    a->mode = FWU_ACTIVATE_MODE_SWAP;
    a->app_offset = 0u;
    a->staging_offset = IMAGE_OFFSET;
    a->region_length = IMAGE_MAX;
    a->journal_a_offset = 16u * PAGE;
    a->journal_b_offset = 17u * PAGE;
    a->journal_length = PAGE;
    a->scratch_offset = 18u * PAGE;
    a->scratch_pages = 1u;
    a->work = g_act_work;
    a->work_length = 64u;
    a->max_trial_boots = 2u;
    a->require_verify = true;
    a->verify = fwu_verify_activation_hook;
    a->verify_context = vc;
    a->on_confirmed = fwu_counters_on_confirmed;
    a->on_confirmed_context = ctx;
}

static void fill_old_app(cn_t *sim)
{
    for(uint32_t i = 0u; i < 8u * PAGE; i++) {
        sim->base.memory[i] = old_byte(i);
    }
}

static int app_is_new(const cn_t *sim)
{
    for(uint32_t i = 0u; i < 700u; i++) {
        if((sim->base.memory[i] & 0xFFu) != img_byte(i)) {
            return 0;
        }
    }
    return 1;
}

static int app_is_old(const cn_t *sim)
{
    for(uint32_t i = 0u; i < 8u * PAGE; i++) {
        if((sim->base.memory[i] & 0xFFu) != old_byte(i)) {
            return 0;
        }
    }
    return 1;
}

/** Bootloader gibi: yeniden baslat, journal'i ac, bekleyen isi calistir. */
static fwu_return_e boot(fwu_activate_t *act, const fwu_activate_config_t *a, cn_t *sim, fwu_activate_outcome_e *outcome)
{
    fwu_flash_sim_reset(&sim->base);
    if(fwu_activate_init(act, a) != FWU_OK) {
        return FWU_ERR_BAD_STATE;
    }
    return fwu_activate_run(act, outcome);
}

static void test_end_to_end(void)
{
    static cn_t sim;
    static fwu_crypto_sim_t cs;
    static fwu_activate_t act;
    fwu_counters_t c;
    fwu_verify_config_t vc;
    fwu_counters_confirm_t ctx;
    fwu_activate_config_t ac;
    fwu_activate_outcome_e outcome = FWU_ACT_OUTCOME_NONE;
    pkg_t p;
    uint32_t min_image = 0u;
    uint16_t min_key = 0u;

    printf("\n--- aktivasyonla uctan uca (SWAP, on_confirmed = fwu_counters_on_confirmed) ---\n");
    init_keys();
    (void)fwu_crypto_sim_init(&cs);
    (void)cn_init(&sim);
    fill_old_app(&sim);
    (void)counters_open(&c, &sim.base.flash);
    make_verify_cfg(&vc, &cs, 0u, 0u);
    ctx.counters = &c;
    ctx.verify = &vc;
    ctx.flash = &sim.base.flash;
    act_config(&ac, &sim, &vc, &ctx);

    /* 1. surum 30 indirildi, commit, bootloader: deneme onyuklemesi */
    p = good_pkg(30u, 2u);
    write_package(&sim, &p, 0u);
    (void)fwu_activate_init(&act, &ac);
    expect("commit (3 sayfa)", fwu_activate_commit(&act, 3u), FWU_OK);
    expect("bootloader: run", boot(&act, &ac, &sim, &outcome), FWU_OK);
    check("... TRIAL, uygulama = yeni imaj", outcome == FWU_ACT_OUTCOME_TRIAL && app_is_new(&sim));
    check("... ONAYDAN ONCE sayac ARTMADI (0, 0): onaysiz geri donulse eski imaj reddedilmesin",
          persisted_is(&sim.base.flash, 0u, 0u));

    /* 2. yeni uygulama acilir ve kendini onaylar */
    (void)counters_open(&c, &sim.base.flash);
    (void)fwu_activate_init(&act, &ac);
    expect("yeni uygulama: confirm", fwu_activate_confirm(&act), FWU_OK);
    check("... sayac (30, 2) ve kalici", counters_are(&c, 30u, 2u) && persisted_is(&sim.base.flash, 30u, 2u));
    {
        const uint32_t before = sim.base.operations_done;
        expect("confirm tekrar cagrilir (kanca yeniden calisir)", fwu_activate_confirm(&act), FWU_OK);
        check("... sayac ayni, flash'a yazilmadi", counters_are(&c, 30u, 2u) && sim.base.operations_done == before);
    }

    /* 3. sayacla doguluyor: eski surumler reddedilir, esit ve yeni kabul */
    (void)fwu_counters_get(&c, &min_image, &min_key);
    make_verify_cfg(&vc, &cs, min_image, min_key);
    p = good_pkg(29u, 2u);
    write_package(&sim, &p, 0u);
    expect("dogrulama: surum 29 < sayac 30", fwu_verify_image(&vc, &sim.base.flash, NULL), FWU_ERR_VERSION_REJECTED);
    p = good_pkg(30u, 2u);
    write_package(&sim, &p, 0u);
    expect("dogrulama: surum 30 (esit kabul)", fwu_verify_image(&vc, &sim.base.flash, NULL), FWU_OK);
    p = good_pkg(31u, 1u);
    write_package(&sim, &p, 0u);
    expect("dogrulama: surum 31 ama anahtar surumu 1 < sayac 2 (iptal)", fwu_verify_image(&vc, &sim.base.flash, NULL), FWU_ERR_KEY_REJECTED);

    /* 4. surum 31 deneme onyuklemesinde reddedilir (geri donus): sayac artmaz, eski imaj calisir */
    (void)cn_init(&sim);
    fill_old_app(&sim);
    (void)counters_open(&c, &sim.base.flash);
    (void)fwu_counters_raise(&c, 30u, 2u);
    (void)fwu_counters_get(&c, &min_image, &min_key);
    make_verify_cfg(&vc, &cs, min_image, min_key);
    p = good_pkg(31u, 2u);
    write_package(&sim, &p, 0u);
    (void)fwu_activate_init(&act, &ac);
    expect("surum 31: commit", fwu_activate_commit(&act, 3u), FWU_OK);
    expect("surum 31: run", boot(&act, &ac, &sim, &outcome), FWU_OK);
    check("... TRIAL, uygulama = yeni imaj", outcome == FWU_ACT_OUTCOME_TRIAL && app_is_new(&sim));
    expect("yeni uygulama beklenmedik: geri donus ister", fwu_activate_request_revert(&act), FWU_OK);
    expect("bootloader: geri donus", boot(&act, &ac, &sim, &outcome), FWU_OK);
    check("... REVERTED, uygulama = ESKI imaj (sayac yuzunden reddedilmedi)", outcome == FWU_ACT_OUTCOME_REVERTED && app_is_old(&sim));
    check("... sayac hala (30, 2): geri donusle ARTMADI", persisted_is(&sim.base.flash, 30u, 2u));
    p = good_pkg(30u, 2u);
    write_package(&sim, &p, 0u);
    expect("... surum 30 (mevcut) sonraki indirmede yine kabul", fwu_verify_image(&vc, &sim.base.flash, NULL), FWU_OK);

    /* 5. onay ile sayac yazimi arasinda guc kesilir: yeniden confirm tamamlar */
    (void)cn_init(&sim);
    fill_old_app(&sim);
    (void)counters_open(&c, &sim.base.flash);
    make_verify_cfg(&vc, &cs, 0u, 0u);
    p = good_pkg(32u, 3u);
    write_package(&sim, &p, 0u);
    (void)fwu_activate_init(&act, &ac);
    (void)fwu_activate_commit(&act, 3u);
    (void)boot(&act, &ac, &sim, &outcome);
    check("surum 32: TRIAL", outcome == FWU_ACT_OUTCOME_TRIAL);
    (void)counters_open(&c, &sim.base.flash);
    (void)fwu_activate_init(&act, &ac);
    fwu_flash_sim_reset(&sim.base);                     /* islem sayaci sifirlanir */
    fwu_flash_sim_cut_power_after(&sim.base, 2u);       /* 1-2. islem: CONFIRMED kaydi (2 granul); 3. islem (sayac kaydinin ilk granulu) kesilir */
    expect("confirm: CONFIRMED yazildi, sayac yazimi sirasinda guc kesildi", fwu_activate_confirm(&act), FWU_ERR_FLASH_POWER_LOSS);
    fwu_flash_sim_reset(&sim.base);
    check("... sayac geride kaldi (0, 0): yarim yazilan kayit gecersiz, guvenli yon", persisted_is(&sim.base.flash, 0u, 0u));
    (void)counters_open(&c, &sim.base.flash);
    (void)fwu_activate_init(&act, &ac);
    check("... aktivasyon: faz IDLE, son durum CONFIRMED", act.phase == FWU_ACT_PHASE_IDLE && act.last_state == FWU_ACT_STATE_CONFIRMED);
    expect("yeniden acilista confirm tekrar: kanca tamamlar", fwu_activate_confirm(&act), FWU_OK);
    check("... sayac (32, 3) ve kalici", counters_are(&c, 32u, 3u) && persisted_is(&sim.base.flash, 32u, 3u));
}

int main(void)
{
    test_init_and_get();
    test_raise();
    test_random_and_rotation();
    test_power_cut_sweep();
    test_flash_faults();
    test_confirm_hook();
    test_end_to_end();

    printf("\nResult: %s (%d failed)\n", g_failures == 0 ? "ALL TESTS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
