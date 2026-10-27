/**
 * @file    test_journal.c
 * @brief   fwu_journal için kendi kendini kontrol eden test (sahte flash üzerinde).
 *
 * Beklentiler journal'ın kendi kodundan DEĞİL, elle yazılmış baytlardan ve bağımsız bir CRC
 * referansından gelir. Kapsam:
 *   1. Kayıt biçimi: bilinen vektör (CRC Python ile hesaplandı), alanların ofsetleri.
 *   2. Temel: boş journal, ekleme, sıra numarası, dolu journal (BUFFER_FULL ve flash değişmez), yeniden
 *      tarama aynı durumu verir.
 *   3. Argümanlar ve bölge doğrulaması; flash okuma hatası iletilir.
 *   4. Bozuk içerik: rastgele çöp, sahte/bozuk/sırası bozuk kayıtlar yok sayılır; 32 bit sıra sarması.
 *   5. Yarım yazılmış kayıt (güç kesintisi) yok sayılır, yuvası boşa gider, sonraki kayıt devam eder.
 *   6. ATOMİKLİK: tek bir append'in HER kesinti noktasında, sonraki tarama eski ya da yeni durumu
 *      verir (hiç bozuk kayıt geçerli sayılmaz); ardından ekleme devam eder.
 *   7. Çoklu ekleme taraması: 6 kayıt boyunca HER kesinti noktası (13 nokta) bağımsız bir modele karşı.
 *   8. Gerçekçi geometri (4 x 2048, 256 yuva), C28x taklidinde kirli üst bitler.
 *
 * Derleme (journal klasöründen):
 *   gcc -Wall -Wextra -Wpedantic -Wconversion -std=c99 -I. -I.. -I../error -I../bytes -I../crc -I../flash fwu_journal.c ../flash/fwu_flash.c ../flash/fwu_flash_sim.c test_journal.c -o test_journal && ./test_journal
 */

#include <stdio.h>
#include <string.h>
#include "fwu_journal.h"
#include "fwu_flash_sim.h"

FWU_FLASH_SIM_DEFINE(jr_small, 4, 64, 8);      /* 4 sayfa x 64; bölge sayfa 1-2 = 8 yuva */
FWU_FLASH_SIM_DEFINE(jr_real, 4, 2048, 8);     /* gerçekçi: bölge sayfa 1-2 = 256 yuva */

#define SMALL_OFFSET    64u
#define SMALL_LENGTH    128u
#define SMALL_SLOTS     8u
#define REAL_OFFSET     2048u
#define REAL_LENGTH     4096u
#define REAL_SLOTS      256u

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

/** Sahte flash belleğine DOĞRUDAN kayıt yazar (flash arayüzünü atlar: bozuk/sahte içerik üretmek için). */
static void forge(fwu_flash_sim_t *sim, uint32_t region_offset, uint32_t slot, uint16_t magic,
                  fwu_byte_t state, fwu_byte_t flags, uint32_t seq, uint32_t arg, int corrupt_crc)
{
    fwu_byte_t raw[16];
    uint16_t crc;
    raw[0] = (fwu_byte_t)((magic >> 8) & 0xFFu);
    raw[1] = (fwu_byte_t)(magic & 0xFFu);
    raw[2] = state;
    raw[3] = flags;
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
static fwu_byte_t model_state(uint32_t i) { return (fwu_byte_t)(0x10u + i); }
static fwu_byte_t model_flags(uint32_t i) { return (fwu_byte_t)(i & 0xFFu); }
static uint32_t   model_arg(uint32_t i)   { return (0x1000u * i) + 7u; }

static fwu_return_e append_model(fwu_journal_t *j, uint32_t i)
{
    return fwu_journal_append(j, model_state(i), model_flags(i), model_arg(i));
}

static int record_is_model(const fwu_journal_record_t *r, uint32_t i)
{
    return (r->seq == i) && (r->state == model_state(i)) && (r->flags == model_flags(i)) && (r->arg == model_arg(i));
}

/* ------------------------------------------------------------------ testler */

static void test_format_and_basic(jr_small_t *sim)
{
    fwu_journal_t j;
    fwu_journal_record_t last;
    const fwu_flash_t *flash;

    printf("\n--- kayit bicimi ve temel islemler ---\n");

    (void)jr_small_init(sim);
    flash = &sim->base.flash;
    expect("bos flash: journal init", fwu_journal_init(&j, flash, SMALL_OFFSET, SMALL_LENGTH), FWU_OK);
    check("bos journal: kayit yok, 8 yuva, ilk bos yuva 0, siradaki seq 1",
          !fwu_journal_get_last(&j, NULL) && j.slot_count == SMALL_SLOTS && j.next_slot == 0u && j.next_seq == 1u &&
          j.valid_slots == 0u && j.invalid_slots == 0u);

    expect("append(state 0x01, flags 0x00, arg 5)", fwu_journal_append(&j, 0x01u, 0x00u, 5u), FWU_OK);
    {
        /* Elle yazılmış beklenen kayıt (CRC 0x9BDA Python ile): 4A 52 01 00 | 00 00 00 01 | 00 00 00 05 | 00 00 | 9B DA */
        const fwu_byte_t want[16] = { 0x4Au, 0x52u, 0x01u, 0x00u, 0x00u, 0x00u, 0x00u, 0x01u,
                                      0x00u, 0x00u, 0x00u, 0x05u, 0x00u, 0x00u, 0x9Bu, 0xDAu };
        int same = 1;
        for(uint32_t i = 0u; i < 16u; i++) {
            if((sim->base.memory[SMALL_OFFSET + i] & 0xFFu) != want[i]) {
                same = 0;
            }
        }
        check("flash'taki ilk yuva elle yazilmis 16 bayta birebir esit (magic, state, flags, seq, arg, CRC)", same);
    }
    check("get_last: state 1, flags 0, seq 1, arg 5",
          fwu_journal_get_last(&j, &last) && last.state == 1u && last.flags == 0u && last.seq == 1u && last.arg == 5u);
    check("ilk bos yuva 1, siradaki seq 2", j.next_slot == 1u && j.next_seq == 2u);
    check("ikinci yuva hala tamamen silinmis", fwu_journal_get_last(&j, NULL) &&
          sim->base.memory[SMALL_OFFSET + 16u] == (fwu_byte_t)0xFFu && sim->base.memory[SMALL_OFFSET + 31u] == (fwu_byte_t)0xFFu);

    /* Dolana kadar ekle: toplam 8 kayıt. */
    {
        int ok = 1;
        for(uint32_t i = 2u; i <= SMALL_SLOTS; i++) {
            if(append_model(&j, i) != FWU_OK) {
                ok = 0;
            }
        }
        check("8 yuvanin hepsi doldu (seq 1..8 ardisik)", ok && j.next_slot == SMALL_SLOTS && j.next_seq == 9u && j.valid_slots == SMALL_SLOTS);
    }
    {
        fwu_byte_t snapshot[128];
        int unchanged = 1;
        for(uint32_t i = 0u; i < 128u; i++) {
            snapshot[i] = sim->base.memory[SMALL_OFFSET + i];
        }
        expect("dolu journal: append -> BUFFER_FULL", append_model(&j, 9u), FWU_ERR_BUFFER_FULL);
        for(uint32_t i = 0u; i < 128u; i++) {
            if(sim->base.memory[SMALL_OFFSET + i] != snapshot[i]) {
                unchanged = 0;
            }
        }
        check("... flash degismedi, journal durumu degismedi", unchanged && j.next_slot == SMALL_SLOTS && j.next_seq == 9u);
    }

    /* Elektrik gitti sanalım: nesneyi at, yeniden tara. Aynı durum çıkmalı. */
    {
        fwu_journal_t again;
        fwu_journal_record_t a;
        fwu_journal_record_t b;
        expect("yeniden init (tarama)", fwu_journal_init(&again, flash, SMALL_OFFSET, SMALL_LENGTH), FWU_OK);
        check("tarama ayni durumu verir: son kayit, ilk bos yuva, siradaki seq, sayaclar",
              fwu_journal_get_last(&again, &a) && fwu_journal_get_last(&j, &b) && a.seq == b.seq && a.state == b.state &&
              a.flags == b.flags && a.arg == b.arg && again.next_slot == j.next_slot && again.next_seq == j.next_seq &&
              again.valid_slots == 8u && again.invalid_slots == 0u);
        check("son kayit modelin 8. kaydi", record_is_model(&a, 8u));
    }
}

static void test_args(jr_small_t *sim)
{
    fwu_journal_t j;
    fwu_flash_t modified;
    const fwu_flash_t *flash;

    printf("\n--- argumanlar ve bolge dogrulamasi ---\n");

    (void)jr_small_init(sim);
    flash = &sim->base.flash;
    expect("journal NULL", fwu_journal_init(NULL, flash, SMALL_OFFSET, SMALL_LENGTH), FWU_ERR_INVALID_ARG);
    expect("flash NULL", fwu_journal_init(&j, NULL, SMALL_OFFSET, SMALL_LENGTH), FWU_ERR_INVALID_ARG);
    expect("uzunluk 0", fwu_journal_init(&j, flash, SMALL_OFFSET, 0u), FWU_ERR_INVALID_ARG);
    expect("ofset sayfa hizali degil (70)", fwu_journal_init(&j, flash, 70u, SMALL_LENGTH), FWU_ERR_INVALID_ARG);
    expect("uzunluk sayfanin kati degil (100)", fwu_journal_init(&j, flash, SMALL_OFFSET, 100u), FWU_ERR_INVALID_ARG);
    expect("bolge flash disinda (ofset 192, uzunluk 128)", fwu_journal_init(&j, flash, 192u, 128u), FWU_ERR_OUT_OF_RANGE);
    expect("ofset tasma (0xFFFFFFC0)", fwu_journal_init(&j, flash, 0xFFFFFFC0u, 128u), FWU_ERR_OUT_OF_RANGE);
    expect("uzunluk flash'tan buyuk (320)", fwu_journal_init(&j, flash, 0u, 320u), FWU_ERR_OUT_OF_RANGE);
    expect("en kucuk bolge (1 sayfa = 4 yuva) gecerli", fwu_journal_init(&j, flash, SMALL_OFFSET, 64u), FWU_OK);
    check("... 4 yuva", j.slot_count == 4u);
    expect("tum flash bolge (256) gecerli", fwu_journal_init(&j, flash, 0u, 256u), FWU_OK);

    modified = *flash;
    modified.geometry.write_granule = 32u;       /* sayfa (64) granülün katı, kayıt (16) DEĞİL */
    expect("kayit boyutu yazma granulunun kati degil (granul 32)", fwu_journal_init(&j, &modified, SMALL_OFFSET, SMALL_LENGTH), FWU_ERR_FLASH_GEOMETRY);
    modified = *flash;
    modified.geometry.page_length = 0u;
    expect("gecersiz flash geometrisi (page_length 0)", fwu_journal_init(&j, &modified, SMALL_OFFSET, SMALL_LENGTH), FWU_ERR_FLASH_GEOMETRY);
    modified = *flash;
    modified.geometry.write_granule = 16u;
    expect("granul 16 = kayit boyutu: gecerli", fwu_journal_init(&j, &modified, SMALL_OFFSET, SMALL_LENGTH), FWU_OK);

    expect("append: journal NULL", fwu_journal_append(NULL, 1u, 0u, 0u), FWU_ERR_INVALID_ARG);
    expect("scan: journal NULL", fwu_journal_scan(NULL), FWU_ERR_INVALID_ARG);
    check("get_last: journal NULL", !fwu_journal_get_last(NULL, NULL));

    /* Güç yokken (kesinti sonrası) okuma hatası olduğu gibi iletilir. */
    (void)jr_small_init(sim);
    fwu_flash_sim_cut_power_after(&sim->base, 0u);
    {
        const fwu_byte_t junk[8] = { 0u };
        (void)fwu_flash_write(flash, 0u, 8u, junk);        /* güç kesilir */
    }
    expect("flash gucsuz: init okuma hatasini iletir", fwu_journal_init(&j, flash, SMALL_OFFSET, SMALL_LENGTH), FWU_ERR_FLASH_POWER_LOSS);
}

static void test_garbage_and_forged(jr_small_t *sim)
{
    fwu_journal_t j;
    fwu_journal_record_t last;
    unsigned rng = 99u;
    const fwu_flash_t *flash;

    printf("\n--- bozuk icerik: cop, sahte ve sirasi bozuk kayitlar ---\n");

    /* Rastgele çöp: çökmez, geçerli sayılan yok, hiç boş yuva yok -> dolu. */
    (void)jr_small_init(sim);
    flash = &sim->base.flash;
    for(uint32_t i = 0u; i < SMALL_LENGTH; i++) {
        rng = (rng * 1103515245u) + 12345u;
        sim->base.memory[SMALL_OFFSET + i] = (fwu_byte_t)(((rng >> 16) & 0xFEu) | ((i % 3u == 0u) ? 0u : 1u));   /* hiçbir yuva tamamen 0xFF değil */
    }
    expect("rastgele cop icerik: init sorunsuz", fwu_journal_init(&j, flash, SMALL_OFFSET, SMALL_LENGTH), FWU_OK);
    check("... gecerli kayit yok, hepsi gecersiz sayildi, journal dolu",
          !fwu_journal_get_last(&j, NULL) && j.valid_slots == 0u && j.invalid_slots == SMALL_SLOTS && j.next_slot == SMALL_SLOTS);
    expect("... cop dolu journal'a append: BUFFER_FULL", fwu_journal_append(&j, 1u, 0u, 0u), FWU_ERR_BUFFER_FULL);

    /* Sahte/bozuk/sıra bozuk kayıtlar. */
    (void)jr_small_init(sim);
    forge(&sim->base, SMALL_OFFSET, 0u, FWU_JOURNAL_MAGIC, 0x21u, 0x01u, 1u, 100u, 0);       /* geçerli, seq 1 */
    forge(&sim->base, SMALL_OFFSET, 1u, FWU_JOURNAL_MAGIC, 0x22u, 0x00u, 5u, 200u, 0);       /* CRC doğru ama seq 5 (2 olmalı) */
    forge(&sim->base, SMALL_OFFSET, 2u, FWU_JOURNAL_MAGIC, 0x23u, 0x00u, 2u, 300u, 1);       /* seq doğru ama CRC bozuk */
    forge(&sim->base, SMALL_OFFSET, 3u, 0x4A53u, 0x24u, 0x00u, 2u, 400u, 0);                 /* CRC doğru ama magic yanlış */
    forge(&sim->base, SMALL_OFFSET, 4u, FWU_JOURNAL_MAGIC, 0x25u, 0x00u, 2u, 500u, 0);       /* geçerli, seq 2 (önceki GEÇERLİ seq 1) */
    expect("sahte kayitlar arasinda: init", fwu_journal_init(&j, flash, SMALL_OFFSET, SMALL_LENGTH), FWU_OK);
    check("... gecerli 2 (yuva 0 ve 4), gecersiz 3 (yuva 1, 2, 3), ilk bos yuva 5",
          j.valid_slots == 2u && j.invalid_slots == 3u && j.next_slot == 5u);
    check("... son gecerli kayit yuva 4'teki (state 0x25, seq 2, arg 500)",
          fwu_journal_get_last(&j, &last) && last.state == 0x25u && last.seq == 2u && last.arg == 500u && last.flags == 0u);
    expect("... append yuva 5'e, seq 3", fwu_journal_append(&j, 0x30u, 0u, 1u), FWU_OK);
    check("... yuva 5 ve seq 3", j.next_slot == 6u && fwu_journal_get_last(&j, &last) && last.seq == 3u);

    /* Boş yuvadan SONRA kalan eski/sahte kayıt: tarama ilk boş yuvada durur, sonrasını yok sayar. */
    (void)jr_small_init(sim);
    forge(&sim->base, SMALL_OFFSET, 0u, FWU_JOURNAL_MAGIC, 0x21u, 0x00u, 1u, 100u, 0);
    forge(&sim->base, SMALL_OFFSET, 2u, FWU_JOURNAL_MAGIC, 0x41u, 0x00u, 2u, 300u, 0);       /* yuva 1 BOŞ, yuva 2 dolu */
    expect("bos yuvadan sonra kayit var: init", fwu_journal_init(&j, flash, SMALL_OFFSET, SMALL_LENGTH), FWU_OK);
    check("... tarama ilk bos yuvada (1) durur: son kayit yuva 0'daki, yuva 2 yok sayilir",
          j.valid_slots == 1u && j.next_slot == 1u && fwu_journal_get_last(&j, &last) && last.seq == 1u && last.state == 0x21u);
    expect("... append bos yuva 1'e yazilir", fwu_journal_append(&j, 0x22u, 0u, 200u), FWU_OK);

    /* Ilk baytı silinmiş görünen ama içeriği olan yuva "boş" SAYILMAMALI (yarım yazma/çöp). */
    (void)jr_small_init(sim);
    forge(&sim->base, SMALL_OFFSET, 0u, FWU_JOURNAL_MAGIC, 0x21u, 0x00u, 1u, 100u, 0);
    sim->base.memory[SMALL_OFFSET + 16u] = (fwu_byte_t)0xFFu;               /* yuva 1: ilk bayt 0xFF, geri kalan 0x00 */
    for(uint32_t i = 1u; i < 16u; i++) {
        sim->base.memory[SMALL_OFFSET + 16u + i] = (fwu_byte_t)0x00u;
    }
    expect("ilk bayti 0xFF ama icerigi dolu yuva: init", fwu_journal_init(&j, flash, SMALL_OFFSET, SMALL_LENGTH), FWU_OK);
    check("... yuva 1 bos SAYILMADI (gecersiz 1), ilk bos yuva 2", j.invalid_slots == 1u && j.valid_slots == 1u && j.next_slot == 2u);
    expect("... append yuva 2'ye sorunsuz yazilir (silinmis yuva)", fwu_journal_append(&j, 0x22u, 0u, 200u), FWU_OK);

    /* 32 bit sıra sarması: seq 0xFFFFFFFE -> 0xFFFFFFFF -> 0. */
    (void)jr_small_init(sim);
    forge(&sim->base, SMALL_OFFSET, 0u, FWU_JOURNAL_MAGIC, 0x7Fu, 0xA5u, 0xFFFFFFFEu, 0xDEADBEEFu, 0);
    expect("seq 0xFFFFFFFE ile baslayan journal: init", fwu_journal_init(&j, flash, SMALL_OFFSET, SMALL_LENGTH), FWU_OK);
    check("... son kayit dogru okundu (state 0x7F, flags 0xA5, arg 0xDEADBEEF)",
          fwu_journal_get_last(&j, &last) && last.seq == 0xFFFFFFFEu && last.state == 0x7Fu && last.flags == 0xA5u && last.arg == 0xDEADBEEFu);
    check("... siradaki seq 0xFFFFFFFF", j.next_seq == 0xFFFFFFFFu);
    expect("... append (seq 0xFFFFFFFF)", fwu_journal_append(&j, 1u, 0u, 1u), FWU_OK);
    expect("... append (seq sarar: 0)", fwu_journal_append(&j, 2u, 0u, 2u), FWU_OK);
    {
        fwu_journal_t again;
        expect("... yeniden tarama", fwu_journal_init(&again, flash, SMALL_OFFSET, SMALL_LENGTH), FWU_OK);
        check("... sarma sonrasi zincir gecerli (3 kayit, son seq 0, state 2)",
              again.valid_slots == 3u && again.invalid_slots == 0u && fwu_journal_get_last(&again, &last) && last.seq == 0u && last.state == 2u);
    }
}

static void test_torn_append(jr_small_t *sim)
{
    fwu_journal_t j;
    fwu_journal_record_t last;
    const fwu_flash_t *flash;

    printf("\n--- yarim yazilmis kayit (guc kesintisi) ---\n");

    for(uint32_t cut = 0u; cut <= 1u; cut++) {
        char name[96];
        (void)jr_small_init(sim);
        flash = &sim->base.flash;
        (void)fwu_journal_init(&j, flash, SMALL_OFFSET, SMALL_LENGTH);
        (void)append_model(&j, 1u);
        fwu_flash_sim_reset(&sim->base);
        fwu_flash_sim_cut_power_after(&sim->base, cut);
        (void)snprintf(name, sizeof(name), "kesinti %u. granulde: append POWER_LOSS", (unsigned)cut);
        expect(name, append_model(&j, 2u), FWU_ERR_FLASH_POWER_LOSS);
        fwu_flash_sim_reset(&sim->base);
        expect("reset sonrasi yeniden tarama", fwu_journal_scan(&j), FWU_OK);
        check("... yalniz ilk kayit gecerli, yarim kayit yok sayildi (gecersiz 1)",
              j.valid_slots == 1u && j.invalid_slots == 1u && fwu_journal_get_last(&j, &last) && record_is_model(&last, 1u));
        check("... yarim yuva atlandi: ilk bos yuva 2, siradaki seq hala 2", j.next_slot == 2u && j.next_seq == 2u);
        expect("... sonraki append yuva 2'ye yazilir", append_model(&j, 2u), FWU_OK);
        check("... zincir devam eder (seq 2, 2 gecerli, 1 gecersiz)",
              fwu_journal_get_last(&j, &last) && record_is_model(&last, 2u) && j.next_slot == 3u && j.valid_slots == 2u);
        {
            fwu_journal_t again;
            (void)fwu_journal_init(&again, flash, SMALL_OFFSET, SMALL_LENGTH);
            check("... yeniden tarama ayni (gecerli 2, gecersiz 1, son seq 2)",
                  again.valid_slots == 2u && again.invalid_slots == 1u && fwu_journal_get_last(&again, &last) && last.seq == 2u);
        }
    }
}

/** Journal'a n model kaydı ekler (kesinti yok). */
static void prepare(jr_small_t *sim, fwu_journal_t *j, uint32_t n)
{
    (void)jr_small_init(sim);
    (void)fwu_journal_init(j, &sim->base.flash, SMALL_OFFSET, SMALL_LENGTH);
    for(uint32_t i = 1u; i <= n; i++) {
        (void)append_model(j, i);
    }
    fwu_flash_sim_reset(&sim->base);
}

static void test_atomic_sweep(jr_small_t *sim)
{
    static const uint32_t priors[] = { 0u, 1u, 3u, 4u, 5u, 7u };    /* 4 ve 5: sayfa sınırı (yuva 4 ikinci sayfanın başı) */
    int ok = 1;
    int count = 0;

    printf("\n--- atomiklik taramasi: tek append, her kesinti noktasi ---\n");

    for(size_t p = 0u; p < sizeof(priors) / sizeof(priors[0]); p++) {
        const uint32_t n = priors[p];
        for(uint32_t cut = 0u; cut <= 3u; cut++) {
            fwu_journal_t j;
            fwu_journal_record_t last;
            fwu_return_e status;
            const int completes = (cut >= 2u);      /* append 2 granül işlemi: cut 2'de kesinti olmaz */

            prepare(sim, &j, n);
            fwu_flash_sim_cut_power_after(&sim->base, cut);
            status = append_model(&j, n + 1u);
            fwu_flash_sim_reset(&sim->base);
            count++;

            if((status == FWU_OK) != completes) {
                ok = 0;
            }
            if(fwu_journal_scan(&j) != FWU_OK) {
                ok = 0;
                continue;
            }
            if(completes) {
                /* Yeni durum: n+1 geçerli, hiç geçersiz yok. */
                if(!(fwu_journal_get_last(&j, &last) && record_is_model(&last, n + 1u) && j.valid_slots == n + 1u && j.invalid_slots == 0u)) {
                    ok = 0;
                }
            } else {
                /* Eski durum: n geçerli, bir yarım yuva. */
                const bool has = fwu_journal_get_last(&j, &last);
                if(n == 0u) {
                    if(has) {
                        ok = 0;
                    }
                } else if(!(has && record_is_model(&last, n))) {
                    ok = 0;
                }
                if(!(j.valid_slots == n && j.invalid_slots == 1u && j.next_slot == n + 1u && j.next_seq == n + 1u)) {
                    ok = 0;
                }
            }
            /* Sonraki ekleme (yer varsa): zincir bozulmadan devam eder. */
            if(j.next_slot < SMALL_SLOTS) {
                if(append_model(&j, j.next_seq) != FWU_OK) {
                    ok = 0;
                }
            } else if(append_model(&j, 99u) != FWU_ERR_BUFFER_FULL) {
                ok = 0;
            }
            if(ok == 0) {
                printf("   (ilk uyumsuzluk: onceki kayit %u, kesinti %u)\n", (unsigned)n, (unsigned)cut);
                break;
            }
        }
        if(ok == 0) {
            break;
        }
    }
    check("24 senaryo (6 onceki durum x 4 kesinti): her seferinde eski ya da yeni durum, bozuk kayit asla gecerli degil, zincir devam eder", ok && count == 24);
}

static void test_multi_sweep(jr_small_t *sim)
{
    int ok = 1;
    int guard = 1;

    printf("\n--- coklu ekleme taramasi: 6 kayit, her kesinti noktasi (k = 0..12) ---\n");

    for(uint32_t k = 0u; k <= 12u; k++) {
        fwu_journal_t j;
        fwu_journal_record_t last;
        uint32_t i;
        const uint32_t complete = (k >= 12u) ? 6u : (k / 2u);       /* her append 2 granül islemi */
        const uint32_t torn = (k >= 12u) ? 0u : 1u;

        (void)jr_small_init(sim);
        (void)fwu_journal_init(&j, &sim->base.flash, SMALL_OFFSET, SMALL_LENGTH);
        fwu_flash_sim_reset(&sim->base);
        fwu_flash_sim_cut_power_after(&sim->base, k);
        for(i = 1u; i <= 6u; i++) {
            if(append_model(&j, i) != FWU_OK) {
                break;
            }
        }
        fwu_flash_sim_reset(&sim->base);
        if(fwu_journal_scan(&j) != FWU_OK) {
            ok = 0;
            continue;
        }
        if(complete == 0u) {
            if(fwu_journal_get_last(&j, NULL)) {
                ok = 0;
            }
        } else if(!(fwu_journal_get_last(&j, &last) && record_is_model(&last, complete))) {
            ok = 0;
        }
        if(!(j.valid_slots == complete && j.invalid_slots == torn && j.next_slot == complete + torn)) {
            ok = 0;
            printf("   (k=%u: gecerli %u/%u, gecersiz %u/%u)\n", (unsigned)k, (unsigned)j.valid_slots, (unsigned)complete,
                   (unsigned)j.invalid_slots, (unsigned)torn);
        }
        /* Kalan kayıtları ekle: zincir devam eder, sonunda 6 kayıt. */
        for(i = complete + 1u; i <= 6u; i++) {
            if(fwu_journal_append(&j, model_state(i), model_flags(i), model_arg(i)) != FWU_OK) {
                guard = 0;
            }
        }
        if(!(fwu_journal_get_last(&j, &last) && last.seq == 6u && last.state == model_state(6u) && last.arg == model_arg(6u))) {
            guard = 0;
        }
    }
    check("13 kesinti noktasi: gecerli kayit sayisi ve yarim yuva sayisi modele birebir uyar", ok);
    check("... her birinde kalan kayitlar eklenir, final son kayit 6. kayit (zincir bozulmaz)", guard);
}

static void test_real_geometry_and_masking(jr_real_t *real, jr_small_t *small)
{
    fwu_journal_t j;
    fwu_journal_record_t last;
    int ok = 1;

    printf("\n--- gercekci geometri (4 x 2048) ve kirli ust bitler ---\n");

    (void)jr_real_init(real);
    expect("gercekci: init (256 yuva)", fwu_journal_init(&j, &real->base.flash, REAL_OFFSET, REAL_LENGTH), FWU_OK);
    check("... 256 yuva", j.slot_count == REAL_SLOTS);
    for(uint32_t i = 1u; i <= 200u; i++) {
        if(append_model(&j, i) != FWU_OK) {
            ok = 0;
        }
    }
    check("200 kayit eklendi (sayfa sinirini asarak: 128. yuvadan sonra ikinci sayfa)", ok && j.next_slot == 200u);
    {
        fwu_journal_t again;
        expect("yeniden tarama", fwu_journal_init(&again, &real->base.flash, REAL_OFFSET, REAL_LENGTH), FWU_OK);
        check("... 200 gecerli, son kayit modelin 200. kaydi, siradaki seq 201",
              again.valid_slots == 200u && again.invalid_slots == 0u && fwu_journal_get_last(&again, &last) &&
              record_is_model(&last, 200u) && again.next_seq == 201u);
    }
    for(uint32_t i = 201u; i <= REAL_SLOTS; i++) {
        if(append_model(&j, i) != FWU_OK) {
            ok = 0;
        }
    }
    check("256 yuvanin hepsi doldu", ok && j.next_slot == REAL_SLOTS);
    expect("257. kayit: BUFFER_FULL", append_model(&j, 257u), FWU_ERR_BUFFER_FULL);

    /* Kirli üst bitler (C28x taklidinde anlamlı). */
    (void)jr_small_init(small);
    (void)fwu_journal_init(&j, &small->base.flash, SMALL_OFFSET, SMALL_LENGTH);
    expect("kirli ust bitli state/flags ile append", fwu_journal_append(&j, (fwu_byte_t)0x1A5u, (fwu_byte_t)0x1FFu, 42u), FWU_OK);
    check("get_last: state 0xA5, flags 0xFF (maskelendi)",
          fwu_journal_get_last(&j, &last) && last.state == (fwu_byte_t)0xA5u && last.flags == (fwu_byte_t)0xFFu && last.arg == 42u);
    {
        fwu_journal_t again;
        (void)fwu_journal_init(&again, &small->base.flash, SMALL_OFFSET, SMALL_LENGTH);
        check("... yeniden taramada ayni (flash'ta temiz baytlar)",
              fwu_journal_get_last(&again, &last) && last.state == (fwu_byte_t)0xA5u && last.flags == (fwu_byte_t)0xFFu);
    }
}

/* ------------------------------------------------------------------ main */

int main(void)
{
    static jr_small_t small;
    static jr_real_t real;

    test_format_and_basic(&small);
    test_args(&small);
    test_garbage_and_forged(&small);
    test_torn_append(&small);
    test_atomic_sweep(&small);
    test_multi_sweep(&small);
    test_real_geometry_and_masking(&real, &small);

    printf("\nResult: %s (%d failed)\n", g_failures == 0 ? "ALL TESTS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
