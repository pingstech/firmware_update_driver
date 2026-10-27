/**
 * @file    test_activate.c
 * @brief   fwu_activate için kendi kendini kontrol eden test (sahte flash üzerinde).
 *
 * Beklentiler modülün kendi kodundan DEĞİL, elle yazılmış imaj desenlerinden ve elle sayılmış
 * temel işlem sayılarından gelir. Kapsam:
 *   1. init doğrulaması: argümanlar, hiza, taşma, çakışma (uygulama/staging/journal/geçici), work
 *      tamponu, mod, geometri; init flash'a yazmaz.
 *   2. COPY: commit/run/tekrar run, işlem sayısı (38), ikinci güncelleme, doğrulama kancası
 *      (kabul, ret, hata, sınır kodları, yalnızca COMMIT'te çağrılır).
 *   3. SWAP: yer değişimi (110 işlem), deneme önyüklemesi (max_trial_boots), onay, geri dönüş (112),
 *      onay kancası sırası, request_revert, tam bölge, kuyruk korunması.
 *   4. API yanlış kullanımı (BAD_STATE), sahte kayıtlarla tutarsız journal durumları.
 *   5. GÜÇ KESİNTİSİ taramaları: HER temel işlemde tekli kesinti, HER ÇİFT (n1, n2) kesinti, rastgele
 *      çoklu kesinti zinciri; COPY kurulum, SWAP kurulum, SWAP geri dönüş, onay.
 *      Değişmez: kurtarma tamamlandığında uygulama bölgesi ya TAM ESKİ ya TAM YENİ imajdır.
 *   6. Sessiz yazma hatası (geri okuma): FLASH_WRITE döner, kayıt yazılmaz, sonra düzelir.
 *   7. Gerçekçi geometri (2048 baytlık sayfa): SWAP kurulumunun HER kesinti noktası.
 *
 * Derleme (activation klasöründen):
 *   gcc -Wall -Wextra -Wpedantic -Wconversion -std=c99 -I. -I.. -I../error -I../bytes -I../crc -I../flash -I../journal fwu_activate.c ../journal/fwu_journal_ab.c ../journal/fwu_journal.c ../flash/fwu_flash.c ../flash/fwu_flash_sim.c test_activate.c -o test_activate && ./test_activate
 */

#include <stdio.h>
#include "fwu_activate.h"
#include "fwu_flash_sim.h"

/* Küçük geometri: 13 sayfa x 64 bayt, granül 8 (journal bölgesi = 4 yuva).
 *   sayfa 0..3 uygulama | 4..7 staging | 8 journal A | 9 journal B | 10..12 geçici (en çok 3 sayfa) */
FWU_FLASH_SIM_DEFINE(act_sim, 13, 64, 8);
/* Gerçekçi geometri: 9 sayfa x 2048, granül 8 (journal bölgesi = 128 yuva).
 *   sayfa 0..2 uygulama | 3..5 staging | 6 journal A | 7 journal B | 8 geçici */
FWU_FLASH_SIM_DEFINE(act_big, 9, 2048, 8);

#define PAGE            64u
#define REGION          256u
#define REGION_PAGES    4u
#define APP_OFFSET      0u
#define STAGING_OFFSET  256u
#define JOURNAL_A       512u
#define JOURNAL_B       576u
#define SCRATCH         640u
#define FLASH_TOTAL     832u
#define PAGES_TOTAL     13u

#define BIG_PAGE        2048u
#define BIG_REGION      6144u
#define BIG_STAGING     6144u
#define BIG_JOURNAL_A   12288u
#define BIG_JOURNAL_B   14336u
#define BIG_SCRATCH     16384u

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

/* ------------------------------------------------------------------ imaj modeli (aktivasyondan BAĞIMSIZ) */

/** Eski imajın i. baytı (bölge başından mutlak indeks; her 64 baytlık blok farklı). */
static fwu_byte_t old_byte(uint32_t i)
{
    return (fwu_byte_t)(((i * 5u) + 1u + ((i >> 6) * 11u) + ((i >> 11) * 73u)) & 0xFFu);
}

/** Yeni imajın i. baytı. */
static fwu_byte_t new_byte(uint32_t i)
{
    return (fwu_byte_t)(((i * 3u) + 200u + ((i >> 6) * 7u) + ((i >> 11) * 41u)) & 0xFFu);
}

static int bytes_match(const fwu_byte_t *memory, uint32_t offset, uint32_t first, uint32_t count,
                       fwu_byte_t (*model)(uint32_t))
{
    for(uint32_t i = first; i < (first + count); i++) {
        if((memory[offset + i] & 0xFFu) != model(i)) {
            return 0;
        }
    }
    return 1;
}

static int bytes_erased(const fwu_byte_t *memory, uint32_t offset, uint32_t first, uint32_t count)
{
    for(uint32_t i = first; i < (first + count); i++) {
        if((memory[offset + i] & 0xFFu) != 0xFFu) {
            return 0;
        }
    }
    return 1;
}

/* Küçük geometri için kontroller (sayfa 64). `n` = yeni imajın sayfa sayısı. */
static int app_is_old(const act_sim_t *sim)
{
    return bytes_match(sim->base.memory, APP_OFFSET, 0u, REGION, old_byte);
}

/** Uygulama: ilk n sayfa YENİ, kalan sayfalar (eski imajın kuyruğu) ESKİ. */
static int app_is_new(const act_sim_t *sim, uint32_t n)
{
    return bytes_match(sim->base.memory, APP_OFFSET, 0u, n * PAGE, new_byte) &&
           bytes_match(sim->base.memory, APP_OFFSET, n * PAGE, REGION - (n * PAGE), old_byte);
}

/** Staging: ilk n sayfa ESKİ imajın sayfaları (takas sonrası), kalanı silinmiş. */
static int staging_holds_old(const act_sim_t *sim, uint32_t n)
{
    return bytes_match(sim->base.memory, STAGING_OFFSET, 0u, n * PAGE, old_byte) &&
           bytes_erased(sim->base.memory, STAGING_OFFSET, n * PAGE, REGION - (n * PAGE));
}

/** Staging: ilk n sayfa YENİ imaj (dokunulmadı), kalanı silinmiş. */
static int staging_holds_new(const act_sim_t *sim, uint32_t n)
{
    return bytes_match(sim->base.memory, STAGING_OFFSET, 0u, n * PAGE, new_byte) &&
           bytes_erased(sim->base.memory, STAGING_OFFSET, n * PAGE, REGION - (n * PAGE));
}

static void setup_flash(act_sim_t *sim, uint32_t new_pages)
{
    (void)act_sim_init(sim);
    for(uint32_t i = 0u; i < REGION; i++) {
        sim->base.memory[APP_OFFSET + i] = old_byte(i);
    }
    for(uint32_t i = 0u; i < (new_pages * PAGE); i++) {
        sim->base.memory[STAGING_OFFSET + i] = new_byte(i);
    }
}

static fwu_byte_t g_work[128];

static void make_config(fwu_activate_config_t *c, act_sim_t *sim, fwu_activate_mode_e mode, uint32_t max_trial)
{
    c->flash = &sim->base.flash;
    c->mode = mode;
    c->app_offset = APP_OFFSET;
    c->staging_offset = STAGING_OFFSET;
    c->region_length = REGION;
    c->journal_a_offset = JOURNAL_A;
    c->journal_b_offset = JOURNAL_B;
    c->journal_length = PAGE;
    c->scratch_offset = SCRATCH;
    c->scratch_pages = 1u;
    c->work = g_work;
    c->work_length = 20u;               /* granül katına yuvarlanır: parça = 16 */
    c->max_trial_boots = max_trial;
    c->require_verify = false;
    c->verify = NULL;
    c->verify_context = NULL;
    c->on_confirmed = NULL;
    c->on_confirmed_context = NULL;
}

/* ------------------------------------------------------------------ kancalar */

typedef struct {
    uint32_t             verify_calls;
    const fwu_flash_t   *verify_flash;
    uint32_t             verify_offset;
    uint32_t             verify_length;
    fwu_return_e         verify_result;
    uint32_t             confirm_calls;
    fwu_return_e         confirm_result;
    const fwu_activate_t *activate;
    fwu_activate_phase_e phase_at_confirm;
    fwu_byte_t           state_at_confirm;
} hooks_t;

static fwu_return_e hook_verify(void *context, const fwu_flash_t *flash, uint32_t offset, uint32_t length)
{
    hooks_t *hooks = (hooks_t *)context;
    hooks->verify_calls++;
    hooks->verify_flash = flash;
    hooks->verify_offset = offset;
    hooks->verify_length = length;
    return hooks->verify_result;
}

static fwu_return_e hook_confirmed(void *context)
{
    hooks_t *hooks = (hooks_t *)context;
    hooks->confirm_calls++;
    hooks->phase_at_confirm = hooks->activate->phase;
    hooks->state_at_confirm = hooks->activate->last_state;
    return hooks->confirm_result;
}

static void hooks_reset(hooks_t *hooks, const fwu_activate_t *activate)
{
    hooks->verify_calls = 0u;
    hooks->verify_flash = NULL;
    hooks->verify_offset = 0u;
    hooks->verify_length = 0u;
    hooks->verify_result = FWU_OK;
    hooks->confirm_calls = 0u;
    hooks->confirm_result = FWU_OK;
    hooks->activate = activate;
    hooks->phase_at_confirm = FWU_ACT_PHASE_IDLE;
    hooks->state_at_confirm = 0u;
}

/* ------------------------------------------------------------------ akış yardımcıları */

/** Güç ver + init. */
static fwu_return_e reboot(act_sim_t *sim, fwu_activate_t *act, const fwu_activate_config_t *cfg)
{
    fwu_flash_sim_reset(&sim->base);
    return fwu_activate_init(act, cfg);
}

/**
 * Bootloader açılışlarını taklit eder: her açılışta [güç ver, init, (kesinti), run]. İlk `cut_count`
 * açılışta verilen kesintiler uygulanır, sonrakiler kesintisizdir. Bir açılış FWU_OK ile biterse döner.
 */
static fwu_return_e boot_until_ok(act_sim_t *sim, fwu_activate_t *act, const fwu_activate_config_t *cfg,
                                  const uint32_t *cuts, uint32_t cut_count, fwu_activate_outcome_e *outcome)
{
    for(uint32_t k = 0u; k < 200u; k++) {
        fwu_return_e status = reboot(sim, act, cfg);
        if(status != FWU_OK) {
            return status;
        }
        if(k < cut_count) {
            fwu_flash_sim_cut_power_after(&sim->base, cuts[k]);
        }
        status = fwu_activate_run(act, outcome);
        if(status == FWU_OK) {
            fwu_flash_sim_reset(&sim->base);     /* kalan kesintiyi kaldır */
            return FWU_OK;
        }
        if(status != FWU_ERR_FLASH_POWER_LOSS) {
            return status;
        }
    }
    return FWU_ERR_BAD_STATE;
}

/** Kesintisiz bir açılış. */
static fwu_return_e boot_clean(act_sim_t *sim, fwu_activate_t *act, const fwu_activate_config_t *cfg,
                               fwu_activate_outcome_e *outcome)
{
    return boot_until_ok(sim, act, cfg, NULL, 0u, outcome);
}

typedef struct {
    fwu_byte_t memory[FLASH_TOTAL];
    uint32_t   erase_counts[PAGES_TOTAL];
} snapshot_t;

static void snapshot_save(const act_sim_t *sim, snapshot_t *s)
{
    for(uint32_t i = 0u; i < FLASH_TOTAL; i++) {
        s->memory[i] = sim->base.memory[i];
    }
    for(uint32_t i = 0u; i < PAGES_TOTAL; i++) {
        s->erase_counts[i] = sim->base.erase_counts[i];
    }
}

static void snapshot_restore(act_sim_t *sim, const snapshot_t *s)
{
    for(uint32_t i = 0u; i < FLASH_TOTAL; i++) {
        sim->base.memory[i] = s->memory[i];
    }
    for(uint32_t i = 0u; i < PAGES_TOTAL; i++) {
        sim->base.erase_counts[i] = s->erase_counts[i];
    }
}

static uint32_t rng_next(uint32_t *state)
{
    *state = (*state * 1664525u) + 1013904223u;
    return *state >> 8;
}

/** Kurtarma bittikten sonra uygulama bölgesi son journal durumuyla tutarlı mı? */
static int app_matches_journal(const fwu_activate_t *act, const act_sim_t *sim, uint32_t n)
{
    const fwu_byte_t s = act->last_state;
    if((act->phase != FWU_ACT_PHASE_IDLE) && (act->phase != FWU_ACT_PHASE_TRIAL)) {
        return 0;                                   /* kurtarma bitmemiş */
    }
    if((s == 0u) || (s == FWU_ACT_STATE_REJECTED) || (s == FWU_ACT_STATE_REVERTED)) {
        return app_is_old(sim);
    }
    if((s == FWU_ACT_STATE_DONE) || (s == FWU_ACT_STATE_CONFIRMED) || (s == FWU_ACT_STATE_TRIAL)) {
        return app_is_new(sim, n);
    }
    return 0;
}

/* ------------------------------------------------------------------ journal'a sahte kayıt */

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

/** `destination`'a (journal A'nın ilk yuvası) geçerli bir journal kaydı yazar (CRC bağımsız referansla). */
static void forge_record_at(fwu_byte_t *destination, fwu_byte_t state, fwu_byte_t flags, uint32_t arg)
{
    fwu_byte_t raw[16];
    uint16_t crc;
    raw[0] = 0x4Au;
    raw[1] = 0x52u;
    raw[2] = state;
    raw[3] = flags;
    raw[4] = 0u;
    raw[5] = 0u;
    raw[6] = 0u;
    raw[7] = 1u;                    /* seq 1: ilk kayıt */
    raw[8] = (fwu_byte_t)((arg >> 24) & 0xFFu);
    raw[9] = (fwu_byte_t)((arg >> 16) & 0xFFu);
    raw[10] = (fwu_byte_t)((arg >> 8) & 0xFFu);
    raw[11] = (fwu_byte_t)(arg & 0xFFu);
    raw[12] = 0u;
    raw[13] = 0u;
    crc = ref_crc16(raw, 14u);
    raw[14] = (fwu_byte_t)((crc >> 8) & 0xFFu);
    raw[15] = (fwu_byte_t)(crc & 0xFFu);
    for(uint32_t i = 0u; i < 16u; i++) {
        destination[i] = raw[i];
    }
}

static void forge_record(act_sim_t *sim, fwu_byte_t state, fwu_byte_t flags, uint32_t arg)
{
    forge_record_at(&sim->base.memory[JOURNAL_A], state, flags, arg);
}

/** Journal'a tek sahte kayıt yaz, belirtilen modda init et. */
static fwu_return_e init_with_forged(act_sim_t *sim, fwu_activate_t *act, fwu_activate_mode_e mode,
                                     fwu_byte_t state, fwu_byte_t flags, uint32_t pages, uint32_t counter)
{
    fwu_activate_config_t c;
    setup_flash(sim, 3u);
    forge_record(sim, state, flags, (pages << 16) | counter);
    make_config(&c, sim, mode, 3u);
    return fwu_activate_init(act, &c);
}

/* ================================================================== 1. init doğrulaması */

static void test_init_validation(void)
{
    static act_sim_t sim;
    static fwu_activate_t act;
    fwu_activate_config_t c;
    fwu_flash_t bad_flash;
    fwu_activate_phase_e phase = FWU_ACT_PHASE_TRIAL;

    printf("\n--- init dogrulamasi ---\n");
    setup_flash(&sim, 3u);

    make_config(&c, &sim, FWU_ACTIVATE_MODE_COPY, 1u);
    fwu_flash_sim_reset(&sim.base);
    expect("gecerli COPY yapilandirmasi", fwu_activate_init(&act, &c), FWU_OK);
    check("init flash'a yazmaz (0 temel islem)", fwu_flash_sim_operations(&sim.base) == 0u);
    check("taze flash: evre IDLE, kayit yok, staging yazilabilir",
          act.phase == FWU_ACT_PHASE_IDLE && act.last_state == 0u && fwu_activate_staging_writable(&act));
    check("bolge sayfa sayisi 4, parca 16", act.region_pages == 4u && act.chunk == 16u);
    check("get_phase IDLE", fwu_activate_get_phase(&act, &phase) == FWU_OK && phase == FWU_ACT_PHASE_IDLE);

    expect("activate NULL", fwu_activate_init(NULL, &c), FWU_ERR_INVALID_ARG);
    expect("config NULL", fwu_activate_init(&act, NULL), FWU_ERR_INVALID_ARG);
    c.flash = NULL;
    expect("flash NULL", fwu_activate_init(&act, &c), FWU_ERR_INVALID_ARG);
    make_config(&c, &sim, FWU_ACTIVATE_MODE_COPY, 1u);
    c.work = NULL;
    expect("work NULL", fwu_activate_init(&act, &c), FWU_ERR_INVALID_ARG);
    make_config(&c, &sim, FWU_ACTIVATE_MODE_COPY, 1u);
    c.require_verify = true;
    expect("dogrulama zorunlu ama kanca NULL", fwu_activate_init(&act, &c), FWU_ERR_INVALID_ARG);
    c.verify = hook_verify;
    expect("dogrulama zorunlu ve kanca var", fwu_activate_init(&act, &c), FWU_OK);
    c.require_verify = false;
    c.verify = NULL;
    expect("dogrulama zorunlu degil, kanca NULL (testlerdeki varsayilan)", fwu_activate_init(&act, &c), FWU_OK);

    bad_flash = sim.base.flash;
    bad_flash.geometry.page_length = 0u;
    make_config(&c, &sim, FWU_ACTIVATE_MODE_COPY, 1u);
    c.flash = &bad_flash;
    expect("gecersiz flash geometrisi", fwu_activate_init(&act, &c), FWU_ERR_FLASH_GEOMETRY);

    make_config(&c, &sim, FWU_ACTIVATE_MODE_COPY, 1u);
    c.mode = (fwu_activate_mode_e)7;
    expect("gecersiz mod", fwu_activate_init(&act, &c), FWU_ERR_INVALID_ARG);

    make_config(&c, &sim, FWU_ACTIVATE_MODE_COPY, 1u);
    c.region_length = 0u;
    expect("bolge uzunlugu 0", fwu_activate_init(&act, &c), FWU_ERR_INVALID_ARG);
    c.region_length = 100u;
    expect("bolge uzunlugu sayfa kati degil", fwu_activate_init(&act, &c), FWU_ERR_INVALID_ARG);
    make_config(&c, &sim, FWU_ACTIVATE_MODE_COPY, 1u);
    c.app_offset = 3u;
    expect("uygulama ofseti hizasiz", fwu_activate_init(&act, &c), FWU_ERR_INVALID_ARG);
    make_config(&c, &sim, FWU_ACTIVATE_MODE_COPY, 1u);
    c.staging_offset = 260u;
    expect("staging ofseti hizasiz", fwu_activate_init(&act, &c), FWU_ERR_INVALID_ARG);
    make_config(&c, &sim, FWU_ACTIVATE_MODE_COPY, 1u);
    c.app_offset = FLASH_TOTAL;
    expect("uygulama bolgesi flash disinda", fwu_activate_init(&act, &c), FWU_ERR_OUT_OF_RANGE);
    make_config(&c, &sim, FWU_ACTIVATE_MODE_COPY, 1u);
    c.staging_offset = 640u;
    expect("staging bolgesi flash sonunu asar", fwu_activate_init(&act, &c), FWU_ERR_OUT_OF_RANGE);
    make_config(&c, &sim, FWU_ACTIVATE_MODE_COPY, 1u);
    c.staging_offset = 192u;
    expect("uygulama ve staging kismen cakisir", fwu_activate_init(&act, &c), FWU_ERR_INVALID_ARG);
    c.staging_offset = APP_OFFSET;
    expect("uygulama ve staging ayni yer", fwu_activate_init(&act, &c), FWU_ERR_INVALID_ARG);
    make_config(&c, &sim, FWU_ACTIVATE_MODE_COPY, 1u);
    c.app_offset = STAGING_OFFSET;
    c.staging_offset = APP_OFFSET;
    expect("staging uygulamadan once ve bitisik gecerli", fwu_activate_init(&act, &c), FWU_OK);

    make_config(&c, &sim, FWU_ACTIVATE_MODE_COPY, 1u);
    c.journal_a_offset = APP_OFFSET;
    expect("journal A uygulamayla cakisir", fwu_activate_init(&act, &c), FWU_ERR_INVALID_ARG);
    make_config(&c, &sim, FWU_ACTIVATE_MODE_COPY, 1u);
    c.journal_b_offset = 320u;
    expect("journal B staging ile cakisir", fwu_activate_init(&act, &c), FWU_ERR_INVALID_ARG);
    make_config(&c, &sim, FWU_ACTIVATE_MODE_COPY, 1u);
    c.journal_a_offset = 320u;
    expect("journal A staging ile cakisir", fwu_activate_init(&act, &c), FWU_ERR_INVALID_ARG);
    make_config(&c, &sim, FWU_ACTIVATE_MODE_COPY, 1u);
    c.journal_b_offset = 64u;
    expect("journal B uygulamayla cakisir", fwu_activate_init(&act, &c), FWU_ERR_INVALID_ARG);
    make_config(&c, &sim, FWU_ACTIVATE_MODE_COPY, 1u);
    c.journal_b_offset = JOURNAL_A;
    expect("journal A ve B ayni (journal reddeder)", fwu_activate_init(&act, &c), FWU_ERR_INVALID_ARG);
    make_config(&c, &sim, FWU_ACTIVATE_MODE_COPY, 1u);
    c.journal_a_offset = FLASH_TOTAL;
    expect("journal flash disinda (journal reddeder)", fwu_activate_init(&act, &c), FWU_ERR_OUT_OF_RANGE);
    make_config(&c, &sim, FWU_ACTIVATE_MODE_COPY, 1u);
    c.journal_length = 0u;
    expect("journal uzunlugu 0", fwu_activate_init(&act, &c), FWU_ERR_INVALID_ARG);

    make_config(&c, &sim, FWU_ACTIVATE_MODE_COPY, 1u);
    c.work_length = 0u;
    expect("work uzunlugu 0", fwu_activate_init(&act, &c), FWU_ERR_INVALID_ARG);
    c.work_length = 7u;
    expect("work tamponu granulden kucuk (7 < 8)", fwu_activate_init(&act, &c), FWU_ERR_INVALID_ARG);
    c.work_length = 8u;
    expect("work tamponu tam bir granul (8)", fwu_activate_init(&act, &c), FWU_OK);
    check("parca 8", act.chunk == 8u);
    c.work_length = 100u;
    expect("work tamponu sayfadan buyuk (100)", fwu_activate_init(&act, &c), FWU_OK);
    check("parca sayfaya sinirlanir (64)", act.chunk == 64u);
    c.work_length = 64u;
    expect("work tamponu tam sayfa", fwu_activate_init(&act, &c), FWU_OK);
    check("parca 64", act.chunk == 64u);

    /* Hiza: baska hicbir kuralin (cakisma, tasma) tetiklenmedigi 1 sayfalik bolge ile, yalniz hiza reddeder */
    make_config(&c, &sim, FWU_ACTIVATE_MODE_COPY, 1u);
    c.region_length = PAGE;
    expect("1 sayfalik bolge, hizali: gecerli", fwu_activate_init(&act, &c), FWU_OK);
    c.app_offset = 3u;
    expect("yalniz uygulama ofseti hizasiz (baska sorun yok)", fwu_activate_init(&act, &c), FWU_ERR_INVALID_ARG);
    c.app_offset = APP_OFFSET;
    c.staging_offset = STAGING_OFFSET + 3u;
    expect("yalniz staging ofseti hizasiz (baska sorun yok)", fwu_activate_init(&act, &c), FWU_ERR_INVALID_ARG);

    /* Tasma: ofset + uzunluk 32 bit'te sarar (0xFFFFFFC0 + 256 = 0xC0); toplamla kontrol bunu kacirir */
    make_config(&c, &sim, FWU_ACTIVATE_MODE_COPY, 1u);
    c.app_offset = 0xFFFFFFC0u;
    expect("uygulama ofseti 32 bit sinirinda (toplam sarar)", fwu_activate_init(&act, &c), FWU_ERR_OUT_OF_RANGE);
    make_config(&c, &sim, FWU_ACTIVATE_MODE_COPY, 1u);
    c.staging_offset = 0xFFFFFFC0u;
    expect("staging ofseti 32 bit sinirinda (toplam sarar)", fwu_activate_init(&act, &c), FWU_ERR_OUT_OF_RANGE);
    make_config(&c, &sim, FWU_ACTIVATE_MODE_SWAP, 1u);
    c.scratch_offset = 0xFFFFFFC0u;
    expect("gecici ofset 32 bit sinirinda (toplam sarar)", fwu_activate_init(&act, &c), FWU_ERR_OUT_OF_RANGE);

    /* COPY, yalnizca SWAP'in kullandigi alanlari yok sayar */
    make_config(&c, &sim, FWU_ACTIVATE_MODE_COPY, 0u);
    c.scratch_offset = 3u;
    expect("COPY: max_trial_boots 0 ve gecersiz scratch yok sayilir", fwu_activate_init(&act, &c), FWU_OK);

    /* SWAP */
    make_config(&c, &sim, FWU_ACTIVATE_MODE_SWAP, 1u);
    expect("gecerli SWAP yapilandirmasi", fwu_activate_init(&act, &c), FWU_OK);
    c.max_trial_boots = 0u;
    expect("SWAP: max_trial_boots 0", fwu_activate_init(&act, &c), FWU_ERR_INVALID_ARG);
    c.max_trial_boots = 255u;
    expect("SWAP: max_trial_boots 255 gecerli", fwu_activate_init(&act, &c), FWU_OK);
    c.max_trial_boots = 256u;
    expect("SWAP: max_trial_boots 256", fwu_activate_init(&act, &c), FWU_ERR_INVALID_ARG);
    make_config(&c, &sim, FWU_ACTIVATE_MODE_SWAP, 1u);
    c.scratch_offset = 650u;
    expect("SWAP: scratch hizasiz", fwu_activate_init(&act, &c), FWU_ERR_INVALID_ARG);
    c.scratch_offset = FLASH_TOTAL;
    expect("SWAP: scratch flash disinda", fwu_activate_init(&act, &c), FWU_ERR_OUT_OF_RANGE);
    c.scratch_offset = 128u;
    expect("SWAP: scratch uygulamayla cakisir", fwu_activate_init(&act, &c), FWU_ERR_INVALID_ARG);
    c.scratch_offset = 384u;
    expect("SWAP: scratch staging ile cakisir", fwu_activate_init(&act, &c), FWU_ERR_INVALID_ARG);
    c.scratch_offset = JOURNAL_A;
    expect("SWAP: scratch journal A ile cakisir", fwu_activate_init(&act, &c), FWU_ERR_INVALID_ARG);
    c.scratch_offset = JOURNAL_B;
    expect("SWAP: scratch journal B ile cakisir", fwu_activate_init(&act, &c), FWU_ERR_INVALID_ARG);
    c.scratch_offset = SCRATCH;
    c.app_offset = STAGING_OFFSET;
    c.staging_offset = APP_OFFSET;
    expect("SWAP: uygulama ve staging yer degistirse de gecerli (scratch disarida)", fwu_activate_init(&act, &c), FWU_OK);

    expect("get_phase: activate NULL", fwu_activate_get_phase(NULL, &phase), FWU_ERR_INVALID_ARG);
    expect("get_phase: phase NULL", fwu_activate_get_phase(&act, NULL), FWU_ERR_INVALID_ARG);
    check("staging_writable: NULL -> false", !fwu_activate_staging_writable(NULL));
}

/* ================================================================== 2. COPY */

static void test_copy_basic(void)
{
    static act_sim_t sim;
    static fwu_activate_t act;
    fwu_activate_config_t c;
    fwu_activate_outcome_e outcome = FWU_ACT_OUTCOME_REVERTED;
    fwu_activate_phase_e phase;

    printf("\n--- COPY: temel akis ---\n");
    setup_flash(&sim, 3u);
    make_config(&c, &sim, FWU_ACTIVATE_MODE_COPY, 1u);
    (void)fwu_activate_init(&act, &c);

    expect("commit(0)", fwu_activate_commit(&act, 0u), FWU_ERR_INVALID_ARG);
    expect("commit(5): bolgeden buyuk", fwu_activate_commit(&act, 5u), FWU_ERR_INVALID_ARG);
    expect("commit: NULL", fwu_activate_commit(NULL, 3u), FWU_ERR_INVALID_ARG);
    expect("IDLE'da confirm", fwu_activate_confirm(&act), FWU_ERR_BAD_STATE);
    expect("IDLE'da request_revert", fwu_activate_request_revert(&act), FWU_ERR_BAD_STATE);
    check("hatali commit'ler journal'a yazmadi", fwu_flash_sim_operations(&sim.base) == 0u);

    expect("commit(3)", fwu_activate_commit(&act, 3u), FWU_OK);
    check("commit = 2 temel islem (bir journal kaydi)", fwu_flash_sim_operations(&sim.base) == 2u);
    check("commit uygulamaya dokunmadi: uygulama hala ESKI", app_is_old(&sim));
    check("evre ACTIVATING, staging yazilamaz",
          fwu_activate_get_phase(&act, &phase) == FWU_OK && phase == FWU_ACT_PHASE_ACTIVATING &&
          !fwu_activate_staging_writable(&act));
    check("son kayit COMMIT, 3 sayfa, sayac 0", act.last_state == FWU_ACT_STATE_COMMIT && act.page_count == 3u && act.counter == 0u);
    expect("bekleyen commit varken ikinci commit", fwu_activate_commit(&act, 2u), FWU_ERR_BAD_STATE);
    expect("ACTIVATING'te confirm", fwu_activate_confirm(&act), FWU_ERR_BAD_STATE);
    expect("ACTIVATING'te request_revert", fwu_activate_request_revert(&act), FWU_ERR_BAD_STATE);

    /* yeniden baslatma: evre journal'dan okunur */
    expect("yeniden baslat", reboot(&sim, &act, &c), FWU_OK);
    check("yeniden baslatmadan sonra evre ACTIVATING", act.phase == FWU_ACT_PHASE_ACTIVATING && act.page_count == 3u);

    expect("run: NULL activate", fwu_activate_run(NULL, &outcome), FWU_ERR_INVALID_ARG);
    expect("run: NULL outcome", fwu_activate_run(&act, NULL), FWU_ERR_INVALID_ARG);
    check("hatali run cagrilari flash'a dokunmadi", fwu_flash_sim_operations(&sim.base) == 0u);

    expect("run", fwu_activate_run(&act, &outcome), FWU_OK);
    check("sonuc ACTIVATED", outcome == FWU_ACT_OUTCOME_ACTIVATED);
    /* elle sayim: 3 sayfa * (1 silme + 8 yazma) = 27; 3 PROGRESS kaydi * 2 = 6; DONE kaydi 2 + bolge degisimi 3 = 5 */
    check("run = 38 temel islem (27 kopya + 6 ilerleme + 5 DONE/bolge degisimi)", fwu_flash_sim_operations(&sim.base) == 38u);
    check("uygulama = yeni imajin 3 sayfasi + eski imajin kuyrugu", app_is_new(&sim, 3u));
    check("COPY staging'e dokunmaz: yeni imaj hala orada", staging_holds_new(&sim, 3u));
    check("evre IDLE, son kayit DONE, staging yazilabilir",
          act.phase == FWU_ACT_PHASE_IDLE && act.last_state == FWU_ACT_STATE_DONE && fwu_activate_staging_writable(&act));
    check("scratch sayfasina COPY'de dokunulmadi", sim.base.erase_counts[10] == 0u);

    /* ikinci run: bekleyen is yok, flash'a dokunma */
    expect("yeniden baslat", reboot(&sim, &act, &c), FWU_OK);
    check("tekrar run: sonuc NONE", fwu_activate_run(&act, &outcome) == FWU_OK && outcome == FWU_ACT_OUTCOME_NONE);
    check("tekrar run: 0 temel islem", fwu_flash_sim_operations(&sim.base) == 0u);
    expect("DONE sonrasi confirm", fwu_activate_confirm(&act), FWU_ERR_BAD_STATE);

    /* ikinci guncelleme: staging yazilabilir, silinir ve 4 sayfalik (tum bolge) ikinci imaj yazilir */
    expect("staging'i sil", fwu_flash_erase(&sim.base.flash, STAGING_OFFSET, REGION), FWU_OK);
    for(uint32_t i = 0u; i < REGION; i++) {
        sim.base.memory[STAGING_OFFSET + i] = (fwu_byte_t)((new_byte(i) ^ 0x55u) & 0xFFu);
    }
    expect("ikinci commit(4)", fwu_activate_commit(&act, 4u), FWU_OK);
    expect("ikinci run", boot_clean(&sim, &act, &c, &outcome), FWU_OK);
    {
        int all_second = 1;
        for(uint32_t i = 0u; i < REGION; i++) {
            if((sim.base.memory[APP_OFFSET + i] & 0xFFu) != ((new_byte(i) ^ 0x55u) & 0xFFu)) {
                all_second = 0;
            }
        }
        check("ikinci run: ACTIVATED, uygulamanin 256 baytinin hepsi ikinci imaj", outcome == FWU_ACT_OUTCOME_ACTIVATED && all_second);
    }
    check("journal A/B arasinda dondu: iki journal sayfasi da silindi",
          sim.base.erase_counts[8] >= 1u && sim.base.erase_counts[9] >= 1u);
}

static void test_copy_verify_hook(void)
{
    static act_sim_t sim;
    static fwu_activate_t act;
    static hooks_t hooks;
    fwu_activate_config_t c;
    fwu_activate_outcome_e outcome = FWU_ACT_OUTCOME_NONE;
    const int codes[] = { 30, 31, 32, 33, 34, 35, 39, 50, 52, 53, 54 };   /* 3x bütünlük + 50, 52, 53, 54 */

    printf("\n--- COPY: dogrulama kancasi ---\n");

    /* kabul */
    setup_flash(&sim, 3u);
    make_config(&c, &sim, FWU_ACTIVATE_MODE_COPY, 1u);
    hooks_reset(&hooks, &act);
    c.verify = hook_verify;
    c.verify_context = &hooks;
    (void)fwu_activate_init(&act, &c);
    (void)fwu_activate_commit(&act, 3u);
    expect("kabul: run", fwu_activate_run(&act, &outcome), FWU_OK);
    check("kabul: ACTIVATED, uygulama yeni", outcome == FWU_ACT_OUTCOME_ACTIVATED && app_is_new(&sim, 3u));
    check("kanca bir kez, dogru flash/ofset/uzunlukla cagrildi (staging, 3 sayfa = 192)",
          hooks.verify_calls == 1u && hooks.verify_flash == &sim.base.flash &&
          hooks.verify_offset == STAGING_OFFSET && hooks.verify_length == 192u);

    /* ret (butunluk hatasi): 30..39 */
    for(unsigned k = 0u; k < 11u; k++) {
        setup_flash(&sim, 3u);
        hooks_reset(&hooks, &act);
        hooks.verify_result = (fwu_return_e)codes[k];
        (void)fwu_activate_init(&act, &c);
        (void)fwu_activate_commit(&act, 3u);
        check("ret kodu 30..39: REJECTED, uygulama degismedi, journal'da REJECTED",
              fwu_activate_run(&act, &outcome) == FWU_OK && outcome == FWU_ACT_OUTCOME_REJECTED &&
              app_is_old(&sim) && act.phase == FWU_ACT_PHASE_IDLE && act.last_state == FWU_ACT_STATE_REJECTED &&
              fwu_activate_staging_writable(&act));
        check("ret: uygulama sayfalari hic silinmedi", sim.base.erase_counts[0] == 0u && sim.base.erase_counts[3] == 0u);
        check("ret: tekrar run kancayi cagirmaz, sonuc NONE",
              boot_clean(&sim, &act, &c, &outcome) == FWU_OK && outcome == FWU_ACT_OUTCOME_NONE && hooks.verify_calls == 1u);
    }

    /* doğrulama yapılamadı (sınır dışı kodlar): REJECTED yazılmaz, hata iletilir */
    {
        const int not_rejection[] = { 29, 40, 45, 46, 11, 49, 51, 55, 59, 60 };   /* 51 = CRYPTO_FAILURE: yapilamadi */
        for(unsigned k = 0u; k < 10u; k++) {
            setup_flash(&sim, 3u);
            hooks_reset(&hooks, &act);
            hooks.verify_result = (fwu_return_e)not_rejection[k];
            (void)fwu_activate_init(&act, &c);
            (void)fwu_activate_commit(&act, 3u);
            check("ret araligi disi kod: hata aynen iletilir, REJECTED yazilmaz, uygulama degismez",
                  fwu_activate_run(&act, &outcome) == (fwu_return_e)not_rejection[k] &&
                  act.phase == FWU_ACT_PHASE_ACTIVATING && act.last_state == FWU_ACT_STATE_COMMIT && app_is_old(&sim));
        }
        /* hata sonrasi kanca duzelir: ayni COMMIT ile devam */
        hooks.verify_result = FWU_OK;
        expect("kanca duzelince ayni commit devam eder", boot_clean(&sim, &act, &c, &outcome), FWU_OK);
        check("... ve ACTIVATED", outcome == FWU_ACT_OUTCOME_ACTIVATED && app_is_new(&sim, 3u));
    }

    /* kanca yalnizca COMMIT'te: yarim kopyadan sonra yeniden cagrilmaz */
    setup_flash(&sim, 3u);
    hooks_reset(&hooks, &act);
    (void)fwu_activate_init(&act, &c);
    (void)fwu_activate_commit(&act, 3u);
    {
        const uint32_t cuts[1] = { 15u };           /* 1. sayfa kopyalanirken */
        expect("yarim kopya sonrasi tamamla", boot_until_ok(&sim, &act, &c, cuts, 1u, &outcome), FWU_OK);
    }
    check("kanca yalnizca ilk acilista cagrildi (ilerleme varken tekrar yok)", hooks.verify_calls == 1u);
    check("... ve sonuc dogru", outcome == FWU_ACT_OUTCOME_ACTIVATED && app_is_new(&sim, 3u));
}

/* ------------------------------------------------------------------ COPY güç kesintisi taramaları */

static void test_copy_sweeps(void)
{
    static act_sim_t sim;
    static fwu_activate_t act;
    static snapshot_t snap;
    fwu_activate_config_t c;
    fwu_activate_outcome_e outcome = FWU_ACT_OUTCOME_NONE;
    int single_ok = 1;
    int single_live_ok = 1;
    int double_ok = 1;
    uint32_t double_points = 0u;
    int chaos_ok = 1;
    uint32_t rng = 12345u;

    printf("\n--- COPY: guc kesintisi taramalari ---\n");

    /* Tekli: commit(2 islem) + run(38 islem) = 40 temel islemin HER birinde kesinti. */
    for(uint32_t n = 0u; n < 40u; n++) {
        fwu_return_e commit_status;
        setup_flash(&sim, 3u);
        make_config(&c, &sim, FWU_ACTIVATE_MODE_COPY, 1u);
        (void)fwu_activate_init(&act, &c);
        fwu_flash_sim_cut_power_after(&sim.base, n);
        commit_status = fwu_activate_commit(&act, 3u);
        if(commit_status == FWU_OK) {
            (void)fwu_activate_run(&act, &outcome);     /* kesilebilir */
        }
        if(boot_clean(&sim, &act, &c, &outcome) != FWU_OK) {
            single_ok = 0;
            continue;
        }
        if(!app_matches_journal(&act, &sim, 3u)) {
            single_ok = 0;
            printf("    tekli n=%u: tutarsiz (son kayit %u)\n", (unsigned)n, (unsigned)act.last_state);
        }
        if((commit_status == FWU_OK) && !app_is_new(&sim, 3u)) {
            single_live_ok = 0;                          /* commit OK dondu ama guncelleme kayboldu */
        }
        if((commit_status != FWU_OK) && (n >= 2u)) {
            single_live_ok = 0;                          /* 2 islemden sonra commit kesilmemeli */
        }
    }
    check("tekli kesinti (40 nokta): kurtarma sonrasi uygulama TAM ESKI veya TAM YENI, journal ile tutarli", single_ok);
    check("tekli kesinti: commit OK dondurduyse guncelleme KAYBOLMAZ", single_live_ok);

    /* Çiftli: ilk kesinti n1 (run'in 38 isleminde), kurtarmada ikinci kesinti n2. */
    for(uint32_t n1 = 0u; n1 < 38u; n1++) {
        setup_flash(&sim, 3u);
        make_config(&c, &sim, FWU_ACTIVATE_MODE_COPY, 1u);
        (void)fwu_activate_init(&act, &c);
        (void)fwu_activate_commit(&act, 3u);
        (void)reboot(&sim, &act, &c);
        fwu_flash_sim_cut_power_after(&sim.base, n1);
        if(fwu_activate_run(&act, &outcome) != FWU_ERR_FLASH_POWER_LOSS) {
            double_ok = 0;
            continue;
        }
        snapshot_save(&sim, &snap);
        for(uint32_t n2 = 0u; n2 < 60u; n2++) {
            const uint32_t cuts[1] = { n2 };
            fwu_return_e status;
            snapshot_restore(&sim, &snap);
            (void)reboot(&sim, &act, &c);
            fwu_flash_sim_cut_power_after(&sim.base, n2);
            status = fwu_activate_run(&act, &outcome);
            if(status == FWU_OK) {
                break;                                   /* kurtarma n2'den once bitti: tarama tamam */
            }
            double_points++;
            snapshot_restore(&sim, &snap);
            if((boot_until_ok(&sim, &act, &c, cuts, 1u, &outcome) != FWU_OK) || !app_matches_journal(&act, &sim, 3u) ||
               !app_is_new(&sim, 3u)) {
                double_ok = 0;
            }
        }
    }
    check("cift kesinti (her n1 x her n2): kurtarma sonunda uygulama TAM YENI", double_ok);
    check("cift kesinti: yeterince nokta tarandi (> 400)", double_points > 400u);

    /* Rastgele cok kesintili zincir */
    for(uint32_t seed = 0u; seed < 300u; seed++) {
        uint32_t cuts[8];
        uint32_t pages = 1u + (rng_next(&rng) % 4u);
        rng = rng + seed;
        for(uint32_t k = 0u; k < 8u; k++) {
            cuts[k] = rng_next(&rng) % 50u;
        }
        setup_flash(&sim, pages);
        make_config(&c, &sim, FWU_ACTIVATE_MODE_COPY, 1u);
        (void)fwu_activate_init(&act, &c);
        (void)fwu_activate_commit(&act, pages);
        if((boot_until_ok(&sim, &act, &c, cuts, 8u, &outcome) != FWU_OK) || !app_is_new(&sim, pages) ||
           (act.last_state != FWU_ACT_STATE_DONE)) {
            chaos_ok = 0;
        }
    }
    check("rastgele cok kesintili zincir (300 tohum, 1..4 sayfa, 8 kesinti): her zaman TAM YENI", chaos_ok);

    /* Kurtarma BITMIS sayfalari tekrarlamaz. 25. islem 3. sayfanin icinde (3. sayfa 22. islemde baslar):
     * kalan is = 38 - 22 = 16 islem (3. sayfa 9 + PROGRESS 2 + DONE 5). */
    setup_flash(&sim, 3u);
    make_config(&c, &sim, FWU_ACTIVATE_MODE_COPY, 1u);
    (void)fwu_activate_init(&act, &c);
    (void)fwu_activate_commit(&act, 3u);
    (void)reboot(&sim, &act, &c);
    fwu_flash_sim_cut_power_after(&sim.base, 25u);
    expect("COPY 25. islemde kes", fwu_activate_run(&act, &outcome), FWU_ERR_FLASH_POWER_LOSS);
    (void)reboot(&sim, &act, &c);
    check("COPY: kurtarma yalnizca kalan 16 islemi yapar (bitmis sayfalari tekrarlamaz)",
          act.counter == 2u && fwu_activate_run(&act, &outcome) == FWU_OK && fwu_flash_sim_operations(&sim.base) == 16u);
}

/* ================================================================== 3. SWAP */

static void test_swap_basic(void)
{
    static act_sim_t sim;
    static fwu_activate_t act;
    static hooks_t hooks;
    fwu_activate_config_t c;
    fwu_activate_outcome_e outcome = FWU_ACT_OUTCOME_NONE;
    fwu_activate_phase_e phase;

    printf("\n--- SWAP: temel akis, deneme, onay ---\n");

    /* kurulum: 3 sayfa, max_trial_boots 1 */
    setup_flash(&sim, 3u);
    make_config(&c, &sim, FWU_ACTIVATE_MODE_SWAP, 1u);
    hooks_reset(&hooks, &act);
    c.verify = hook_verify;
    c.verify_context = &hooks;
    c.on_confirmed = hook_confirmed;
    c.on_confirmed_context = &hooks;
    (void)fwu_activate_init(&act, &c);
    expect("commit(3)", fwu_activate_commit(&act, 3u), FWU_OK);
    check("commit SWAP bayragi ile yazildi, uygulamaya dokunmadi", app_is_old(&sim) && act.last_state == FWU_ACT_STATE_COMMIT);
    expect("yeniden baslat", reboot(&sim, &act, &c), FWU_OK);
    expect("run", fwu_activate_run(&act, &outcome), FWU_OK);
    check("sonuc TRIAL", outcome == FWU_ACT_OUTCOME_TRIAL);
    /* elle sayim: 9 adim * (1 silme + 8 yazma) = 81; 9 PROGRESS + TRIAL = 10 kayit * 2 = 20; bolge degisimi 5., 8., 11. kayitta: 3 * 3 = 9 */
    check("run = 110 temel islem (81 + 20 + 9)", fwu_flash_sim_operations(&sim.base) == 110u);
    check("uygulama = yeni imajin 3 sayfasi + eski imajin kuyrugu", app_is_new(&sim, 3u));
    check("staging = ESKI imajin ilk 3 sayfasi (yer degisti), 4. sayfa silinmis", staging_holds_old(&sim, 3u));
    check("evre TRIAL, deneme 1, staging yazilamaz",
          fwu_activate_get_phase(&act, &phase) == FWU_OK && phase == FWU_ACT_PHASE_TRIAL && act.counter == 1u &&
          act.last_state == FWU_ACT_STATE_TRIAL && !fwu_activate_staging_writable(&act));
    check("dogrulama kancasi bir kez (staging, 192)", hooks.verify_calls == 1u && hooks.verify_offset == STAGING_OFFSET && hooks.verify_length == 192u);
    check("gecici sayfa, sayfa basina bir kez silindi (3 sayfa = 3 silme)", sim.base.erase_counts[10] == 3u);
    expect("TRIAL'da yeni commit", fwu_activate_commit(&act, 2u), FWU_ERR_BAD_STATE);

    /* onay: max_trial_boots 1 oldugu icin onaylamadan yeniden baslama geri doner; once onayla */
    expect("confirm", fwu_activate_confirm(&act), FWU_OK);
    check("onay kancasi bir kez, kayit KALICI olduktan SONRA (evre IDLE, durum CONFIRMED)",
          hooks.confirm_calls == 1u && hooks.phase_at_confirm == FWU_ACT_PHASE_IDLE && hooks.state_at_confirm == FWU_ACT_STATE_CONFIRMED);
    check("onay sonrasi: evre IDLE, staging yazilabilir, uygulama yeni",
          act.phase == FWU_ACT_PHASE_IDLE && fwu_activate_staging_writable(&act) && app_is_new(&sim, 3u));
    expect("onayi tekrarla", fwu_activate_confirm(&act), FWU_OK);
    check("tekrar onay: kanca yeniden cagrilir (2), yeni kayit yazilmaz", hooks.confirm_calls == 2u && act.last_state == FWU_ACT_STATE_CONFIRMED);
    check("yeniden baslat: sonuc NONE, flash'a dokunulmaz",
          reboot(&sim, &act, &c) == FWU_OK && fwu_activate_run(&act, &outcome) == FWU_OK &&
          outcome == FWU_ACT_OUTCOME_NONE && fwu_flash_sim_operations(&sim.base) == 0u && app_is_new(&sim, 3u));
    expect("onayli surumden sonra request_revert", fwu_activate_request_revert(&act), FWU_ERR_BAD_STATE);

    /* kanca hatasi: kayit kalir, confirm hatayi doner, tekrar denenince duzelir */
    setup_flash(&sim, 3u);
    hooks_reset(&hooks, &act);
    (void)fwu_activate_init(&act, &c);
    (void)fwu_activate_commit(&act, 3u);
    expect("kurulum", boot_clean(&sim, &act, &c, &outcome), FWU_OK);
    hooks.confirm_result = FWU_ERR_FLASH_WRITE;
    expect("kanca hata verirse confirm hatayi doner", fwu_activate_confirm(&act), FWU_ERR_FLASH_WRITE);
    check("... ama CONFIRMED kaydi yazildi", act.last_state == FWU_ACT_STATE_CONFIRMED && act.phase == FWU_ACT_PHASE_IDLE);
    hooks.confirm_result = FWU_OK;
    expect("tekrar confirm kancayi yeniden dener", fwu_activate_confirm(&act), FWU_OK);
    check("... kanca 2 kez cagrildi", hooks.confirm_calls == 2u);

    /* request_revert */
    setup_flash(&sim, 3u);
    hooks_reset(&hooks, &act);
    (void)fwu_activate_init(&act, &c);
    (void)fwu_activate_commit(&act, 3u);
    expect("kurulum", boot_clean(&sim, &act, &c, &outcome), FWU_OK);
    expect("request_revert", fwu_activate_request_revert(&act), FWU_OK);
    check("evre REVERTING, uygulama hala yeni", act.phase == FWU_ACT_PHASE_REVERTING && act.last_state == FWU_ACT_STATE_REVERT && app_is_new(&sim, 3u));
    expect("REVERTING'de confirm", fwu_activate_confirm(&act), FWU_ERR_BAD_STATE);
    expect("REVERTING'de commit", fwu_activate_commit(&act, 2u), FWU_ERR_BAD_STATE);
    expect("REVERTING'de tekrar request_revert", fwu_activate_request_revert(&act), FWU_ERR_BAD_STATE);
    check("REVERTING'de staging yazilamaz", !fwu_activate_staging_writable(&act));
    expect("geri donus", boot_clean(&sim, &act, &c, &outcome), FWU_OK);
    check("sonuc REVERTED, uygulama TAM ESKI, staging = yeni imaj", outcome == FWU_ACT_OUTCOME_REVERTED && app_is_old(&sim) && staging_holds_new(&sim, 3u));
    check("evre IDLE, son kayit REVERTED, staging yazilabilir", act.phase == FWU_ACT_PHASE_IDLE && act.last_state == FWU_ACT_STATE_REVERTED && fwu_activate_staging_writable(&act));
    expect("REVERTED sonrasi confirm", fwu_activate_confirm(&act), FWU_ERR_BAD_STATE);
    check("tekrar run: NONE", boot_clean(&sim, &act, &c, &outcome) == FWU_OK && outcome == FWU_ACT_OUTCOME_NONE);

    /* tam bolge: 4 sayfa */
    setup_flash(&sim, 4u);
    hooks_reset(&hooks, &act);
    (void)fwu_activate_init(&act, &c);
    (void)fwu_activate_commit(&act, 4u);
    expect("4 sayfa kurulum", boot_clean(&sim, &act, &c, &outcome), FWU_OK);
    check("4 sayfa: uygulama tamamen yeni, staging tamamen eski", outcome == FWU_ACT_OUTCOME_TRIAL && app_is_new(&sim, 4u) && staging_holds_old(&sim, 4u));
    (void)fwu_activate_request_revert(&act);
    expect("4 sayfa geri donus", boot_clean(&sim, &act, &c, &outcome), FWU_OK);
    check("4 sayfa: geri donus sonrasi uygulama tamamen eski, staging tamamen yeni", app_is_old(&sim) && staging_holds_new(&sim, 4u));

    /* 1 sayfa */
    setup_flash(&sim, 1u);
    (void)fwu_activate_init(&act, &c);
    (void)fwu_activate_commit(&act, 1u);
    expect("1 sayfa kurulum", boot_clean(&sim, &act, &c, &outcome), FWU_OK);
    check("1 sayfa: uygulamanin yalnizca ilk sayfasi yeni, kuyruk eski", app_is_new(&sim, 1u) && staging_holds_old(&sim, 1u));
}

static void test_swap_trial_boots(void)
{
    static act_sim_t sim;
    static fwu_activate_t act;
    fwu_activate_config_t c;
    fwu_activate_outcome_e outcome = FWU_ACT_OUTCOME_NONE;

    printf("\n--- SWAP: deneme onyuklemeleri (max_trial_boots) ---\n");

    /* max 1: onaylamadan ikinci acilis geri doner */
    setup_flash(&sim, 3u);
    make_config(&c, &sim, FWU_ACTIVATE_MODE_SWAP, 1u);
    (void)fwu_activate_init(&act, &c);
    (void)fwu_activate_commit(&act, 3u);
    check("max=1: 1. acilis TRIAL", boot_clean(&sim, &act, &c, &outcome) == FWU_OK && outcome == FWU_ACT_OUTCOME_TRIAL && act.counter == 1u);
    check("max=1: onaysiz 2. acilis REVERTED (uygulama eski)",
          boot_clean(&sim, &act, &c, &outcome) == FWU_OK && outcome == FWU_ACT_OUTCOME_REVERTED && app_is_old(&sim) && staging_holds_new(&sim, 3u));
    check("max=1: sonra NONE", boot_clean(&sim, &act, &c, &outcome) == FWU_OK && outcome == FWU_ACT_OUTCOME_NONE);

    /* max 3: 3 deneme, 4. acilista geri donus */
    setup_flash(&sim, 3u);
    make_config(&c, &sim, FWU_ACTIVATE_MODE_SWAP, 3u);
    (void)fwu_activate_init(&act, &c);
    (void)fwu_activate_commit(&act, 3u);
    check("max=3: 1. acilis TRIAL(1)", boot_clean(&sim, &act, &c, &outcome) == FWU_OK && outcome == FWU_ACT_OUTCOME_TRIAL && act.counter == 1u);
    check("max=3: 2. acilis TRIAL(2), uygulama yeni",
          boot_clean(&sim, &act, &c, &outcome) == FWU_OK && outcome == FWU_ACT_OUTCOME_TRIAL && act.counter == 2u && app_is_new(&sim, 3u));
    check("max=3: 3. acilis TRIAL(3), uygulama yeni",
          boot_clean(&sim, &act, &c, &outcome) == FWU_OK && outcome == FWU_ACT_OUTCOME_TRIAL && act.counter == 3u && app_is_new(&sim, 3u));
    check("max=3: 4. acilis REVERTED, uygulama eski",
          boot_clean(&sim, &act, &c, &outcome) == FWU_OK && outcome == FWU_ACT_OUTCOME_REVERTED && app_is_old(&sim));

    /* max 3 ama 2. denemede onay: kalici */
    setup_flash(&sim, 3u);
    (void)fwu_activate_init(&act, &c);
    (void)fwu_activate_commit(&act, 3u);
    (void)boot_clean(&sim, &act, &c, &outcome);
    (void)boot_clean(&sim, &act, &c, &outcome);
    expect("2. denemede onay", fwu_activate_confirm(&act), FWU_OK);
    check("onaydan sonraki acilislar NONE, uygulama yeni",
          boot_clean(&sim, &act, &c, &outcome) == FWU_OK && outcome == FWU_ACT_OUTCOME_NONE && app_is_new(&sim, 3u));

    /* SWAP dogrulama reddi: uygulama, staging ve gecici sayfaya dokunulmaz */
    {
        static hooks_t hooks;
        setup_flash(&sim, 3u);
        make_config(&c, &sim, FWU_ACTIVATE_MODE_SWAP, 1u);
        hooks_reset(&hooks, &act);
        hooks.verify_result = FWU_ERR_CRC_MISMATCH;
        c.verify = hook_verify;
        c.verify_context = &hooks;
        (void)fwu_activate_init(&act, &c);
        (void)fwu_activate_commit(&act, 3u);
        check("SWAP ret: REJECTED, uygulama eski, staging yeni, hicbir sayfa silinmedi",
              boot_clean(&sim, &act, &c, &outcome) == FWU_OK && outcome == FWU_ACT_OUTCOME_REJECTED && app_is_old(&sim) &&
              staging_holds_new(&sim, 3u) && sim.base.erase_counts[10] == 0u && sim.base.erase_counts[0] == 0u &&
              sim.base.erase_counts[4] == 0u);
    }
}

/* ------------------------------------------------------------------ geçici bölge: aşınma yayılımı (K sayfa) */

/** Modelden: `rounds` tam takas turunda geçici sayfa j kaç kez silinir (sayfa p -> geçici sayfa p mod K). */
static void expected_scratch_erases(uint32_t scratch_pages, uint32_t pages, uint32_t rounds, uint32_t *counts)
{
    for(uint32_t j = 0u; j < 3u; j++) {
        counts[j] = 0u;
    }
    for(uint32_t r = 0u; r < rounds; r++) {
        for(uint32_t p = 0u; p < pages; p++) {
            counts[p % scratch_pages]++;
        }
    }
}

static void test_scratch_pages(void)
{
    static act_sim_t sim;
    static fwu_activate_t act;
    fwu_activate_config_t c;
    fwu_activate_outcome_e outcome = FWU_ACT_OUTCOME_NONE;
    int roundtrip_ok = 1;
    int wear_ok = 1;
    int sweep_install_ok = 1;
    int sweep_revert_ok = 1;
    uint32_t install_ops = 0u;
    uint32_t revert_ops = 0u;
    uint32_t points = 0u;

    printf("\n--- gecici bolge: K sayfa (asinma yayilimi) ---\n");

    /* init dogrulamasi */
    setup_flash(&sim, 3u);
    make_config(&c, &sim, FWU_ACTIVATE_MODE_SWAP, 1u);
    c.scratch_pages = 0u;
    expect("scratch_pages 0", fwu_activate_init(&act, &c), FWU_ERR_INVALID_ARG);
    c.scratch_pages = 2u;
    expect("scratch_pages 2 gecerli", fwu_activate_init(&act, &c), FWU_OK);
    check("... gecici bolge uzunlugu 128", act.scratch_length == 128u);
    c.scratch_pages = 3u;
    expect("scratch_pages 3 gecerli (flash sonuna tam oturur)", fwu_activate_init(&act, &c), FWU_OK);
    check("... gecici bolge uzunlugu 192", act.scratch_length == 192u);
    c.scratch_pages = 4u;
    expect("scratch_pages 4: bolge flash sonunu asar", fwu_activate_init(&act, &c), FWU_ERR_OUT_OF_RANGE);
    c.scratch_pages = 0x04000000u;
    expect("scratch_pages 0x04000000: K * sayfa 32 bit'te sarar (0 olur), reddedilmeli", fwu_activate_init(&act, &c), FWU_ERR_OUT_OF_RANGE);
    c.scratch_pages = 0xFFFFFFFFu;
    expect("scratch_pages 0xFFFFFFFF", fwu_activate_init(&act, &c), FWU_ERR_OUT_OF_RANGE);
    c.scratch_offset = 0u;
    c.scratch_pages = 13u;
    expect("scratch_pages 13: bolge tum flash'a sigar (tasma degil), uygulamayla cakisir", fwu_activate_init(&act, &c), FWU_ERR_INVALID_ARG);
    c.scratch_offset = SCRATCH;
    make_config(&c, &sim, FWU_ACTIVATE_MODE_COPY, 1u);
    c.scratch_pages = 0u;
    expect("COPY: scratch_pages yok sayilir", fwu_activate_init(&act, &c), FWU_OK);
    check("COPY: gecici bolge uzunlugu 0", act.scratch_length == 0u);

    /* K arttikca ikinci/ucuncu sayfa da cakisma kontrolune girer (yalnizca ilk sayfa degil) */
    make_config(&c, &sim, FWU_ACTIVATE_MODE_SWAP, 1u);
    c.journal_a_offset = 704u;
    c.journal_b_offset = 768u;
    c.scratch_offset = 640u;
    c.scratch_pages = 1u;
    expect("gecici 1 sayfa, journal hemen arkasinda: gecerli", fwu_activate_init(&act, &c), FWU_OK);
    c.scratch_pages = 2u;
    expect("gecici 2 sayfa journal A ile cakisir", fwu_activate_init(&act, &c), FWU_ERR_INVALID_ARG);
    c.journal_a_offset = 768u;
    c.journal_b_offset = 704u;
    expect("gecici 2 sayfa journal B ile cakisir (A ile degil)", fwu_activate_init(&act, &c), FWU_ERR_INVALID_ARG);

    make_config(&c, &sim, FWU_ACTIVATE_MODE_SWAP, 1u);
    c.scratch_offset = 0u;
    c.app_offset = 128u;
    c.staging_offset = 384u;
    c.journal_a_offset = 640u;
    c.journal_b_offset = 704u;
    c.scratch_pages = 2u;
    expect("gecici 2 sayfa uygulamanin hemen oncesinde: gecerli", fwu_activate_init(&act, &c), FWU_OK);
    c.scratch_pages = 3u;
    expect("gecici 3 sayfa uygulamayla cakisir", fwu_activate_init(&act, &c), FWU_ERR_INVALID_ARG);

    make_config(&c, &sim, FWU_ACTIVATE_MODE_SWAP, 1u);
    c.scratch_offset = 256u;
    c.staging_offset = 384u;
    c.journal_a_offset = 640u;
    c.journal_b_offset = 704u;
    c.scratch_pages = 2u;
    expect("gecici 2 sayfa staging'in hemen oncesinde: gecerli", fwu_activate_init(&act, &c), FWU_OK);
    c.scratch_pages = 3u;
    expect("gecici 3 sayfa staging ile cakisir", fwu_activate_init(&act, &c), FWU_ERR_INVALID_ARG);

    /* davranis: K = 1..3, imaj 1..4 sayfa; kurulum, geri donus, her gecici sayfanin silme sayisi modelle ayni */
    for(uint32_t k = 1u; k <= 3u; k++) {
        for(uint32_t n = 1u; n <= 4u; n++) {
            uint32_t want[3];
            setup_flash(&sim, n);
            make_config(&c, &sim, FWU_ACTIVATE_MODE_SWAP, 1u);
            c.scratch_pages = k;
            (void)fwu_activate_init(&act, &c);
            (void)fwu_activate_commit(&act, n);
            if((boot_clean(&sim, &act, &c, &outcome) != FWU_OK) || (outcome != FWU_ACT_OUTCOME_TRIAL) ||
               !app_is_new(&sim, n) || !staging_holds_old(&sim, n)) {
                roundtrip_ok = 0;
            }
            expected_scratch_erases(k, n, 1u, want);
            for(uint32_t j = 0u; j < 3u; j++) {
                if(sim.base.erase_counts[10u + j] != want[j]) {
                    wear_ok = 0;
                }
            }
            (void)fwu_activate_request_revert(&act);
            if((boot_clean(&sim, &act, &c, &outcome) != FWU_OK) || (outcome != FWU_ACT_OUTCOME_REVERTED) ||
               !app_is_old(&sim) || !staging_holds_new(&sim, n)) {
                roundtrip_ok = 0;
            }
            expected_scratch_erases(k, n, 2u, want);
            for(uint32_t j = 0u; j < 3u; j++) {
                if(sim.base.erase_counts[10u + j] != want[j]) {
                    wear_ok = 0;
                }
            }
        }
    }
    check("K=1..3 x imaj 1..4 sayfa: kurulum (TRIAL, yeni/eski) ve geri donus (tam eski) dogru", roundtrip_ok);
    check("K=1..3 x imaj 1..4 sayfa: her gecici sayfa tam modeldeki kadar silindi (sayfa p -> gecici p mod K)", wear_ok);

    /* K=3, imaj 4 sayfa: kurulumun HER kesinti noktasi */
    setup_flash(&sim, 4u);
    make_config(&c, &sim, FWU_ACTIVATE_MODE_SWAP, 1u);
    c.scratch_pages = 3u;
    (void)fwu_activate_init(&act, &c);
    (void)fwu_activate_commit(&act, 4u);
    (void)reboot(&sim, &act, &c);
    (void)fwu_activate_run(&act, &outcome);
    install_ops = fwu_flash_sim_operations(&sim.base);
    for(uint32_t n = 0u; n < install_ops; n++) {
        setup_flash(&sim, 4u);
        (void)fwu_activate_init(&act, &c);
        (void)fwu_activate_commit(&act, 4u);
        (void)reboot(&sim, &act, &c);
        fwu_flash_sim_cut_power_after(&sim.base, n);
        if(fwu_activate_run(&act, &outcome) != FWU_ERR_FLASH_POWER_LOSS) {
            sweep_install_ok = 0;
            continue;
        }
        if((boot_clean(&sim, &act, &c, &outcome) != FWU_OK) || (outcome != FWU_ACT_OUTCOME_TRIAL) ||
           !app_is_new(&sim, 4u) || !staging_holds_old(&sim, 4u)) {
            sweep_install_ok = 0;
        }
        points++;
    }
    /* K=2, imaj 4 sayfa: geri donusun HER kesinti noktasi */
    make_config(&c, &sim, FWU_ACTIVATE_MODE_SWAP, 1u);
    c.scratch_pages = 2u;
    setup_flash(&sim, 4u);
    (void)fwu_activate_init(&act, &c);
    (void)fwu_activate_commit(&act, 4u);
    (void)boot_clean(&sim, &act, &c, &outcome);
    (void)reboot(&sim, &act, &c);
    (void)fwu_activate_run(&act, &outcome);
    revert_ops = fwu_flash_sim_operations(&sim.base);
    for(uint32_t n = 0u; n < revert_ops; n++) {
        setup_flash(&sim, 4u);
        (void)fwu_activate_init(&act, &c);
        (void)fwu_activate_commit(&act, 4u);
        (void)boot_clean(&sim, &act, &c, &outcome);
        (void)reboot(&sim, &act, &c);
        fwu_flash_sim_cut_power_after(&sim.base, n);
        if(fwu_activate_run(&act, &outcome) != FWU_ERR_FLASH_POWER_LOSS) {
            sweep_revert_ok = 0;
            continue;
        }
        if((boot_clean(&sim, &act, &c, &outcome) != FWU_OK) || (outcome != FWU_ACT_OUTCOME_REVERTED) ||
           !app_is_old(&sim) || !staging_holds_new(&sim, 4u)) {
            sweep_revert_ok = 0;
        }
        points++;
    }
    check("K=3, 4 sayfa: kurulumun HER kesinti noktasinda kurtarma sonunda uygulama TAM YENI, staging TAM ESKI", sweep_install_ok);
    check("K=2, 4 sayfa: geri donusun HER kesinti noktasinda kurtarma sonunda uygulama TAM ESKI, staging TAM YENI", sweep_revert_ok);
    check("kesinti taramasi yeterli nokta (> 200)", points > 200u);
}

/* ------------------------------------------------------------------ SWAP güç kesintisi taramaları */

static void test_swap_sweeps(void)
{
    static act_sim_t sim;
    static fwu_activate_t act;
    static snapshot_t snap;
    fwu_activate_config_t c;
    fwu_activate_outcome_e outcome = FWU_ACT_OUTCOME_NONE;
    int install_ok = 1;
    int install_live_ok = 1;
    int install_double_ok = 1;
    uint32_t install_points = 0u;
    int revert_ok = 1;
    int revert_double_ok = 1;
    uint32_t revert_points = 0u;
    int confirm_ok = 1;
    int chaos_ok = 1;
    uint32_t rng = 777u;

    printf("\n--- SWAP: guc kesintisi taramalari ---\n");

    /* Kurulum, tekli: commit (2 islem) + run (110 islem) = 112 temel islemin HER birinde kesinti. */
    for(uint32_t n = 0u; n < 112u; n++) {
        fwu_return_e commit_status;
        setup_flash(&sim, 3u);
        make_config(&c, &sim, FWU_ACTIVATE_MODE_SWAP, 1u);
        (void)fwu_activate_init(&act, &c);
        fwu_flash_sim_cut_power_after(&sim.base, n);
        commit_status = fwu_activate_commit(&act, 3u);
        if(commit_status == FWU_OK) {
            (void)fwu_activate_run(&act, &outcome);
        }
        if(boot_clean(&sim, &act, &c, &outcome) != FWU_OK) {
            install_ok = 0;
            continue;
        }
        if(!app_matches_journal(&act, &sim, 3u)) {
            install_ok = 0;
            printf("    kurulum n=%u: tutarsiz (son kayit %u)\n", (unsigned)n, (unsigned)act.last_state);
        }
        if(commit_status == FWU_OK) {
            /* commit kalici: sonuc yeni imaj, staging eski imaji tutuyor, evre TRIAL (tek deneme) */
            if(!app_is_new(&sim, 3u) || !staging_holds_old(&sim, 3u) || (act.phase != FWU_ACT_PHASE_TRIAL)) {
                install_live_ok = 0;
            }
        }
        else if(!app_is_old(&sim) || !staging_holds_new(&sim, 3u)) {
            install_live_ok = 0;                          /* commit yazilamadiysa her sey ilk haliyle */
        }
    }
    check("SWAP kurulum, tekli kesinti (112 nokta): uygulama TAM ESKI veya TAM YENI, journal ile tutarli", install_ok);
    check("SWAP kurulum: commit kalici ise sonuc yeni imaj + staging'de eski imaj + TRIAL; degilse hepsi ilk halinde", install_live_ok);

    /* Kurulum, cift kesinti: n1 (run'in 110 islemi) x n2. */
    for(uint32_t n1 = 0u; n1 < 110u; n1++) {
        setup_flash(&sim, 3u);
        make_config(&c, &sim, FWU_ACTIVATE_MODE_SWAP, 1u);
        (void)fwu_activate_init(&act, &c);
        (void)fwu_activate_commit(&act, 3u);
        (void)reboot(&sim, &act, &c);
        fwu_flash_sim_cut_power_after(&sim.base, n1);
        if(fwu_activate_run(&act, &outcome) != FWU_ERR_FLASH_POWER_LOSS) {
            install_double_ok = 0;
            continue;
        }
        snapshot_save(&sim, &snap);
        for(uint32_t n2 = 0u; n2 < 130u; n2++) {
            const uint32_t cuts[1] = { n2 };
            snapshot_restore(&sim, &snap);
            (void)reboot(&sim, &act, &c);
            fwu_flash_sim_cut_power_after(&sim.base, n2);
            if(fwu_activate_run(&act, &outcome) == FWU_OK) {
                break;
            }
            install_points++;
            snapshot_restore(&sim, &snap);
            if((boot_until_ok(&sim, &act, &c, cuts, 1u, &outcome) != FWU_OK) || (outcome != FWU_ACT_OUTCOME_TRIAL) ||
               !app_is_new(&sim, 3u) || !staging_holds_old(&sim, 3u) || (act.phase != FWU_ACT_PHASE_TRIAL)) {
                install_double_ok = 0;
            }
        }
    }
    check("SWAP kurulum, cift kesinti (her n1 x her n2): sonunda TRIAL, uygulama TAM YENI, staging TAM ESKI", install_double_ok);
    check("SWAP kurulum cift kesinti: yeterince nokta (> 4000)", install_points > 4000u);

    /* Geri donus, tekli: kurulum bitti (TRIAL), onaysiz yeniden baslama: REVERT(0) + 9 adim + 9 kayit + REVERTED = 112 islem. */
    {
        uint32_t revert_ops;
        setup_flash(&sim, 3u);
        make_config(&c, &sim, FWU_ACTIVATE_MODE_SWAP, 1u);
        (void)fwu_activate_init(&act, &c);
        (void)fwu_activate_commit(&act, 3u);
        (void)boot_clean(&sim, &act, &c, &outcome);
        (void)reboot(&sim, &act, &c);
        (void)fwu_activate_run(&act, &outcome);
        revert_ops = fwu_flash_sim_operations(&sim.base);
        /* elle sayim: 9 adim * 9 = 81; 11 kayit (REVERT(0), 9 adim, REVERTED) * 2 = 22; bolge degisimi 14., 17., 20. kayitta: 9 */
        check("geri donus = 112 temel islem (81 + 22 + 9)", revert_ops == 112u && outcome == FWU_ACT_OUTCOME_REVERTED);
    }
    for(uint32_t n = 0u; n < 112u; n++) {
        setup_flash(&sim, 3u);
        make_config(&c, &sim, FWU_ACTIVATE_MODE_SWAP, 1u);
        (void)fwu_activate_init(&act, &c);
        (void)fwu_activate_commit(&act, 3u);
        (void)boot_clean(&sim, &act, &c, &outcome);
        (void)reboot(&sim, &act, &c);
        fwu_flash_sim_cut_power_after(&sim.base, n);
        if(fwu_activate_run(&act, &outcome) != FWU_ERR_FLASH_POWER_LOSS) {
            revert_ok = 0;
            continue;
        }
        if((boot_clean(&sim, &act, &c, &outcome) != FWU_OK) || (outcome != FWU_ACT_OUTCOME_REVERTED) ||
           !app_is_old(&sim) || !staging_holds_new(&sim, 3u) || (act.last_state != FWU_ACT_STATE_REVERTED)) {
            revert_ok = 0;
            printf("    geri donus n=%u basarisiz\n", (unsigned)n);
        }
    }
    check("SWAP geri donus, tekli kesinti (112 nokta): sonunda uygulama TAM ESKI, staging TAM YENI", revert_ok);

    /* Geri donus, cift kesinti */
    for(uint32_t n1 = 0u; n1 < 112u; n1++) {
        setup_flash(&sim, 3u);
        make_config(&c, &sim, FWU_ACTIVATE_MODE_SWAP, 1u);
        (void)fwu_activate_init(&act, &c);
        (void)fwu_activate_commit(&act, 3u);
        (void)boot_clean(&sim, &act, &c, &outcome);
        (void)reboot(&sim, &act, &c);
        fwu_flash_sim_cut_power_after(&sim.base, n1);
        if(fwu_activate_run(&act, &outcome) != FWU_ERR_FLASH_POWER_LOSS) {
            revert_double_ok = 0;
            continue;
        }
        snapshot_save(&sim, &snap);
        for(uint32_t n2 = 0u; n2 < 130u; n2++) {
            const uint32_t cuts[1] = { n2 };
            snapshot_restore(&sim, &snap);
            (void)reboot(&sim, &act, &c);
            fwu_flash_sim_cut_power_after(&sim.base, n2);
            if(fwu_activate_run(&act, &outcome) == FWU_OK) {
                break;
            }
            revert_points++;
            snapshot_restore(&sim, &snap);
            if((boot_until_ok(&sim, &act, &c, cuts, 1u, &outcome) != FWU_OK) || (outcome != FWU_ACT_OUTCOME_REVERTED) ||
               !app_is_old(&sim) || !staging_holds_new(&sim, 3u)) {
                revert_double_ok = 0;
            }
        }
    }
    check("SWAP geri donus, cift kesinti (her n1 x her n2): sonunda uygulama TAM ESKI, staging TAM YENI", revert_double_ok);
    check("SWAP geri donus cift kesinti: yeterince nokta (> 4000)", revert_points > 4000u);

    /* Onay, tekli: CONFIRMED kaydi (2 islem; bolge degisimi gerekirse +3) her kesinti noktasinda. */
    for(uint32_t n = 0u; n < 6u; n++) {
        static hooks_t hooks;
        fwu_return_e status;
        setup_flash(&sim, 3u);
        make_config(&c, &sim, FWU_ACTIVATE_MODE_SWAP, 3u);
        hooks_reset(&hooks, &act);
        c.on_confirmed = hook_confirmed;
        c.on_confirmed_context = &hooks;
        (void)fwu_activate_init(&act, &c);
        (void)fwu_activate_commit(&act, 3u);
        (void)boot_clean(&sim, &act, &c, &outcome);
        fwu_flash_sim_cut_power_after(&sim.base, n);
        status = fwu_activate_confirm(&act);
        if(status == FWU_OK) {
            if(hooks.confirm_calls != 1u) {
                confirm_ok = 0;
            }
        }
        else if((status != FWU_ERR_FLASH_POWER_LOSS) || (hooks.confirm_calls != 0u)) {
            confirm_ok = 0;                              /* kesilen onayda kanca CAGRILMAMALI */
        }
        if(boot_clean(&sim, &act, &c, &outcome) != FWU_OK) {
            confirm_ok = 0;
            continue;
        }
        /* kesilmis onay: hala deneme (TRIAL) -> app yeni; onaylanmis -> app yeni */
        if(!app_is_new(&sim, 3u) || ((act.last_state != FWU_ACT_STATE_TRIAL) && (act.last_state != FWU_ACT_STATE_CONFIRMED)) ||
           ((status == FWU_OK) && (act.last_state != FWU_ACT_STATE_CONFIRMED))) {
            confirm_ok = 0;
        }
    }
    check("onay, tekli kesinti: uygulama hep yeni; kayit TRIAL veya CONFIRMED; kesilen onayda kanca cagrilmaz", confirm_ok);

    /* Rastgele cok kesintili zincir: tum akis (kurulum + [onay | tukenme | request_revert]) */
    for(uint32_t seed = 0u; seed < 240u; seed++) {
        uint32_t cuts[10];
        uint32_t pages = 1u + (rng_next(&rng) % 4u);
        const uint32_t path = seed % 3u;
        const uint32_t max_trial = (path == 1u) ? 2u : 3u;
        rng = rng + seed;
        for(uint32_t k = 0u; k < 10u; k++) {
            cuts[k] = rng_next(&rng) % 130u;
        }
        setup_flash(&sim, pages);
        make_config(&c, &sim, FWU_ACTIVATE_MODE_SWAP, max_trial);
        (void)fwu_activate_init(&act, &c);
        (void)fwu_activate_commit(&act, pages);
        if((boot_until_ok(&sim, &act, &c, cuts, 10u, &outcome) != FWU_OK) || (outcome != FWU_ACT_OUTCOME_TRIAL) ||
           !app_is_new(&sim, pages) || !staging_holds_old(&sim, pages)) {
            chaos_ok = 0;
            continue;
        }
        if(path == 0u) {                                 /* onay (kesintili olabilir) */
            fwu_flash_sim_cut_power_after(&sim.base, rng_next(&rng) % 4u);
            if(fwu_activate_confirm(&act) != FWU_OK) {
                (void)reboot(&sim, &act, &c);
                (void)boot_clean(&sim, &act, &c, &outcome);     /* TRIAL(2) */
                (void)fwu_activate_confirm(&act);
            }
            fwu_flash_sim_reset(&sim.base);
            if(!app_is_new(&sim, pages) || (act.last_state != FWU_ACT_STATE_CONFIRMED)) {
                chaos_ok = 0;
            }
        }
        else if(path == 1u) {                            /* onaysiz acilislar: sonunda geri donus */
            fwu_activate_outcome_e last_outcome = FWU_ACT_OUTCOME_NONE;
            for(uint32_t boots = 0u; boots < 4u; boots++) {
                if(boot_until_ok(&sim, &act, &c, cuts, 10u, &last_outcome) != FWU_OK) {
                    chaos_ok = 0;
                    break;
                }
                if(last_outcome == FWU_ACT_OUTCOME_REVERTED) {
                    break;
                }
            }
            if((last_outcome != FWU_ACT_OUTCOME_REVERTED) || !app_is_old(&sim) || !staging_holds_new(&sim, pages)) {
                chaos_ok = 0;
            }
        }
        else {                                           /* uygulama reddeder */
            if(fwu_activate_request_revert(&act) != FWU_OK) {
                chaos_ok = 0;
                continue;
            }
            if((boot_until_ok(&sim, &act, &c, cuts, 10u, &outcome) != FWU_OK) || (outcome != FWU_ACT_OUTCOME_REVERTED) ||
               !app_is_old(&sim) || !staging_holds_new(&sim, pages)) {
                chaos_ok = 0;
            }
        }
    }
    check("rastgele cok kesintili tum akis (240 tohum: onay / tukenme / request_revert): hep TAM YENI veya TAM ESKI", chaos_ok);

    /* Kurtarma BITMIS adimlari tekrarlamaz. 50. islem 5. adimin icinde (adim 4: 47..55): adim 0..3 ve
     * 5. kayda kadar olan journal kayitlari kalici (47 islem); kalan is = 110 - 47 = 63. */
    setup_flash(&sim, 3u);
    make_config(&c, &sim, FWU_ACTIVATE_MODE_SWAP, 1u);
    (void)fwu_activate_init(&act, &c);
    (void)fwu_activate_commit(&act, 3u);
    (void)reboot(&sim, &act, &c);
    fwu_flash_sim_cut_power_after(&sim.base, 50u);
    expect("SWAP 50. islemde kes", fwu_activate_run(&act, &outcome), FWU_ERR_FLASH_POWER_LOSS);
    (void)reboot(&sim, &act, &c);
    check("SWAP: kurtarma yalnizca kalan 63 islemi yapar (4 biten adimi tekrarlamaz)",
          act.counter == 4u && fwu_activate_run(&act, &outcome) == FWU_OK && fwu_flash_sim_operations(&sim.base) == 63u &&
          outcome == FWU_ACT_OUTCOME_TRIAL);
}

/* ================================================================== 4. tutarsız journal durumları */

static void test_forged_states(void)
{
    static act_sim_t sim;
    static fwu_activate_t act;
    const fwu_byte_t swap = (fwu_byte_t)FWU_ACT_FLAG_SWAP;

    printf("\n--- tutarsiz journal kayitlari ---\n");

    expect("bilinmeyen durum 0", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_COPY, 0u, 0u, 3u, 0u), FWU_ERR_BAD_STATE);
    expect("bilinmeyen durum 9", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_COPY, 9u, 0u, 3u, 0u), FWU_ERR_BAD_STATE);
    expect("bilinmeyen durum 99", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_COPY, 99u, 0u, 3u, 0u), FWU_ERR_BAD_STATE);
    expect("SWAP modunda bilinmeyen durum 0", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_SWAP, 0u, swap, 3u, 1u), FWU_ERR_BAD_STATE);
    expect("SWAP modunda bilinmeyen durum 9", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_SWAP, 9u, swap, 3u, 1u), FWU_ERR_BAD_STATE);
    expect("SWAP modunda bilinmeyen durum 200", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_SWAP, 200u, swap, 3u, 0u), FWU_ERR_BAD_STATE);

    /* COMMIT */
    expect("COMMIT 3 sayfa sayac 0 gecerli", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_COPY, FWU_ACT_STATE_COMMIT, 0u, 3u, 0u), FWU_OK);
    check("... evre ACTIVATING, sayfa 3", act.phase == FWU_ACT_PHASE_ACTIVATING && act.page_count == 3u && act.counter == 0u);
    expect("COMMIT 4 sayfa (bolge boyu) gecerli", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_COPY, FWU_ACT_STATE_COMMIT, 0u, 4u, 0u), FWU_OK);
    expect("COMMIT sayac 1", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_COPY, FWU_ACT_STATE_COMMIT, 0u, 3u, 1u), FWU_ERR_BAD_STATE);
    expect("COMMIT 0 sayfa", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_COPY, FWU_ACT_STATE_COMMIT, 0u, 0u, 0u), FWU_ERR_BAD_STATE);
    expect("COMMIT 5 sayfa (bolgeden buyuk)", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_COPY, FWU_ACT_STATE_COMMIT, 0u, 5u, 0u), FWU_ERR_BAD_STATE);
    expect("COMMIT sayfa > 21845", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_COPY, FWU_ACT_STATE_COMMIT, 0u, 21846u, 0u), FWU_ERR_BAD_STATE);
    expect("COPY modunda SWAP bayrakli kayit", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_COPY, FWU_ACT_STATE_COMMIT, swap, 3u, 0u), FWU_ERR_BAD_STATE);
    expect("SWAP modunda bayraksiz kayit", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_SWAP, FWU_ACT_STATE_COMMIT, 0u, 3u, 0u), FWU_ERR_BAD_STATE);
    expect("SWAP modunda SWAP bayrakli COMMIT gecerli", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_SWAP, FWU_ACT_STATE_COMMIT, swap, 3u, 0u), FWU_OK);

    /* PROGRESS */
    expect("COPY PROGRESS sayac 0", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_COPY, FWU_ACT_STATE_PROGRESS, 0u, 3u, 0u), FWU_ERR_BAD_STATE);
    expect("COPY PROGRESS sayac 1 gecerli", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_COPY, FWU_ACT_STATE_PROGRESS, 0u, 3u, 1u), FWU_OK);
    expect("COPY PROGRESS sayac = sayfa (3) gecerli", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_COPY, FWU_ACT_STATE_PROGRESS, 0u, 3u, 3u), FWU_OK);
    expect("COPY PROGRESS sayac > sayfa (4)", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_COPY, FWU_ACT_STATE_PROGRESS, 0u, 3u, 4u), FWU_ERR_BAD_STATE);
    expect("SWAP PROGRESS sayac 9 (3 sayfa * 3) gecerli", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_SWAP, FWU_ACT_STATE_PROGRESS, swap, 3u, 9u), FWU_OK);
    expect("SWAP PROGRESS sayac 10", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_SWAP, FWU_ACT_STATE_PROGRESS, swap, 3u, 10u), FWU_ERR_BAD_STATE);
    expect("SWAP PROGRESS sayac 0", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_SWAP, FWU_ACT_STATE_PROGRESS, swap, 3u, 0u), FWU_ERR_BAD_STATE);

    /* TRIAL */
    expect("TRIAL sayac 1 gecerli", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_SWAP, FWU_ACT_STATE_TRIAL, swap, 3u, 1u), FWU_OK);
    check("... evre TRIAL", act.phase == FWU_ACT_PHASE_TRIAL && act.counter == 1u);
    expect("TRIAL sayac 255 gecerli", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_SWAP, FWU_ACT_STATE_TRIAL, swap, 3u, 255u), FWU_OK);
    expect("TRIAL sayac 0", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_SWAP, FWU_ACT_STATE_TRIAL, swap, 3u, 0u), FWU_ERR_BAD_STATE);
    expect("TRIAL sayac 256", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_SWAP, FWU_ACT_STATE_TRIAL, swap, 3u, 256u), FWU_ERR_BAD_STATE);
    expect("COPY modunda TRIAL", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_COPY, FWU_ACT_STATE_TRIAL, 0u, 3u, 1u), FWU_ERR_BAD_STATE);
    expect("COPY modunda TRIAL (bayrakli)", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_COPY, FWU_ACT_STATE_TRIAL, swap, 3u, 1u), FWU_ERR_BAD_STATE);

    /* REVERT */
    expect("REVERT sayac 0 gecerli", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_SWAP, FWU_ACT_STATE_REVERT, swap, 3u, 0u), FWU_OK);
    check("... evre REVERTING", act.phase == FWU_ACT_PHASE_REVERTING);
    expect("REVERT sayac 9 gecerli", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_SWAP, FWU_ACT_STATE_REVERT, swap, 3u, 9u), FWU_OK);
    expect("REVERT sayac 10", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_SWAP, FWU_ACT_STATE_REVERT, swap, 3u, 10u), FWU_ERR_BAD_STATE);
    expect("COPY modunda REVERT", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_COPY, FWU_ACT_STATE_REVERT, 0u, 3u, 0u), FWU_ERR_BAD_STATE);

    /* Son durumlar: arg ve bayrak ne olursa olsun IDLE (bekleyen is yok) */
    expect("DONE (sacma arg, SWAP bayrakli, COPY modunda) IDLE", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_COPY, FWU_ACT_STATE_DONE, swap, 0u, 77u), FWU_OK);
    check("... IDLE", act.phase == FWU_ACT_PHASE_IDLE && act.last_state == FWU_ACT_STATE_DONE);
    expect("REJECTED IDLE", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_COPY, FWU_ACT_STATE_REJECTED, swap, 9u, 9u), FWU_OK);
    check("... IDLE", act.phase == FWU_ACT_PHASE_IDLE);
    expect("CONFIRMED IDLE", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_COPY, FWU_ACT_STATE_CONFIRMED, 0u, 0u, 0u), FWU_OK);
    check("... IDLE", act.phase == FWU_ACT_PHASE_IDLE && act.last_state == FWU_ACT_STATE_CONFIRMED);
    expect("REVERTED IDLE", init_with_forged(&sim, &act, FWU_ACTIVATE_MODE_SWAP, FWU_ACT_STATE_REVERTED, 0u, 5u, 5u), FWU_OK);
    check("... IDLE", act.phase == FWU_ACT_PHASE_IDLE);

    /* BAD_STATE'te run/commit hic denenmez: init zaten hata verdi; nesne kullanilmaz */
}

/* ================================================================== 5. sessiz yazma hatası ve work boyutları */

#define NO_FAULT    0xFFFFFFFFu

/* Enjekte edilen hata kodları BİLEREK birbirinden ve geri okuma uyuşmazlığının kodundan
 * (FWU_ERR_FLASH_WRITE) farklıdır: modül hatayı yutup başka bir yolla aynı kodu üretirse fark edilir. */
#define FAULT_ERASE FWU_ERR_ERASE_ALIGNMENT
#define FAULT_WRITE FWU_ERR_FLASH_NOT_ERASED
#define FAULT_READ  FWU_ERR_OUT_OF_RANGE

typedef struct {
    fwu_flash_t          flash;
    fwu_flash_callback_t callback;
    act_sim_t           *sim;
    int                  armed;                 /* sessiz bozulma kurulu mu */
    uint32_t             corrupt_offset;        /* yazma bu ofseti kapsarsa 1 bit bozulur (hata dönmez) */
    uint32_t             corruptions;
    uint32_t             fail_erase_offset;     /* silme bu ofseti kapsarsa FLASH_WRITE döner, flash değişmez */
    uint32_t             fail_write_offset;     /* yazma bu ofseti kapsarsa FLASH_WRITE döner, flash değişmez */
    uint32_t             fail_read_offset;      /* okuma bu ofseti kapsarsa FLASH_WRITE döner */
} faulty_t;

static int fault_hits(uint32_t fault_offset, uint32_t offset, uint32_t length)
{
    return (fault_offset != NO_FAULT) && (fault_offset >= offset) && (fault_offset < (offset + length));
}

static fwu_return_e faulty_erase(void *context, uint32_t offset, uint32_t length)
{
    faulty_t *f = (faulty_t *)context;
    if(fault_hits(f->fail_erase_offset, offset, length)) {
        return FAULT_ERASE;
    }
    return fwu_flash_erase(&f->sim->base.flash, offset, length);
}

static fwu_return_e faulty_write(void *context, uint32_t offset, uint32_t length, const fwu_byte_t *data)
{
    faulty_t *f = (faulty_t *)context;
    fwu_return_e status;
    if(fault_hits(f->fail_write_offset, offset, length)) {
        return FAULT_WRITE;
    }
    status = fwu_flash_write(&f->sim->base.flash, offset, length, data);
    if((status == FWU_OK) && (f->armed != 0) && (f->corrupt_offset >= offset) && (f->corrupt_offset < (offset + length))) {
        f->sim->base.memory[f->corrupt_offset] = (fwu_byte_t)(f->sim->base.memory[f->corrupt_offset] ^ 0x01u);
        f->corruptions++;
    }
    return status;
}

static fwu_return_e faulty_read(void *context, uint32_t offset, uint32_t length, fwu_byte_t *data)
{
    faulty_t *f = (faulty_t *)context;
    if(fault_hits(f->fail_read_offset, offset, length)) {
        return FAULT_READ;
    }
    return fwu_flash_read(&f->sim->base.flash, offset, length, data);
}

static void faulty_init(faulty_t *f, act_sim_t *sim)
{
    f->callback.flash_erase = faulty_erase;
    f->callback.flash_write = faulty_write;
    f->callback.flash_read = faulty_read;
    f->flash.context = f;
    f->flash.callback = &f->callback;
    f->flash.geometry = sim->base.flash.geometry;
    f->sim = sim;
    f->armed = 0;
    f->corrupt_offset = 0u;
    f->corruptions = 0u;
    f->fail_erase_offset = NO_FAULT;
    f->fail_write_offset = NO_FAULT;
    f->fail_read_offset = NO_FAULT;
}

static void faulty_clear(faulty_t *f)
{
    f->armed = 0;
    f->fail_erase_offset = NO_FAULT;
    f->fail_write_offset = NO_FAULT;
    f->fail_read_offset = NO_FAULT;
}

/** Bir hata durumu: hangi modda, hangi çağrı başarısız olur, hata sonrası beklenen ilerleme sayacı. */
typedef struct {
    const char          *name;
    fwu_activate_mode_e  mode;
    uint32_t             erase_offset;
    uint32_t             write_offset;
    uint32_t             read_offset;
    uint32_t             counter_after;      /* hata anında journal'daki ilerleme */
} error_case_t;

static void test_flash_errors(void)
{
    static act_sim_t sim;
    static fwu_activate_t act;
    static faulty_t faulty;
    static hooks_t hooks;
    fwu_activate_config_t c;
    fwu_activate_outcome_e outcome = FWU_ACT_OUTCOME_NONE;
    const error_case_t cases[] = {
        { "COPY: uygulama sayfa 1 silinemez",         FWU_ACTIVATE_MODE_COPY, APP_OFFSET + 64u,     NO_FAULT,             NO_FAULT,             1u },
        { "COPY: uygulama sayfa 0'a yazilamaz",       FWU_ACTIVATE_MODE_COPY, NO_FAULT,             APP_OFFSET + 8u,      NO_FAULT,             0u },
        { "COPY: staging sayfa 2 okunamaz (kaynak)",  FWU_ACTIVATE_MODE_COPY, NO_FAULT,             NO_FAULT,             STAGING_OFFSET + 130u, 2u },
        { "COPY: uygulama okunamaz (geri okuma)",     FWU_ACTIVATE_MODE_COPY, NO_FAULT,             NO_FAULT,             APP_OFFSET + 3u,      0u },
        { "COPY: ilk ilerleme kaydi yazilamaz",       FWU_ACTIVATE_MODE_COPY, NO_FAULT,             JOURNAL_A + 16u,      NO_FAULT,             0u },
        { "SWAP: gecici sayfa silinemez",             FWU_ACTIVATE_MODE_SWAP, SCRATCH,              NO_FAULT,             NO_FAULT,             0u },
        { "SWAP: gecici sayfa okunamaz (geri okuma)", FWU_ACTIVATE_MODE_SWAP, NO_FAULT,             NO_FAULT,             SCRATCH + 20u,        0u },
        { "SWAP: staging'e yazilamaz (adim 2)",       FWU_ACTIVATE_MODE_SWAP, NO_FAULT,             STAGING_OFFSET + 8u,  NO_FAULT,             2u },
        { "SWAP: uygulama silinemez (adim 1)",        FWU_ACTIVATE_MODE_SWAP, APP_OFFSET,           NO_FAULT,             NO_FAULT,             1u },
        { "SWAP: journal bolge degisimi yazilamaz",   FWU_ACTIVATE_MODE_SWAP, NO_FAULT,             JOURNAL_B,            NO_FAULT,             3u },
        { "SWAP: journal B silinemez (bolge degisimi)", FWU_ACTIVATE_MODE_SWAP, JOURNAL_B,          NO_FAULT,             NO_FAULT,             3u }
    };
    int all_error = 1;
    int all_state = 1;
    int all_recover = 1;

    printf("\n--- flash islem hatalari (hata kodu doner, durum bozulmaz, hata giderilince devam) ---\n");
    for(unsigned k = 0u; k < 11u; k++) {
        const error_case_t *e = &cases[k];
        const int swap_mode = (e->mode == FWU_ACTIVATE_MODE_SWAP);
        setup_flash(&sim, 3u);
        faulty_init(&faulty, &sim);
        make_config(&c, &sim, e->mode, 1u);
        c.flash = &faulty.flash;
        (void)fwu_activate_init(&act, &c);
        (void)fwu_activate_commit(&act, 3u);
        faulty.fail_erase_offset = e->erase_offset;
        faulty.fail_write_offset = e->write_offset;
        faulty.fail_read_offset = e->read_offset;
        {
            const fwu_return_e want = (e->erase_offset != NO_FAULT) ? FAULT_ERASE :
                                      ((e->write_offset != NO_FAULT) ? FAULT_WRITE : FAULT_READ);
            const fwu_return_e got = fwu_activate_run(&act, &outcome);
            if(got != want) {
                all_error = 0;
                printf("    %s: gelen %d, beklenen %d\n", e->name, (int)got, (int)want);
                continue;
            }
        }
        /* hata anindaki journal durumu: ilerleme degismedi / ilerledi; REJECTED veya TRIAL YAZILMAMIS */
        if((act.counter != e->counter_after) || (act.phase != FWU_ACT_PHASE_ACTIVATING)) {
            all_state = 0;
            printf("    %s: sayac %u (beklenen %u)\n", e->name, (unsigned)act.counter, (unsigned)e->counter_after);
        }
        faulty_clear(&faulty);
        (void)reboot(&sim, &act, &c);
        if((fwu_activate_run(&act, &outcome) != FWU_OK) || !app_is_new(&sim, 3u) ||
           (swap_mode ? (outcome != FWU_ACT_OUTCOME_TRIAL || !staging_holds_old(&sim, 3u)) : (outcome != FWU_ACT_OUTCOME_ACTIVATED))) {
            all_recover = 0;
            printf("    %s: hata giderilince tamamlanamadi\n", e->name);
        }
    }
    check("11 hata durumu: run, enjekte edilen hata kodunu AYNEN doner (hata YUTULMAZ)", all_error);
    check("11 hata durumu: journal ilerlemesi dogru, REJECTED/TRIAL/DONE yazilmaz, evre ACTIVATING", all_state);
    check("11 hata durumu: hata giderilince ayni commit'ten devam eder, sonuc TAM YENI", all_recover);

    /* commit yazilamazsa: hata doner, hicbir sey degismez, sonra yeniden denenebilir */
    setup_flash(&sim, 3u);
    faulty_init(&faulty, &sim);
    make_config(&c, &sim, FWU_ACTIVATE_MODE_COPY, 1u);
    c.flash = &faulty.flash;
    (void)fwu_activate_init(&act, &c);
    faulty.fail_write_offset = JOURNAL_A;
    expect("commit kaydi yazilamaz -> yazma hatasi aynen doner", fwu_activate_commit(&act, 3u), FAULT_WRITE);
    check("... evre IDLE, kayit yok, uygulama hala eski", act.phase == FWU_ACT_PHASE_IDLE && act.last_state == 0u && app_is_old(&sim));
    faulty_clear(&faulty);
    expect("hata giderilince commit basarili", fwu_activate_commit(&act, 3u), FWU_OK);

    /* onay ve geri donus istegi yazilamazsa: hata doner, evre TRIAL kalir, kanca CAGRILMAZ */
    setup_flash(&sim, 3u);
    faulty_init(&faulty, &sim);
    make_config(&c, &sim, FWU_ACTIVATE_MODE_SWAP, 3u);
    c.flash = &faulty.flash;
    hooks_reset(&hooks, &act);
    c.on_confirmed = hook_confirmed;
    c.on_confirmed_context = &hooks;
    (void)fwu_activate_init(&act, &c);
    (void)fwu_activate_commit(&act, 3u);
    expect("kurulum", boot_clean(&sim, &act, &c, &outcome), FWU_OK);
    faulty.fail_write_offset = JOURNAL_B + 32u;          /* 11 kayit sonrasi aktif B, sonraki yuva 2 */
    expect("onay kaydi yazilamaz -> yazma hatasi aynen doner", fwu_activate_confirm(&act), FAULT_WRITE);
    check("... evre TRIAL, kanca cagrilmadi", act.phase == FWU_ACT_PHASE_TRIAL && hooks.confirm_calls == 0u);
    expect("geri donus istegi yazilamaz -> yazma hatasi aynen doner", fwu_activate_request_revert(&act), FAULT_WRITE);
    check("... evre TRIAL", act.phase == FWU_ACT_PHASE_TRIAL);
    faulty_clear(&faulty);
    expect("hata giderilince onay basarili", fwu_activate_confirm(&act), FWU_OK);
    check("... kanca 1 kez", hooks.confirm_calls == 1u && act.last_state == FWU_ACT_STATE_CONFIRMED);

    /* dogrulama REDDETTI ama REJECTED kaydi yazilamadi: hata doner (uygulama zaten degismedi), kayit yok */
    setup_flash(&sim, 3u);
    faulty_init(&faulty, &sim);
    make_config(&c, &sim, FWU_ACTIVATE_MODE_COPY, 1u);
    c.flash = &faulty.flash;
    hooks_reset(&hooks, &act);
    hooks.verify_result = FWU_ERR_CRC_MISMATCH;
    c.verify = hook_verify;
    c.verify_context = &hooks;
    (void)fwu_activate_init(&act, &c);
    (void)fwu_activate_commit(&act, 3u);
    faulty.fail_write_offset = JOURNAL_A + 16u;
    expect("ret + REJECTED kaydi yazilamaz -> yazma hatasi aynen doner", fwu_activate_run(&act, &outcome), FAULT_WRITE);
    check("... evre ACTIVATING, kayit hala COMMIT, uygulama eski", act.phase == FWU_ACT_PHASE_ACTIVATING &&
          act.last_state == FWU_ACT_STATE_COMMIT && app_is_old(&sim));
    faulty_clear(&faulty);
    (void)reboot(&sim, &act, &c);
    check("... hata giderilince REJECTED yazilir", fwu_activate_run(&act, &outcome) == FWU_OK &&
          outcome == FWU_ACT_OUTCOME_REJECTED && act.last_state == FWU_ACT_STATE_REJECTED);

    /* geri donus adimi basarisiz: hata aynen doner, ilerleme kaydi YAZILMAZ, hata giderilince tamamlanir */
    setup_flash(&sim, 3u);
    faulty_init(&faulty, &sim);
    make_config(&c, &sim, FWU_ACTIVATE_MODE_SWAP, 3u);
    c.flash = &faulty.flash;
    (void)fwu_activate_init(&act, &c);
    (void)fwu_activate_commit(&act, 3u);
    expect("kurulum", boot_clean(&sim, &act, &c, &outcome), FWU_OK);
    expect("geri donus istegi", fwu_activate_request_revert(&act), FWU_OK);
    faulty.fail_erase_offset = SCRATCH;
    expect("geri donus 0. adim: gecici sayfa silinemez -> silme hatasi aynen doner", fwu_activate_run(&act, &outcome), FAULT_ERASE);
    check("... ilerleme kaydi yazilmadi (REVERT sayac 0), uygulama hala yeni",
          act.phase == FWU_ACT_PHASE_REVERTING && act.counter == 0u && app_is_new(&sim, 3u));
    faulty_clear(&faulty);
    (void)reboot(&sim, &act, &c);
    check("... hata giderilince geri donus tamamlanir: uygulama TAM ESKI",
          fwu_activate_run(&act, &outcome) == FWU_OK && outcome == FWU_ACT_OUTCOME_REVERTED && app_is_old(&sim));
}

static void test_silent_write_fault(void)
{
    static act_sim_t sim;
    static fwu_activate_t act;
    static faulty_t faulty;
    fwu_activate_config_t c;
    fwu_activate_outcome_e outcome = FWU_ACT_OUTCOME_NONE;
    const uint32_t targets[] = { APP_OFFSET + 70u, SCRATCH + 3u, STAGING_OFFSET + 10u };   /* COPY: uygulama sayfa 1; SWAP: gecici ve staging */

    printf("\n--- sessiz yazma hatasi (geri okuma) ---\n");

    /* COPY: uygulama sayfa 1'in bir baytini yazma sirasinda boz */
    setup_flash(&sim, 3u);
    faulty_init(&faulty, &sim);
    make_config(&c, &sim, FWU_ACTIVATE_MODE_COPY, 1u);
    c.flash = &faulty.flash;
    (void)fwu_activate_init(&act, &c);
    (void)fwu_activate_commit(&act, 3u);
    faulty.armed = 1;
    faulty.corrupt_offset = targets[0];
    check("COPY: bozuk yazma FLASH_WRITE olarak yakalanir", fwu_activate_run(&act, &outcome) == FWU_ERR_FLASH_WRITE);
    check("... bozulma gerceklesti, 1. sayfa kaydi yazilmis (sayac 1), 2. sayfa kaydi YAZILMAMIS",
          faulty.corruptions == 1u && act.counter == 1u && act.phase == FWU_ACT_PHASE_ACTIVATING);
    faulty.armed = 0;
    expect("hata giderilince yeniden baslat", reboot(&sim, &act, &c), FWU_OK);
    check("... bozuk sayfa baştan yapilir, sonuc TAM YENI", fwu_activate_run(&act, &outcome) == FWU_OK &&
          outcome == FWU_ACT_OUTCOME_ACTIVATED && app_is_new(&sim, 3u));

    /* SWAP: gecici sayfa (adim 0) bozulursa uygulama (A) hala saglam kalmali */
    setup_flash(&sim, 3u);
    faulty_init(&faulty, &sim);
    make_config(&c, &sim, FWU_ACTIVATE_MODE_SWAP, 1u);
    c.flash = &faulty.flash;
    (void)fwu_activate_init(&act, &c);
    (void)fwu_activate_commit(&act, 3u);
    faulty.armed = 1;
    faulty.corrupt_offset = targets[1];
    check("SWAP: gecici sayfa bozuk yazilirsa FLASH_WRITE, uygulama hala TAM ESKI",
          fwu_activate_run(&act, &outcome) == FWU_ERR_FLASH_WRITE && app_is_old(&sim) && act.counter == 0u);
    faulty.armed = 0;
    expect("yeniden baslat", reboot(&sim, &act, &c), FWU_OK);
    check("... duzelince kurulum tamamlanir", fwu_activate_run(&act, &outcome) == FWU_OK &&
          outcome == FWU_ACT_OUTCOME_TRIAL && app_is_new(&sim, 3u) && staging_holds_old(&sim, 3u));

    /* SWAP: adim 2'de (staging'e geri yazma) bozulma: kaynak (gecici) saglam, tekrar yapilir */
    setup_flash(&sim, 3u);
    faulty_init(&faulty, &sim);
    make_config(&c, &sim, FWU_ACTIVATE_MODE_SWAP, 1u);
    c.flash = &faulty.flash;
    (void)fwu_activate_init(&act, &c);
    (void)fwu_activate_commit(&act, 3u);
    faulty.armed = 1;
    faulty.corrupt_offset = targets[2];
    check("SWAP: staging'e geri yazma bozulursa FLASH_WRITE, ilerleme 2 adimda kaldi",
          fwu_activate_run(&act, &outcome) == FWU_ERR_FLASH_WRITE && act.counter == 2u);
    faulty.armed = 0;
    (void)reboot(&sim, &act, &c);
    check("... duzelince kurulum dogru tamamlanir (eski imaj staging'e eksiksiz yazilir)",
          fwu_activate_run(&act, &outcome) == FWU_OK && app_is_new(&sim, 3u) && staging_holds_old(&sim, 3u));
}

static void test_work_sizes(void)
{
    static act_sim_t sim;
    static fwu_activate_t act;
    fwu_activate_config_t c;
    fwu_activate_outcome_e outcome = FWU_ACT_OUTCOME_NONE;
    const uint32_t sizes[] = { 8u, 9u, 16u, 24u, 40u, 64u, 100u, 128u };
    int ok = 1;
    int ops_ok = 1;

    printf("\n--- work tamponu boyutlari ---\n");
    for(unsigned k = 0u; k < 8u; k++) {
        setup_flash(&sim, 3u);
        make_config(&c, &sim, FWU_ACTIVATE_MODE_SWAP, 1u);
        c.work_length = sizes[k];
        (void)fwu_activate_init(&act, &c);
        (void)fwu_activate_commit(&act, 3u);
        fwu_flash_sim_reset(&sim.base);
        if((fwu_activate_run(&act, &outcome) != FWU_OK) || !app_is_new(&sim, 3u) || !staging_holds_old(&sim, 3u)) {
            ok = 0;
        }
        if(fwu_flash_sim_operations(&sim.base) != 110u) {
            ops_ok = 0;                                  /* temel islem sayisi parca boyutundan bagimsiz */
        }
        (void)fwu_activate_request_revert(&act);
        if((boot_clean(&sim, &act, &c, &outcome) != FWU_OK) || !app_is_old(&sim) || !staging_holds_new(&sim, 3u)) {
            ok = 0;
        }
    }
    check("8 farkli work boyutu (8..128; 9 -> 8, 100 -> 64): SWAP kurulum ve geri donus dogru", ok);
    check("temel islem sayisi (granul sayisi) work boyutundan bagimsiz: 110", ops_ok);
}

/* ================================================================== 5b. sayfa sayısı üst sınırı */

/* Çok büyük bölge: 21846 sayfa (sınırı 1 aşar). Sayfa 32 bayt (2 yuvalık journal), granül 16.
 *   uygulama 0..21845 | staging 21846..43691 | journal A 43692 | journal B 43693 | geçici 43694 */
FWU_FLASH_SIM_DEFINE(act_huge, 43695, 32, 16);

#define HUGE_REGION_PAGES   21846u
#define HUGE_PAGE           32u
#define HUGE_REGION         (HUGE_REGION_PAGES * HUGE_PAGE)

static void huge_config(fwu_activate_config_t *c, act_huge_t *sim, fwu_activate_mode_e mode)
{
    c->flash = &sim->base.flash;
    c->mode = mode;
    c->app_offset = 0u;
    c->staging_offset = HUGE_REGION;
    c->region_length = HUGE_REGION;
    c->journal_a_offset = 2u * HUGE_REGION;
    c->journal_b_offset = (2u * HUGE_REGION) + HUGE_PAGE;
    c->journal_length = HUGE_PAGE;
    c->scratch_offset = (2u * HUGE_REGION) + (2u * HUGE_PAGE);
    c->scratch_pages = 1u;
    c->work = g_work;
    c->work_length = 64u;
    c->max_trial_boots = 1u;
    c->require_verify = false;
    c->verify = NULL;
    c->verify_context = NULL;
    c->on_confirmed = NULL;
    c->on_confirmed_context = NULL;
}

static void test_page_limits(void)
{
    static act_huge_t sim;
    static fwu_activate_t act;
    fwu_activate_config_t c;
    const fwu_byte_t swap = (fwu_byte_t)FWU_ACT_FLAG_SWAP;

    printf("\n--- sayfa sayisi ust siniri (21845) ---\n");
    (void)act_huge_init(&sim);
    huge_config(&c, &sim, FWU_ACTIVATE_MODE_COPY);
    expect("21846 sayfalik bolgeyle init (bolge siniri yok, yalniz imaj siniri var)", fwu_activate_init(&act, &c), FWU_OK);
    check("bolge 21846 sayfa", act.region_pages == HUGE_REGION_PAGES);
    expect("commit(21846): sinirin 1 uzerinde", fwu_activate_commit(&act, 21846u), FWU_ERR_INVALID_ARG);
    expect("commit(21845): sinirda gecerli", fwu_activate_commit(&act, 21845u), FWU_OK);
    check("... kayit: 21845 sayfa", act.page_count == 21845u && act.phase == FWU_ACT_PHASE_ACTIVATING);

    (void)act_huge_init(&sim);
    forge_record_at(&sim.base.memory[2u * HUGE_REGION], FWU_ACT_STATE_COMMIT, 0u, (21846u << 16) | 0u);
    expect("kayit: COMMIT 21846 sayfa (bolgeye sigsa da sinirin uzerinde)", fwu_activate_init(&act, &c), FWU_ERR_BAD_STATE);
    (void)act_huge_init(&sim);
    forge_record_at(&sim.base.memory[2u * HUGE_REGION], FWU_ACT_STATE_COMMIT, 0u, (21845u << 16) | 0u);
    expect("kayit: COMMIT 21845 sayfa gecerli", fwu_activate_init(&act, &c), FWU_OK);

    /* SWAP: sayac 16 bit; 3 * 21845 = 65535 en buyuk gecerli adim sayisi */
    huge_config(&c, &sim, FWU_ACTIVATE_MODE_SWAP);
    (void)act_huge_init(&sim);
    forge_record_at(&sim.base.memory[2u * HUGE_REGION], FWU_ACT_STATE_PROGRESS, swap, (21845u << 16) | 65535u);
    expect("kayit: SWAP PROGRESS 65535/65535 adim gecerli", fwu_activate_init(&act, &c), FWU_OK);
    check("... sayac 65535", act.counter == 65535u && act.page_count == 21845u);
}

/* ================================================================== 6. gerçekçi geometri */

static fwu_byte_t g_big_work[512];

static void big_setup(act_big_t *sim)
{
    (void)act_big_init(sim);
    for(uint32_t i = 0u; i < BIG_REGION; i++) {
        sim->base.memory[i] = old_byte(i);
    }
    for(uint32_t i = 0u; i < (2u * BIG_PAGE); i++) {
        sim->base.memory[BIG_STAGING + i] = new_byte(i);
    }
}

static void big_config(fwu_activate_config_t *c, act_big_t *sim)
{
    c->flash = &sim->base.flash;
    c->mode = FWU_ACTIVATE_MODE_SWAP;
    c->app_offset = 0u;
    c->staging_offset = BIG_STAGING;
    c->region_length = BIG_REGION;
    c->journal_a_offset = BIG_JOURNAL_A;
    c->journal_b_offset = BIG_JOURNAL_B;
    c->journal_length = BIG_PAGE;
    c->scratch_offset = BIG_SCRATCH;
    c->scratch_pages = 1u;
    c->work = g_big_work;
    c->work_length = 512u;
    c->max_trial_boots = 1u;
    c->require_verify = false;
    c->verify = NULL;
    c->verify_context = NULL;
    c->on_confirmed = NULL;
    c->on_confirmed_context = NULL;
}

static int big_app_is_new(const act_big_t *sim)
{
    return bytes_match(sim->base.memory, 0u, 0u, 2u * BIG_PAGE, new_byte) &&
           bytes_match(sim->base.memory, 0u, 2u * BIG_PAGE, BIG_PAGE, old_byte);
}

static int big_app_is_old(const act_big_t *sim)
{
    return bytes_match(sim->base.memory, 0u, 0u, BIG_REGION, old_byte);
}

static void test_realistic_geometry(void)
{
    static act_big_t sim;
    static fwu_activate_t act;
    fwu_activate_config_t c;
    fwu_activate_outcome_e outcome = FWU_ACT_OUTCOME_NONE;
    uint32_t total_ops;
    int ok = 1;
    uint32_t points = 0u;

    printf("\n--- gercekci geometri (2048 baytlik sayfa, 128 yuvali journal) ---\n");

    big_setup(&sim);
    big_config(&c, &sim);
    (void)fwu_activate_init(&act, &c);
    (void)fwu_activate_commit(&act, 2u);
    fwu_flash_sim_reset(&sim.base);
    expect("kesintisiz kurulum", fwu_activate_run(&act, &outcome), FWU_OK);
    total_ops = fwu_flash_sim_operations(&sim.base);
    /* elle: 6 adim * (1 silme + 2048/8 = 256 yazma) = 1542; 6 PROGRESS + TRIAL = 7 kayit * 2 = 14 (journal'da 128 yuva, bolge degisimi yok) */
    check("kurulum = 1556 temel islem (1542 + 14)", total_ops == 1556u);
    check("sonuc: TRIAL, uygulama yeni (2 sayfa + eski kuyruk)", outcome == FWU_ACT_OUTCOME_TRIAL && big_app_is_new(&sim));

    /* HER kesinti noktasi (commit kaydi dahil degil: commit onceden yazildi) */
    for(uint32_t n = 0u; n < total_ops; n++) {
        big_setup(&sim);
        big_config(&c, &sim);
        (void)fwu_activate_init(&act, &c);
        (void)fwu_activate_commit(&act, 2u);
        fwu_flash_sim_reset(&sim.base);
        fwu_flash_sim_cut_power_after(&sim.base, n);
        if(fwu_activate_run(&act, &outcome) != FWU_ERR_FLASH_POWER_LOSS) {
            ok = 0;
            continue;
        }
        fwu_flash_sim_reset(&sim.base);
        if((fwu_activate_init(&act, &c) != FWU_OK) || (fwu_activate_run(&act, &outcome) != FWU_OK) ||
           (outcome != FWU_ACT_OUTCOME_TRIAL) || !big_app_is_new(&sim) || (act.phase != FWU_ACT_PHASE_TRIAL)) {
            ok = 0;
        }
        points++;
    }
    check("gercekci geometri: kurulumun HER kesinti noktasinda kurtarma sonunda uygulama TAM YENI", ok);
    check("gercekci geometri: tum noktalar tarandi (1556)", points == 1556u);

    /* geri donus tam akis */
    (void)fwu_activate_request_revert(&act);
    check("gercekci geometri: geri donus sonunda uygulama TAM ESKI", fwu_activate_init(&act, &c) == FWU_OK &&
          fwu_activate_run(&act, &outcome) == FWU_OK && outcome == FWU_ACT_OUTCOME_REVERTED && big_app_is_old(&sim));
}

/* ================================================================== main */

int main(void)
{
    test_init_validation();
    test_copy_basic();
    test_copy_verify_hook();
    test_copy_sweeps();
    test_swap_basic();
    test_swap_trial_boots();
    test_scratch_pages();
    test_swap_sweeps();
    test_forged_states();
    test_silent_write_fault();
    test_flash_errors();
    test_work_sizes();
    test_page_limits();
    test_realistic_geometry();

    printf("\nResult: %s (%d failed)\n", g_failures == 0 ? "ALL TESTS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
