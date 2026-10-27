/**
 * @file    test_session.c
 * @brief   fwu_session (güncelleme oturumu durum makinesi) için kendi kendini kontrol eden test. Sahte flash, sahte
 *          kriptografi, GERÇEK aktivasyon (fwu_activate), GERÇEK parser ve ring buffer üzerinde, bir "host"u
 *          çerçeveler göndererek taklit eder.
 *
 * Kapsam:
 *   1. init doğrulaması ve INFO/KEEPALIVE.
 *   2. Mutlu yol (COPY ve SWAP): indir, doğrula, commit, "yeniden başlat", bootloader uygular; işlem sayıları elle türetildi.
 *   3. DURUM x MESAJ TABLOSU: 7 durum x 14 mesaj, her hücre elle yazılmış beklentiyle (tür, hata kodu, sonraki durum).
 *   4. Yetkilendirme: akış, tek kullanımlık meydan okuma, kilitlenme (sınır değerleri, sarma), kanca hataları.
 *   5. Teklif reddi: her reddedilen manifest için flash'a TEK BAYT dokunulmaz (işlem sayısı ve silme sayaçları değişmez).
 *   6. İndirme kuralları: ofset, uzunluk, tekrar, kısa son parça (granüle dolgu), erken çıkış, boyut iddiaları.
 *   7. Devam (aynı açılışta), doğrulama aşaması (bozuk imaj), ABORT, zaman aşımı (sınır ve sarma), SEQ yankısı.
 *   8. Flash hataları (ayrı kodlarla enjekte: yutulan hata görünür), sessiz bozulma, commit hatası.
 *   9. Güç kesintisi taraması: oturumun HER flash işleminde kesinti; yeniden başlatınca çalışan sürüm ya tam eski ya
 *      tam yeni, ve yeni oturum tamamlanabilir.
 *  10. Düşman akış (fuzz): rastgele ve yarı geçerli çerçeveler; sahte imzalı/bozuk imajlı akışlar ASLA commit edilmez.
 *  11. Gerçek parser + ring buffer ile (rastgele parçalı bayt akışı, araya çöp) uçtan uca.
 *
 * Derleme (session klasöründen):
 *   gcc -Wall -Wextra -Wpedantic -Wconversion -std=c99 -I. -I.. -I../error -I../bytes -I../crc -I../ring_buffer -I../frame -I../flash -I../journal -I../activation -I../manifest -I../crypto -I../verify fwu_session.c fwu_proto.c ../verify/fwu_verify.c ../manifest/fwu_manifest.c ../crypto/fwu_crypto.c ../crypto/fwu_crypto_sim.c ../activation/fwu_activate.c ../journal/fwu_journal_ab.c ../journal/fwu_journal.c ../flash/fwu_flash.c ../flash/fwu_flash_sim.c ../frame/fwu_frame.c ../frame/fwu_decode.c ../frame/fwu_parser.c ../ring_buffer/fwu_ringbuf.c test_session.c -o test_session && ./test_session
 */

#include <stdio.h>
#include "fwu_session.h"
#include "fwu_bytes.h"
#include "fwu_parser.h"
#include "fwu_ringbuf.h"
#include "fwu_crypto_sim.h"
#include "fwu_flash_sim.h"

/* 19 sayfa x 256, granül 8:  0..7 uygulama | 8 manifest | 9..15 imaj (7 sayfa) | 16,17 aktivasyon journal | 18 geçici */
FWU_FLASH_SIM_DEFINE(ss, 19, 256, 8);

#define PAGE            256u
#define MANIFEST_OFFSET 2048u
#define IMAGE_OFFSET    2304u
#define IMAGE_MAX       1792u
#define PRODUCT         12345u
#define CHUNK           64u
#define IMG_SIZE        700u                /* 10 tam parça + 60 baytlık son parça (60 % 8 = 4: granüle dolgu gerekir) */
#define TIMEOUT_MS      1000u
#define LOCKOUT_MS      5000u
#define MAX_FAILURES    3u
#define ERASED          0xFFu

/* Enjekte hata kodları: doğal yolda üretilemeyen (veya ayırt edilebilen) kodlar; yutulan bir hata görünür olsun. */
#define FAULT_ERASE     FWU_ERR_FLASH_ERASE
#define FAULT_WRITE     FWU_ERR_FLASH_NOT_ERASED
#define FAULT_READ      FWU_ERR_OUT_OF_RANGE

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

/* ------------------------------------------------------------------ paket kurma (oturumdan BAGIMSIZ) */

static fwu_byte_t g_pk[3][32];
static fwu_verify_key_t g_keys[2];

static void init_keys(void)
{
    for(uint32_t i = 0u; i < 32u; i++) {
        g_pk[0][i] = (fwu_byte_t)(i + 1u);
        g_pk[1][i] = (fwu_byte_t)(i + 101u);
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
    uint32_t   signer;                  /* imzalayan anahtar indeksi (2 = saldirgan) */
} pkg_t;

static pkg_t good_pkg(void)
{
    pkg_t p;
    p.product = PRODUCT;
    p.version = 25u;
    p.size = IMG_SIZE;
    p.key_id = 1u;
    p.key_version = 1u;
    p.signer = 0u;
    return p;
}

static fwu_byte_t g_manifest[138];              /* host'un elindeki manifest */
static fwu_byte_t g_image[IMAGE_MAX + 64u];     /* host'un elindeki imaj */

static void put_u32(fwu_byte_t *m, uint32_t offset, uint32_t value)
{
    m[offset] = (fwu_byte_t)((value >> 24) & 0xFFu);
    m[offset + 1u] = (fwu_byte_t)((value >> 16) & 0xFFu);
    m[offset + 2u] = (fwu_byte_t)((value >> 8) & 0xFFu);
    m[offset + 3u] = (fwu_byte_t)(value & 0xFFu);
}

static void make_package(const pkg_t *p)
{
    for(uint32_t i = 0u; i < sizeof(g_image) / sizeof(g_image[0]); i++) {
        g_image[i] = img_byte(i);
    }
    g_manifest[0] = 0x46u; g_manifest[1] = 0x57u; g_manifest[2] = 0x4Du; g_manifest[3] = 0x46u;
    g_manifest[4] = 0x01u;
    g_manifest[5] = p->key_id;
    g_manifest[6] = 0x00u; g_manifest[7] = 0x00u;
    put_u32(g_manifest, 8u, p->product);
    put_u32(g_manifest, 12u, p->version);
    g_manifest[16] = (fwu_byte_t)((p->key_version >> 8) & 0xFFu);
    g_manifest[17] = (fwu_byte_t)(p->key_version & 0xFFu);
    put_u32(g_manifest, 18u, p->size);
    fwu_crypto_sim_sha256(g_image, p->size, &g_manifest[22]);
    for(uint32_t i = 0u; i < 20u; i++) {
        g_manifest[54u + i] = (fwu_byte_t)(0x30u + i);
    }
    fwu_crypto_sim_sign(g_pk[p->signer], g_manifest, 74u, &g_manifest[74]);
}

/* ------------------------------------------------------------------ sahte flash'ı saran hata enjektörü */

typedef struct {
    fwu_flash_t          flash;
    fwu_flash_callback_t callback;
    ss_t                *sim;
    uint32_t             fail_erase_offset;     /* bu ofseti kapsayan silme FAULT_ERASE doner */
    uint32_t             fail_write_offset;     /* bu ofseti kapsayan yazma, YAZMADAN FAULT_WRITE doner */
    uint32_t             corrupt_write_offset;  /* bu ofseti kapsayan yazma yapilir, sonra bellekte bir bit bozulur (SESSIZ) */
    uint32_t             fail_read_offset;      /* bu ofseti kapsayan okuma read_fault_code doner */
    fwu_return_e         read_fault_code;       /* okuma hatasinin kodu (varsayilan FAULT_READ) */
} ff_t;

#define NO_OFFSET 0xFFFFFFFFu

static int in_range(uint32_t where, uint32_t offset, uint32_t length)
{
    return (where != NO_OFFSET) && (where >= offset) && (where < (offset + length));
}

static fwu_return_e ff_erase(void *context, uint32_t offset, uint32_t length)
{
    ff_t *f = (ff_t *)context;
    if(in_range(f->fail_erase_offset, offset, length)) {
        return FAULT_ERASE;
    }
    return fwu_flash_erase(&f->sim->base.flash, offset, length);
}

static fwu_return_e ff_write(void *context, uint32_t offset, uint32_t length, const fwu_byte_t *data)
{
    ff_t *f = (ff_t *)context;
    fwu_return_e status;

    if(in_range(f->fail_write_offset, offset, length)) {
        return FAULT_WRITE;
    }
    status = fwu_flash_write(&f->sim->base.flash, offset, length, data);
    if((status == FWU_OK) && in_range(f->corrupt_write_offset, offset, length)) {
        f->sim->base.memory[f->corrupt_write_offset] = (fwu_byte_t)(f->sim->base.memory[f->corrupt_write_offset] ^ 0x04u);
    }
    return status;
}

static fwu_return_e ff_read(void *context, uint32_t offset, uint32_t length, fwu_byte_t *data)
{
    ff_t *f = (ff_t *)context;
    if(in_range(f->fail_read_offset, offset, length)) {
        return f->read_fault_code;
    }
    return fwu_flash_read(&f->sim->base.flash, offset, length, data);
}

static void ff_init(ff_t *f, ss_t *sim)
{
    f->callback.flash_erase = ff_erase;
    f->callback.flash_write = ff_write;
    f->callback.flash_read = ff_read;
    f->flash.context = f;
    f->flash.callback = &f->callback;
    f->flash.geometry = sim->base.flash.geometry;
    f->sim = sim;
    f->fail_erase_offset = NO_OFFSET;
    f->fail_write_offset = NO_OFFSET;
    f->corrupt_write_offset = NO_OFFSET;
    f->fail_read_offset = NO_OFFSET;
    f->read_fault_code = FAULT_READ;
}

/* ------------------------------------------------------------------ yetkilendirme kancasi (test uygulamasi) */

typedef struct {
    uint32_t     begin_calls;
    uint32_t     check_calls;
    bool         fresh;                     /* her begin farkli deger uretir */
    bool         force_begin;
    fwu_return_e begin_status;
    uint32_t     begin_length;
    bool         force_check;
    fwu_return_e check_status;
    fwu_byte_t   last_challenge[FWU_PROTO_AUTH_CHALLENGE_MAX];
    uint32_t     last_challenge_length;
    uint32_t     last_response_length;
    uint32_t     seen_challenge_length;     /* check'e verilen meydan okuma uzunlugu */
    int          seen_challenge_matches;    /* check'e verilen meydan okuma, begin'in verdigiyle ayni mi */
} auth_ctx_t;

static auth_ctx_t g_auth_ctx;

static fwu_return_e auth_begin(void *context, fwu_byte_t *challenge, uint32_t capacity, uint32_t *length)
{
    auth_ctx_t *a = (auth_ctx_t *)context;

    a->begin_calls++;
    if(a->force_begin) {
        return a->begin_status;
    }
    *length = a->begin_length;
    for(uint32_t i = 0u; (i < a->begin_length) && (i < capacity); i++) {
        challenge[i] = (fwu_byte_t)((a->fresh ? (i + (a->begin_calls * 16u)) : (i + 1u)) & 0xFFu);
        a->last_challenge[i] = challenge[i];
    }
    a->last_challenge_length = a->begin_length;
    return FWU_OK;
}

static fwu_return_e auth_check(void *context, const fwu_byte_t *challenge, uint32_t challenge_length,
                               const fwu_byte_t *response, uint32_t response_length)
{
    auth_ctx_t *a = (auth_ctx_t *)context;
    int match = (challenge_length == a->last_challenge_length);

    a->check_calls++;
    a->last_response_length = response_length;
    a->seen_challenge_length = challenge_length;
    for(uint32_t i = 0u; (i < challenge_length) && match; i++) {
        match = (challenge[i] == a->last_challenge[i]);
    }
    a->seen_challenge_matches = match;
    if(a->force_check) {
        return a->check_status;
    }
    /* Yanit = meydan okuma XOR 0x5A (test icin; gercek gerceklemenin yerine). */
    if(response_length != challenge_length) {
        return FWU_ERR_BAD_SIGNATURE;
    }
    for(uint32_t i = 0u; i < challenge_length; i++) {
        if((response[i] & 0xFFu) != (fwu_byte_t)((challenge[i] ^ 0x5Au) & 0xFFu)) {
            return FWU_ERR_BAD_SIGNATURE;
        }
    }
    return FWU_OK;
}

static const fwu_session_auth_t g_auth = { auth_begin, auth_check, &g_auth_ctx };

/* ------------------------------------------------------------------ dunya: flash + aktivasyon + oturum */

static ss_t g_sim;
static ff_t g_ff;
static fwu_crypto_sim_t g_cs;
static fwu_verify_config_t g_vc;
static fwu_activate_config_t g_ac;
static fwu_activate_t g_act;
static fwu_session_config_t g_cfg;
static fwu_session_t g_s;
static fwu_byte_t g_work[CHUNK];
static fwu_byte_t g_vwork[64];
static fwu_byte_t g_awork[64];
static uint32_t g_now;
static uint32_t g_granule = 8u;             /* sahte flash'in yazma granulu (test_granules degistirir) */

/** Sahte flash'i g_granule ile baslatir (silinmis). */
static fwu_return_e sim_init(void)
{
    return fwu_flash_sim_init_raw(&g_sim.base, g_sim.memory, g_sim.erase_counts, 19u, PAGE, g_granule);
}

static void fill_old_app(void)
{
    for(uint32_t i = 0u; i < 8u * PAGE; i++) {
        g_sim.base.memory[i] = old_byte(i);
    }
}

static void make_verify_cfg(void)
{
    g_vc.crypto = &g_cs.crypto;
    g_vc.manifest_offset = MANIFEST_OFFSET;
    g_vc.image_offset = IMAGE_OFFSET;
    g_vc.max_image_size = IMAGE_MAX;
    g_vc.product_id = PRODUCT;
    g_vc.min_image_version = 25u;
    g_vc.min_key_version = 1u;
    g_vc.keys = g_keys;
    g_vc.key_count = 2u;
    g_vc.require_signature = true;
    g_vc.work = g_vwork;
    g_vc.work_length = 64u;
}

static void make_act_cfg(fwu_activate_mode_e mode)
{
    g_ac.flash = &g_ff.flash;
    g_ac.mode = mode;
    g_ac.app_offset = 0u;
    g_ac.staging_offset = IMAGE_OFFSET;
    g_ac.region_length = IMAGE_MAX;
    g_ac.journal_a_offset = 16u * PAGE;
    g_ac.journal_b_offset = 17u * PAGE;
    g_ac.journal_length = PAGE;
    g_ac.scratch_offset = 18u * PAGE;
    g_ac.scratch_pages = 1u;
    g_ac.work = g_awork;
    g_ac.work_length = 64u;
    g_ac.max_trial_boots = 2u;
    g_ac.require_verify = true;
    g_ac.verify = fwu_verify_activation_hook;
    g_ac.verify_context = &g_vc;
    g_ac.on_confirmed = NULL;
    g_ac.on_confirmed_context = NULL;
}

static void make_session_cfg(bool with_auth)
{
    g_cfg.flash = &g_ff.flash;
    g_cfg.activate = &g_act;
    g_cfg.verify = &g_vc;
    g_cfg.auth = with_auth ? &g_auth : NULL;
    g_cfg.require_auth = with_auth;
    g_cfg.running_image_version = 20u;
    g_cfg.chunk_size = CHUNK;
    g_cfg.idle_timeout_ms = TIMEOUT_MS;
    g_cfg.auth_max_failures = MAX_FAILURES;
    g_cfg.auth_lockout_ms = LOCKOUT_MS;
    g_cfg.work = g_work;
    g_cfg.work_length = CHUNK;
}

static void reset_auth_ctx(void)
{
    g_auth_ctx.begin_calls = 0u;
    g_auth_ctx.check_calls = 0u;
    g_auth_ctx.fresh = false;
    g_auth_ctx.force_begin = false;
    g_auth_ctx.begin_status = FWU_OK;
    g_auth_ctx.begin_length = 16u;
    g_auth_ctx.force_check = false;
    g_auth_ctx.check_status = FWU_OK;
    g_auth_ctx.last_challenge_length = 0u;
    g_auth_ctx.last_response_length = 0u;
    g_auth_ctx.seen_challenge_length = 0u;
    g_auth_ctx.seen_challenge_matches = 0;
    for(uint32_t i = 0u; i < FWU_PROTO_AUTH_CHALLENGE_MAX; i++) {
        g_auth_ctx.last_challenge[i] = 0u;
    }
}

/** Temiz dunya: bos flash, eski uygulama, paket (gecerli), aktivasyon bosta, oturum bosta. */
static void world(bool with_auth, fwu_activate_mode_e mode)
{
    (void)sim_init();
    init_keys();
    (void)fwu_crypto_sim_init(&g_cs);
    reset_auth_ctx();
    ff_init(&g_ff, &g_sim);
    fill_old_app();
    make_verify_cfg();
    make_act_cfg(mode);
    (void)fwu_activate_init(&g_act, &g_ac);
    make_session_cfg(with_auth);
    (void)fwu_session_init(&g_s, &g_cfg);
    make_package(&(pkg_t){ PRODUCT, 25u, IMG_SIZE, 1u, 1u, 0u });
    g_now = 1000u;
}

/* ------------------------------------------------------------------ host: istek gonder, yaniti coz */

typedef struct {
    fwu_return_e rc;                    /* on_frame/poll donusu */
    size_t       length;                /* yanit cerceve boyutu (0 = yanit yok) */
    int          decoded;               /* yanit cercevesi cozuldu mu */
    fwu_byte_t   type;
    fwu_byte_t   seq;
    fwu_byte_t   payload[FWU_FRAME_MAX_PAYLOAD];
    size_t       payload_length;
} resp_t;

static resp_t g_r;
static fwu_byte_t g_last_type;

static void decode_response(const fwu_byte_t *buf, size_t n)
{
    fwu_frame_view_t view;

    g_r.length = n;
    g_r.decoded = 0;
    g_r.payload_length = 0u;
    if(n == 0u) {
        return;
    }
    if(fwu_frame_decode(buf, n, &view) == FWU_OK) {
        g_r.decoded = (view.frame_length == n) ? 1 : 0;
        g_r.type = view.type;
        g_r.seq = view.seq;
        g_r.payload_length = view.payload_length;
        for(size_t i = 0u; i < view.payload_length; i++) {
            g_r.payload[i] = view.payload[i];
        }
    }
}

/** Istegi gercek cerceve olarak kurar, gercek decode'dan gecirir, oturuma verir; yaniti g_r'ye koyar. */
static void tx(fwu_byte_t type, fwu_byte_t seq, const fwu_byte_t *payload, size_t payload_length)
{
    fwu_byte_t frame[FWU_FRAME_MAX_SIZE];
    fwu_byte_t out[FWU_FRAME_MAX_SIZE];
    size_t frame_length = 0u;
    size_t out_length = 0u;
    fwu_frame_view_t view;

    g_last_type = type;
    (void)fwu_frame_encode(type, seq, payload, payload_length, frame, sizeof(frame), &frame_length);
    (void)fwu_frame_decode(frame, frame_length, &view);
    g_r.rc = fwu_session_on_frame(&g_s, &view, g_now, out, sizeof(out), &out_length);
    decode_response(out, (g_r.rc == FWU_OK) ? out_length : 0u);
}

static void tx1(fwu_byte_t type, fwu_byte_t seq)
{
    const fwu_byte_t one[1] = { 0u };
    tx(type, seq, one, 1u);
}

static void poll_session(void)
{
    fwu_byte_t out[FWU_FRAME_MAX_SIZE];
    size_t out_length = 0u;

    g_r.rc = fwu_session_poll(&g_s, g_now, out, sizeof(out), &out_length);
    decode_response(out, (g_r.rc == FWU_OK) ? out_length : 0u);
}

static void tx_u32(fwu_byte_t type, fwu_byte_t seq, uint32_t value)
{
    fwu_byte_t p[4];
    size_t n = 0u;
    (void)fwu_proto_encode_u32(value, p, 4u, &n);
    tx(type, seq, p, n);
}

static void tx_chunk(uint32_t offset, const fwu_byte_t *data, size_t length)
{
    fwu_byte_t p[FWU_FRAME_MAX_PAYLOAD];
    size_t n = 0u;
    (void)fwu_proto_encode_data(offset, data, length, p, sizeof(p), &n);
    tx(FWU_MSG_TRANSFER_DATA, 1u, p, n);
}

static void tx_manifest(void)
{
    tx(FWU_MSG_REQUEST_DOWNLOAD, 1u, g_manifest, 138u);
}

static int is_type(fwu_byte_t type)
{
    return (g_r.rc == FWU_OK) && g_r.decoded && (g_r.type == type);
}

/** Yanit ERROR, kod ve istek turu beklenen mi? */
static int is_error(fwu_return_e code)
{
    return (g_r.rc == FWU_OK) && g_r.decoded && (g_r.type == FWU_MSG_ERROR) && (g_r.payload_length == 2u) &&
           (g_r.payload[0] == (fwu_byte_t)code) && (g_r.payload[1] == g_last_type);
}

static uint32_t ack_offset(void)
{
    uint32_t v = 0xFFFFFFFFu;
    if(is_type(FWU_MSG_DATA_ACK)) {
        (void)fwu_proto_parse_u32(g_r.payload, g_r.payload_length, &v);
    }
    return v;
}

static void authenticate(void)
{
    fwu_byte_t resp[16];
    tx1(FWU_MSG_AUTH_REQ, 1u);
    for(uint32_t i = 0u; i < 16u; i++) {
        resp[i] = (fwu_byte_t)((g_r.payload[i] ^ 0x5Au) & 0xFFu);
    }
    tx(FWU_MSG_AUTH_RESP, 2u, resp, 16u);
}

/** Parca `index`'i gonderir (son parca kisa). */
static void send_chunk(uint32_t index, uint32_t size)
{
    const uint32_t offset = index * CHUNK;
    uint32_t n = size - offset;

    if(n > CHUNK) {
        n = CHUNK;
    }
    tx_chunk(offset, &g_image[offset], n);
}

/** Parcalari [from, to) arasinda gonderir; hepsi DATA_ACK ve beklenen ofsetle onaylandiysa 1. */
static int send_chunks(uint32_t from, uint32_t to, uint32_t size)
{
    for(uint32_t i = from; i < to; i++) {
        uint32_t expected = (i + 1u) * CHUNK;

        if(expected > size) {
            expected = size;
        }
        send_chunk(i, size);
        if(ack_offset() != expected) {
            return 0;
        }
    }
    return 1;
}

#define CHUNK_COUNT ((IMG_SIZE + CHUNK - 1u) / CHUNK)       /* 11 */

static int flash_image_matches(uint32_t size)
{
    for(uint32_t i = 0u; i < size; i++) {
        if((g_sim.base.memory[IMAGE_OFFSET + i] & 0xFFu) != img_byte(i)) {
            return 0;
        }
    }
    return 1;
}

static int app_is_old(void)
{
    for(uint32_t i = 0u; i < 8u * PAGE; i++) {
        if((g_sim.base.memory[i] & 0xFFu) != old_byte(i)) {
            return 0;
        }
    }
    return 1;
}

/** Yeni uygulama: ilk 700 = imaj, 700..767 = 0xFF (sayfa dolgusu), gerisi eski. */
static int app_is_new(void)
{
    for(uint32_t i = 0u; i < IMG_SIZE; i++) {
        if((g_sim.base.memory[i] & 0xFFu) != img_byte(i)) {
            return 0;
        }
    }
    for(uint32_t i = IMG_SIZE; i < 3u * PAGE; i++) {
        if((g_sim.base.memory[i] & 0xFFu) != ERASED) {
            return 0;
        }
    }
    for(uint32_t i = 3u * PAGE; i < 8u * PAGE; i++) {
        if((g_sim.base.memory[i] & 0xFFu) != old_byte(i)) {
            return 0;
        }
    }
    return 1;
}

/** Bootloader: guc geri gelir, aktivasyon yeniden acilir ve bekleyen isi calistirir. */
static fwu_return_e bootloader_run(fwu_activate_outcome_e *outcome)
{
    fwu_flash_sim_reset(&g_sim.base);
    if(fwu_activate_init(&g_act, &g_ac) != FWU_OK) {
        return FWU_ERR_BAD_STATE;
    }
    return fwu_activate_run(&g_act, outcome);
}

/** Tum akis: HELLO atlanir; manifest, parcalar, cikis, poll, aktivasyon. Hepsi basarili ise 1. */
static int full_flow(uint32_t size)
{
    tx_manifest();
    if(!is_type(FWU_MSG_DOWNLOAD_ACCEPTED)) {
        return 0;
    }
    if(!send_chunks(0u, (size + CHUNK - 1u) / CHUNK, size)) {
        return 0;
    }
    tx_u32(FWU_MSG_TRANSFER_EXIT, 3u, size);
    if(!is_type(FWU_MSG_EXIT_PENDING)) {
        return 0;
    }
    poll_session();
    if(!is_type(FWU_MSG_VERIFY_RESULT) || (g_r.payload_length != 1u) || (g_r.payload[0] != 0u)) {
        return 0;
    }
    tx_u32(FWU_MSG_ACTIVATE, 4u, size);
    return is_type(FWU_MSG_ACTIVATE_OK) && fwu_session_reboot_requested(&g_s);
}

/* ------------------------------------------------------------------ 1. init, INFO, KEEPALIVE */

static void test_init(void)
{
    fwu_session_config_t c;
    static fwu_session_t s;
    static fwu_session_t poison;
    fwu_byte_t out[FWU_FRAME_MAX_SIZE];
    size_t n = 0u;
    fwu_frame_view_t view;
    const fwu_byte_t one[1] = { 0u };

    printf("\n--- init dogrulamasi ---\n");
    world(false, FWU_ACTIVATE_MODE_COPY);
    c = g_cfg;
    expect("gecerli yapilandirma", fwu_session_init(&s, &c), FWU_OK);
    expect("session NULL", fwu_session_init(NULL, &c), FWU_ERR_INVALID_ARG);
    expect("config NULL", fwu_session_init(&s, NULL), FWU_ERR_INVALID_ARG);

    c = g_cfg; c.flash = NULL;
    expect("flash NULL", fwu_session_init(&s, &c), FWU_ERR_INVALID_ARG);
    c = g_cfg; c.activate = NULL;
    expect("activate NULL", fwu_session_init(&s, &c), FWU_ERR_INVALID_ARG);
    c = g_cfg; c.verify = NULL;
    expect("verify NULL", fwu_session_init(&s, &c), FWU_ERR_INVALID_ARG);
    c = g_cfg; c.work = NULL;
    expect("work NULL", fwu_session_init(&s, &c), FWU_ERR_INVALID_ARG);
    c = g_cfg; c.work_length = CHUNK - 1u;
    expect("work_length < chunk_size", fwu_session_init(&s, &c), FWU_ERR_INVALID_ARG);
    c = g_cfg; c.work_length = CHUNK;
    expect("work_length == chunk_size", fwu_session_init(&s, &c), FWU_OK);

    c = g_cfg; c.chunk_size = 0u;
    expect("chunk_size 0", fwu_session_init(&s, &c), FWU_ERR_INVALID_ARG);
    c = g_cfg; c.chunk_size = 4u; c.work_length = 4u;
    expect("chunk_size 4 (granul 8'in kati degil)", fwu_session_init(&s, &c), FWU_ERR_INVALID_ARG);
    c = g_cfg; c.chunk_size = 72u; c.work_length = 72u;
    expect("chunk_size 72 (8'in kati ama sayfa 256'yi bolmuyor)", fwu_session_init(&s, &c), FWU_ERR_INVALID_ARG);
    c = g_cfg; c.chunk_size = 8u;
    expect("chunk_size 8 (granul = en kucuk)", fwu_session_init(&s, &c), FWU_OK);
    c = g_cfg; c.chunk_size = 256u; c.work_length = 256u;
    expect("chunk_size 256 (= sayfa)", fwu_session_init(&s, &c), FWU_OK);
    c = g_cfg; c.chunk_size = 512u; c.work_length = 512u;
    expect("chunk_size 512 (sayfayi asar)", fwu_session_init(&s, &c), FWU_ERR_INVALID_ARG);

    c = g_cfg; c.idle_timeout_ms = 0u;
    expect("zaman asimi 0", fwu_session_init(&s, &c), FWU_ERR_INVALID_ARG);
    c = g_cfg; c.idle_timeout_ms = 1u;
    expect("zaman asimi 1", fwu_session_init(&s, &c), FWU_OK);

    c = g_cfg; c.require_auth = true; c.auth = NULL;
    expect("require_auth ama auth NULL", fwu_session_init(&s, &c), FWU_ERR_INVALID_ARG);
    c = g_cfg; c.auth = &g_auth; c.require_auth = true;
    expect("require_auth ve auth var", fwu_session_init(&s, &c), FWU_OK);
    {
        fwu_session_auth_t a = g_auth;
        a.begin = NULL;
        c = g_cfg; c.auth = &a;
        expect("auth.begin NULL", fwu_session_init(&s, &c), FWU_ERR_INVALID_ARG);
        a = g_auth; a.check = NULL;
        expect("auth.check NULL", fwu_session_init(&s, &c), FWU_ERR_INVALID_ARG);
    }
    c = g_cfg; c.auth = &g_auth; c.auth_max_failures = 0u;
    expect("auth var, max_failures 0", fwu_session_init(&s, &c), FWU_ERR_INVALID_ARG);
    c = g_cfg; c.auth = &g_auth; c.auth_lockout_ms = 0u;
    expect("auth var, lockout 0", fwu_session_init(&s, &c), FWU_ERR_INVALID_ARG);
    c = g_cfg; c.auth = NULL; c.auth_max_failures = 0u; c.auth_lockout_ms = 0u;
    expect("auth yok: kilit alanlari yok sayilir", fwu_session_init(&s, &c), FWU_OK);

    /* bolgeler */
    {
        fwu_verify_config_t v = g_vc;
        c = g_cfg; c.verify = &v;
        v.manifest_offset = MANIFEST_OFFSET + 8u;
        expect("manifest ofseti hizasiz", fwu_session_init(&s, &c), FWU_ERR_INVALID_ARG);
        v = g_vc; v.image_offset = IMAGE_OFFSET + 8u;
        expect("imaj ofseti hizasiz", fwu_session_init(&s, &c), FWU_ERR_INVALID_ARG);
        v = g_vc; v.manifest_offset = 8u;
        expect("manifest ofseti hizasiz ve imaj bolgesinden UZAK (yalniz hiza kurali devrede)", fwu_session_init(&s, &c), FWU_ERR_INVALID_ARG);
        v = g_vc; v.image_offset = 20u * PAGE;
        expect("imaj ofseti flash'in disinda (5120 > 4864)", fwu_session_init(&s, &c), FWU_ERR_OUT_OF_RANGE);
        v = g_vc; v.image_offset = 19u * PAGE;
        expect("imaj ofseti tam flash sonunda (4864): bolge sigmaz", fwu_session_init(&s, &c), FWU_ERR_OUT_OF_RANGE);
        v = g_vc; v.max_image_size = 0u;
        expect("imaj kapasitesi 0", fwu_session_init(&s, &c), FWU_ERR_INVALID_ARG);
        v = g_vc; v.max_image_size = IMAGE_MAX - 8u;
        expect("imaj kapasitesi sayfa kati degil", fwu_session_init(&s, &c), FWU_ERR_INVALID_ARG);
        v = g_vc; v.max_image_size = IMAGE_MAX + (4u * PAGE);
        expect("imaj bolgesi flash disina tasar", fwu_session_init(&s, &c), FWU_ERR_OUT_OF_RANGE);
        v = g_vc; v.manifest_offset = 18u * PAGE + PAGE;
        expect("manifest sayfasi flash disinda", fwu_session_init(&s, &c), FWU_ERR_OUT_OF_RANGE);
        v = g_vc; v.manifest_offset = IMAGE_OFFSET + PAGE;
        expect("manifest sayfasi imaj bolgesinin ICINDE (ortusuyor)", fwu_session_init(&s, &c), FWU_ERR_INVALID_ARG);
        v = g_vc; v.manifest_offset = IMAGE_OFFSET;
        expect("manifest sayfasi imaj bolgesinin ilk sayfasi (ortusuyor)", fwu_session_init(&s, &c), FWU_ERR_INVALID_ARG);
        v = g_vc; v.manifest_offset = IMAGE_OFFSET + IMAGE_MAX;
        expect("manifest sayfasi imaj bolgesinin hemen arkasinda (ortusmuyor)", fwu_session_init(&s, &c), FWU_OK);
        v = g_vc; v.manifest_offset = IMAGE_OFFSET - PAGE;
        expect("manifest sayfasi imaj bolgesinin hemen oncesinde (ortusmuyor)", fwu_session_init(&s, &c), FWU_OK);
        v = g_vc; v.image_offset = IMAGE_OFFSET - PAGE; v.manifest_offset = MANIFEST_OFFSET;
        expect("imaj bolgesi manifest sayfasina taşar (ortusuyor)", fwu_session_init(&s, &c), FWU_ERR_INVALID_ARG);
    }

    /* geometri */
    {
        fwu_flash_t f = g_ff.flash;
        f.callback = NULL;
        c = g_cfg; c.flash = &f;
        expect("gecersiz flash nesnesi (tablo yok)", fwu_session_init(&s, &c), FWU_ERR_FLASH_GEOMETRY);
        f = g_ff.flash;
        f.geometry.write_granule = 128u;
        f.geometry.page_length = 256u;
        expect("granul 128 (> 64)", fwu_session_init(&s, &c), FWU_ERR_FLASH_GEOMETRY);
    }

    /* farkli sayfa boyutlari: parca siniri (508) ve manifest'in sayfaya sigmasi (yalniz bu geometrilerde gorunur) */
    {
        fwu_flash_t f = g_ff.flash;
        fwu_verify_config_t v = g_vc;

        f.geometry.page_length = 1024u;
        f.geometry.flash_length = 8192u;
        v.manifest_offset = 0u; v.image_offset = 1024u; v.max_image_size = 4096u;
        c = g_cfg; c.flash = &f; c.verify = &v; c.chunk_size = 256u; c.work_length = 256u;
        expect("sayfa 1024: parca 256", fwu_session_init(&s, &c), FWU_OK);
        c.chunk_size = 504u; c.work_length = 504u;
        expect("sayfa 1024: parca 504 (granul kati, <= 508 ama sayfayi bolmuyor)", fwu_session_init(&s, &c), FWU_ERR_INVALID_ARG);
        c.chunk_size = 512u; c.work_length = 512u;
        expect("sayfa 1024: parca 512 (sayfayi boluyor, granul kati ama > 508)", fwu_session_init(&s, &c), FWU_ERR_INVALID_ARG);
        c.chunk_size = 1024u; c.work_length = 1024u;
        expect("sayfa 1024: parca 1024 (> 508)", fwu_session_init(&s, &c), FWU_ERR_INVALID_ARG);

        f = g_ff.flash;
        f.geometry.page_length = 144u;
        f.geometry.flash_length = 144u * 34u;
        v = g_vc;
        v.manifest_offset = 144u * 14u; v.image_offset = 144u * 15u; v.max_image_size = 144u * 10u;
        c = g_cfg; c.flash = &f; c.verify = &v; c.chunk_size = 48u; c.work_length = 48u;
        expect("sayfa 144: manifest (138 -> 144 bayt) TAM sigar", fwu_session_init(&s, &c), FWU_OK);
        f.geometry.page_length = 136u;
        f.geometry.flash_length = 136u * 36u;
        v.manifest_offset = 136u * 14u; v.image_offset = 136u * 15u; v.max_image_size = 136u * 10u;
        c.chunk_size = 8u; c.work_length = 8u;
        expect("sayfa 136: manifest (144 bayt yuvarlanmis) sayfaya SIGMAZ", fwu_session_init(&s, &c), FWU_ERR_INVALID_ARG);
    }

    /* basarisiz init oturumu kullanilamaz birakir */
    poison.initialized = true;
    c = g_cfg; c.flash = NULL;
    (void)fwu_session_init(&poison, &c);
    {
        fwu_byte_t frame[FWU_FRAME_MAX_SIZE];
        size_t fl = 0u;
        (void)fwu_frame_encode(FWU_MSG_HELLO, 1u, one, 1u, frame, sizeof(frame), &fl);
        (void)fwu_frame_decode(frame, fl, &view);
        expect("basarisiz init sonrasi on_frame: INVALID_ARG", fwu_session_on_frame(&poison, &view, 0u, out, sizeof(out), &n), FWU_ERR_INVALID_ARG);
        expect("basarisiz init sonrasi poll: INVALID_ARG", fwu_session_poll(&poison, 0u, out, sizeof(out), &n), FWU_ERR_INVALID_ARG);
    }
    check("... durum sorgusu BOSTA, yeniden baslatma bayragi yok", fwu_session_get_state(&poison) == FWU_SESSION_IDLE && !fwu_session_reboot_requested(&poison));
    check("sorgular NULL'a dayanikli", fwu_session_get_state(NULL) == FWU_SESSION_IDLE && !fwu_session_reboot_requested(NULL));
}

static void test_hello_keepalive(void)
{
    fwu_proto_info_t info;
    fwu_proto_info_t info2;
    fwu_byte_t k;

    printf("\n--- HELLO / INFO ve KEEPALIVE ---\n");
    world(false, FWU_ACTIVATE_MODE_COPY);
    tx1(FWU_MSG_HELLO, 7u);
    check("HELLO -> INFO, SEQ yankilandi (7)", is_type(FWU_MSG_INFO) && g_r.seq == 7u);
    expect("... INFO ayristirilir", fwu_proto_parse_info(g_r.payload, g_r.payload_length, &info), FWU_OK);
    check("... surum 1, imza zorunlu (bit1), yetkilendirme YOK (bit0 = 0), urun 12345, calisan surum 20, parca 64, durum BOSTA",
          info.proto_version == 1u && info.flags == FWU_PROTO_FLAG_SIGNED && info.product_id == PRODUCT &&
          info.running_image_version == 20u && info.max_chunk == CHUNK && info.state == FWU_PROTO_STATE_IDLE);

    world(true, FWU_ACTIVATE_MODE_COPY);
    tx1(FWU_MSG_HELLO, 1u);
    (void)fwu_proto_parse_info(g_r.payload, g_r.payload_length, &info2);
    check("yetkilendirme kancasi var: bayraklar 0x03", info2.flags == (FWU_PROTO_FLAG_SIGNED | FWU_PROTO_FLAG_AUTH_REQUIRED));
    g_vc.require_signature = false;
    tx1(FWU_MSG_HELLO, 1u);
    (void)fwu_proto_parse_info(g_r.payload, g_r.payload_length, &info2);
    check("DEV (imza zorunlu degil), yetkilendirme var: bayraklar 0x01", info2.flags == FWU_PROTO_FLAG_AUTH_REQUIRED);
    g_vc.require_signature = true;

    {
        const fwu_byte_t two[2] = { 1u, 2u };
        tx(FWU_MSG_HELLO, 1u, two, 2u);
        check("HELLO payload 2 bayt: BAD_LENGTH", is_error(FWU_ERR_BAD_LENGTH));
        tx(FWU_MSG_KEEPALIVE, 1u, two, 2u);
        check("KEEPALIVE payload 2 bayt: BAD_LENGTH", is_error(FWU_ERR_BAD_LENGTH));
        tx(FWU_MSG_ABORT, 1u, two, 2u);
        check("ABORT payload 2 bayt: BAD_LENGTH", is_error(FWU_ERR_BAD_LENGTH));
        tx(FWU_MSG_AUTH_REQ, 1u, two, 2u);
        check("AUTH_REQ payload 2 bayt: BAD_LENGTH", is_error(FWU_ERR_BAD_LENGTH));
    }

    tx1(FWU_MSG_KEEPALIVE, 9u);
    k = g_r.payload[0];
    check("KEEPALIVE -> KEEPALIVE_OK(durum BOSTA), SEQ 9", is_type(FWU_MSG_KEEPALIVE_OK) && g_r.seq == 9u && g_r.payload_length == 1u && k == FWU_PROTO_STATE_IDLE);
}

/* ------------------------------------------------------------------ 2. mutlu yol */

static void test_happy_path(void)
{
    fwu_activate_outcome_e outcome = FWU_ACT_OUTCOME_NONE;
    fwu_proto_accepted_t acc;

    printf("\n--- mutlu yol (COPY): indir, dogrula, commit, yeniden baslat, bootloader uygular ---\n");
    world(false, FWU_ACTIVATE_MODE_COPY);
    tx_manifest();
    check("REQUEST_DOWNLOAD -> DOWNLOAD_ACCEPTED", is_type(FWU_MSG_DOWNLOAD_ACCEPTED));
    expect("... ayristirilir", fwu_proto_parse_accepted(g_r.payload, g_r.payload_length, &acc), FWU_OK);
    check("... parca 64, mod 0 (dur-bekle), zaman asimi 1 sn, sonraki ofset 0",
          acc.chunk_size == CHUNK && acc.mode == FWU_PROTO_MODE_HANDSHAKE && acc.timeout_s == 1u && acc.next_offset == 0u);
    check("... durum INDIRIYOR", fwu_session_get_state(&g_s) == FWU_SESSION_DOWNLOADING);
    check("... manifest sayfasi silindi (1 kez), 144 bayt (138 + 6 dolgu) yazildi ve 6 dolgu bayti 0xFF",
          g_sim.base.erase_counts[8] == 1u && g_sim.base.operations_done == 19u &&
          (g_sim.base.memory[MANIFEST_OFFSET + 137u] & 0xFFu) == g_manifest[137] &&
          (g_sim.base.memory[MANIFEST_OFFSET + 138u] & 0xFFu) == ERASED && (g_sim.base.memory[MANIFEST_OFFSET + 143u] & 0xFFu) == ERASED);
    check("... manifest baytlari birebir flash'ta", (g_sim.base.memory[MANIFEST_OFFSET] & 0xFFu) == 0x46u &&
          (g_sim.base.memory[MANIFEST_OFFSET + 74u] & 0xFFu) == g_manifest[74] && (g_sim.base.memory[MANIFEST_OFFSET + 100u] & 0xFFu) == g_manifest[100]);

    check("11 parca: hepsi DATA_ACK, ofsetler 64, 128, ... 640, 700", send_chunks(0u, CHUNK_COUNT, IMG_SIZE));
    check("... flash'ta imaj birebir (700 bayt)", flash_image_matches(IMG_SIZE));
    check("... son parcanin granul dolgusu: 700..703 = 0xFF", (g_sim.base.memory[IMAGE_OFFSET + 700u] & 0xFFu) == ERASED &&
          (g_sim.base.memory[IMAGE_OFFSET + 703u] & 0xFFu) == ERASED);
    check("... her imaj sayfasi TAM 1 kez silindi (3 sayfa), 4. sayfa dokunulmadi",
          g_sim.base.erase_counts[9] == 1u && g_sim.base.erase_counts[10] == 1u && g_sim.base.erase_counts[11] == 1u &&
          g_sim.base.erase_counts[12] == 0u);
    check("... toplam flash islemi 110 (manifest 1 silme + 18 yazma; imaj 3 silme + 88 granul yazma)", g_sim.base.operations_done == 110u);
    check("... durum INDIRIYOR", fwu_session_get_state(&g_s) == FWU_SESSION_DOWNLOADING);

    tx_u32(FWU_MSG_TRANSFER_EXIT, 33u, IMG_SIZE);
    check("TRANSFER_EXIT -> EXIT_PENDING (hemen), SEQ 33", is_type(FWU_MSG_EXIT_PENDING) && g_r.seq == 33u && g_r.payload[0] == 1u);
    check("... durum DOGRULUYOR; imza yalniz teklifte (1 kez) dogrulandi, imaj ozeti henuz HESAPLANMADI", fwu_session_get_state(&g_s) == FWU_SESSION_VERIFYING && g_cs.verify_calls == 1u && g_cs.begin_calls == 0u);
    poll_session();
    check("poll -> VERIFY_RESULT(0), SEQ = EXIT'in SEQ'i (33)", is_type(FWU_MSG_VERIFY_RESULT) && g_r.seq == 33u && g_r.payload_length == 1u && g_r.payload[0] == 0u);
    check("... durum HAZIR; imza toplam 2 kez (teklif + tam dogrulama), imaj ozeti 1 kez", fwu_session_get_state(&g_s) == FWU_SESSION_READY && g_cs.verify_calls == 2u && g_cs.finish_calls == 1u);
    poll_session();
    check("ikinci poll: gonderilecek bir sey yok", g_r.rc == FWU_OK && g_r.length == 0u);
    check("... bu ana kadar uygulamaya ve journal'a HIC dokunulmadi", app_is_old() && g_sim.base.erase_counts[16] == 0u && g_sim.base.erase_counts[17] == 0u &&
          g_sim.base.erase_counts[0] == 0u && g_sim.base.erase_counts[18] == 0u && !fwu_session_reboot_requested(&g_s));

    tx_u32(FWU_MSG_ACTIVATE, 44u, IMG_SIZE);
    check("ACTIVATE -> ACTIVATE_OK, SEQ 44, durum COMMIT, yeniden baslatma istendi",
          is_type(FWU_MSG_ACTIVATE_OK) && g_r.seq == 44u && fwu_session_get_state(&g_s) == FWU_SESSION_COMMITTED && fwu_session_reboot_requested(&g_s));
    check("... commit yazildi ama uygulama hala ESKI (commit en son, uygulamaya dokunulmaz)", app_is_old() && g_act.phase == FWU_ACT_PHASE_ACTIVATING);
    check("... aktivasyon: son durum COMMIT, sayfa sayisi 3 (700 / 256 yukari)", g_act.last_state == FWU_ACT_STATE_COMMIT && g_act.page_count == 3u);

    expect("bootloader: run", bootloader_run(&outcome), FWU_OK);
    check("... ACTIVATED, uygulama = yeni imaj", outcome == FWU_ACT_OUTCOME_ACTIVATED && app_is_new());

    /* SWAP */
    printf("\n--- mutlu yol (SWAP + deneme onyuklemesi) ---\n");
    world(false, FWU_ACTIVATE_MODE_SWAP);
    check("tum akis (SWAP)", full_flow(IMG_SIZE));
    expect("bootloader: run", bootloader_run(&outcome), FWU_OK);
    check("... TRIAL, uygulama yeni imaj", outcome == FWU_ACT_OUTCOME_TRIAL && (g_sim.base.memory[0] & 0xFFu) == img_byte(0) && (g_sim.base.memory[699] & 0xFFu) == img_byte(699));
    check("... manifest sayfasi takasa girmedi: staging'in ilk sayfasinda hala manifest", (g_sim.base.memory[MANIFEST_OFFSET] & 0xFFu) == 0x46u);
}

/* ------------------------------------------------------------------ 2b. farkli yazma granulleri */

static uint32_t ceil_div(uint32_t a, uint32_t b)
{
    return (a + b - 1u) / b;
}

static void test_granules(void)
{
    static const uint32_t granules[] = { 1u, 2u, 4u, 8u, 16u, 32u, 64u };
    int ops_ok = 1;
    int manifest_ok = 1;
    int image_ok = 1;
    int activate_ok = 1;
    int init_ok = 1;

    printf("\n--- farkli yazma granulleri: manifest ve kisa son parca granule dolgusu ---\n");
    for(unsigned k = 0u; k < sizeof(granules) / sizeof(granules[0]); k++) {
        const uint32_t g = granules[k];
        const uint32_t padded = ceil_div(138u, g) * g;
        /* elle turetilen islem sayisi: manifest (1 silme + padded/g yazma) + imaj (10 tam parca x 64/g + son parca 60 bayt: ceil(60/g) + 3 sayfa silme) */
        const uint32_t expected_ops = 1u + (padded / g) + (10u * (64u / g)) + ceil_div(60u, g) + 3u;

        g_granule = g;
        world(false, FWU_ACTIVATE_MODE_COPY);
        init_ok = init_ok && (fwu_session_get_state(&g_s) == FWU_SESSION_IDLE) && g_s.initialized;
        tx_manifest();
        manifest_ok = manifest_ok && is_type(FWU_MSG_DOWNLOAD_ACCEPTED);
        for(uint32_t i = 0u; i < 138u; i++) {
            manifest_ok = manifest_ok && ((g_sim.base.memory[MANIFEST_OFFSET + i] & 0xFFu) == g_manifest[i]);
        }
        for(uint32_t i = 138u; i < padded + 8u; i++) {
            manifest_ok = manifest_ok && ((g_sim.base.memory[MANIFEST_OFFSET + i] & 0xFFu) == ERASED);
        }
        image_ok = image_ok && send_chunks(0u, CHUNK_COUNT, IMG_SIZE) && flash_image_matches(IMG_SIZE);
        for(uint32_t i = IMG_SIZE; i < ceil_div(IMG_SIZE, g) * g + 8u; i++) {
            image_ok = image_ok && ((g_sim.base.memory[IMAGE_OFFSET + i] & 0xFFu) == ERASED);
        }
        ops_ok = ops_ok && (g_sim.base.operations_done == expected_ops);
        if(g_sim.base.operations_done != expected_ops) {
            printf("[FAIL] granul %u: islem sayisi %u (expected %u)\n", (unsigned)g, (unsigned)g_sim.base.operations_done, (unsigned)expected_ops);
        }
        tx_u32(FWU_MSG_TRANSFER_EXIT, 1u, IMG_SIZE);
        poll_session();
        image_ok = image_ok && is_type(FWU_MSG_VERIFY_RESULT) && (g_r.payload[0] == 0u) && (fwu_session_get_state(&g_s) == FWU_SESSION_READY);
        if(g <= 16u) {
            fwu_activate_outcome_e outcome = FWU_ACT_OUTCOME_NONE;
            tx_u32(FWU_MSG_ACTIVATE, 1u, IMG_SIZE);
            activate_ok = activate_ok && is_type(FWU_MSG_ACTIVATE_OK) && (bootloader_run(&outcome) == FWU_OK) &&
                          (outcome == FWU_ACT_OUTCOME_ACTIVATED) && app_is_new();
        }
    }
    g_granule = 8u;
    check("granul 1, 2, 4, 8, 16, 32, 64: oturum baslatilir", init_ok);
    check("... manifest: 138 bayt birebir, granule yuvarlanmis dolgu 0xFF, otesine yazilmadi", manifest_ok);
    check("... imaj birebir flash'ta, kisa son parcanin granul dolgusu 0xFF, dogrulama gecer (HAZIR)", image_ok);
    check("... toplam flash islemi elle turetilen formulle ayni: 1 + ceil(138/g) + 10 x 64/g + ceil(60/g) + 3", ops_ok);
    check("... granul <= 16 icin commit ve bootloader da tamam (aktivasyon journal kaydi 16 bayt)", activate_ok);
    world(false, FWU_ACTIVATE_MODE_COPY);
}

/* ------------------------------------------------------------------ 3. durum x mesaj tablosu */

#define ROWS 14u
#define COLS 7u

typedef struct {
    fwu_byte_t response;                /* beklenen yanit turu */
    fwu_byte_t error;                   /* response == ERROR ise beklenen kod */
    uint32_t   next_state;
} cell_t;

#define R(type, st)     { (type), 0u, (st) }
#define E(code, st)     { FWU_MSG_ERROR, (fwu_byte_t)(code), (st) }

#define S0 0u
#define S1 1u
#define S2 2u
#define S3 3u
#define S4 4u
#define S5 5u
#define S6 6u

static const fwu_byte_t k_matrix_types[ROWS] = {
    FWU_MSG_HELLO, FWU_MSG_KEEPALIVE, FWU_MSG_AUTH_REQ, FWU_MSG_AUTH_RESP, FWU_MSG_REQUEST_DOWNLOAD,
    FWU_MSG_TRANSFER_DATA, FWU_MSG_TRANSFER_EXIT, FWU_MSG_ACTIVATE, FWU_MSG_ABORT,
    0x00u, 0x20u, 0x7Fu, 0x81u, 0xFFu
};

static const char *const k_matrix_names[ROWS] = {
    "HELLO", "KEEPALIVE", "AUTH_REQ", "AUTH_RESP", "REQUEST_DOWNLOAD", "TRANSFER_DATA", "TRANSFER_EXIT", "ACTIVATE",
    "ABORT", "tur 0x00", "tur 0x20", "tur 0x7F (ERROR)", "tur 0x81 (yanit turu)", "tur 0xFF"
};

static const char *const k_state_names[COLS] = { "BOSTA", "YETKI_BEKL", "YETKILI", "INDIRIYOR", "DOGRULUYOR", "HAZIR", "COMMIT" };

/* Elle yazilmis tablo (fwu_session.h basligindaki tablonun kendisi). Sutunlar: S0..S6. */
static const cell_t k_matrix[ROWS][COLS] = {
    /* HELLO    */ { R(FWU_MSG_INFO, S0), R(FWU_MSG_INFO, S1), R(FWU_MSG_INFO, S2), R(FWU_MSG_INFO, S3), R(FWU_MSG_INFO, S4), R(FWU_MSG_INFO, S5), R(FWU_MSG_INFO, S6) },
    /* KEEPALIVE*/ { R(FWU_MSG_KEEPALIVE_OK, S0), R(FWU_MSG_KEEPALIVE_OK, S1), R(FWU_MSG_KEEPALIVE_OK, S2), R(FWU_MSG_KEEPALIVE_OK, S3), R(FWU_MSG_KEEPALIVE_OK, S4), R(FWU_MSG_KEEPALIVE_OK, S5), R(FWU_MSG_KEEPALIVE_OK, S6) },
    /* AUTH_REQ */ { R(FWU_MSG_AUTH_CHALLENGE, S1), R(FWU_MSG_AUTH_CHALLENGE, S1), E(FWU_ERR_BAD_STATE, S2), E(FWU_ERR_BAD_STATE, S3), E(FWU_ERR_BAD_STATE, S4), E(FWU_ERR_BAD_STATE, S5), E(FWU_ERR_BAD_STATE, S6) },
    /* AUTH_RESP*/ { E(FWU_ERR_BAD_STATE, S0), R(FWU_MSG_AUTH_OK, S2), R(FWU_MSG_AUTH_OK, S2), E(FWU_ERR_BAD_STATE, S3), E(FWU_ERR_BAD_STATE, S4), E(FWU_ERR_BAD_STATE, S5), E(FWU_ERR_BAD_STATE, S6) },
    /* REQ_DOWNL*/ { E(FWU_ERR_BAD_STATE, S0), E(FWU_ERR_BAD_STATE, S1), R(FWU_MSG_DOWNLOAD_ACCEPTED, S3), R(FWU_MSG_DOWNLOAD_ACCEPTED, S3), E(FWU_ERR_BAD_STATE, S4), E(FWU_ERR_BAD_STATE, S5), E(FWU_ERR_BAD_STATE, S6) },
    /* DATA     */ { E(FWU_ERR_BAD_STATE, S0), E(FWU_ERR_BAD_STATE, S1), E(FWU_ERR_BAD_STATE, S2), R(FWU_MSG_DATA_ACK, S3), E(FWU_ERR_BAD_STATE, S4), E(FWU_ERR_BAD_STATE, S5), E(FWU_ERR_BAD_STATE, S6) },
    /* EXIT     */ { E(FWU_ERR_BAD_STATE, S0), E(FWU_ERR_BAD_STATE, S1), E(FWU_ERR_BAD_STATE, S2), E(FWU_ERR_BAD_BLOCK, S3), R(FWU_MSG_EXIT_PENDING, S4), R(FWU_MSG_VERIFY_RESULT, S5), E(FWU_ERR_BAD_STATE, S6) },
    /* ACTIVATE */ { E(FWU_ERR_BAD_STATE, S0), E(FWU_ERR_BAD_STATE, S1), E(FWU_ERR_BAD_STATE, S2), E(FWU_ERR_BAD_STATE, S3), E(FWU_ERR_BAD_STATE, S4), R(FWU_MSG_ACTIVATE_OK, S6), R(FWU_MSG_ACTIVATE_OK, S6) },
    /* ABORT    */ { R(FWU_MSG_ABORT_OK, S0), R(FWU_MSG_ABORT_OK, S0), R(FWU_MSG_ABORT_OK, S0), R(FWU_MSG_ABORT_OK, S0), R(FWU_MSG_ABORT_OK, S0), R(FWU_MSG_ABORT_OK, S0), E(FWU_ERR_BAD_STATE, S6) },
    /* 0x00     */ { E(FWU_ERR_UNKNOWN_MESSAGE, S0), E(FWU_ERR_UNKNOWN_MESSAGE, S1), E(FWU_ERR_UNKNOWN_MESSAGE, S2), E(FWU_ERR_UNKNOWN_MESSAGE, S3), E(FWU_ERR_UNKNOWN_MESSAGE, S4), E(FWU_ERR_UNKNOWN_MESSAGE, S5), E(FWU_ERR_UNKNOWN_MESSAGE, S6) },
    /* 0x20     */ { E(FWU_ERR_UNKNOWN_MESSAGE, S0), E(FWU_ERR_UNKNOWN_MESSAGE, S1), E(FWU_ERR_UNKNOWN_MESSAGE, S2), E(FWU_ERR_UNKNOWN_MESSAGE, S3), E(FWU_ERR_UNKNOWN_MESSAGE, S4), E(FWU_ERR_UNKNOWN_MESSAGE, S5), E(FWU_ERR_UNKNOWN_MESSAGE, S6) },
    /* 0x7F     */ { E(FWU_ERR_UNKNOWN_MESSAGE, S0), E(FWU_ERR_UNKNOWN_MESSAGE, S1), E(FWU_ERR_UNKNOWN_MESSAGE, S2), E(FWU_ERR_UNKNOWN_MESSAGE, S3), E(FWU_ERR_UNKNOWN_MESSAGE, S4), E(FWU_ERR_UNKNOWN_MESSAGE, S5), E(FWU_ERR_UNKNOWN_MESSAGE, S6) },
    /* 0x81     */ { E(FWU_ERR_UNKNOWN_MESSAGE, S0), E(FWU_ERR_UNKNOWN_MESSAGE, S1), E(FWU_ERR_UNKNOWN_MESSAGE, S2), E(FWU_ERR_UNKNOWN_MESSAGE, S3), E(FWU_ERR_UNKNOWN_MESSAGE, S4), E(FWU_ERR_UNKNOWN_MESSAGE, S5), E(FWU_ERR_UNKNOWN_MESSAGE, S6) },
    /* 0xFF     */ { E(FWU_ERR_UNKNOWN_MESSAGE, S0), E(FWU_ERR_UNKNOWN_MESSAGE, S1), E(FWU_ERR_UNKNOWN_MESSAGE, S2), E(FWU_ERR_UNKNOWN_MESSAGE, S3), E(FWU_ERR_UNKNOWN_MESSAGE, S4), E(FWU_ERR_UNKNOWN_MESSAGE, S5), E(FWU_ERR_UNKNOWN_MESSAGE, S6) }
};

/** Dunyayi (yetkilendirmeli) verilen duruma getirir; basarili ise 1. */
static int bring_to(uint32_t state)
{
    world(true, FWU_ACTIVATE_MODE_COPY);
    if(state >= S1) {
        tx1(FWU_MSG_AUTH_REQ, 1u);
    }
    if(state >= S2) {
        fwu_byte_t resp[16];
        for(uint32_t i = 0u; i < 16u; i++) {
            resp[i] = (fwu_byte_t)((g_r.payload[i] ^ 0x5Au) & 0xFFu);
        }
        tx(FWU_MSG_AUTH_RESP, 2u, resp, 16u);
    }
    if(state >= S3) {
        tx_manifest();
    }
    if(state >= S4) {
        if(!send_chunks(0u, CHUNK_COUNT, IMG_SIZE)) {
            return 0;
        }
        tx_u32(FWU_MSG_TRANSFER_EXIT, 3u, IMG_SIZE);
    }
    if(state >= S5) {
        poll_session();
    }
    if(state >= S6) {
        tx_u32(FWU_MSG_ACTIVATE, 4u, IMG_SIZE);
    }
    return (uint32_t)fwu_session_get_state(&g_s) == state;
}

/** Hucrenin istegini gonderir: gecerli bir yuk ile (yalnizca DURUM kontrolu sinansin). */
static void send_matrix_message(uint32_t row, uint32_t state)
{
    const fwu_byte_t type = k_matrix_types[row];
    const fwu_byte_t one[1] = { 0u };

    (void)state;
    switch(type) {
    case FWU_MSG_HELLO:
    case FWU_MSG_KEEPALIVE:
    case FWU_MSG_AUTH_REQ:
    case FWU_MSG_ABORT:
        tx(type, 5u, one, 1u);
        break;
    case FWU_MSG_AUTH_RESP: {
        fwu_byte_t resp[16];
        for(uint32_t i = 0u; i < 16u; i++) {
            resp[i] = (fwu_byte_t)(((i + 1u) ^ 0x5Au) & 0xFFu);     /* kancanin sabit meydan okumasina dogru yanit */
        }
        tx(type, 5u, resp, 16u);
        break;
    }
    case FWU_MSG_REQUEST_DOWNLOAD:
        tx(type, 5u, g_manifest, 138u);
        break;
    case FWU_MSG_TRANSFER_DATA:
        {
            fwu_byte_t p[FWU_FRAME_MAX_PAYLOAD];
            size_t n = 0u;
            (void)fwu_proto_encode_data(0u, g_image, CHUNK, p, sizeof(p), &n);      /* durum INDIRIYOR (0 bayt indirilmis) ise gecerli parca 0 */
            tx(type, 5u, p, n);
        }
        break;
    case FWU_MSG_TRANSFER_EXIT:
    case FWU_MSG_ACTIVATE:
        tx_u32(type, 5u, IMG_SIZE);
        break;
    default:
        tx(type, 5u, one, 1u);
        break;
    }
}

static void test_state_matrix(void)
{
    int all_ok = 1;
    int errors_carry_type = 1;
    int seq_ok = 1;
    unsigned cells = 0u;

    printf("\n--- durum x mesaj tablosu (7 durum x 14 mesaj = 98 hucre, elle yazilmis beklenti) ---\n");
    for(uint32_t st = 0u; st < COLS; st++) {
        for(uint32_t row = 0u; row < ROWS; row++) {
            const cell_t *cell = &k_matrix[row][st];
            int cell_ok;

            if(!bring_to(st)) {
                printf("[FAIL] durum %s'e getirilemedi\n", k_state_names[st]);
                g_failures++;
                all_ok = 0;
                continue;
            }
            send_matrix_message(row, st);
            cell_ok = (g_r.rc == FWU_OK) && g_r.decoded && (g_r.type == cell->response) &&
                      ((uint32_t)fwu_session_get_state(&g_s) == cell->next_state);
            if(cell->response == FWU_MSG_ERROR) {
                cell_ok = cell_ok && (g_r.payload_length == 2u) && (g_r.payload[0] == cell->error);
                errors_carry_type = errors_carry_type && (g_r.payload[1] == k_matrix_types[row]);
            }
            if((k_matrix_types[row] == FWU_MSG_HELLO) && (cell->response == FWU_MSG_INFO)) {
                cell_ok = cell_ok && (g_r.payload_length == 13u) && (g_r.payload[12] == (fwu_byte_t)st);      /* INFO durum baytı */
            }
            seq_ok = seq_ok && (g_r.seq == 5u);
            if(!cell_ok) {
                printf("[FAIL] durum %s, mesaj %s: yanit 0x%02X (expected 0x%02X), kod %u (expected %u), durum %d (expected %u)\n",
                       k_state_names[st], k_matrix_names[row], (unsigned)g_r.type, (unsigned)cell->response,
                       (unsigned)g_r.payload[0], (unsigned)cell->error, (int)fwu_session_get_state(&g_s), (unsigned)cell->next_state);
                g_failures++;
                all_ok = 0;
            }
            cells++;
        }
    }
    check("98 hucrenin hepsi: yanit turu, hata kodu ve sonraki durum beklenen", all_ok && cells == 98u);
    check("... her ERROR yaniti istegin turunu tasir", errors_carry_type);
    check("... her yanit istegin SEQ'ini (5) yankilar", seq_ok);

    /* KEEPALIVE durum baytini tasir (her durumda) */
    {
        int states_ok = 1;
        for(uint32_t st = 0u; st < COLS; st++) {
            if(!bring_to(st)) {
                states_ok = 0;
                continue;
            }
            tx1(FWU_MSG_KEEPALIVE, 1u);
            states_ok = states_ok && is_type(FWU_MSG_KEEPALIVE_OK) && (g_r.payload[0] == (fwu_byte_t)st);
        }
        check("KEEPALIVE_OK yuku = oturum durumu (0..6, kalici numaralar)", states_ok);
    }

    /* yetkilendirme kancasi YOK: AUTH_REQ/AUTH_RESP her durumda BAD_STATE, indirme BOSTA'dan baslar */
    {
        int ok = 1;
        world(false, FWU_ACTIVATE_MODE_COPY);
        tx1(FWU_MSG_AUTH_REQ, 1u);
        ok = ok && is_error(FWU_ERR_BAD_STATE) && (g_auth_ctx.begin_calls == 0u);
        tx(FWU_MSG_AUTH_RESP, 1u, g_manifest, 16u);
        ok = ok && is_error(FWU_ERR_BAD_STATE);
        check("kanca yok: AUTH_REQ ve AUTH_RESP BAD_STATE, kanca cagrilmadi", ok);
        tx_manifest();
        check("kanca yok: REQUEST_DOWNLOAD BOSTA'dan kabul edilir", is_type(FWU_MSG_DOWNLOAD_ACCEPTED) && fwu_session_get_state(&g_s) == FWU_SESSION_DOWNLOADING);
        tx1(FWU_MSG_AUTH_REQ, 1u);
        check("kanca yok, INDIRIYOR: AUTH_REQ yine BAD_STATE", is_error(FWU_ERR_BAD_STATE));
    }
}

/* ------------------------------------------------------------------ 4. yetkilendirme */

static void fail_auth_once(void)
{
    fwu_byte_t bad[16];
    tx1(FWU_MSG_AUTH_REQ, 1u);
    for(uint32_t i = 0u; i < 16u; i++) {
        bad[i] = 0x11u;
    }
    tx(FWU_MSG_AUTH_RESP, 2u, bad, 16u);
}

static void test_auth(void)
{
    fwu_byte_t first[16];

    printf("\n--- yetkilendirme: akis, tek kullanimlik meydan okuma, kilitlenme ---\n");
    world(true, FWU_ACTIVATE_MODE_COPY);
    g_auth_ctx.fresh = true;
    tx1(FWU_MSG_AUTH_REQ, 1u);
    check("AUTH_REQ -> AUTH_CHALLENGE (16 bayt, kancanin urettigi), durum YETKI_BEKL.",
          is_type(FWU_MSG_AUTH_CHALLENGE) && g_r.payload_length == 16u && fwu_session_get_state(&g_s) == FWU_SESSION_AUTH_PENDING &&
          g_auth_ctx.begin_calls == 1u && g_r.payload[0] == g_auth_ctx.last_challenge[0] && g_r.payload[15] == g_auth_ctx.last_challenge[15]);
    for(uint32_t i = 0u; i < 16u; i++) {
        first[i] = g_r.payload[i];
    }
    tx1(FWU_MSG_AUTH_REQ, 2u);
    check("YETKI_BEKL.'te yeni AUTH_REQ: yeni meydan okuma (farkli), durum YETKI_BEKL.",
          is_type(FWU_MSG_AUTH_CHALLENGE) && g_auth_ctx.begin_calls == 2u && g_r.payload[0] != first[0] && fwu_session_get_state(&g_s) == FWU_SESSION_AUTH_PENDING);
    {
        fwu_byte_t resp[16];
        for(uint32_t i = 0u; i < 16u; i++) {
            resp[i] = (fwu_byte_t)((g_r.payload[i] ^ 0x5Au) & 0xFFu);
        }
        tx(FWU_MSG_AUTH_RESP, 3u, resp, 16u);
        check("dogru yanit -> AUTH_OK, YETKILI", is_type(FWU_MSG_AUTH_OK) && fwu_session_get_state(&g_s) == FWU_SESSION_AUTHENTICATED);
        check("... check'e giden meydan okuma, begin'in verdigiyle ayni (16 bayt) ve yanit uzunlugu 16",
              g_auth_ctx.check_calls == 1u && g_auth_ctx.seen_challenge_length == 16u && g_auth_ctx.seen_challenge_matches && g_auth_ctx.last_response_length == 16u);
    }

    /* basarisiz yanit: BOSTA'ya don, hata koduna dokunma, meydan okuma tek kullanimlik */
    world(true, FWU_ACTIVATE_MODE_COPY);
    fail_auth_once();
    check("yanlis yanit -> ERROR(BAD_SIGNATURE: kancanin kodu aynen), durum BOSTA", is_error(FWU_ERR_BAD_SIGNATURE) && fwu_session_get_state(&g_s) == FWU_SESSION_IDLE);
    tx(FWU_MSG_AUTH_RESP, 3u, first, 16u);
    check("... ayni yanit tekrar: BAD_STATE (meydan okuma tek kullanimlik, BOSTA)", is_error(FWU_ERR_BAD_STATE));
    tx_manifest();
    check("... yetkisiz REQUEST_DOWNLOAD BAD_STATE", is_error(FWU_ERR_BAD_STATE));
    check("... yetkisiz denemede flash'a HIC dokunulmadi", g_sim.base.operations_done == 0u && g_sim.base.erase_counts[8] == 0u);

    /* kanca baska kodlar dondurur */
    world(true, FWU_ACTIVATE_MODE_COPY);
    g_auth_ctx.force_check = true;
    g_auth_ctx.check_status = FWU_ERR_CRYPTO_FAILURE;
    fail_auth_once();
    check("kanca CRYPTO_FAILURE (yapilamadi) donerse aynen iletilir", is_error(FWU_ERR_CRYPTO_FAILURE));
    g_auth_ctx.check_status = (fwu_return_e)300;
    fail_auth_once();
    check("kanca saçma kod (300, 1 bayta sigmaz): CRYPTO_FAILURE", is_error(FWU_ERR_CRYPTO_FAILURE));
    g_auth_ctx.check_status = FWU_ERR_LOCKED_OUT;
    fail_auth_once();
    check("kanca LOCKED_OUT donerse aynen iletilir", is_error(FWU_ERR_LOCKED_OUT));

    /* AUTH_REQ hatalari */
    world(true, FWU_ACTIVATE_MODE_COPY);
    g_auth_ctx.force_begin = true;
    g_auth_ctx.begin_status = FWU_ERR_CRYPTO_FAILURE;
    tx1(FWU_MSG_AUTH_REQ, 1u);
    check("begin hatasi (CRYPTO_FAILURE) aynen iletilir, durum BOSTA", is_error(FWU_ERR_CRYPTO_FAILURE) && fwu_session_get_state(&g_s) == FWU_SESSION_IDLE);
    g_auth_ctx.force_begin = false;
    g_auth_ctx.begin_length = 0u;
    tx1(FWU_MSG_AUTH_REQ, 1u);
    check("begin uzunluk 0: INVALID_ARG (kanca sozlesmeyi bozdu), BOSTA", is_error(FWU_ERR_INVALID_ARG) && fwu_session_get_state(&g_s) == FWU_SESSION_IDLE);
    g_auth_ctx.begin_length = 65u;
    tx1(FWU_MSG_AUTH_REQ, 1u);
    check("begin uzunluk 65 (> 64): INVALID_ARG", is_error(FWU_ERR_INVALID_ARG));
    g_auth_ctx.begin_length = 64u;
    tx1(FWU_MSG_AUTH_REQ, 1u);
    check("begin uzunluk 64 (sinir): meydan okuma 64 bayt", is_type(FWU_MSG_AUTH_CHALLENGE) && g_r.payload_length == 64u);
    check("... 64 baytin HEPSI kancanin urettigi (kapasite 64 verildi): bayt 0 = 1, bayt 32 = 33, bayt 63 = 64",
          g_r.payload[0] == 1u && g_r.payload[32] == 33u && g_r.payload[63] == 64u);
    g_auth_ctx.begin_length = 1u;
    tx1(FWU_MSG_AUTH_REQ, 2u);
    check("begin uzunluk 1 (sinir): meydan okuma 1 bayt", is_type(FWU_MSG_AUTH_CHALLENGE) && g_r.payload_length == 1u);
    g_auth_ctx.begin_length = 16u;
    tx1(FWU_MSG_AUTH_REQ, 1u);
    {
        fwu_byte_t big[FWU_PROTO_AUTH_RESPONSE_MAX + 1u];
        for(uint32_t i = 0u; i < sizeof(big) / sizeof(big[0]); i++) {
            big[i] = 0x22u;
        }
        tx(FWU_MSG_AUTH_RESP, 2u, big, 129u);
        check("AUTH_RESP yuku 129 bayt: BAD_LENGTH, durum YETKI_BEKL. (kanca cagrilmadi)", is_error(FWU_ERR_BAD_LENGTH) && fwu_session_get_state(&g_s) == FWU_SESSION_AUTH_PENDING && g_auth_ctx.check_calls == 0u);
        tx(FWU_MSG_AUTH_RESP, 2u, big, 128u);
        check("AUTH_RESP yuku 128 bayt (sinir): kancaya iletilir, uzunluk 128", is_error(FWU_ERR_BAD_SIGNATURE) && g_auth_ctx.last_response_length == 128u);
    }

    /* YETKI_BEKL.'ten yeni AUTH_REQ'in begin'i basarisiz olursa: eski meydan okuma gecersiz, BOSTA */
    world(true, FWU_ACTIVATE_MODE_COPY);
    tx1(FWU_MSG_AUTH_REQ, 1u);
    check("AUTH_REQ: YETKI_BEKL.", fwu_session_get_state(&g_s) == FWU_SESSION_AUTH_PENDING);
    g_auth_ctx.force_begin = true;
    g_auth_ctx.begin_status = FWU_ERR_CRYPTO_FAILURE;
    tx1(FWU_MSG_AUTH_REQ, 2u);
    check("YETKI_BEKL.'te yeni AUTH_REQ, begin basarisiz: ERROR(51) ve durum BOSTA (eski meydan okuma gecersiz)",
          is_error(FWU_ERR_CRYPTO_FAILURE) && fwu_session_get_state(&g_s) == FWU_SESSION_IDLE);

    /* kilitlenme: 3 art arda basarisiz -> 5000 ms kilit */
    printf("\n--- kilitlenme: 3 art arda basarisiz deneme, 5000 ms ---\n");
    world(true, FWU_ACTIVATE_MODE_COPY);
    fail_auth_once();
    g_now += 100u;
    fail_auth_once();
    g_now += 100u;
    tx1(FWU_MSG_AUTH_REQ, 1u);
    check("2 basarisiz sonrasi AUTH_REQ hala serbest", is_type(FWU_MSG_AUTH_CHALLENGE));
    g_now += 100u;
    fail_auth_once();                                       /* 3. basarisiz: g_now'da kilitlenir */
    {
        const uint32_t locked_at = g_now;
        const uint32_t calls = g_auth_ctx.begin_calls;
        tx1(FWU_MSG_AUTH_REQ, 1u);
        check("3. basarisizdan hemen sonra AUTH_REQ: LOCKED_OUT, kanca cagrilmadi", is_error(FWU_ERR_LOCKED_OUT) && g_auth_ctx.begin_calls == calls);
        g_now = locked_at + LOCKOUT_MS - 1u;
        tx1(FWU_MSG_AUTH_REQ, 1u);
        check("kilit bitmesine 1 ms kala: hala LOCKED_OUT", is_error(FWU_ERR_LOCKED_OUT));
        tx(FWU_MSG_HELLO, 1u, (const fwu_byte_t[1]){ 0u }, 1u);
        check("... kilit bilgi sorgusunu (HELLO) engellemez", is_type(FWU_MSG_INFO));
        tx_manifest();
        check("... kilitliyken indirme yine yetkisiz: BAD_STATE", is_error(FWU_ERR_BAD_STATE));
        g_now = locked_at + LOCKOUT_MS;
        tx1(FWU_MSG_AUTH_REQ, 1u);
        check("kilit suresi tam doldu: AUTH_REQ serbest", is_type(FWU_MSG_AUTH_CHALLENGE));
        fail_auth_once();
        fail_auth_once();
        tx1(FWU_MSG_AUTH_REQ, 1u);
        check("kilit bittikten sonra sayac SIFIRLANDI: 2 basarisiz daha kilitlemez", is_type(FWU_MSG_AUTH_CHALLENGE));
        fail_auth_once();
        tx1(FWU_MSG_AUTH_REQ, 1u);
        check("... 3. art arda basarisiz yine kilitler", is_error(FWU_ERR_LOCKED_OUT));
    }

    /* basari sayaci sifirlar */
    world(true, FWU_ACTIVATE_MODE_COPY);
    fail_auth_once();
    fail_auth_once();
    authenticate();
    check("2 basarisiz + 1 basari: YETKILI", fwu_session_get_state(&g_s) == FWU_SESSION_AUTHENTICATED);
    tx1(FWU_MSG_ABORT, 1u);
    fail_auth_once();
    fail_auth_once();
    tx1(FWU_MSG_AUTH_REQ, 1u);
    check("... basari sayaci sifirladi: sonraki 2 basarisiz kilitlemez", is_type(FWU_MSG_AUTH_CHALLENGE));

    /* kilit zaman sarmasi */
    world(true, FWU_ACTIVATE_MODE_COPY);
    g_now = 0xFFFFF000u;
    fail_auth_once();
    fail_auth_once();
    fail_auth_once();
    tx1(FWU_MSG_AUTH_REQ, 1u);
    check("zaman 0xFFFFF000'da kilitlendi: LOCKED_OUT", is_error(FWU_ERR_LOCKED_OUT));
    g_now = 0xFFFFF000u + LOCKOUT_MS - 1u;                  /* 32 bit sarmasini asar: 0x00000387 */
    tx1(FWU_MSG_AUTH_REQ, 1u);
    check("sarma sonrasi, 1 ms kala: hala LOCKED_OUT", is_error(FWU_ERR_LOCKED_OUT) && g_now < 0x1000u);
    g_now = 0xFFFFF000u + LOCKOUT_MS;
    tx1(FWU_MSG_AUTH_REQ, 1u);
    check("sarma sonrasi, sure doldu: serbest", is_type(FWU_MSG_AUTH_CHALLENGE));

    /* yetkili oturum ABORT ile yetkisini kaybeder */
    world(true, FWU_ACTIVATE_MODE_COPY);
    authenticate();
    tx1(FWU_MSG_ABORT, 1u);
    tx_manifest();
    check("ABORT yetkiyi siler: yeniden yetkilendirme gerekir (REQUEST_DOWNLOAD BAD_STATE)", is_error(FWU_ERR_BAD_STATE));
}

/* ------------------------------------------------------------------ 5. teklif reddi: flash'a hic dokunulmaz */

static uint32_t flash_fingerprint(void)
{
    uint32_t h = 2166136261u;
    for(uint32_t i = 0u; i < 19u * PAGE; i++) {
        h = (h ^ (uint32_t)(g_sim.base.memory[i] & 0xFFu)) * 16777619u;
    }
    for(uint32_t p = 0u; p < 19u; p++) {
        h = (h ^ g_sim.base.erase_counts[p]) * 16777619u;
    }
    return h;
}

static void expect_rejected_untouched(const char *name, fwu_return_e code)
{
    const uint32_t before = flash_fingerprint();
    const uint32_t ops = g_sim.base.operations_done;
    char text[160];

    tx_manifest();
    (void)snprintf(text, sizeof(text), "%s: ERROR(%d), durum degismedi", name, (int)code);
    check(text, is_error(code) && fwu_session_get_state(&g_s) == FWU_SESSION_IDLE);
    (void)snprintf(text, sizeof(text), "%s: flash'a TEK BAYT dokunulmadi (bellek, silme sayaclari, islem sayisi)", name);
    check(text, flash_fingerprint() == before && g_sim.base.operations_done == ops);
}

static void test_offer_rejection(void)
{
    pkg_t p;

    printf("\n--- teklif reddi: her reddedilen manifest icin flash'a TEK BAYT dokunulmaz ---\n");
    world(false, FWU_ACTIVATE_MODE_COPY);
    p = good_pkg(); p.product = PRODUCT + 1u; make_package(&p);
    expect_rejected_untouched("baska urun", FWU_ERR_PRODUCT_MISMATCH);
    p = good_pkg(); p.version = 24u; make_package(&p);
    expect_rejected_untouched("surum 24 < asgari 25", FWU_ERR_VERSION_REJECTED);
    p = good_pkg(); p.size = IMAGE_MAX + 1u; make_package(&p);
    expect_rejected_untouched("imaj kapasiteyi asiyor", FWU_ERR_IMAGE_TOO_LARGE);
    p = good_pkg(); p.key_id = 9u; make_package(&p);
    expect_rejected_untouched("bilinmeyen anahtar", FWU_ERR_KEY_REJECTED);
    p = good_pkg(); p.key_version = 0u; make_package(&p);
    expect_rejected_untouched("anahtar surumu asgarinin altinda", FWU_ERR_KEY_REJECTED);
    p = good_pkg(); p.signer = 2u; make_package(&p);
    expect_rejected_untouched("saldirganin anahtariyla imzali", FWU_ERR_BAD_SIGNATURE);
    p = good_pkg(); make_package(&p);
    g_manifest[20] = (fwu_byte_t)(g_manifest[20] ^ 0x01u);
    expect_rejected_untouched("imzali alan degistirilmis (imza eski)", FWU_ERR_BAD_SIGNATURE);
    make_package(&p);
    g_manifest[0] = (fwu_byte_t)(g_manifest[0] ^ 0x01u);
    expect_rejected_untouched("magic bozuk", FWU_ERR_BAD_MANIFEST);
    make_package(&p);
    (void)fwu_crypto_sim_init(&g_cs);
    g_cs.fail_verify = FWU_ERR_CRYPTO_FAILURE;
    expect_rejected_untouched("kripto: imza dogrulama yapilamadi", FWU_ERR_CRYPTO_FAILURE);
    g_cs.fail_verify = FWU_OK;

    {
        const uint32_t before = flash_fingerprint();
        tx(FWU_MSG_REQUEST_DOWNLOAD, 1u, g_manifest, 137u);
        check("manifest 137 bayt: BAD_LENGTH, flash'a dokunulmadi", is_error(FWU_ERR_BAD_LENGTH) && flash_fingerprint() == before);
        tx(FWU_MSG_REQUEST_DOWNLOAD, 1u, (const fwu_byte_t *)g_image, 139u);
        check("manifest 139 bayt: BAD_LENGTH", is_error(FWU_ERR_BAD_LENGTH) && flash_fingerprint() == before);
        tx(FWU_MSG_REQUEST_DOWNLOAD, 1u, g_manifest, 1u);
        check("manifest 1 bayt: BAD_LENGTH", is_error(FWU_ERR_BAD_LENGTH));
    }

    /* gecerli teklif reddedilenlerden sonra yine kabul */
    make_package(&(pkg_t){ PRODUCT, 25u, IMG_SIZE, 1u, 1u, 0u });
    tx_manifest();
    check("reddedilenlerden sonra gecerli teklif kabul edilir", is_type(FWU_MSG_DOWNLOAD_ACCEPTED));

    /* sinir degerleri kabul */
    world(false, FWU_ACTIVATE_MODE_COPY);
    p = good_pkg(); p.size = IMAGE_MAX; make_package(&p);
    tx_manifest();
    check("imaj boyutu tam kapasite (1792): kabul", is_type(FWU_MSG_DOWNLOAD_ACCEPTED));
    world(false, FWU_ACTIVATE_MODE_COPY);
    p = good_pkg(); p.version = 25u; make_package(&p);
    tx_manifest();
    check("surum asgariye esit (25): kabul", is_type(FWU_MSG_DOWNLOAD_ACCEPTED));

    /* bekleyen aktivasyon varken staging'e dokunulmaz (acik konu 30) */
    printf("\n--- bekleyen aktivasyon: staging korumasi ---\n");
    world(false, FWU_ACTIVATE_MODE_COPY);
    (void)fwu_activate_commit(&g_act, 3u);
    {
        const uint32_t before = flash_fingerprint();
        const uint32_t ops = g_sim.base.operations_done;
        tx_manifest();
        check("commit bekliyor: REQUEST_DOWNLOAD BAD_STATE (staging kaynak olabilir)", is_error(FWU_ERR_BAD_STATE) && fwu_session_get_state(&g_s) == FWU_SESSION_IDLE);
        check("... flash'a dokunulmadi", flash_fingerprint() == before && g_sim.base.operations_done == ops);
    }
    world(false, FWU_ACTIVATE_MODE_SWAP);
    check("SWAP: tum akis", full_flow(IMG_SIZE));
    {
        fwu_activate_outcome_e trial_outcome = FWU_ACT_OUTCOME_NONE;
        (void)bootloader_run(&trial_outcome);
        check("... bootloader: TRIAL", trial_outcome == FWU_ACT_OUTCOME_TRIAL);
    }
    (void)fwu_session_init(&g_s, &g_cfg);                   /* yeniden baslatma: oturum RAM'i sifirlandi */
    {
        const uint32_t before = flash_fingerprint();
        tx_manifest();
        check("deneme onyuklemesi (TRIAL) suruyor: staging'deki eski imaj yedek; REQUEST_DOWNLOAD BAD_STATE", is_error(FWU_ERR_BAD_STATE) && flash_fingerprint() == before);
    }
    (void)fwu_activate_init(&g_act, &g_ac);
    (void)fwu_activate_confirm(&g_act);
    tx_manifest();
    check("onaydan sonra staging serbest: REQUEST_DOWNLOAD kabul", is_type(FWU_MSG_DOWNLOAD_ACCEPTED));
}

/* ------------------------------------------------------------------ 6. indirme kurallari */

static void test_download_rules(void)
{
    const uint32_t chunk3 = 3u * CHUNK;
    fwu_byte_t big[CHUNK + 8u];
    uint32_t ops;

    printf("\n--- indirme kurallari ---\n");
    world(false, FWU_ACTIVATE_MODE_COPY);
    tx_manifest();
    check("0. parca", send_chunks(0u, 1u, IMG_SIZE));

    tx_chunk(2u * CHUNK, &g_image[2u * CHUNK], CHUNK);
    check("ileri atlama (ofset 128, beklenen 64): BAD_BLOCK, durum INDIRIYOR", is_error(FWU_ERR_BAD_BLOCK) && fwu_session_get_state(&g_s) == FWU_SESSION_DOWNLOADING);
    ops = g_sim.base.operations_done;
    tx_chunk(0u, g_image, CHUNK);
    check("geri gitme: 0. parcanin aynen tekrari bir ONCEKI parca degil (son parca 0): yeniden onay, yazma yok",
          is_type(FWU_MSG_DATA_ACK) && ack_offset() == CHUNK && g_sim.base.operations_done == ops);
    {
        fwu_byte_t changed[CHUNK];
        for(uint32_t i = 0u; i < CHUNK; i++) {
            changed[i] = g_image[i];
        }
        changed[10] = (fwu_byte_t)(changed[10] ^ 0x01u);
        tx_chunk(0u, changed, CHUNK);
        check("son parcanin ayni ofsette FARKLI icerikle tekrari: BAD_BLOCK, yazma yok", is_error(FWU_ERR_BAD_BLOCK) && g_sim.base.operations_done == ops);
    }
    check("1. ve 2. parca", send_chunks(1u, 3u, IMG_SIZE));
    tx_chunk(0u, g_image, CHUNK);
    check("2 parca once (ofset 0): BAD_BLOCK (yalniz SON parcanin tekrari yeniden onaylanir)", is_error(FWU_ERR_BAD_BLOCK));
    tx_chunk(chunk3 - CHUNK, &g_image[chunk3 - CHUNK], CHUNK);
    check("SON parcanin (2.) aynen tekrari: yeniden onay", is_type(FWU_MSG_DATA_ACK) && ack_offset() == chunk3);
    tx_chunk(chunk3, &g_image[chunk3], 8u);
    check("uzunluk 8 (parca 64, son parca degil): BAD_LENGTH", is_error(FWU_ERR_BAD_LENGTH));
    tx_chunk(chunk3, &g_image[chunk3], CHUNK - 1u);
    check("uzunluk 63: BAD_LENGTH", is_error(FWU_ERR_BAD_LENGTH));
    for(uint32_t i = 0u; i < sizeof(big) / sizeof(big[0]); i++) {
        big[i] = 0x33u;
    }
    tx_chunk(chunk3, big, CHUNK + 1u);
    check("uzunluk 65 (> parca boyutu): BAD_LENGTH", is_error(FWU_ERR_BAD_LENGTH));
    tx(FWU_MSG_TRANSFER_DATA, 1u, big, 4u);
    check("TRANSFER_DATA payload 4 bayt (veri yok): BAD_LENGTH", is_error(FWU_ERR_BAD_LENGTH));
    check("... hata yanitlarinda durum INDIRIYOR, ilerleme korundu", fwu_session_get_state(&g_s) == FWU_SESSION_DOWNLOADING);
    tx_chunk(0xFFFFFFC0u, g_image, CHUNK);
    check("ofset 0xFFFFFFC0: BAD_BLOCK", is_error(FWU_ERR_BAD_BLOCK));

    /* erken cikis / boyut iddialari */
    tx_u32(FWU_MSG_TRANSFER_EXIT, 1u, IMG_SIZE);
    check("TRANSFER_EXIT imaj eksikken: BAD_BLOCK, durum INDIRIYOR (host devam edebilir)", is_error(FWU_ERR_BAD_BLOCK) && fwu_session_get_state(&g_s) == FWU_SESSION_DOWNLOADING);
    check("kalan parcalar (3..10)", send_chunks(3u, CHUNK_COUNT, IMG_SIZE));
    tx_chunk(IMG_SIZE, g_image, CHUNK);
    check("imajin SONUNDAN sonra parca (ofset 700): BAD_LENGTH (kalan 0)", is_error(FWU_ERR_BAD_LENGTH));
    tx_u32(FWU_MSG_TRANSFER_EXIT, 1u, IMG_SIZE + 1u);
    check("EXIT imaj boyutu iddiasi yanlis (701): BAD_BLOCK, durum INDIRIYOR", is_error(FWU_ERR_BAD_BLOCK) && fwu_session_get_state(&g_s) == FWU_SESSION_DOWNLOADING);
    tx_u32(FWU_MSG_TRANSFER_EXIT, 1u, IMG_SIZE - 1u);
    check("EXIT iddiasi 699: BAD_BLOCK", is_error(FWU_ERR_BAD_BLOCK));
    tx(FWU_MSG_TRANSFER_EXIT, 1u, big, 3u);
    check("EXIT yuku 3 bayt: BAD_LENGTH", is_error(FWU_ERR_BAD_LENGTH));
    tx(FWU_MSG_TRANSFER_EXIT, 1u, big, 5u);
    check("EXIT yuku 5 bayt: BAD_LENGTH", is_error(FWU_ERR_BAD_LENGTH));
    tx_u32(FWU_MSG_TRANSFER_EXIT, 1u, IMG_SIZE);
    check("EXIT dogru: EXIT_PENDING", is_type(FWU_MSG_EXIT_PENDING));
    poll_session();
    check("... poll: gecerli", is_type(FWU_MSG_VERIFY_RESULT) && g_r.payload[0] == 0u);

    tx_u32(FWU_MSG_ACTIVATE, 1u, IMG_SIZE + 1u);
    check("ACTIVATE imaj boyutu iddiasi yanlis: BAD_BLOCK, durum HAZIR", is_error(FWU_ERR_BAD_BLOCK) && fwu_session_get_state(&g_s) == FWU_SESSION_READY);
    tx(FWU_MSG_ACTIVATE, 1u, big, 3u);
    check("ACTIVATE yuku 3 bayt: BAD_LENGTH", is_error(FWU_ERR_BAD_LENGTH) && fwu_session_get_state(&g_s) == FWU_SESSION_READY);
    check("... commit henuz yazilmadi (journal'a dokunulmadi)", g_sim.base.erase_counts[16] == 0u && g_act.last_state == 0u);
    tx_u32(FWU_MSG_ACTIVATE, 1u, IMG_SIZE);
    check("ACTIVATE dogru: ACTIVATE_OK", is_type(FWU_MSG_ACTIVATE_OK));

    /* son parca parca boyutundan UZUN olamaz (imajin sonuna tam otursa bile) */
    world(false, FWU_ACTIVATE_MODE_COPY);
    tx_manifest();
    check("0..8. parcalar (576 bayt)", send_chunks(0u, 9u, IMG_SIZE));
    tx_chunk(576u, &g_image[576], 124u);
    check("imajin sonunu TAM kapatan ama parca boyutundan uzun (124 > 64) parca: BAD_LENGTH, durum INDIRIYOR",
          is_error(FWU_ERR_BAD_LENGTH) && fwu_session_get_state(&g_s) == FWU_SESSION_DOWNLOADING);
    check("... normal parcalarla devam eder (9., 10.)", send_chunks(9u, CHUNK_COUNT, IMG_SIZE));

    /* ayni ofset ama FARKLI uzunluk bir tekrar degildir */
    world(false, FWU_ACTIVATE_MODE_COPY);
    tx_manifest();
    (void)send_chunks(0u, 3u, IMG_SIZE);
    ops = g_sim.base.operations_done;
    tx_chunk(2u * CHUNK, &g_image[2u * CHUNK], 8u);
    check("son parcanin ofseti, ama uzunluk 8 (64 degil): tekrar degil; BAD_BLOCK, yazma yok", is_error(FWU_ERR_BAD_BLOCK) && g_sim.base.operations_done == ops);

    /* imaj boyutu parca ve sayfa katlari: sinir boyutlari */
    {
        const uint32_t sizes[] = { 1u, 7u, 8u, 9u, 63u, 64u, 65u, 255u, 256u, 257u, 1792u };
        int sizes_ok = 1;
        for(unsigned k = 0u; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
            const uint32_t size = sizes[k];
            fwu_activate_outcome_e outcome = FWU_ACT_OUTCOME_NONE;
            int ok;
            world(false, FWU_ACTIVATE_MODE_COPY);
            make_package(&(pkg_t){ PRODUCT, 25u, size, 1u, 1u, 0u });
            ok = full_flow(size);
            ok = ok && flash_image_matches(size);
            ok = ok && (bootloader_run(&outcome) == FWU_OK) && (outcome == FWU_ACT_OUTCOME_ACTIVATED);
            for(uint32_t i = 0u; ok && (i < size); i++) {
                ok = ((g_sim.base.memory[i] & 0xFFu) == img_byte(i));
            }
            if(!ok) {
                printf("[FAIL] imaj boyutu %u: akis veya sonuc yanlis\n", (unsigned)size);
                g_failures++;
                sizes_ok = 0;
            }
        }
        check("imaj boyutlari 1, 7, 8, 9, 63, 64, 65, 255, 256, 257, 1792: tam akis, flash'ta ve uygulamada birebir", sizes_ok);
    }
}

/* ------------------------------------------------------------------ 7. devam, dogrulama asamasi, ABORT, zaman asimi, SEQ */

static void test_resume_abort_verify(void)
{
    uint32_t ops;
    fwu_proto_accepted_t acc;

    printf("\n--- devam (ayni acilista), ABORT ---\n");
    world(false, FWU_ACTIVATE_MODE_COPY);
    tx_manifest();
    check("5 parca (320 bayt)", send_chunks(0u, 5u, IMG_SIZE));
    ops = g_sim.base.operations_done;
    tx_manifest();
    check("ayni manifest tekrar (kopma sonrasi devam): DOWNLOAD_ACCEPTED", is_type(FWU_MSG_DOWNLOAD_ACCEPTED));
    (void)fwu_proto_parse_accepted(g_r.payload, g_r.payload_length, &acc);
    check("... sonraki ofset 320 (kaldigi yer), flash'a dokunulmadi (silme/yazma yok)", acc.next_offset == 320u && g_sim.base.operations_done == ops);
    check("... durum INDIRIYOR; ilerleme korundu: kalan parcalar yazilir ve imaj dogrulanir", send_chunks(5u, CHUNK_COUNT, IMG_SIZE) && flash_image_matches(IMG_SIZE));
    {
        fwu_byte_t other[138];
        for(uint32_t i = 0u; i < 138u; i++) {
            other[i] = g_manifest[i];
        }
        other[30] = (fwu_byte_t)(other[30] ^ 0x01u);
        tx(FWU_MSG_REQUEST_DOWNLOAD, 1u, other, 138u);
        check("FARKLI manifest ile REQUEST_DOWNLOAD (INDIRIYOR'da): BAD_STATE (once ABORT)", is_error(FWU_ERR_BAD_STATE) && fwu_session_get_state(&g_s) == FWU_SESSION_DOWNLOADING);
        other[30] = g_manifest[30];
        other[137] = (fwu_byte_t)(other[137] ^ 0x80u);
        tx(FWU_MSG_REQUEST_DOWNLOAD, 1u, other, 138u);
        check("manifest SON baytta farkli: yine BAD_STATE (tum 138 bayt karsilastirilir)", is_error(FWU_ERR_BAD_STATE));
        other[137] = g_manifest[137];
        other[0] = (fwu_byte_t)(other[0] ^ 0x01u);
        tx(FWU_MSG_REQUEST_DOWNLOAD, 1u, other, 138u);
        check("manifest ILK baytta farkli: yine BAD_STATE", is_error(FWU_ERR_BAD_STATE));
        tx_manifest();
        check("... orijinal manifest hala devam olarak kabul edilir", is_type(FWU_MSG_DOWNLOAD_ACCEPTED));
    }
    tx1(FWU_MSG_ABORT, 1u);
    check("ABORT -> ABORT_OK, BOSTA", is_type(FWU_MSG_ABORT_OK) && fwu_session_get_state(&g_s) == FWU_SESSION_IDLE);
    tx_manifest();
    (void)fwu_proto_parse_accepted(g_r.payload, g_r.payload_length, &acc);
    check("ABORT'tan sonra REQUEST_DOWNLOAD: yeni indirme (sonraki ofset 0), manifest sayfasi yeniden silindi", is_type(FWU_MSG_DOWNLOAD_ACCEPTED) && acc.next_offset == 0u && g_sim.base.erase_counts[8] == 2u);
    tx_chunk(0u, g_image, CHUNK);
    check("... ilk parca yazilir (sayfa yeniden silinir)", ack_offset() == CHUNK && g_sim.base.erase_counts[9] == 2u);
    tx1(FWU_MSG_ABORT, 1u);
    tx1(FWU_MSG_ABORT, 1u);
    check("BOSTA'da ABORT: ABORT_OK (zararsiz)", is_type(FWU_MSG_ABORT_OK));

    printf("\n--- dogrulama asamasi ---\n");
    /* bozuk imaj: bir parcada tek bit */
    world(false, FWU_ACTIVATE_MODE_COPY);
    g_image[300] = (fwu_byte_t)(g_image[300] ^ 0x08u);
    tx_manifest();
    check("bozuk veri (manifest'teki ozetle uyusmaz): parcalar yine kabul", send_chunks(0u, CHUNK_COUNT, IMG_SIZE));
    tx_u32(FWU_MSG_TRANSFER_EXIT, 9u, IMG_SIZE);
    check("EXIT_PENDING", is_type(FWU_MSG_EXIT_PENDING));
    poll_session();
    check("poll: VERIFY_RESULT(HASH_MISMATCH = 35), SEQ 9, durum BOSTA", is_type(FWU_MSG_VERIFY_RESULT) && g_r.seq == 9u && g_r.payload[0] == (fwu_byte_t)FWU_ERR_HASH_MISMATCH && fwu_session_get_state(&g_s) == FWU_SESSION_IDLE);
    tx_u32(FWU_MSG_ACTIVATE, 1u, IMG_SIZE);
    check("... ACTIVATE reddedilir (BAD_STATE): bozuk imaj asla commit edilemez, journal'a dokunulmadi", is_error(FWU_ERR_BAD_STATE) && g_sim.base.erase_counts[16] == 0u && g_act.last_state == 0u);
    make_package(&(pkg_t){ PRODUCT, 25u, IMG_SIZE, 1u, 1u, 0u });
    check("oturum yeniden kullanilabilir: duzgun imajla tum akis", full_flow(IMG_SIZE));

    /* GEC doğrulama: zaman aşımı sayacı doğrulamanın BİTTİĞİ andan başlar */
    world(false, FWU_ACTIVATE_MODE_COPY);
    tx_manifest();
    (void)send_chunks(0u, CHUNK_COUNT, IMG_SIZE);
    tx_u32(FWU_MSG_TRANSFER_EXIT, 1u, IMG_SIZE);
    g_now += 5000u;                                          /* dogrulama uzun surdu: zaman asimindan (1000 ms) cok daha uzun */
    poll_session();
    check("EXIT'ten 5000 ms sonra poll: dogrulama sonucu verilir (VERIFYING'de zaman asimi yok), HAZIR",
          is_type(FWU_MSG_VERIFY_RESULT) && g_r.payload[0] == 0u && fwu_session_get_state(&g_s) == FWU_SESSION_READY);
    g_now += 500u;
    poll_session();
    check("... 500 ms sonra hala HAZIR (zaman asimi sayaci dogrulamanin bittigi andan baslar)", fwu_session_get_state(&g_s) == FWU_SESSION_READY);
    tx_u32(FWU_MSG_ACTIVATE, 1u, IMG_SIZE);
    check("... ACTIVATE basarili", is_type(FWU_MSG_ACTIVATE_OK));

    /* kripto 'yapilamadi' */
    world(false, FWU_ACTIVATE_MODE_COPY);
    tx_manifest();
    (void)send_chunks(0u, CHUNK_COUNT, IMG_SIZE);
    tx_u32(FWU_MSG_TRANSFER_EXIT, 1u, IMG_SIZE);
    g_cs.fail_finish = FWU_ERR_CRYPTO_FAILURE;
    poll_session();
    check("ozet bitirme yapilamadi: VERIFY_RESULT(CRYPTO_FAILURE = 51), BOSTA", is_type(FWU_MSG_VERIFY_RESULT) && g_r.payload[0] == (fwu_byte_t)FWU_ERR_CRYPTO_FAILURE && fwu_session_get_state(&g_s) == FWU_SESSION_IDLE);
    g_cs.fail_finish = FWU_OK;

    /* DOGRULUYOR'da yardimci mesajlar */
    world(false, FWU_ACTIVATE_MODE_COPY);
    tx_manifest();
    (void)send_chunks(0u, CHUNK_COUNT, IMG_SIZE);
    tx_u32(FWU_MSG_TRANSFER_EXIT, 21u, IMG_SIZE);
    tx1(FWU_MSG_KEEPALIVE, 99u);
    check("DOGRULUYOR'da KEEPALIVE: KEEPALIVE_OK(4), SEQ kendisinin", is_type(FWU_MSG_KEEPALIVE_OK) && g_r.payload[0] == FWU_PROTO_STATE_VERIFYING && g_r.seq == 99u);
    tx1(FWU_MSG_HELLO, 98u);
    check("DOGRULUYOR'da HELLO: INFO(durum 4)", is_type(FWU_MSG_INFO));
    poll_session();
    check("VERIFY_RESULT, EXIT'in SEQ'ini (21) tasir, ara mesajlarin degil", is_type(FWU_MSG_VERIFY_RESULT) && g_r.seq == 21u);

    /* ABORT DOGRULUYOR'da (poll'dan once) */
    world(false, FWU_ACTIVATE_MODE_COPY);
    tx_manifest();
    (void)send_chunks(0u, CHUNK_COUNT, IMG_SIZE);
    tx_u32(FWU_MSG_TRANSFER_EXIT, 1u, IMG_SIZE);
    tx1(FWU_MSG_ABORT, 1u);
    check("DOGRULUYOR'da ABORT (poll'dan once): BOSTA", is_type(FWU_MSG_ABORT_OK) && fwu_session_get_state(&g_s) == FWU_SESSION_IDLE);
    poll_session();
    check("... sonraki poll: dogrulama CALISMADI (imaj ozeti hesaplanmadi), gonderilecek bir sey yok", g_r.length == 0u && g_cs.begin_calls == 0u && g_cs.verify_calls == 1u);

    /* COMMIT sonrasi ABORT / yeni indirme yok */
    world(false, FWU_ACTIVATE_MODE_COPY);
    check("tum akis", full_flow(IMG_SIZE));
    tx1(FWU_MSG_ABORT, 1u);
    check("COMMIT'te ABORT: BAD_STATE, durum COMMIT, yeniden baslatma bayragi duruyor", is_error(FWU_ERR_BAD_STATE) && fwu_session_get_state(&g_s) == FWU_SESSION_COMMITTED && fwu_session_reboot_requested(&g_s));
    tx_manifest();
    check("COMMIT'te REQUEST_DOWNLOAD: BAD_STATE", is_error(FWU_ERR_BAD_STATE));
}

static void test_duplicates(void)
{
    fwu_activate_outcome_e outcome = FWU_ACT_OUTCOME_NONE;
    uint32_t ops;
    uint32_t verify_calls;
    uint32_t finish_calls;
    const fwu_byte_t three[3] = { 0u, 0u, 0u };
    const fwu_byte_t five[5] = { 0u, 0u, 0u, 0u, 0u };

    printf("\n--- tekrarlanan istekler (yanit kaybi): EXIT ve ACTIVATE idempotent ---\n");
    world(false, FWU_ACTIVATE_MODE_COPY);
    tx_manifest();
    (void)send_chunks(0u, CHUNK_COUNT, IMG_SIZE);
    tx_u32(FWU_MSG_TRANSFER_EXIT, 10u, IMG_SIZE);
    check("EXIT -> EXIT_PENDING (SEQ 10)", is_type(FWU_MSG_EXIT_PENDING) && g_r.seq == 10u);
    tx_u32(FWU_MSG_TRANSFER_EXIT, 11u, IMG_SIZE);
    check("DOGRULUYOR'da EXIT tekrari: yeniden EXIT_PENDING (SEQ 11), durum DOGRULUYOR, hesap baslamadi",
          is_type(FWU_MSG_EXIT_PENDING) && g_r.seq == 11u && fwu_session_get_state(&g_s) == FWU_SESSION_VERIFYING && g_cs.begin_calls == 0u);
    tx_u32(FWU_MSG_TRANSFER_EXIT, 12u, IMG_SIZE + 1u);
    check("yanlis boyut iddiali tekrar: BAD_BLOCK, durum DOGRULUYOR", is_error(FWU_ERR_BAD_BLOCK) && fwu_session_get_state(&g_s) == FWU_SESSION_VERIFYING);
    poll_session();
    check("poll: VERIFY_RESULT(0), SEQ = EN SON GECERLI EXIT'in SEQ'i (11)", is_type(FWU_MSG_VERIFY_RESULT) && g_r.seq == 11u && g_r.payload[0] == 0u);
    check("... durum HAZIR", fwu_session_get_state(&g_s) == FWU_SESSION_READY);
    verify_calls = g_cs.verify_calls;
    finish_calls = g_cs.finish_calls;
    ops = g_sim.base.operations_done;

    tx_u32(FWU_MSG_TRANSFER_EXIT, 20u, IMG_SIZE);
    check("HAZIR'da EXIT tekrari: VERIFY_RESULT(0) hemen yanit, SEQ 20, durum HAZIR",
          is_type(FWU_MSG_VERIFY_RESULT) && g_r.seq == 20u && g_r.payload_length == 1u && g_r.payload[0] == 0u && fwu_session_get_state(&g_s) == FWU_SESSION_READY);
    check("... hicbir sey yeniden hesaplanmadi (imza ve ozet sayaclari, flash islemi ayni)",
          g_cs.verify_calls == verify_calls && g_cs.finish_calls == finish_calls && g_sim.base.operations_done == ops);
    tx_u32(FWU_MSG_TRANSFER_EXIT, 21u, IMG_SIZE - 1u);
    check("HAZIR'da yanlis boyut iddiasi: BAD_BLOCK, durum HAZIR", is_error(FWU_ERR_BAD_BLOCK) && fwu_session_get_state(&g_s) == FWU_SESSION_READY);
    tx(FWU_MSG_TRANSFER_EXIT, 22u, three, 3u);
    check("HAZIR'da EXIT yuku 3 bayt: BAD_LENGTH", is_error(FWU_ERR_BAD_LENGTH));

    tx_u32(FWU_MSG_ACTIVATE, 30u, IMG_SIZE);
    check("ACTIVATE -> ACTIVATE_OK", is_type(FWU_MSG_ACTIVATE_OK) && fwu_session_get_state(&g_s) == FWU_SESSION_COMMITTED);
    ops = g_sim.base.operations_done;
    tx_u32(FWU_MSG_ACTIVATE, 31u, IMG_SIZE);
    check("COMMIT'te ACTIVATE tekrari: yeniden ACTIVATE_OK (SEQ 31), durum COMMIT, yeniden baslatma bayragi duruyor",
          is_type(FWU_MSG_ACTIVATE_OK) && g_r.seq == 31u && fwu_session_get_state(&g_s) == FWU_SESSION_COMMITTED && fwu_session_reboot_requested(&g_s));
    check("... commit IKINCI KEZ YAZILMADI (flash islemi ayni, sayfa sayisi 3)", g_sim.base.operations_done == ops && g_act.page_count == 3u);
    tx_u32(FWU_MSG_ACTIVATE, 32u, IMG_SIZE + 1u);
    check("COMMIT'te yanlis boyut iddiasi: BAD_BLOCK, durum COMMIT", is_error(FWU_ERR_BAD_BLOCK) && fwu_session_get_state(&g_s) == FWU_SESSION_COMMITTED);
    tx(FWU_MSG_ACTIVATE, 33u, five, 5u);
    check("COMMIT'te ACTIVATE yuku 5 bayt: BAD_LENGTH", is_error(FWU_ERR_BAD_LENGTH));
    tx_u32(FWU_MSG_TRANSFER_EXIT, 34u, IMG_SIZE);
    check("COMMIT'te EXIT: BAD_STATE (tekrar yalniz ACTIVATE icin)", is_error(FWU_ERR_BAD_STATE));
    expect("bootloader: run", bootloader_run(&outcome), FWU_OK);
    check("... tek commit: ACTIVATED, uygulama tam yeni", outcome == FWU_ACT_OUTCOME_ACTIVATED && app_is_new());

    /* basarisiz doğrulamadan sonra EXIT tekrari: BOSTA'da BAD_STATE (host durumu KEEPALIVE ile de anlayabilir) */
    world(false, FWU_ACTIVATE_MODE_COPY);
    g_image[300] = (fwu_byte_t)(g_image[300] ^ 0x08u);
    tx_manifest();
    (void)send_chunks(0u, CHUNK_COUNT, IMG_SIZE);
    tx_u32(FWU_MSG_TRANSFER_EXIT, 1u, IMG_SIZE);
    poll_session();
    check("bozuk imaj: VERIFY_RESULT(35), BOSTA", is_type(FWU_MSG_VERIFY_RESULT) && g_r.payload[0] == (fwu_byte_t)FWU_ERR_HASH_MISMATCH && fwu_session_get_state(&g_s) == FWU_SESSION_IDLE);
    tx_u32(FWU_MSG_TRANSFER_EXIT, 2u, IMG_SIZE);
    check("... EXIT tekrari BOSTA'da: BAD_STATE (basarisizlik basariya donusmez)", is_error(FWU_ERR_BAD_STATE));
    tx_u32(FWU_MSG_ACTIVATE, 3u, IMG_SIZE);
    check("... ACTIVATE BOSTA'da: BAD_STATE, commit yok", is_error(FWU_ERR_BAD_STATE) && g_act.last_state == 0u);
}

static void test_timeouts_and_seq(void)
{
    printf("\n--- zaman asimi (sinir, sarma) ve SEQ yankisi ---\n");

    /* her baslamis durum: tam T-1'de yasar, tam T'de BOSTA'ya duser */
    for(uint32_t st = S1; st <= S5; st++) {
        char text[120];
        uint32_t t0;
        int alive;
        int dead;

        if(st == S4) {
            continue;                                       /* DOGRULUYOR'da zaman asimi yok: ayri test */
        }
        (void)bring_to(st);
        t0 = g_now;
        poll_session();
        g_now = t0 + TIMEOUT_MS - 1u;
        poll_session();
        alive = ((uint32_t)fwu_session_get_state(&g_s) == st);
        g_now = t0 + TIMEOUT_MS;
        poll_session();
        dead = (fwu_session_get_state(&g_s) == FWU_SESSION_IDLE);
        (void)snprintf(text, sizeof(text), "durum %s: zaman asimi tam 1000 ms'de (999'da yasar)", k_state_names[st]);
        check(text, alive && dead);
    }

    /* BOSTA ve COMMIT'te zaman asimi yok */
    world(true, FWU_ACTIVATE_MODE_COPY);
    g_now += 1000000u;
    poll_session();
    check("BOSTA'da poll: zaman asimi etkisiz, gonderilecek bir sey yok", g_r.rc == FWU_OK && g_r.length == 0u && fwu_session_get_state(&g_s) == FWU_SESSION_IDLE);
    {
        fwu_byte_t out[FWU_FRAME_MAX_SIZE];
        size_t poisoned = 77u;
        expect("bosta poll, out_length onceden 77", fwu_session_poll(&g_s, g_now, out, sizeof(out), &poisoned), FWU_OK);
        check("... out_length 0'a ayarlanir (zehirli onceki deger kalmaz)", poisoned == 0u);
    }
    (void)bring_to(S6);
    g_now += 100000000u;
    poll_session();
    check("COMMIT'te zaman asimi yok (yeniden baslatma bekleniyor)", fwu_session_get_state(&g_s) == FWU_SESSION_COMMITTED && fwu_session_reboot_requested(&g_s));

    /* her cerceve zamanlayiciyi yeniler */
    (void)bring_to(S3);
    {
        const uint32_t t0 = g_now;
        g_now = t0 + 900u;
        tx1(FWU_MSG_KEEPALIVE, 1u);
        g_now = t0 + 1800u;
        poll_session();
        check("KEEPALIVE zamanlayiciyi yeniler (900 + 900 ms: hala INDIRIYOR)", fwu_session_get_state(&g_s) == FWU_SESSION_DOWNLOADING);
        g_now = t0 + 1900u;
        poll_session();
        check("... son cerceveden 1000 ms sonra BOSTA", fwu_session_get_state(&g_s) == FWU_SESSION_IDLE);
    }

    /* zaman sarmasi: 32 bit */
    (void)bring_to(S3);
    g_now = 0xFFFFFF00u;
    tx1(FWU_MSG_KEEPALIVE, 1u);
    g_now = 0xFFFFFF00u + 999u;                            /* 0x000002E7: sarma */
    poll_session();
    check("zaman 32 bit sarmasini asiyor (999 ms): hala INDIRIYOR", g_now < 0x1000u && fwu_session_get_state(&g_s) == FWU_SESSION_DOWNLOADING);
    g_now = 0xFFFFFF00u + 1000u;
    poll_session();
    check("... 1000 ms: BOSTA", fwu_session_get_state(&g_s) == FWU_SESSION_IDLE);

    /* sarmadan HENUZ gecilmemis kisa aralik: toplam (son cerceve + zaman asimi) sarar, ama gecen sure 16 ms */
    (void)bring_to(S3);
    g_now = 0xFFFFFF00u;
    tx1(FWU_MSG_KEEPALIVE, 1u);
    g_now = 0xFFFFFF10u;
    poll_session();
    check("son cerceveden 16 ms sonra (sarma esigine yakin): hala INDIRIYOR", fwu_session_get_state(&g_s) == FWU_SESSION_DOWNLOADING);

    /* zaman asimindan sonra devam yok: yeni indirme sifirdan */
    (void)bring_to(S3);
    (void)send_chunks(0u, 4u, IMG_SIZE);
    g_now += TIMEOUT_MS;
    poll_session();
    tx_manifest();
    {
        fwu_proto_accepted_t acc;
        check("zaman asimindan sonra REQUEST_DOWNLOAD: yetkisiz (kanca var, BOSTA)", is_error(FWU_ERR_BAD_STATE));
        world(false, FWU_ACTIVATE_MODE_COPY);
        tx_manifest();
        (void)send_chunks(0u, 4u, IMG_SIZE);
        g_now += TIMEOUT_MS;
        poll_session();
        tx_manifest();
        (void)fwu_proto_parse_accepted(g_r.payload, g_r.payload_length, &acc);
        check("kanca yok, zaman asimindan sonra: indirme SIFIRDAN (sonraki ofset 0)", is_type(FWU_MSG_DOWNLOAD_ACCEPTED) && acc.next_offset == 0u);
    }

    /* zaman asimi bilgisi: saniye, tavan */
    {
        fwu_proto_accepted_t acc;
        world(false, FWU_ACTIVATE_MODE_COPY);
        g_cfg.idle_timeout_ms = 2500u;
        (void)fwu_session_init(&g_s, &g_cfg);
        tx_manifest();
        (void)fwu_proto_parse_accepted(g_r.payload, g_r.payload_length, &acc);
        check("zaman asimi 2500 ms: bildirilen 2 sn", acc.timeout_s == 2u);
        world(false, FWU_ACTIVATE_MODE_COPY);
        g_cfg.idle_timeout_ms = 0xFFFFFFFFu;
        (void)fwu_session_init(&g_s, &g_cfg);
        tx_manifest();
        (void)fwu_proto_parse_accepted(g_r.payload, g_r.payload_length, &acc);
        check("zaman asimi 0xFFFFFFFF ms: bildirilen 65535 sn (tavan)", acc.timeout_s == 0xFFFFu);
    }

    /* SEQ yankisi: her istek turu, uc degerler */
    {
        const fwu_byte_t seqs[3] = { 0u, 0xA5u, 255u };
        int ok = 1;
        for(unsigned k = 0u; k < 3u; k++) {
            world(true, FWU_ACTIVATE_MODE_COPY);
            tx1(FWU_MSG_HELLO, seqs[k]);
            ok = ok && (g_r.seq == seqs[k]);
            tx1(FWU_MSG_AUTH_REQ, seqs[k]);
            ok = ok && (g_r.seq == seqs[k]);
            tx1(FWU_MSG_ABORT, seqs[k]);
            ok = ok && (g_r.seq == seqs[k]);
            tx1(0x42u, seqs[k]);
            ok = ok && (g_r.seq == seqs[k]) && is_error(FWU_ERR_UNKNOWN_MESSAGE);
        }
        check("SEQ yankisi: 0, 0xA5, 255 (yanit ve hata yanitlari)", ok);
    }
}

/* ------------------------------------------------------------------ 8. flash hatalari */

static void test_flash_faults(void)
{
    printf("\n--- flash hatalari (ayri kodlarla enjekte: yutulan hata gorunur) ---\n");

    /* manifest silme */
    world(false, FWU_ACTIVATE_MODE_COPY);
    g_ff.fail_erase_offset = MANIFEST_OFFSET;
    tx_manifest();
    check("manifest sayfasi silinemedi: ERROR(FLASH_ERASE), durum BOSTA", is_error(FWU_ERR_FLASH_ERASE) && fwu_session_get_state(&g_s) == FWU_SESSION_IDLE);
    g_ff.fail_erase_offset = NO_OFFSET;
    tx_manifest();
    check("... hata giderilince ayni teklif kabul edilir", is_type(FWU_MSG_DOWNLOAD_ACCEPTED));

    /* manifest yazma */
    world(false, FWU_ACTIVATE_MODE_COPY);
    g_ff.fail_write_offset = MANIFEST_OFFSET + 40u;
    tx_manifest();
    check("manifest yazilamadi: ERROR(kodu aynen = 45), durum BOSTA", is_error(FAULT_WRITE) && fwu_session_get_state(&g_s) == FWU_SESSION_IDLE);

    /* manifest sessiz bozulma */
    world(false, FWU_ACTIVATE_MODE_COPY);
    g_ff.corrupt_write_offset = MANIFEST_OFFSET + 100u;
    tx_manifest();
    check("manifest SESSIZ bozuldu (yazma OK ama bellekte bit degisti): geri okuma yakalar, ERROR(FLASH_WRITE), BOSTA",
          is_error(FWU_ERR_FLASH_WRITE) && fwu_session_get_state(&g_s) == FWU_SESSION_IDLE);

    /* manifest dolgu granuluu (son granul) */
    world(false, FWU_ACTIVATE_MODE_COPY);
    g_ff.corrupt_write_offset = MANIFEST_OFFSET + 140u;
    tx_manifest();
    check("manifest SON granulu (dolgulu) sessiz bozuldu: yakalanir", is_error(FWU_ERR_FLASH_WRITE));

    /* parca silme */
    world(false, FWU_ACTIVATE_MODE_COPY);
    tx_manifest();
    (void)send_chunks(0u, 4u, IMG_SIZE);
    g_ff.fail_erase_offset = IMAGE_OFFSET + PAGE;
    send_chunk(4u, IMG_SIZE);
    check("sayfa silinemedi (ikinci imaj sayfasi): ERROR(FLASH_ERASE), oturum BOSTA", is_error(FWU_ERR_FLASH_ERASE) && fwu_session_get_state(&g_s) == FWU_SESSION_IDLE);

    /* parca yazma */
    world(false, FWU_ACTIVATE_MODE_COPY);
    tx_manifest();
    g_ff.fail_write_offset = IMAGE_OFFSET + 2u * CHUNK + 8u;
    (void)send_chunks(0u, 2u, IMG_SIZE);
    send_chunk(2u, IMG_SIZE);
    check("parca yazilamadi: ERROR(kodu aynen = 45), oturum BOSTA", is_error(FAULT_WRITE) && fwu_session_get_state(&g_s) == FWU_SESSION_IDLE);

    /* parca sessiz bozulma: ilk granul, orta, son granul, kisa son parcanin dolgulu granulu */
    {
        const uint32_t where[] = { IMAGE_OFFSET + CHUNK, IMAGE_OFFSET + CHUNK + 30u, IMAGE_OFFSET + CHUNK + 63u };
        int ok = 1;
        for(unsigned k = 0u; k < 3u; k++) {
            world(false, FWU_ACTIVATE_MODE_COPY);
            tx_manifest();
            (void)send_chunks(0u, 1u, IMG_SIZE);
            g_ff.corrupt_write_offset = where[k];
            send_chunk(1u, IMG_SIZE);
            ok = ok && is_error(FWU_ERR_FLASH_WRITE) && (fwu_session_get_state(&g_s) == FWU_SESSION_IDLE);
        }
        check("parca SESSIZ bozuldu (ilk, orta, son bayt): geri okuma yakalar, ERROR(FLASH_WRITE), BOSTA", ok);
    }
    {
        int ok = 1;
        const uint32_t where[] = { IMAGE_OFFSET + 640u, IMAGE_OFFSET + 696u, IMAGE_OFFSET + 699u, IMAGE_OFFSET + 703u };
        for(unsigned k = 0u; k < 4u; k++) {
            world(false, FWU_ACTIVATE_MODE_COPY);
            tx_manifest();
            (void)send_chunks(0u, 10u, IMG_SIZE);
            g_ff.corrupt_write_offset = where[k];
            send_chunk(10u, IMG_SIZE);
            ok = ok && is_error(FWU_ERR_FLASH_WRITE) && (fwu_session_get_state(&g_s) == FWU_SESSION_IDLE);
        }
        check("kisa SON parca: baslangic, 696 (7. granul), 699 (veri sonu), 703 (dolgu) sessiz bozuldu: hepsi yakalanir", ok);
    }

    /* tekrar kontrolunde okuma hatasi */
    world(false, FWU_ACTIVATE_MODE_COPY);
    tx_manifest();
    (void)send_chunks(0u, 2u, IMG_SIZE);
    g_ff.fail_read_offset = IMAGE_OFFSET + CHUNK + 5u;
    tx_chunk(CHUNK, &g_image[CHUNK], CHUNK);
    check("son parcanin tekrar kontrolunde okuma hatasi: ERROR(kodu aynen = 41), BOSTA", is_error(FAULT_READ) && fwu_session_get_state(&g_s) == FWU_SESSION_IDLE);

    /* dogrulama sirasinda okuma hatasi: yapilamadi */
    world(false, FWU_ACTIVATE_MODE_COPY);
    tx_manifest();
    (void)send_chunks(0u, CHUNK_COUNT, IMG_SIZE);
    tx_u32(FWU_MSG_TRANSFER_EXIT, 1u, IMG_SIZE);
    g_ff.fail_read_offset = IMAGE_OFFSET + 300u;
    poll_session();
    check("dogrulama sirasinda okuma hatasi: VERIFY_RESULT(41 aynen), BOSTA", is_type(FWU_MSG_VERIFY_RESULT) && g_r.payload[0] == (fwu_byte_t)FAULT_READ && fwu_session_get_state(&g_s) == FWU_SESSION_IDLE);

    /* port 1 bayta sigmayan saçma bir kod dondururse (300): VERIFY_RESULT'ta CRYPTO_FAILURE */
    world(false, FWU_ACTIVATE_MODE_COPY);
    tx_manifest();
    (void)send_chunks(0u, CHUNK_COUNT, IMG_SIZE);
    tx_u32(FWU_MSG_TRANSFER_EXIT, 1u, IMG_SIZE);
    g_ff.fail_read_offset = IMAGE_OFFSET + 300u;
    g_ff.read_fault_code = (fwu_return_e)300;
    poll_session();
    check("dogrulama sirasinda port saçma kod (300) dondurur: VERIFY_RESULT(CRYPTO_FAILURE = 51), BOSTA",
          is_type(FWU_MSG_VERIFY_RESULT) && g_r.payload[0] == (fwu_byte_t)FWU_ERR_CRYPTO_FAILURE && fwu_session_get_state(&g_s) == FWU_SESSION_IDLE);
    g_ff.read_fault_code = FAULT_READ;

    /* commit: guc kesintisi */
    world(false, FWU_ACTIVATE_MODE_COPY);
    tx_manifest();
    (void)send_chunks(0u, CHUNK_COUNT, IMG_SIZE);
    tx_u32(FWU_MSG_TRANSFER_EXIT, 1u, IMG_SIZE);
    poll_session();
    fwu_flash_sim_reset(&g_sim.base);
    fwu_flash_sim_cut_power_after(&g_sim.base, 1u);          /* commit kaydinin ilk granulu yazilir, ikincisi kesilir */
    tx_u32(FWU_MSG_ACTIVATE, 1u, IMG_SIZE);
    check("commit sirasinda guc kesildi: ERROR(FLASH_POWER_LOSS), BOSTA, yeniden baslatma ISTENMEDI",
          is_error(FWU_ERR_FLASH_POWER_LOSS) && fwu_session_get_state(&g_s) == FWU_SESSION_IDLE && !fwu_session_reboot_requested(&g_s));
    {
        fwu_activate_outcome_e outcome = FWU_ACT_OUTCOME_NONE;
        expect("yeniden baslatma: bootloader calisir", bootloader_run(&outcome), FWU_OK);
        check("... yarim commit gecersiz: bekleyen is yok, uygulama ESKI", outcome == FWU_ACT_OUTCOME_NONE && app_is_old());
    }

    /* commit: aktivasyon baska evrede (deneme suruyor) */
    world(false, FWU_ACTIVATE_MODE_SWAP);
    tx_manifest();
    (void)send_chunks(0u, CHUNK_COUNT, IMG_SIZE);
    tx_u32(FWU_MSG_TRANSFER_EXIT, 1u, IMG_SIZE);
    poll_session();
    (void)fwu_activate_commit(&g_act, 3u);                   /* araya baska bir commit girdi */
    tx_u32(FWU_MSG_ACTIVATE, 1u, IMG_SIZE);
    check("aktivasyon bosta degil: ACTIVATE hatasi aynen (BAD_STATE), oturum BOSTA, bayrak yok",
          is_error(FWU_ERR_BAD_STATE) && fwu_session_get_state(&g_s) == FWU_SESSION_IDLE && !fwu_session_reboot_requested(&g_s));
}

/* ------------------------------------------------------------------ 9. guc kesintisi taramasi */

/** Host akisi: hata alirsa durur. Her adim tamamlandiysa 1. */
static int drive(uint32_t size)
{
    return full_flow(size);
}

static void test_power_cut_sweep(void)
{
    int old_or_new_ok = 1;
    int unchanged_ok = 1;
    int recover_ok = 1;
    int final_ok = 1;
    unsigned cuts = 0u;
    unsigned completed = 0u;
    uint32_t first_clean = 0u;

    printf("\n--- guc kesintisi: oturumun HER flash isleminde kesinti, sonra yeniden baslatma ve yeni oturum ---\n");
    for(uint32_t k = 0u; k < 600u; k++) {
        fwu_activate_outcome_e outcome = FWU_ACT_OUTCOME_NONE;
        int drove;

        world(false, FWU_ACTIVATE_MODE_COPY);
        fwu_flash_sim_reset(&g_sim.base);
        fwu_flash_sim_cut_power_after(&g_sim.base, k);
        drove = drive(IMG_SIZE);
        if(drove && g_sim.base.powered) {
            completed++;
            if(first_clean == 0u) {
                first_clean = k;
            }
        } else {
            cuts++;
        }

        /* yeniden baslatma oncesi: commit yazilmadiysa uygulama bit bit eski */
        if(!(drove && g_sim.base.powered) && (g_act.last_state == 0u) && !app_is_old()) {
            unchanged_ok = 0;
        }
        if(bootloader_run(&outcome) != FWU_OK) {
            recover_ok = 0;
            break;
        }
        if(!app_is_old() && !app_is_new()) {
            old_or_new_ok = 0;
        }
        /* yeni oturum (RAM sifirlandi) tamamlanabilir mi */
        (void)fwu_session_init(&g_s, &g_cfg);
        make_package(&(pkg_t){ PRODUCT, 25u, IMG_SIZE, 1u, 1u, 0u });
        if(!full_flow(IMG_SIZE)) {
            final_ok = 0;
        } else {
            fwu_activate_outcome_e outcome2 = FWU_ACT_OUTCOME_NONE;
            if((bootloader_run(&outcome2) != FWU_OK) || !app_is_new()) {
                final_ok = 0;
            }
        }
        if(drove && g_sim.base.powered && (k > first_clean + 3u)) {
            break;                                            /* kesinti hicbir islemde olusmuyor: tarama bitti */
        }
    }
    check("her kesinti noktasinda bootloader sorunsuz calisir (hata yok)", recover_ok);
    check("... COMMIT yazilmadan kesilen oturumlarda uygulama BIT BIT ESKI (yeniden baslatmadan once)", unchanged_ok);
    check("... yeniden baslatma sonrasi uygulama HER ZAMAN tam eski ya da tam yeni (karisik yok)", old_or_new_ok);
    check("... her kesintiden sonra YENI oturum tum akisi tamamlar ve uygulama tam yeni imaj olur", final_ok);
    check("tarama anlamli: onlarca kesinti noktasi denendi, en az biri kesintisiz tamamlandi", cuts > 100u && completed > 0u);
    printf("(bilgi: %u kesinti noktasi denendi, ilk kesintisiz tamamlanan nokta %u)\n", cuts, (unsigned)first_clean);
}

/* ------------------------------------------------------------------ 10. dusman akis (fuzz) */

static uint32_t g_rng = 123456789u;

static uint32_t rnd(void)
{
    g_rng = (g_rng * 1664525u) + 1013904223u;
    return g_rng >> 8;
}

static int known_response_type(fwu_byte_t t)
{
    return (t == FWU_MSG_ERROR) || (t == FWU_MSG_INFO) || (t == FWU_MSG_AUTH_CHALLENGE) || (t == FWU_MSG_AUTH_OK) ||
           (t == FWU_MSG_DOWNLOAD_ACCEPTED) || (t == FWU_MSG_DATA_ACK) || (t == FWU_MSG_EXIT_PENDING) ||
           (t == FWU_MSG_ACTIVATE_OK) || (t == FWU_MSG_VERIFY_RESULT) || (t == FWU_MSG_ABORT_OK) || (t == FWU_MSG_KEEPALIVE_OK);
}

/** Rastgele veya yari gecerli bir istek uretip gonderir. `valid_chunks`: DOGRU ofsetteki gecerli parcalar da uretilsin. */
static void fuzz_step(int with_auth, int valid_chunks)
{
    static const fwu_byte_t kinds[] = { FWU_MSG_HELLO, FWU_MSG_AUTH_REQ, FWU_MSG_AUTH_RESP, FWU_MSG_REQUEST_DOWNLOAD,
                                        FWU_MSG_TRANSFER_DATA, FWU_MSG_TRANSFER_EXIT, FWU_MSG_ACTIVATE, FWU_MSG_ABORT,
                                        FWU_MSG_KEEPALIVE };
    static const size_t lengths[] = { 1u, 2u, 4u, 5u, 9u, 16u, 64u, 68u, 137u, 138u, 139u, 200u, 512u };
    fwu_byte_t payload[FWU_FRAME_MAX_PAYLOAD];
    fwu_byte_t type;
    size_t n;
    const uint32_t pick = rnd() % 100u;

    (void)with_auth;
    type = (pick < 8u) ? (fwu_byte_t)(rnd() & 0xFFu) : kinds[rnd() % (sizeof(kinds) / sizeof(kinds[0]))];
    n = lengths[rnd() % (sizeof(lengths) / sizeof(lengths[0]))];
    for(size_t i = 0u; i < n; i++) {
        payload[i] = (fwu_byte_t)(rnd() & 0xFFu);
    }
    if((type == FWU_MSG_REQUEST_DOWNLOAD) && ((rnd() % 3u) != 0u)) {
        for(size_t i = 0u; i < 138u; i++) {
            payload[i] = g_manifest[i];
        }
        n = 138u;
    }
    if((type == FWU_MSG_TRANSFER_EXIT) || (type == FWU_MSG_ACTIVATE)) {
        if((rnd() % 2u) == 0u) {
            fwu_write_u32_big_endian(payload, IMG_SIZE);
            n = 4u;
        }
    }
    if(valid_chunks && (type == FWU_MSG_TRANSFER_DATA) && ((rnd() % 2u) == 0u)) {
        size_t m = 0u;
        const uint32_t off = (uint32_t)g_s.next_offset;
        uint32_t len = IMG_SIZE - off;
        if(len > CHUNK) {
            len = CHUNK;
        }
        if(off < IMG_SIZE) {
            (void)fwu_proto_encode_data(off, &g_image[off], len, payload, sizeof(payload), &m);
            n = m;
        }
    }
    if(type == FWU_MSG_AUTH_RESP) {
        if((rnd() % 2u) == 0u) {
            for(uint32_t i = 0u; i < 16u; i++) {
                payload[i] = (fwu_byte_t)(((i + 1u) ^ 0x5Au) & 0xFFu);
            }
            n = 16u;
        }
    }
    if((type == FWU_MSG_ABORT) && ((rnd() % 6u) != 0u)) {
        type = FWU_MSG_KEEPALIVE;                           /* ABORT seyrek: oturum derin durumlara ulasabilsin */
        n = 1u;
    }
    if((rnd() % 40u) == 0u) {
        g_now += rnd() % 1500u;                             /* seyrek zaman sicramasi: zaman asimi da denenir */
        poll_session();                                     /* once poll: sonraki tx'in yaniti g_r'de kalsin */
    }
    g_now += rnd() % 20u;
    tx(type, (fwu_byte_t)(rnd() & 0xFFu), payload, n);
}

/** Durum farkinda adim: gercek bir host gibi oturumun bir sonraki GECERLI adimini yapar (derin durumlara ulasmak icin). */
static void guided_step(void)
{
    const fwu_session_state_e state = fwu_session_get_state(&g_s);
    const fwu_byte_t seq = (fwu_byte_t)(rnd() & 0xFFu);

    g_now += rnd() % 20u;
    if(state == FWU_SESSION_IDLE) {
        if(g_s.config.auth != NULL) {
            tx(FWU_MSG_AUTH_REQ, seq, (const fwu_byte_t[1]){ 0u }, 1u);
        } else {
            tx(FWU_MSG_REQUEST_DOWNLOAD, seq, g_manifest, 138u);
        }
    } else if(state == FWU_SESSION_AUTH_PENDING) {
        fwu_byte_t resp[16];
        for(uint32_t i = 0u; i < 16u; i++) {
            resp[i] = (fwu_byte_t)(((i + 1u) ^ 0x5Au) & 0xFFu);
        }
        tx(FWU_MSG_AUTH_RESP, seq, resp, 16u);
    } else if(state == FWU_SESSION_AUTHENTICATED) {
        tx(FWU_MSG_REQUEST_DOWNLOAD, seq, g_manifest, 138u);
    } else if(state == FWU_SESSION_DOWNLOADING) {
        if(g_s.next_offset < IMG_SIZE) {
            fwu_byte_t p[FWU_FRAME_MAX_PAYLOAD];
            size_t m = 0u;
            const uint32_t off = g_s.next_offset;
            uint32_t len = IMG_SIZE - off;
            if(len > CHUNK) {
                len = CHUNK;
            }
            (void)fwu_proto_encode_data(off, &g_image[off], len, p, sizeof(p), &m);
            tx(FWU_MSG_TRANSFER_DATA, seq, p, m);
        } else {
            tx_u32(FWU_MSG_TRANSFER_EXIT, seq, IMG_SIZE);
        }
    } else if(state == FWU_SESSION_VERIFYING) {
        poll_session();
    } else if(state == FWU_SESSION_READY) {
        tx_u32(FWU_MSG_ACTIVATE, seq, IMG_SIZE);
    } else {
        tx(FWU_MSG_HELLO, seq, (const fwu_byte_t[1]){ 1u }, 1u);
    }
}

/** %65 yonlendirilmis gecerli adim, %35 gurultu. */
static void fuzz_mix(int with_auth)
{
    if((rnd() % 100u) < 65u) {
        guided_step();
    } else {
        fuzz_step(with_auth, 1);
    }
}

static int response_sane(void)
{
    return (g_r.rc == FWU_OK) && (g_r.length >= FWU_FRAME_OVERHEAD) && g_r.decoded && known_response_type(g_r.type) &&
           ((int)fwu_session_get_state(&g_s) >= 0) && ((int)fwu_session_get_state(&g_s) <= 6);
}

static void test_fuzz(void)
{
    int sane = 1;
    int app_untouched = 1;
    int no_commit = 1;
    int commit_record_clean = 1;
    unsigned commits = 0u;
    unsigned visited[7] = { 0u, 0u, 0u, 0u, 0u, 0u, 0u };
    const uint32_t iterations = 30000u;

    printf("\n--- dusman akis (fuzz): rastgele ve yari gecerli cerceveler ---\n");

    /* A: gecerli paket, rastgele/yari gecerli cerceveler; yanitlar daima iyi bicimli, uygulama/journal/gecici dokunulmaz */
    world(false, FWU_ACTIVATE_MODE_COPY);
    for(uint32_t i = 0u; i < iterations; i++) {
        fuzz_mix(0);
        sane = sane && response_sane();
        visited[(unsigned)fwu_session_get_state(&g_s)]++;
        if(fwu_session_get_state(&g_s) == FWU_SESSION_COMMITTED) {
            commits++;
            world(false, FWU_ACTIVATE_MODE_COPY);               /* yeni dunya: akis surebilsin */
        }
    }
    app_untouched = app_is_old() && g_sim.base.erase_counts[0] == 0u && g_sim.base.erase_counts[7] == 0u && g_sim.base.erase_counts[18] == 0u;
    check("A: 30000 rastgele/yari gecerli cerceve: her yanit iyi bicimli, gecerli turde, durum 0..6", sane);
    check("... oturum asla uygulama (0..7) veya gecici (18) sayfaya dokunmadi", app_untouched);
    printf("(bilgi: A akisinda durum ziyaretleri: BOSTA %u, INDIRIYOR %u, DOGRULUYOR %u, HAZIR %u, COMMIT %u; %u commit)\n",
           visited[0], visited[3], visited[4], visited[5], visited[6], commits);
    check("... fuzz derin durumlara gercekten girdi (INDIRIYOR ve HAZIR en az bir kez)", visited[3] > 0u && visited[5] > 0u);

    /* B: SAHTE IMZALI manifest; her sey rastgele; commit ASLA olmaz */
    world(false, FWU_ACTIVATE_MODE_COPY);
    make_package(&(pkg_t){ PRODUCT, 25u, IMG_SIZE, 1u, 1u, 2u });      /* saldirganin anahtariyla imzali */
    sane = 1;
    for(uint32_t i = 0u; i < iterations; i++) {
        fuzz_mix(0);
        sane = sane && response_sane();
        if((fwu_session_get_state(&g_s) == FWU_SESSION_COMMITTED) || (fwu_session_get_state(&g_s) == FWU_SESSION_DOWNLOADING) ||
           (fwu_session_get_state(&g_s) == FWU_SESSION_READY)) {
            no_commit = 0;
        }
    }
    check("B: sahte imzali manifest, 30000 cerceve: yanitlar iyi bicimli", sane);
    check("... oturum ASLA INDIRIYOR/HAZIR/COMMIT'e girmedi (imzasiz imaj hicbir sirayla kabul edilmez)", no_commit);
    check("... saldiri gercekten denendi: sahte imza yuzlerce kez dogrulandi ve reddedildi", g_cs.verify_calls > 100u);
    check("... flash'a TEK BAYT dokunulmadi (islem sayisi 0, hicbir sayfa silinmedi)",g_sim.base.operations_done == 0u && g_sim.base.erase_counts[8] == 0u && g_act.last_state == 0u);

    /* C: gecerli manifest ama imaj BOZUK; dogrulamadan gecemez, commit yok */
    world(false, FWU_ACTIVATE_MODE_COPY);
    g_image[321] = (fwu_byte_t)(g_image[321] ^ 0x10u);
    sane = 1;
    no_commit = 1;
    commit_record_clean = 1;
    for(uint32_t i = 0u; i < iterations; i++) {
        fuzz_mix(0);
        sane = sane && response_sane();
        if((fwu_session_get_state(&g_s) == FWU_SESSION_READY) || (fwu_session_get_state(&g_s) == FWU_SESSION_COMMITTED)) {
            no_commit = 0;
        }
        if(g_act.last_state != 0u) {
            commit_record_clean = 0;
        }
    }
    check("C: gecerli manifest + BOZUK imaj, 30000 cerceve: yanitlar iyi bicimli", sane);
    check("... saldiri gercekten denendi: bozuk imajin ozeti onlarca kez hesaplanip reddedildi", g_cs.finish_calls > 10u);
    check("... oturum ASLA HAZIR/COMMIT'e girmedi, commit kaydi yazilmadi (bozuk imaj hicbir sirayla commit edilmez)", no_commit && commit_record_clean);

    /* D: yetkilendirmeli oturum, yanit hep yanlis: indirme hic baslamaz, kilitlenme calisir */
    world(true, FWU_ACTIVATE_MODE_COPY);
    g_auth_ctx.force_check = true;
    g_auth_ctx.check_status = FWU_ERR_BAD_SIGNATURE;
    sane = 1;
    no_commit = 1;
    for(uint32_t i = 0u; i < iterations; i++) {
        fuzz_mix(1);
        sane = sane && response_sane();
        if((fwu_session_get_state(&g_s) == FWU_SESSION_AUTHENTICATED) || ((int)fwu_session_get_state(&g_s) >= (int)FWU_SESSION_DOWNLOADING)) {
            no_commit = 0;
        }
    }
    check("D: kanca her yaniti reddediyor, 30000 cerceve: yanitlar iyi bicimli", sane);
    check("... saldiri gercekten denendi: yetkilendirme yanitlari yuzlerce kez kancaya gitti ve reddedildi", g_auth_ctx.check_calls > 100u);
    check("... oturum ASLA YETKILI/INDIRIYOR/HAZIR/COMMIT'e girmedi; flash'a dokunulmadi", no_commit && g_sim.base.operations_done == 0u);
}

/* ------------------------------------------------------------------ 11. gercek parser + ring buffer */

FWU_RINGBUF_DEFINE(wire_rb, 1024);

static wire_rb_t g_wire;
static fwu_parser_t g_parser;

/** Istegi cerceve bayt akisi olarak ring buffer'a rastgele parcalarla ve araya cop koyarak verir; parser + oturum + yanit. */
static void wire_send(fwu_byte_t type, fwu_byte_t seq, const fwu_byte_t *payload, size_t payload_length, int with_garbage)
{
    fwu_byte_t frame[FWU_FRAME_MAX_SIZE + 8u];
    fwu_byte_t out[FWU_FRAME_MAX_SIZE];
    size_t frame_length = 0u;
    size_t out_length = 0u;
    size_t sent = 0u;
    fwu_frame_view_t view;
    int got = 0;
    size_t start = 0u;

    g_last_type = type;
    if(with_garbage) {
        frame[0] = 0x00u; frame[1] = 0xFFu; frame[2] = 0x12u; frame[3] = 0xA5u;   /* SOF benzeri cop */
        start = 4u;
    }
    (void)fwu_frame_encode(type, seq, payload, payload_length, &frame[start], sizeof(frame) - start, &frame_length);
    frame_length += start;

    g_r.rc = FWU_ERR_NOT_ENOUGH_DATA;
    g_r.length = 0u;
    g_r.decoded = 0;
    for(unsigned guard = 0u; ((sent < frame_length) || !got) && (guard < 100000u); guard++) {
        fwu_return_e status;

        if(sent < frame_length) {
            size_t part = 1u + (size_t)(rnd() % 40u);
            if(part > (frame_length - sent)) {
                part = frame_length - sent;
            }
            (void)fwu_ringbuf_push(&g_wire.base, &frame[sent], part);
            sent += part;
        }
        g_now += 1u;
        status = fwu_parser_poll(&g_parser, g_now, &view);
        if(status == FWU_OK) {
            g_r.rc = fwu_session_on_frame(&g_s, &view, g_now, out, sizeof(out), &out_length);
            decode_response(out, (g_r.rc == FWU_OK) ? out_length : 0u);
            got = 1;
        }
        else if((sent >= frame_length) && (status == FWU_ERR_NOT_ENOUGH_DATA)) {
            break;                                           /* tum bayt verildi ve cerceve cikmadi */
        }
        else {
            /* bekle */
        }
    }
}

static void test_wire(void)
{
    uint32_t off = 0u;
    int ok = 1;
    fwu_activate_outcome_e outcome = FWU_ACT_OUTCOME_NONE;

    printf("\n--- gercek parser + ring buffer: rastgele parcali bayt akisi, araya cop ---\n");
    world(false, FWU_ACTIVATE_MODE_COPY);
    (void)wire_rb_init(&g_wire);
    (void)fwu_parser_init(&g_parser, &g_wire.base);

    wire_send(FWU_MSG_HELLO, 1u, (const fwu_byte_t[1]){ 1u }, 1u, 0);
    check("HELLO bayt akisindan: INFO", is_type(FWU_MSG_INFO));
    wire_send(FWU_MSG_REQUEST_DOWNLOAD, 2u, g_manifest, 138u, 1);
    check("REQUEST_DOWNLOAD (araya cop, 138 bayt): DOWNLOAD_ACCEPTED", is_type(FWU_MSG_DOWNLOAD_ACCEPTED));
    while(off < IMG_SIZE) {
        fwu_byte_t p[FWU_FRAME_MAX_PAYLOAD];
        size_t n = 0u;
        uint32_t len = IMG_SIZE - off;
        if(len > CHUNK) {
            len = CHUNK;
        }
        (void)fwu_proto_encode_data(off, &g_image[off], len, p, sizeof(p), &n);
        wire_send(FWU_MSG_TRANSFER_DATA, 3u, p, n, (int)(rnd() % 2u));
        off += len;
        ok = ok && (ack_offset() == off);
    }
    check("11 parca bayt akisindan (rastgele bolunmus, bazilarinin onunde cop): hepsi onaylandi", ok && off == IMG_SIZE);
    {
        fwu_byte_t p[4];
        fwu_write_u32_big_endian(p, IMG_SIZE);
        wire_send(FWU_MSG_TRANSFER_EXIT, 4u, p, 4u, 0);
        check("EXIT: EXIT_PENDING", is_type(FWU_MSG_EXIT_PENDING));
        poll_session();
        check("poll: VERIFY_RESULT(0)", is_type(FWU_MSG_VERIFY_RESULT) && g_r.payload[0] == 0u);
        wire_send(FWU_MSG_ACTIVATE, 5u, p, 4u, 1);
        check("ACTIVATE: ACTIVATE_OK, yeniden baslatma istendi", is_type(FWU_MSG_ACTIVATE_OK) && fwu_session_reboot_requested(&g_s));
    }
    expect("bootloader: run", bootloader_run(&outcome), FWU_OK);
    check("... uygulama = yeni imaj (bayt akisindan gelen)", outcome == FWU_ACT_OUTCOME_ACTIVATED && app_is_new());
}

/* ------------------------------------------------------------------ 12. tampon ve arguman hatalari */

static void test_out_capacity_and_args(void)
{
    fwu_byte_t frame[FWU_FRAME_MAX_SIZE];
    fwu_byte_t out[FWU_FRAME_MAX_SIZE];
    size_t frame_length = 0u;
    size_t out_length = 99u;
    fwu_frame_view_t view;
    fwu_frame_view_t bad;
    uint32_t before;

    printf("\n--- cikis tamponu ve arguman hatalari ---\n");
    world(false, FWU_ACTIVATE_MODE_COPY);
    (void)fwu_frame_encode(FWU_MSG_REQUEST_DOWNLOAD, 1u, g_manifest, 138u, frame, sizeof(frame), &frame_length);
    (void)fwu_frame_decode(frame, frame_length, &view);
    before = flash_fingerprint();

    expect("cikis tamponu 72 bayt (< 73): BUFFER_TOO_SMALL", fwu_session_on_frame(&g_s, &view, g_now, out, FWU_SESSION_MIN_OUT - 1u, &out_length), FWU_ERR_BUFFER_TOO_SMALL);
    check("... durum DEGISMEDI, flash'a dokunulmadi, out_length degismedi", fwu_session_get_state(&g_s) == FWU_SESSION_IDLE && flash_fingerprint() == before && out_length == 99u);
    expect("poll: cikis tamponu 72 bayt", fwu_session_poll(&g_s, g_now, out, FWU_SESSION_MIN_OUT - 1u, &out_length), FWU_ERR_BUFFER_TOO_SMALL);
    expect("cikis tamponu 73 bayt (tam)", fwu_session_on_frame(&g_s, &view, g_now, out, FWU_SESSION_MIN_OUT, &out_length), FWU_OK);
    check("... yanit sigdi (ACCEPTED 9 bayt + 9 = 18)", out_length == 18u && fwu_session_get_state(&g_s) == FWU_SESSION_DOWNLOADING);

    expect("session NULL", fwu_session_on_frame(NULL, &view, 0u, out, sizeof(out), &out_length), FWU_ERR_INVALID_ARG);
    expect("istek NULL", fwu_session_on_frame(&g_s, NULL, 0u, out, sizeof(out), &out_length), FWU_ERR_INVALID_ARG);
    expect("out NULL", fwu_session_on_frame(&g_s, &view, 0u, NULL, sizeof(out), &out_length), FWU_ERR_INVALID_ARG);
    expect("out_length NULL", fwu_session_on_frame(&g_s, &view, 0u, out, sizeof(out), NULL), FWU_ERR_INVALID_ARG);
    {
        /* durumu DEGISTIRECEK bir istek (ABORT) gecersiz argumanla: durum degismemeli */
        fwu_byte_t af[FWU_FRAME_MAX_SIZE];
        size_t afl = 0u;
        fwu_frame_view_t abort_view;
        const fwu_byte_t zero[1] = { 0u };

        (void)fwu_frame_encode(FWU_MSG_ABORT, 1u, zero, 1u, af, sizeof(af), &afl);
        (void)fwu_frame_decode(af, afl, &abort_view);
        expect("ABORT, out NULL", fwu_session_on_frame(&g_s, &abort_view, 0u, NULL, sizeof(out), &out_length), FWU_ERR_INVALID_ARG);
        check("... durum INDIRIYOR kaldi (gecersiz argumanla durum degismez)", fwu_session_get_state(&g_s) == FWU_SESSION_DOWNLOADING);
        expect("ABORT, out_length NULL", fwu_session_on_frame(&g_s, &abort_view, 0u, out, sizeof(out), NULL), FWU_ERR_INVALID_ARG);
        check("... durum INDIRIYOR kaldi", fwu_session_get_state(&g_s) == FWU_SESSION_DOWNLOADING);
    }
    bad = view; bad.payload = NULL;
    expect("istek payload NULL", fwu_session_on_frame(&g_s, &bad, 0u, out, sizeof(out), &out_length), FWU_ERR_INVALID_ARG);
    bad = view; bad.payload_length = 0u;
    expect("istek payload uzunlugu 0", fwu_session_on_frame(&g_s, &bad, 0u, out, sizeof(out), &out_length), FWU_ERR_INVALID_ARG);
    bad = view; bad.payload_length = 513u;
    expect("istek payload uzunlugu 513", fwu_session_on_frame(&g_s, &bad, 0u, out, sizeof(out), &out_length), FWU_ERR_INVALID_ARG);
    expect("poll: session NULL", fwu_session_poll(NULL, 0u, out, sizeof(out), &out_length), FWU_ERR_INVALID_ARG);
    expect("poll: out NULL", fwu_session_poll(&g_s, 0u, NULL, sizeof(out), &out_length), FWU_ERR_INVALID_ARG);
    expect("poll: out_length NULL", fwu_session_poll(&g_s, 0u, out, sizeof(out), NULL), FWU_ERR_INVALID_ARG);
}

/** AUTH tekrarlari (kayip AUTH_OK / kayip meydan okuma / geciken kopya): idempotent, sayac ve kilit etkilenmez. */
static void test_auth_duplicates(void)
{
    fwu_byte_t first[16];
    fwu_byte_t good[16];
    fwu_byte_t bad[16];

    printf("\n--- AUTH tekrarlari: ayni SEQ = ayni meydan okuma, YETKILI'de dogru yanit yeniden onaylanir ---\n");
    world(true, FWU_ACTIVATE_MODE_COPY);
    g_auth_ctx.fresh = true;
    tx1(FWU_MSG_AUTH_REQ, 1u);
    for(uint32_t i = 0u; i < 16u; i++) {
        first[i] = g_r.payload[i];
    }
    check("AUTH_REQ (SEQ 1): meydan okuma, begin 1 kez", is_type(FWU_MSG_AUTH_CHALLENGE) && g_auth_ctx.begin_calls == 1u);
    tx1(FWU_MSG_AUTH_REQ, 1u);
    {
        int same = is_type(FWU_MSG_AUTH_CHALLENGE) && (g_r.payload_length == 16u);
        for(uint32_t i = 0u; same && (i < 16u); i++) {
            same = (g_r.payload[i] == first[i]);
        }
        check("AYNI SEQ ile AUTH_REQ tekrari: AYNI meydan okuma yeniden verilir, begin cagrilmaz (1 kalir)",
              same && g_auth_ctx.begin_calls == 1u && fwu_session_get_state(&g_s) == FWU_SESSION_AUTH_PENDING);
    }
    check("... tekrar hata sayacini degistirmedi", g_s.auth_failures == 0u);
    tx1(FWU_MSG_AUTH_REQ, 2u);
    check("FARKLI SEQ ile AUTH_REQ (yeni deneme): YENI meydan okuma (begin 2), eskisi gecersiz", is_type(FWU_MSG_AUTH_CHALLENGE) &&
          g_auth_ctx.begin_calls == 2u && g_r.payload[0] != first[0]);
    for(uint32_t i = 0u; i < 16u; i++) {
        good[i] = (fwu_byte_t)((g_r.payload[i] ^ 0x5Au) & 0xFFu);
        bad[i] = (fwu_byte_t)(good[i] ^ 0x01u);
    }
    tx(FWU_MSG_AUTH_RESP, 3u, good, 16u);
    check("dogru yanit: AUTH_OK, YETKILI", is_type(FWU_MSG_AUTH_OK) && fwu_session_get_state(&g_s) == FWU_SESSION_AUTHENTICATED);
    tx(FWU_MSG_AUTH_RESP, 3u, good, 16u);
    check("AUTH_OK kayboldu, AUTH_RESP tekrari (ayni dogru yanit): AUTH_OK yeniden, durum YETKILI, sayac 0",
          is_type(FWU_MSG_AUTH_OK) && fwu_session_get_state(&g_s) == FWU_SESSION_AUTHENTICATED && g_s.auth_failures == 0u && !g_s.locked);
    for(uint32_t k = 0u; k < 5u; k++) {
        tx(FWU_MSG_AUTH_RESP, 4u, bad, 16u);
    }
    check("YETKILI'de YANLIS yanit (5 kez): BAD_STATE, durum YETKILI kalir, hata SAYILMAZ, KILIT YOK",
          is_error(FWU_ERR_BAD_STATE) && fwu_session_get_state(&g_s) == FWU_SESSION_AUTHENTICATED && g_s.auth_failures == 0u && !g_s.locked);
    tx(FWU_MSG_REQUEST_DOWNLOAD, 5u, g_manifest, 138u);
    check("REQUEST_DOWNLOAD: kabul, INDIRIYOR; meydan okuma silindi", is_type(FWU_MSG_DOWNLOAD_ACCEPTED) && g_s.challenge_length == 0u);
    tx(FWU_MSG_AUTH_RESP, 3u, good, 16u);
    check("INDIRIYOR'da gecikmis AUTH_RESP kopyasi: BAD_STATE (tekrar onay yalniz YETKILI'de)", is_error(FWU_ERR_BAD_STATE) && fwu_session_get_state(&g_s) == FWU_SESSION_DOWNLOADING);

    /* ABORT meydan okumayi siler */
    world(true, FWU_ACTIVATE_MODE_COPY);
    g_auth_ctx.fresh = true;
    tx1(FWU_MSG_AUTH_REQ, 1u);
    for(uint32_t i = 0u; i < 16u; i++) {
        good[i] = (fwu_byte_t)((g_r.payload[i] ^ 0x5Au) & 0xFFu);
    }
    tx(FWU_MSG_AUTH_RESP, 2u, good, 16u);
    tx1(FWU_MSG_ABORT, 3u);
    tx(FWU_MSG_AUTH_RESP, 2u, good, 16u);
    check("YETKILI -> ABORT -> eski dogru yanit tekrar: BAD_STATE (BOSTA'da yanit gecersiz), meydan okuma silindi",
          is_error(FWU_ERR_BAD_STATE) && g_s.challenge_length == 0u && fwu_session_get_state(&g_s) == FWU_SESSION_IDLE);

    /* tekrar + yanlis yanit: tek deneme sayilir */
    world(true, FWU_ACTIVATE_MODE_COPY);
    g_auth_ctx.fresh = true;
    tx1(FWU_MSG_AUTH_REQ, 1u);
    tx1(FWU_MSG_AUTH_REQ, 1u);
    tx1(FWU_MSG_AUTH_REQ, 1u);
    for(uint32_t i = 0u; i < 16u; i++) {
        bad[i] = (fwu_byte_t)(((g_r.payload[i] ^ 0x5Au) ^ 0x01u) & 0xFFu);
    }
    tx(FWU_MSG_AUTH_RESP, 2u, bad, 16u);
    check("3 AUTH_REQ tekrari + 1 yanlis yanit: TEK basarisiz deneme sayilir (kilit icin tekrar sayilmaz)",
          is_error(FWU_ERR_BAD_SIGNATURE));
    check("... sayac 1, kilit yok", g_s.auth_failures == 1u && !g_s.locked);

    /* 1 baytlik meydan okuma + 1 baytlik yanit (alt sinir) kabul edilir */
    world(true, FWU_ACTIVATE_MODE_COPY);
    g_auth_ctx.begin_length = 1u;
    tx1(FWU_MSG_AUTH_REQ, 1u);
    {
        fwu_byte_t one = (fwu_byte_t)((g_r.payload[0] ^ 0x5Au) & 0xFFu);
        check("meydan okuma 1 bayt", is_type(FWU_MSG_AUTH_CHALLENGE) && g_r.payload_length == 1u);
        tx(FWU_MSG_AUTH_RESP, 2u, &one, 1u);
        check("1 baytlik dogru yanit (alt sinir): AUTH_OK, YETKILI", is_type(FWU_MSG_AUTH_OK) && fwu_session_get_state(&g_s) == FWU_SESSION_AUTHENTICATED);
    }
}

/* ------------------------------------------------------------------ fwu_session_push_chunk / touch / getters (fwu_pull'un kullandığı giriş) */

static void test_push_chunk(void)
{
    uint32_t next = 12345u;
    fwu_byte_t other[CHUNK];

    world(false, FWU_ACTIVATE_MODE_COPY);
    expect("push_chunk: oturum BOSTA: BAD_STATE", fwu_session_push_chunk(&g_s, 0u, g_image, CHUNK, g_now, &next), FWU_ERR_BAD_STATE);
    check("hatada next_offset degismez", next == 12345u);
    check("getter'lar BOSTA oturumda: boyut 0, ofset 0, parca boyutu CHUNK",
          fwu_session_get_image_size(&g_s) == 0u && fwu_session_get_next_offset(&g_s) == 0u && fwu_session_get_chunk_size(&g_s) == CHUNK);
    check("getter'lar NULL'da 0", fwu_session_get_image_size(NULL) == 0u && fwu_session_get_next_offset(NULL) == 0u && fwu_session_get_chunk_size(NULL) == 0u);

    tx_manifest();
    check("manifest kabul (DOWNLOAD_ACCEPTED)", is_type(FWU_MSG_DOWNLOAD_ACCEPTED));
    check("getter'lar INDIRIYOR'da: boyut IMG_SIZE, ofset 0", fwu_session_get_image_size(&g_s) == IMG_SIZE && fwu_session_get_next_offset(&g_s) == 0u);

    expect("push_chunk: oturum NULL", fwu_session_push_chunk(NULL, 0u, g_image, CHUNK, g_now, &next), FWU_ERR_INVALID_ARG);
    expect("push_chunk: veri NULL", fwu_session_push_chunk(&g_s, 0u, NULL, CHUNK, g_now, &next), FWU_ERR_INVALID_ARG);
    expect("push_chunk: next_offset NULL", fwu_session_push_chunk(&g_s, 0u, g_image, CHUNK, g_now, NULL), FWU_ERR_INVALID_ARG);
    {
        fwu_session_t cold;
        for(size_t i = 0u; i < sizeof(cold); i++) {
            ((unsigned char *)&cold)[i] = 0u;
        }
        expect("push_chunk: baslatilmamis oturum: INVALID_ARG", fwu_session_push_chunk(&cold, 0u, g_image, CHUNK, g_now, &next), FWU_ERR_INVALID_ARG);
    }
    expect("push_chunk: uzunluk 0: BAD_LENGTH", fwu_session_push_chunk(&g_s, 0u, g_image, 0u, g_now, &next), FWU_ERR_BAD_LENGTH);
    expect("push_chunk: uzunluk CHUNK+1: BAD_LENGTH", fwu_session_push_chunk(&g_s, 0u, g_image, CHUNK + 1u, g_now, &next), FWU_ERR_BAD_LENGTH);
    expect("push_chunk: beklenen ofset 0 iken 64: BAD_BLOCK", fwu_session_push_chunk(&g_s, CHUNK, &g_image[CHUNK], CHUNK, g_now, &next), FWU_ERR_BAD_BLOCK);
    check("bu hatalar oturumu ve ofseti degistirmez", fwu_session_get_state(&g_s) == FWU_SESSION_DOWNLOADING && fwu_session_get_next_offset(&g_s) == 0u);

    expect("push_chunk: parca 0", fwu_session_push_chunk(&g_s, 0u, g_image, CHUNK, g_now, &next), FWU_OK);
    check("parca 0 onaylandi: next = 64, getter 64", next == CHUNK && fwu_session_get_next_offset(&g_s) == CHUNK);
    next = 0u;
    expect("push_chunk: ayni parcanin tekrari (onay kayboldu)", fwu_session_push_chunk(&g_s, 0u, g_image, CHUNK, g_now, &next), FWU_OK);
    check("tekrar yeniden onaylanir, ofset ilerlemez", next == CHUNK && fwu_session_get_next_offset(&g_s) == CHUNK);
    for(uint32_t i = 0u; i < CHUNK; i++) {
        other[i] = (fwu_byte_t)(g_image[i] ^ 0x01u);
    }
    expect("push_chunk: ayni ofset FARKLI icerik: BAD_BLOCK", fwu_session_push_chunk(&g_s, 0u, other, CHUNK, g_now, &next), FWU_ERR_BAD_BLOCK);
    expect("push_chunk: son olmayan KISA parca: BAD_LENGTH", fwu_session_push_chunk(&g_s, CHUNK, &g_image[CHUNK], 30u, g_now, &next), FWU_ERR_BAD_LENGTH);
    check("hatalardan sonra oturum hala INDIRIYOR, ofset 64", fwu_session_get_state(&g_s) == FWU_SESSION_DOWNLOADING && fwu_session_get_next_offset(&g_s) == CHUNK);

    {
        int all_ok = 1;
        for(uint32_t i = 1u; i < CHUNK_COUNT; i++) {
            const uint32_t offset = i * CHUNK;
            uint32_t n = IMG_SIZE - offset;
            if(n > CHUNK) {
                n = CHUNK;
            }
            if(fwu_session_push_chunk(&g_s, offset, &g_image[offset], n, g_now, &next) != FWU_OK || next != (offset + n)) {
                all_ok = 0;
            }
        }
        check("push_chunk ile kalan parcalar (kisa son parca dahil): hepsi OK ve ofset dogru", all_ok && fwu_session_get_next_offset(&g_s) == IMG_SIZE);
    }
    check("flash'taki imaj birebir dogru (granule dolgu dahil)", flash_image_matches(IMG_SIZE));
    tx_u32(FWU_MSG_TRANSFER_EXIT, 2u, IMG_SIZE);
    check("push_chunk ile yazilan imaj icin TRANSFER_EXIT kabul (EXIT_PENDING)", is_type(FWU_MSG_EXIT_PENDING));
    poll_session();
    check("dogrulama (imza + SHA-256) gecer: VERIFY_RESULT(0)", is_type(FWU_MSG_VERIFY_RESULT) && g_r.payload_length == 1u && g_r.payload[0] == 0u);
    tx_u32(FWU_MSG_ACTIVATE, 3u, IMG_SIZE);
    check("ACTIVATE_OK ve yeniden baslatma istendi", is_type(FWU_MSG_ACTIVATE_OK) && fwu_session_reboot_requested(&g_s));

    /* TRANSFER_DATA ile push_chunk KARISIK kullanilabilir (ayni gövde) */
    world(false, FWU_ACTIVATE_MODE_COPY);
    tx_manifest();
    check("(karisik) manifest kabul", is_type(FWU_MSG_DOWNLOAD_ACCEPTED));
    check("(karisik) ilk 3 parca TRANSFER_DATA ile", send_chunks(0u, 3u, IMG_SIZE));
    {
        int all_ok = 1;
        for(uint32_t i = 3u; i < CHUNK_COUNT; i++) {
            const uint32_t offset = i * CHUNK;
            uint32_t n = IMG_SIZE - offset;
            if(n > CHUNK) {
                n = CHUNK;
            }
            if(fwu_session_push_chunk(&g_s, offset, &g_image[offset], n, g_now, &next) != FWU_OK) {
                all_ok = 0;
            }
        }
        check("(karisik) kalan parcalar push_chunk ile", all_ok && fwu_session_get_next_offset(&g_s) == IMG_SIZE);
    }
    check("(karisik) flash'taki imaj birebir dogru", flash_image_matches(IMG_SIZE));
    /* ters yon: push_chunk ile baslayip TRANSFER_DATA ile devam: TRANSFER_DATA, push_chunk'in yazdigi ilerlemeyi gorur */
    world(false, FWU_ACTIVATE_MODE_COPY);
    tx_manifest();
    expect("(ters) push_chunk ile parca 0", fwu_session_push_chunk(&g_s, 0u, g_image, CHUNK, g_now, &next), FWU_OK);
    send_chunk(1u, IMG_SIZE);
    check("(ters) TRANSFER_DATA parca 1'i kabul eder (DATA_ACK 128)", ack_offset() == 2u * CHUNK);

    /* flash hatasi: push_chunk hata kodunu aynen dondurur ve oturum BOSTA'ya duser */
    world(false, FWU_ACTIVATE_MODE_COPY);
    tx_manifest();
    g_ff.fail_write_offset = IMAGE_OFFSET + (3u * CHUNK);
    for(uint32_t i = 0u; i < 3u; i++) {
        (void)fwu_session_push_chunk(&g_s, i * CHUNK, &g_image[i * CHUNK], CHUNK, g_now, &next);
    }
    check("(flash hatasi) ilk 3 parca yazildi", fwu_session_get_next_offset(&g_s) == 3u * CHUNK);
    expect("(flash hatasi) 4. parca: enjekte edilen kod aynen doner", fwu_session_push_chunk(&g_s, 3u * CHUNK, &g_image[3u * CHUNK], CHUNK, g_now, &next), FAULT_WRITE);
    check("(flash hatasi) oturum BOSTA'ya dustu, ofset sifirlandi", fwu_session_get_state(&g_s) == FWU_SESSION_IDLE && fwu_session_get_next_offset(&g_s) == 0u);
    expect("(flash hatasi) sonraki push_chunk: BAD_STATE", fwu_session_push_chunk(&g_s, 0u, g_image, CHUNK, g_now, &next), FWU_ERR_BAD_STATE);

    /* touch ve push_chunk boşta zaman aşımını erteler */
    world(false, FWU_ACTIVATE_MODE_COPY);
    g_now = 1000u;
    tx_manifest();                                  /* son etkinlik: 1000 */
    fwu_session_touch(&g_s, 1900u);                 /* son etkinlik: 1900 */
    g_now = 2400u;
    poll_session();
    check("touch: 1900'de dokunuldu, 2400'de (500 ms) oturum hala INDIRIYOR", fwu_session_get_state(&g_s) == FWU_SESSION_DOWNLOADING);
    expect("push_chunk now_ms ile etkinligi ilerletir", fwu_session_push_chunk(&g_s, 0u, g_image, CHUNK, 2800u, &next), FWU_OK);
    g_now = 3700u;
    poll_session();
    check("push_chunk 2800'de yazdi, 3700'de (900 ms) oturum hala INDIRIYOR", fwu_session_get_state(&g_s) == FWU_SESSION_DOWNLOADING);
    g_now = 3800u;
    poll_session();
    check("2800 + 1000 ms = 3800: oturum BOSTA'ya duser (zaman asimi sinirinda)", fwu_session_get_state(&g_s) == FWU_SESSION_IDLE);
    fwu_session_touch(NULL, 5u);
    {
        fwu_session_t cold;
        for(size_t i = 0u; i < sizeof(cold); i++) {
            ((unsigned char *)&cold)[i] = 0u;
        }
        fwu_session_touch(&cold, 5u);
    }
    check("touch(NULL) ve baslatilmamis oturumda touch cokmez", 1);
}

int main(void)
{
    test_init();
    test_hello_keepalive();
    test_happy_path();
    test_granules();
    test_state_matrix();
    test_auth();
    test_offer_rejection();
    test_download_rules();
    test_resume_abort_verify();
    test_duplicates();
    test_auth_duplicates();
    test_timeouts_and_seq();
    test_flash_faults();
    test_power_cut_sweep();
    test_fuzz();
    test_wire();
    test_out_capacity_and_args();
    test_push_chunk();

    printf("\nResult: %s (%d failed)\n", g_failures == 0 ? "ALL TESTS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
