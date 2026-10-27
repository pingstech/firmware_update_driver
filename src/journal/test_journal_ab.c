/**
 * @file    test_journal_ab.c
 * @brief   fwu_journal_ab için kendi kendini kontrol eden test (sahte flash üzerinde).
 *
 * Beklentiler modülün kendi kodundan DEĞİL, elle yazılmış bir modelden ve elle sayılmış işlem
 * sayılarından gelir. Kapsam:
 *   1. Argümanlar ve bölge doğrulaması (çakışma, hiza, taşma, 2 yuvadan az yer, granül).
 *   2. Temel: boş journal, ekleme, sorgu.
 *   3. Dönüş modeli: 40 kayıt boyunca (12 bölge değişimi) her adımda son kayıt ve sıra numarası
 *      modelle aynı; her adımda yeniden başlatma (init) aynı durumu verir; aşınma dengeli.
 *   4. Sıra numarası 32 bit sarması: bölge değişimi sarmanın üzerinden; aktif bölge seçimi.
 *   5. Çöp/eşitlik: yalnızca geçersiz kayıtla dolu bölge, eşit seq.
 *   6. TEK bir bölge değişiminin HER kesinti noktasında (5 temel işlem) beklenen durum.
 *   7. 12 kayıt boyunca HER kesinti noktası: kayıt kaybı yok, kayıt devam ediyor.
 *   8. Güç yokken init/scan/append hatası iletilir.
 *
 * Derleme (journal klasöründen):
 *   gcc -Wall -Wextra -Wpedantic -Wconversion -std=c99 -I. -I.. -I../error -I../bytes -I../crc -I../flash fwu_journal_ab.c fwu_journal.c ../flash/fwu_flash.c ../flash/fwu_flash_sim.c test_journal_ab.c -o test_journal_ab && ./test_journal_ab
 */

#include <stdio.h>
#include "fwu_journal_ab.h"
#include "fwu_flash_sim.h"

FWU_FLASH_SIM_DEFINE(ab_sim, 8, 64, 8);        /* 8 sayfa x 64 bayt; bölge = 1 sayfa = 4 yuva */
FWU_FLASH_SIM_DEFINE(ab_tiny, 4, 16, 8);       /* sayfa 16 bayt = 1 yuva (az) */
FWU_FLASH_SIM_DEFINE(ab_min, 4, 32, 8);        /* sayfa 32 bayt = 2 yuva (asgari) */
FWU_FLASH_SIM_DEFINE(ab_coarse, 4, 64, 32);    /* granül 32: kayıt (16) granülün katı değil */

#define OFFSET_A        128u        /* sayfa 2 */
#define OFFSET_B        320u        /* sayfa 5 */
#define PAGE_A          2u
#define PAGE_B          5u
#define REGION          64u
#define SLOTS           4u

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

/* ------------------------------------------------------------------ bağımsız yardımcılar */

static uint16_t ref_crc16(const fwu_byte_t *data, size_t length)
{
    uint16_t crc = 0xFFFFu;
    for(size_t i = 0u; i < length; i++) {
        crc = (uint16_t)(crc ^ (uint16_t)(data[i] & 0xFFu));
        for(unsigned bit = 0u; bit < 8u; bit++) {
            crc = ((crc & 1u) != 0u) ? (uint16_t)((crc >> 1) ^ 0xA001u) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

/** Sahte flash belleğine DOĞRUDAN kayıt yazar (flash arayüzünü atlar). */
static void forge(fwu_flash_sim_t *sim, uint32_t region_offset, uint32_t slot,
                  fwu_byte_t state, uint32_t seq, uint32_t arg, int corrupt_crc)
{
    fwu_byte_t raw[16];
    uint16_t crc;
    raw[0] = 0x4Au;
    raw[1] = 0x52u;
    raw[2] = state;
    raw[3] = 0u;
    raw[4] = (fwu_byte_t)((seq >> 24) & 0xFFu);
    raw[5] = (fwu_byte_t)((seq >> 16) & 0xFFu);
    raw[6] = (fwu_byte_t)((seq >> 8) & 0xFFu);
    raw[7] = (fwu_byte_t)(seq & 0xFFu);
    raw[8] = (fwu_byte_t)((arg >> 24) & 0xFFu);
    raw[9] = (fwu_byte_t)((arg >> 16) & 0xFFu);
    raw[10] = (fwu_byte_t)((arg >> 8) & 0xFFu);
    raw[11] = (fwu_byte_t)(arg & 0xFFu);
    raw[12] = 0u;
    raw[13] = 0u;
    crc = ref_crc16(raw, 14u);
    if(corrupt_crc) {
        crc = (uint16_t)(crc ^ 0x0001u);
    }
    raw[14] = (fwu_byte_t)((crc >> 8) & 0xFFu);
    raw[15] = (fwu_byte_t)(crc & 0xFFu);
    for(uint32_t i = 0u; i < 16u; i++) {
        sim->memory[region_offset + (slot * 16u) + i] = raw[i];
    }
}

/** Modeldeki i. kaydın içeriği (journal'dan BAĞIMSIZ). */
static fwu_byte_t model_state(uint32_t i) { return (fwu_byte_t)(0x20u + i); }
static fwu_byte_t model_flags(uint32_t i) { return (fwu_byte_t)(i & 0x7Fu); }
static uint32_t   model_arg(uint32_t i)   { return (0x100u * i) + 3u; }

/** i. append'den sonraki sıra numarası: ilk kayıt 1; her bölge değişimi bir anlık görüntü (bir seq) harcar. */
static uint32_t model_seq(uint32_t i)
{
    uint32_t used = 0u;
    uint32_t seq = 0u;
    for(uint32_t k = 1u; k <= i; k++) {
        if(used == SLOTS) {
            used = 1u;
            seq++;
        }
        used++;
        seq++;
    }
    return seq;
}

/** i. append'den sonraki toplam bölge değişimi sayısı. */
static uint32_t model_rotations(uint32_t i)
{
    uint32_t used = 0u;
    uint32_t rotations = 0u;
    for(uint32_t k = 1u; k <= i; k++) {
        if(used == SLOTS) {
            used = 1u;
            rotations++;
        }
        used++;
    }
    return rotations;
}

static fwu_return_e append_model(fwu_journal_ab_t *j, uint32_t i)
{
    return fwu_journal_ab_append(j, model_state(i), model_flags(i), model_arg(i));
}

/** Son kaydın YÜKÜ i. kayıtla aynı mı (seq hariç)? */
static int payload_is_model(const fwu_journal_record_t *r, uint32_t i)
{
    return (r->state == model_state(i)) && (r->flags == model_flags(i)) && (r->arg == model_arg(i));
}

static void fresh(ab_sim_t *sim, fwu_journal_ab_t *j)
{
    (void)ab_sim_init(sim);
    (void)fwu_journal_ab_init(j, &sim->base.flash, OFFSET_A, OFFSET_B, REGION);
}

/* ------------------------------------------------------------------ testler */

static void test_arguments(void)
{
    static ab_sim_t sim;
    static ab_tiny_t tiny;
    static ab_min_t minimal;
    static ab_coarse_t coarse;
    fwu_journal_ab_t j;
    fwu_journal_record_t last;

    printf("\n--- argumanlar ve bolge dogrulamasi ---\n");
    (void)ab_sim_init(&sim);
    (void)ab_tiny_init(&tiny);
    (void)ab_min_init(&minimal);
    (void)ab_coarse_init(&coarse);

    expect("init: journal NULL", fwu_journal_ab_init(NULL, &sim.base.flash, OFFSET_A, OFFSET_B, REGION), FWU_ERR_INVALID_ARG);
    expect("init: flash NULL", fwu_journal_ab_init(&j, NULL, OFFSET_A, OFFSET_B, REGION), FWU_ERR_INVALID_ARG);
    expect("init: gecerli", fwu_journal_ab_init(&j, &sim.base.flash, OFFSET_A, OFFSET_B, REGION), FWU_OK);
    expect("init: A ve B ayni bolge", fwu_journal_ab_init(&j, &sim.base.flash, OFFSET_A, OFFSET_A, REGION), FWU_ERR_INVALID_ARG);
    expect("init: kismen cakisan bolgeler (2 sayfa, 1 sayfa kayik)",
           fwu_journal_ab_init(&j, &sim.base.flash, 128u, 192u, 128u), FWU_ERR_INVALID_ARG);
    expect("init: B, A'nin icinde baslar (B > A)",
           fwu_journal_ab_init(&j, &sim.base.flash, 128u, 192u, 128u), FWU_ERR_INVALID_ARG);
    expect("init: A, B'nin icinde biter (A > B)",
           fwu_journal_ab_init(&j, &sim.base.flash, 192u, 128u, 128u), FWU_ERR_INVALID_ARG);
    expect("init: bitisik (cakismayan) bolgeler gecerli",
           fwu_journal_ab_init(&j, &sim.base.flash, 128u, 192u, 64u), FWU_OK);
    expect("init: B, A'dan once ve bitisik gecerli",
           fwu_journal_ab_init(&j, &sim.base.flash, 192u, 128u, 64u), FWU_OK);
    expect("init: A sayfa hizasiz", fwu_journal_ab_init(&j, &sim.base.flash, 130u, OFFSET_B, REGION), FWU_ERR_INVALID_ARG);
    expect("init: B sayfa hizasiz", fwu_journal_ab_init(&j, &sim.base.flash, OFFSET_A, 322u, REGION), FWU_ERR_INVALID_ARG);
    expect("init: uzunluk 0", fwu_journal_ab_init(&j, &sim.base.flash, OFFSET_A, OFFSET_B, 0u), FWU_ERR_INVALID_ARG);
    expect("init: uzunluk sayfa katı degil", fwu_journal_ab_init(&j, &sim.base.flash, OFFSET_A, OFFSET_B, 40u), FWU_ERR_INVALID_ARG);
    expect("init: A flash disinda", fwu_journal_ab_init(&j, &sim.base.flash, 512u, OFFSET_B, REGION), FWU_ERR_OUT_OF_RANGE);
    expect("init: B flash disinda", fwu_journal_ab_init(&j, &sim.base.flash, OFFSET_A, 512u, REGION), FWU_ERR_OUT_OF_RANGE);
    expect("init: bolge flash sonunu asar", fwu_journal_ab_init(&j, &sim.base.flash, OFFSET_A, 448u, 128u), FWU_ERR_OUT_OF_RANGE);
    expect("init: 1 yuvalik bolge (anlik goruntu + kayit icin az)",
           fwu_journal_ab_init(&j, &tiny.base.flash, 0u, 16u, 16u), FWU_ERR_INVALID_ARG);
    expect("init: 2 yuvalik bolge asgari olarak gecerli",
           fwu_journal_ab_init(&j, &minimal.base.flash, 0u, 32u, 32u), FWU_OK);
    expect("init: kayit granulun kati degil", fwu_journal_ab_init(&j, &coarse.base.flash, 0u, 64u, 64u), FWU_ERR_FLASH_GEOMETRY);

    expect("scan: NULL", fwu_journal_ab_scan(NULL), FWU_ERR_INVALID_ARG);
    expect("append: NULL", fwu_journal_ab_append(NULL, 1u, 0u, 0u), FWU_ERR_INVALID_ARG);
    check("get_last: NULL journal -> false", !fwu_journal_ab_get_last(NULL, &last));
    check("get_last: NULL journal, NULL out -> false", !fwu_journal_ab_get_last(NULL, NULL));
}

static void test_basic(void)
{
    static ab_sim_t sim;
    fwu_journal_ab_t j;
    fwu_journal_record_t last = { 0xEEu, 0xEEu, 0xEEEEu, 0xEEEEu };

    printf("\n--- temel islemler ---\n");
    fresh(&sim, &j);
    check("bos journal: kayit yok", !fwu_journal_ab_get_last(&j, &last));
    check("bos journal: out degismez", last.state == 0xEEu && last.seq == 0xEEEEu);
    check("bos journal: aktif A", j.active == 0u);

    expect("ilk ekleme", append_model(&j, 1u), FWU_OK);
    check("ilk ekleme: varlik sorgusu (out NULL) -> true", fwu_journal_ab_get_last(&j, NULL));
    check("ilk ekleme: kayit dolduruldu", fwu_journal_ab_get_last(&j, &last));
    check("ilk ekleme: yuk modele esit, seq 1", payload_is_model(&last, 1u) && last.seq == 1u);
    check("ilk ekleme: A'ya yazildi", j.active == 0u);
}

static void test_rotation_model(void)
{
    static ab_sim_t sim;
    fwu_journal_ab_t j;
    fwu_journal_ab_t reboot;
    fwu_journal_record_t last;
    uint32_t erases_a = 0u;
    uint32_t erases_b = 0u;
    int all_ok = 1;
    int reboot_ok = 1;
    int wear_ok = 1;

    printf("\n--- donus modeli: 40 kayit, 12 bolge degisimi ---\n");
    fresh(&sim, &j);
    for(uint32_t i = 1u; i <= 40u; i++) {
        if(append_model(&j, i) != FWU_OK) {
            all_ok = 0;
        }
        if(!fwu_journal_ab_get_last(&j, &last) || !payload_is_model(&last, i) || (last.seq != model_seq(i))) {
            all_ok = 0;
        }
        /* her adimda "yeniden baslatma": flash'tan sifirdan okunan durum ayni olmali */
        if((fwu_journal_ab_init(&reboot, &sim.base.flash, OFFSET_A, OFFSET_B, REGION) != FWU_OK) ||
           !fwu_journal_ab_get_last(&reboot, &last) || !payload_is_model(&last, i) || (last.seq != model_seq(i))) {
            reboot_ok = 0;
        }
        (void)fwu_flash_sim_get_erase_count(&sim.base, PAGE_A, &erases_a);
        (void)fwu_flash_sim_get_erase_count(&sim.base, PAGE_B, &erases_b);
        if(((erases_a + erases_b) != model_rotations(i)) ||
           ((erases_a > erases_b) ? ((erases_a - erases_b) > 1u) : ((erases_b - erases_a) > 1u))) {
            wear_ok = 0;
        }
    }
    check("her adimda: ekleme OK, son kayit ve seq modelle ayni", all_ok);
    check("her adimda: yeniden baslatma ayni durumu verir", reboot_ok);
    check("her adimda: silme sayisi = bolge degisimi sayisi, A/B dengeli (fark <= 1)", wear_ok);
    check("model (elle): 40 kayitta 12 bolge degisimi (5., 8., 11., ... 38. kayitta)", model_rotations(40u) == 12u);
    check("40 kayit sonunda seq 52 (40 kayit + 12 anlik goruntu)", last.seq == 52u);
    check("4. kayit A'nin son yuvasi, 5. kayit B'ye gecer (aktif B)", model_rotations(4u) == 0u && model_rotations(5u) == 1u);
}

static void test_active_selection(void)
{
    static ab_sim_t sim;
    fwu_journal_ab_t j;
    fwu_journal_record_t last;

    printf("\n--- aktif bolge secimi, seq sarmasi, cop ve esitlik ---\n");

    /* A: seq 0xFFFFFFFD..0x00000000 (sarma), B bos: bolge degisimi sarmanin uzerinden gecer. */
    (void)ab_sim_init(&sim);
    forge(&sim.base, OFFSET_A, 0u, 0x31u, 0xFFFFFFFDu, 11u, 0);
    forge(&sim.base, OFFSET_A, 1u, 0x32u, 0xFFFFFFFEu, 12u, 0);
    forge(&sim.base, OFFSET_A, 2u, 0x33u, 0xFFFFFFFFu, 13u, 0);
    forge(&sim.base, OFFSET_A, 3u, 0x34u, 0x00000000u, 14u, 0);
    expect("sarma: init", fwu_journal_ab_init(&j, &sim.base.flash, OFFSET_A, OFFSET_B, REGION), FWU_OK);
    check("sarma: son kayit (seq 0) A'da", fwu_journal_ab_get_last(&j, &last) && last.state == 0x34u && last.seq == 0u && j.active == 0u);
    expect("sarma: ekleme bolge degistirir", fwu_journal_ab_append(&j, 0x35u, 0u, 15u), FWU_OK);
    check("sarma: yeni kayit seq 2 (anlik goruntu 1)", fwu_journal_ab_get_last(&j, &last) && last.state == 0x35u && last.seq == 2u && j.active == 1u);
    expect("sarma: yeniden tarama", fwu_journal_ab_scan(&j), FWU_OK);
    check("sarma: tarama sonrasi B aktif (seq 2, A'nin seq 0'indan yeni)", j.active == 1u && fwu_journal_ab_get_last(&j, &last) && last.seq == 2u);

    /* A son seq 0xFFFFFFFF, B son seq 1: B yeni (fark kucuk), sarma karsilastirmasi */
    (void)ab_sim_init(&sim);
    forge(&sim.base, OFFSET_A, 0u, 0x41u, 0xFFFFFFFFu, 0u, 0);
    forge(&sim.base, OFFSET_B, 0u, 0x42u, 0x00000001u, 0u, 0);
    (void)fwu_journal_ab_init(&j, &sim.base.flash, OFFSET_A, OFFSET_B, REGION);
    check("sarma: A seq 0xFFFFFFFF, B seq 1 -> B aktif", j.active == 1u && fwu_journal_ab_get_last(&j, &last) && last.state == 0x42u);

    /* A son seq 1, B son seq 0xFFFFFFFF: A yeni */
    (void)ab_sim_init(&sim);
    forge(&sim.base, OFFSET_A, 0u, 0x43u, 0x00000001u, 0u, 0);
    forge(&sim.base, OFFSET_B, 0u, 0x44u, 0xFFFFFFFFu, 0u, 0);
    (void)fwu_journal_ab_init(&j, &sim.base.flash, OFFSET_A, OFFSET_B, REGION);
    check("sarma: A seq 1, B seq 0xFFFFFFFF -> A aktif", j.active == 0u && fwu_journal_ab_get_last(&j, &last) && last.state == 0x43u);

    /* tam yari: fark 0x80000000 "yeni" sayilmaz (belirsiz) -> esitlik gibi A */
    (void)ab_sim_init(&sim);
    forge(&sim.base, OFFSET_A, 0u, 0x45u, 0x00000000u, 0u, 0);
    forge(&sim.base, OFFSET_B, 0u, 0x46u, 0x80000000u, 0u, 0);
    (void)fwu_journal_ab_init(&j, &sim.base.flash, OFFSET_A, OFFSET_B, REGION);
    check("fark 0x80000000: B yeni sayilmaz, A aktif", j.active == 0u);

    /* fark 0x7FFFFFFF: B yeni */
    (void)ab_sim_init(&sim);
    forge(&sim.base, OFFSET_A, 0u, 0x47u, 0x00000000u, 0u, 0);
    forge(&sim.base, OFFSET_B, 0u, 0x48u, 0x7FFFFFFFu, 0u, 0);
    (void)fwu_journal_ab_init(&j, &sim.base.flash, OFFSET_A, OFFSET_B, REGION);
    check("fark 0x7FFFFFFF: B yeni, B aktif", j.active == 1u);

    /* esit seq: A */
    (void)ab_sim_init(&sim);
    forge(&sim.base, OFFSET_A, 0u, 0x49u, 5u, 0u, 0);
    forge(&sim.base, OFFSET_B, 0u, 0x4Au, 5u, 0u, 0);
    (void)fwu_journal_ab_init(&j, &sim.base.flash, OFFSET_A, OFFSET_B, REGION);
    check("esit seq: A aktif", j.active == 0u && fwu_journal_ab_get_last(&j, &last) && last.state == 0x49u);

    /* yalnizca B'de gecerli kayit: B aktif */
    (void)ab_sim_init(&sim);
    forge(&sim.base, OFFSET_B, 0u, 0x4Bu, 9u, 0u, 0);
    (void)fwu_journal_ab_init(&j, &sim.base.flash, OFFSET_A, OFFSET_B, REGION);
    check("yalniz B'de kayit: B aktif", j.active == 1u && fwu_journal_ab_get_last(&j, &last) && last.state == 0x4Bu);

    /* A yalnizca gecersiz kayitla DOLU, B'de gecerli kayit var: B aktif (A'nin cop dolulugu B'yi etkilemez) */
    (void)ab_sim_init(&sim);
    for(uint32_t s = 0u; s < SLOTS; s++) {
        forge(&sim.base, OFFSET_A, s, 0x50u, 100u + s, 0u, 1);
    }
    forge(&sim.base, OFFSET_B, 0u, 0x4Cu, 3u, 0u, 0);
    (void)fwu_journal_ab_init(&j, &sim.base.flash, OFFSET_A, OFFSET_B, REGION);
    check("A cop dolu, B gecerli: B aktif", j.active == 1u && fwu_journal_ab_get_last(&j, &last) && last.state == 0x4Cu);

    /* iki bolge de YALNIZCA gecersiz kayitla dolu: A aktif ve dolu; ekleme A'yi siler ve devam eder */
    {
        uint32_t erases_a = 0u;
        uint32_t erases_b = 0u;
        (void)ab_sim_init(&sim);
        for(uint32_t s = 0u; s < SLOTS; s++) {
            forge(&sim.base, OFFSET_A, s, 0x51u, 200u + s, 0u, 1);
            forge(&sim.base, OFFSET_B, s, 0x52u, 300u + s, 0u, 1);
        }
        (void)fwu_journal_ab_init(&j, &sim.base.flash, OFFSET_A, OFFSET_B, REGION);
        check("iki bolge cop dolu: kayit yok, A aktif", !fwu_journal_ab_get_last(&j, NULL) && j.active == 0u);
        expect("iki bolge cop dolu: ekleme basarili (A silinir)", fwu_journal_ab_append(&j, 0x53u, 0u, 1u), FWU_OK);
        (void)fwu_flash_sim_get_erase_count(&sim.base, PAGE_A, &erases_a);
        (void)fwu_flash_sim_get_erase_count(&sim.base, PAGE_B, &erases_b);
        check("iki bolge cop dolu: yalniz A silindi (1), B dokunulmadi", erases_a == 1u && erases_b == 0u);
        check("iki bolge cop dolu: yeni kayit okunur, A aktif",
              fwu_journal_ab_get_last(&j, &last) && last.state == 0x53u && j.active == 0u);
    }
}

/** A'yi doldurur (4 kayit): bir sonraki ekleme bolge degistirir. */
static void prepare_full_a(ab_sim_t *sim, fwu_journal_ab_t *j)
{
    fresh(sim, j);
    for(uint32_t i = 1u; i <= SLOTS; i++) {
        (void)append_model(j, i);
    }
}

static void test_rotation_cut_sweep(void)
{
    static ab_sim_t sim;
    fwu_journal_ab_t j;
    fwu_journal_record_t last;
    uint32_t total_ops;
    int membership_ok = 1;
    int exact_ok = 1;
    int continue_ok = 1;

    printf("\n--- tek bolge degisimi: HER kesinti noktasi ---\n");

    /* Elle sayim: silme(1) + anlik goruntu (16 bayt / granul 8 = 2) + kayit (2) = 5 temel islem. */
    prepare_full_a(&sim, &j);
    fwu_flash_sim_reset(&sim.base);
    expect("kesintisiz: 5. ekleme", append_model(&j, 5u), FWU_OK);
    total_ops = fwu_flash_sim_operations(&sim.base);
    check("bolge degisimi = 5 temel islem (1 silme + 2 + 2 yazma)", total_ops == 5u);

    for(uint32_t n = 0u; n <= total_ops; n++) {
        fwu_return_e status;
        uint32_t want_seq;
        uint32_t want_index;     /* son kaydin yuku: 4 (eski) veya 5 (yeni) */

        prepare_full_a(&sim, &j);
        fwu_flash_sim_reset(&sim.base);
        fwu_flash_sim_cut_power_after(&sim.base, n);
        status = append_model(&j, 5u);
        if(status != ((n >= total_ops) ? FWU_OK : FWU_ERR_FLASH_POWER_LOSS)) {
            continue_ok = 0;
        }
        fwu_flash_sim_reset(&sim.base);
        if((fwu_journal_ab_init(&j, &sim.base.flash, OFFSET_A, OFFSET_B, REGION) != FWU_OK) ||
           !fwu_journal_ab_get_last(&j, &last)) {
            membership_ok = 0;
            continue;
        }
        if(!payload_is_model(&last, 4u) && !payload_is_model(&last, 5u)) {
            membership_ok = 0;
        }
        /* elle turetilmis beklenti: n=0..2 anlik goruntu tamamlanmadi (seq 4, kayit 4);
         * n=3,4 anlik goruntu tamam (seq 5, yuk 4); n=5 kayit tamam (seq 6, yuk 5). */
        want_seq = (n <= 2u) ? 4u : ((n <= 4u) ? 5u : 6u);
        want_index = (n <= 4u) ? 4u : 5u;
        if((last.seq != want_seq) || !payload_is_model(&last, want_index)) {
            exact_ok = 0;
            printf("    n=%u: seq %u (beklenen %u)\n", (unsigned)n, (unsigned)last.seq, (unsigned)want_seq);
        }
        /* kesintiden sonra yazmaya devam. n=0..2'de A hala dolu: yeni bir bolge degisimi gerekir
         * (anlik goruntu seq 5, kayit seq 6); digerlerinde kayit dogrudan ardindan gelir. */
        if((fwu_journal_ab_append(&j, model_state(6u), model_flags(6u), model_arg(6u)) != FWU_OK) ||
           !fwu_journal_ab_get_last(&j, &last) || !payload_is_model(&last, 6u) ||
           (last.seq != ((n <= 2u) ? 6u : (want_seq + 1u)))) {
            continue_ok = 0;
        }
        if((fwu_journal_ab_init(&j, &sim.base.flash, OFFSET_A, OFFSET_B, REGION) != FWU_OK) ||
           !fwu_journal_ab_get_last(&j, &last) || !payload_is_model(&last, 6u)) {
            continue_ok = 0;
        }
    }
    check("her kesinti noktasi: son kayit ESKI (4) veya YENI (5) durum", membership_ok);
    check("her kesinti noktasi: seq ve yuk elle turetilen beklentiyle ayni", exact_ok);
    check("her kesinti noktasi: hata kodu dogru, sonra yazma ve yeniden baslatma calisir", continue_ok);
}

static void test_multi_cut_sweep(void)
{
    static ab_sim_t sim;
    fwu_journal_ab_t j;
    fwu_journal_record_t last;
    uint32_t total_ops;
    int lost_ok = 1;
    int resume_ok = 1;
    uint32_t points = 0u;

    printf("\n--- 12 kayit boyunca HER kesinti noktasi ---\n");

    fresh(&sim, &j);
    fwu_flash_sim_reset(&sim.base);
    for(uint32_t i = 1u; i <= 12u; i++) {
        (void)append_model(&j, i);
    }
    total_ops = fwu_flash_sim_operations(&sim.base);
    /* Elle: her ekleme 2 yazma islemi; her bolge degisimi +3 (1 silme + 2 anlik goruntu yazmasi).
     * Bolge degisimi 5., 8. ve 11. kayitta: 12*2 + 3*3 = 33. */
    check("12 kayit = 33 temel islem (12*2 yazma + 3 bolge degisimi * 3)", total_ops == 33u && model_rotations(12u) == 3u);

    for(uint32_t n = 0u; n < total_ops; n++) {
        uint32_t done = 0u;
        uint32_t index;
        int found;

        fresh(&sim, &j);
        fwu_flash_sim_reset(&sim.base);
        fwu_flash_sim_cut_power_after(&sim.base, n);
        for(uint32_t i = 1u; i <= 12u; i++) {
            if(append_model(&j, i) != FWU_OK) {
                break;
            }
            done = i;
        }
        fwu_flash_sim_reset(&sim.base);
        if(fwu_journal_ab_init(&j, &sim.base.flash, OFFSET_A, OFFSET_B, REGION) != FWU_OK) {
            lost_ok = 0;
            continue;
        }
        found = fwu_journal_ab_get_last(&j, &last);
        /* OK donen her ekleme KALICI olmali; kesilen ekleme ya yazildi ya yazilmadi. */
        if(done == 0u) {
            index = (found && payload_is_model(&last, 1u)) ? 1u : 0u;
            if(found && (index == 0u)) {
                lost_ok = 0;
            }
        } else {
            if(!found) {
                lost_ok = 0;
                continue;
            }
            index = payload_is_model(&last, done) ? done : (payload_is_model(&last, done + 1u) ? (done + 1u) : 0u);
            if(index == 0u) {
                lost_ok = 0;
                continue;
            }
        }
        /* kalan kayitlari ekle: 12'ye kadar */
        for(uint32_t i = index + 1u; i <= 12u; i++) {
            if(append_model(&j, i) != FWU_OK) {
                resume_ok = 0;
            }
        }
        if(!fwu_journal_ab_get_last(&j, &last) || !payload_is_model(&last, 12u)) {
            resume_ok = 0;
        }
        if((fwu_journal_ab_init(&j, &sim.base.flash, OFFSET_A, OFFSET_B, REGION) != FWU_OK) ||
           !fwu_journal_ab_get_last(&j, &last) || !payload_is_model(&last, 12u)) {
            resume_ok = 0;
        }
        points++;
    }
    check("her kesinti noktasi: tamamlanan kayit kaybolmaz, son kayit done veya done+1", lost_ok);
    check("her kesinti noktasi: kalan kayitlar eklenir, sonunda 12. kayit okunur", resume_ok);
    check("33 kesinti noktasinin hepsi calisti", points == 33u);
}

static void test_power_failures(void)
{
    static ab_sim_t sim;
    fwu_journal_ab_t j;

    printf("\n--- guc yokken hata iletilir ---\n");
    fresh(&sim, &j);
    fwu_flash_sim_reset(&sim.base);
    fwu_flash_sim_cut_power_after(&sim.base, 0u);
    (void)append_model(&j, 1u);                 /* gucu keser */
    expect("init: guc yok -> okuma hatasi iletilir",
           fwu_journal_ab_init(&j, &sim.base.flash, OFFSET_A, OFFSET_B, REGION), FWU_ERR_FLASH_POWER_LOSS);
    fwu_flash_sim_reset(&sim.base);
    (void)fwu_journal_ab_init(&j, &sim.base.flash, OFFSET_A, OFFSET_B, REGION);
    fwu_flash_sim_cut_power_after(&sim.base, 0u);
    (void)append_model(&j, 2u);
    expect("scan: guc yok -> hata iletilir", fwu_journal_ab_scan(&j), FWU_ERR_FLASH_POWER_LOSS);
    expect("append: guc yok -> hata iletilir", append_model(&j, 3u), FWU_ERR_FLASH_POWER_LOSS);
}

/* ------------------------------------------------------------------ hata enjeksiyonu (güç kesintisi DIŞI flash hataları) */

#define NO_FAULT    0xFFFFFFFFu

/* Enjekte edilen hata kodları birbirinden farklıdır: modül bir hatayı yutup başka bir yoldan başka bir
 * kod üretirse fark edilir. */
#define FAULT_ERASE FWU_ERR_ERASE_ALIGNMENT
#define FAULT_WRITE FWU_ERR_FLASH_NOT_ERASED
#define FAULT_READ  FWU_ERR_OUT_OF_RANGE

typedef struct {
    fwu_flash_t          flash;
    fwu_flash_callback_t callback;
    ab_sim_t            *sim;
    uint32_t             fail_erase;     /* silme bu ofseti kapsarsa FAULT_ERASE döner, flash değişmez */
    uint32_t             fail_write;     /* yazma bu ofseti kapsarsa FAULT_WRITE döner, flash değişmez */
    uint32_t             fail_read;      /* okuma bu ofseti kapsarsa FAULT_READ döner */
} faulty_t;

static int fault_hits(uint32_t fault, uint32_t offset, uint32_t length)
{
    return (fault != NO_FAULT) && (fault >= offset) && (fault < (offset + length));
}

static fwu_return_e faulty_erase(void *context, uint32_t offset, uint32_t length)
{
    faulty_t *f = (faulty_t *)context;
    return fault_hits(f->fail_erase, offset, length) ? FAULT_ERASE : fwu_flash_erase(&f->sim->base.flash, offset, length);
}

static fwu_return_e faulty_write(void *context, uint32_t offset, uint32_t length, const fwu_byte_t *data)
{
    faulty_t *f = (faulty_t *)context;
    return fault_hits(f->fail_write, offset, length) ? FAULT_WRITE : fwu_flash_write(&f->sim->base.flash, offset, length, data);
}

static fwu_return_e faulty_read(void *context, uint32_t offset, uint32_t length, fwu_byte_t *data)
{
    faulty_t *f = (faulty_t *)context;
    return fault_hits(f->fail_read, offset, length) ? FAULT_READ : fwu_flash_read(&f->sim->base.flash, offset, length, data);
}

static void faulty_init(faulty_t *f, ab_sim_t *sim)
{
    f->callback.flash_erase = faulty_erase;
    f->callback.flash_write = faulty_write;
    f->callback.flash_read = faulty_read;
    f->flash.context = f;
    f->flash.callback = &f->callback;
    f->flash.geometry = sim->base.flash.geometry;
    f->sim = sim;
    f->fail_erase = NO_FAULT;
    f->fail_write = NO_FAULT;
    f->fail_read = NO_FAULT;
}

static void faulty_clear(faulty_t *f)
{
    f->fail_erase = NO_FAULT;
    f->fail_write = NO_FAULT;
    f->fail_read = NO_FAULT;
}

static void test_flash_errors(void)
{
    static ab_sim_t sim;
    static faulty_t faulty;
    fwu_journal_ab_t j;
    fwu_journal_record_t last;
    uint32_t erases_b = 0u;

    printf("\n--- flash islem hatalari (guc kesintisi disi): hata aynen doner, durum bozulmaz ---\n");

    /* A dolu; 5. ekleme bolge degistirir. Her adimda hata enjekte et. */
    (void)ab_sim_init(&sim);
    faulty_init(&faulty, &sim);
    (void)fwu_journal_ab_init(&j, &faulty.flash, OFFSET_A, OFFSET_B, REGION);
    for(uint32_t i = 1u; i <= SLOTS; i++) {
        (void)append_model(&j, i);
    }

    faulty.fail_erase = OFFSET_B;
    expect("bolge degisimi: hedef silinemez -> silme hatasi aynen doner", append_model(&j, 5u), FAULT_ERASE);
    check("... aktif A, son kayit hala 4", j.active == 0u && fwu_journal_ab_get_last(&j, &last) && payload_is_model(&last, 4u) && last.seq == 4u);
    faulty_clear(&faulty);

    faulty.fail_read = OFFSET_B;
    expect("bolge degisimi: silinen hedef okunamaz (tarama) -> okuma hatasi aynen doner", append_model(&j, 5u), FAULT_READ);
    faulty_clear(&faulty);

    faulty.fail_write = OFFSET_B;
    expect("bolge degisimi: anlik goruntu yazilamaz -> yazma hatasi aynen doner", append_model(&j, 5u), FAULT_WRITE);
    check("... aktif hala A (bolge degismedi)", j.active == 0u);
    faulty_clear(&faulty);

    expect("hatalar giderilince 5. ekleme basarili", append_model(&j, 5u), FWU_OK);
    check("... aktif B, son kayit 5, seq 6 (modelle ayni)", j.active == 1u && fwu_journal_ab_get_last(&j, &last) &&
          payload_is_model(&last, 5u) && last.seq == model_seq(5u));
    (void)fwu_flash_sim_get_erase_count(&sim.base, PAGE_B, &erases_b);
    check("B, her denemede silindi (hata sonrasi tekrar silinir)", erases_b >= 1u);
    expect("yeniden baslat", fwu_journal_ab_init(&j, &faulty.flash, OFFSET_A, OFFSET_B, REGION), FWU_OK);
    check("... ayni durum", fwu_journal_ab_get_last(&j, &last) && payload_is_model(&last, 5u));

    /* iki bolge de cop dolu: aktif A kendi silinir; silme ve tarama hatalari aynen doner */
    (void)ab_sim_init(&sim);
    faulty_init(&faulty, &sim);
    for(uint32_t s = 0u; s < SLOTS; s++) {
        forge(&sim.base, OFFSET_A, s, 0x51u, 200u + s, 0u, 1);
        forge(&sim.base, OFFSET_B, s, 0x52u, 300u + s, 0u, 1);
    }
    (void)fwu_journal_ab_init(&j, &faulty.flash, OFFSET_A, OFFSET_B, REGION);
    faulty.fail_erase = OFFSET_A;
    expect("cop dolu aktif: silinemez -> silme hatasi aynen doner (BUFFER_FULL degil)", append_model(&j, 1u), FAULT_ERASE);
    faulty_clear(&faulty);
    faulty.fail_read = OFFSET_A;
    expect("cop dolu aktif: silindikten sonra okunamaz -> okuma hatasi aynen doner", append_model(&j, 1u), FAULT_READ);
    faulty_clear(&faulty);
    expect("hatalar giderilince ekleme basarili", append_model(&j, 1u), FWU_OK);

    /* tarama / init okuma hatalari */
    (void)ab_sim_init(&sim);
    faulty_init(&faulty, &sim);
    (void)fwu_journal_ab_init(&j, &faulty.flash, OFFSET_A, OFFSET_B, REGION);
    faulty.fail_read = OFFSET_A;
    expect("scan: A okunamaz -> okuma hatasi aynen doner", fwu_journal_ab_scan(&j), FAULT_READ);
    expect("init: A okunamaz -> okuma hatasi aynen doner", fwu_journal_ab_init(&j, &faulty.flash, OFFSET_A, OFFSET_B, REGION), FAULT_READ);
    faulty_clear(&faulty);
    faulty.fail_read = OFFSET_B;
    expect("scan: B okunamaz -> okuma hatasi aynen doner", fwu_journal_ab_scan(&j), FAULT_READ);
    expect("init: B okunamaz -> okuma hatasi aynen doner", fwu_journal_ab_init(&j, &faulty.flash, OFFSET_A, OFFSET_B, REGION), FAULT_READ);
    faulty_clear(&faulty);
}

/* ------------------------------------------------------------------ journal'ın ARKASINDAN flash değişir, yeniden tara */

static void test_rescan_sees_flash(void)
{
    static ab_sim_t sim;
    fwu_journal_ab_t j;
    fwu_journal_record_t last;

    printf("\n--- scan: flash journal'in arkasindan degisti (yeniden baslatma/baska yazici) ---\n");

    /* bos journal init edilir; sonra flash'a A'da seq 7, B'de seq 3 yazilir (A yeni) */
    (void)ab_sim_init(&sim);
    (void)fwu_journal_ab_init(&j, &sim.base.flash, OFFSET_A, OFFSET_B, REGION);
    forge(&sim.base, OFFSET_A, 0u, 0x61u, 7u, 70u, 0);
    forge(&sim.base, OFFSET_B, 0u, 0x62u, 3u, 30u, 0);
    check("tarama oncesi RAM durumu eski: kayit yok", !fwu_journal_ab_get_last(&j, NULL));
    expect("scan", fwu_journal_ab_scan(&j), FWU_OK);
    check("scan: iki bolge de okundu, A yeni -> aktif A, son kayit seq 7",
          j.active == 0u && fwu_journal_ab_get_last(&j, &last) && last.state == 0x61u && last.seq == 7u);
    check("scan: B de okundu (RAM'de geçerli kayit)", j.part[1].has_last && j.part[1].last.seq == 3u);

    /* simdi B daha yeni olur: aktif B'ye gecmeli */
    forge(&sim.base, OFFSET_B, 1u, 0x63u, 4u, 0u, 0);
    forge(&sim.base, OFFSET_B, 2u, 0x64u, 5u, 0u, 0);
    forge(&sim.base, OFFSET_B, 3u, 0x65u, 6u, 0u, 0);
    expect("scan 2 (B ayni seq 6, A 7: A hala yeni)", fwu_journal_ab_scan(&j), FWU_OK);
    check("... aktif A", j.active == 0u && fwu_journal_ab_get_last(&j, &last) && last.seq == 7u);

    (void)ab_sim_init(&sim);
    (void)fwu_journal_ab_init(&j, &sim.base.flash, OFFSET_A, OFFSET_B, REGION);
    forge(&sim.base, OFFSET_B, 0u, 0x66u, 9u, 0u, 0);
    expect("scan 3: yalniz B'ye kayit yazildi", fwu_journal_ab_scan(&j), FWU_OK);
    check("... aktif B, son kayit seq 9 (aktif secimi tekrar yapildi)", j.active == 1u && fwu_journal_ab_get_last(&j, &last) && last.seq == 9u);
    forge(&sim.base, OFFSET_A, 0u, 0x67u, 12u, 0, 0);
    expect("scan 4: A'ya daha yeni kayit", fwu_journal_ab_scan(&j), FWU_OK);
    check("... aktif A (seq 12 > 9)", j.active == 0u && fwu_journal_ab_get_last(&j, &last) && last.seq == 12u);
}

int main(void)
{
    test_arguments();
    test_basic();
    test_rotation_model();
    test_active_selection();
    test_rotation_cut_sweep();
    test_multi_cut_sweep();
    test_power_failures();
    test_flash_errors();
    test_rescan_sees_flash();

    printf("\nResult: %s (%d failed)\n", g_failures == 0 ? "ALL TESTS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
