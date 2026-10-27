/**
 * @file    flash_test.c
 * @brief   fwu_flash.h sarmalayıcıları (fwu_flash_erase/write/read) için kendi kendini kontrol eden test.
 *
 * Gerçek bir flash KULLANILMAZ. Testin içinde küçük bir SAHTE PORT vardır: hiçbir şey saklamaz,
 * yalnızca hangi işlevin kaç kez ve hangi parametrelerle çağrıldığını sayar. Böylece her senaryoda
 * üç şey birlikte sınanır:
 *   1. Sarmalayıcının döndürdüğü kod beklenenle aynı mı?
 *   2. Geçersiz istek porta ULAŞMADI mı (hiçbir sayaç artmadı)? Geçerli istek TAM BİR KEZ ulaştı mı?
 *   3. Geçerli istekte parametreler (offset, length, veri işaretçisi) DEĞİŞTİRİLMEDEN iletildi mi?
 *
 * Test flash'ının geometrisi: toplam 64, sayfa 32, yazma granülü 8, silinmiş değer 0xFF.
 * Sayfa (32) ve granül (8) bilerek FARKLI seçildi: yanlış geometri alanını kullanan bir
 * sarmalayıcı (örn. yazmada page_length) bu farktan yakalanır.
 *
 * Her senaryo TEK BİR kuralı ihlal eder (iki kuralı birden ihlal eden satırlar yalnızca
 * "kontrol sırası" bölümündedir). Kontrol sırası: geçersiz argüman, sonra aralık, sonra hiza.
 *
 * Derleme (flash klasöründen):
 *   gcc -Wall -Wextra -Wpedantic -Wconversion -std=c99 -I. -I.. -I../error fwu_flash.c flash_test.c -o flash_test && ./flash_test
 */

#include <stdio.h>
#include "fwu_flash.h"

#define TOTAL_SIZE      64u
#define PAGE_SIZE       32u
#define WRITE_GRANULE   8u
#define STUB_MARKER     0xA5u   /* sahte okuma tamponu bununla doldurur */

/* ------------------------------------------------------------------ sahte port */

/** Sahte portun durumu: flash nesnesinin `context`'i olarak kullanılır. */
typedef struct {
    int                erase_calls;    /**< stub_erase kaç kez çağrıldı. */
    int                write_calls;    /**< stub_write kaç kez çağrıldı. */
    int                read_calls;     /**< stub_read kaç kez çağrıldı. */
    uint32_t           last_offset;    /**< Son çağrıya gelen offset. */
    uint32_t           last_length;    /**< Son çağrıya gelen length. */
    const fwu_byte_t  *last_data;      /**< Son write/read çağrısına gelen veri işaretçisi. */
    fwu_return_e       return_value;   /**< Sahte portun döndüreceği sonuç (hata iletimini sınamak için). */
} stub_port_t;

static fwu_return_e stub_erase(void *context, uint32_t offset, uint32_t length)
{
    stub_port_t *port = context;
    port->erase_calls++;
    port->last_offset = offset;
    port->last_length = length;
    return port->return_value;
}

static fwu_return_e stub_write(void *context, uint32_t offset, uint32_t length, const fwu_byte_t *data)
{
    stub_port_t *port = context;
    port->write_calls++;
    port->last_offset = offset;
    port->last_length = length;
    port->last_data   = data;
    return port->return_value;
}

static fwu_return_e stub_read(void *context, uint32_t offset, uint32_t length, fwu_byte_t *data)
{
    stub_port_t *port = context;
    port->read_calls++;
    port->last_offset = offset;
    port->last_length = length;
    port->last_data   = data;
    /* Sarmalayıcının doğru tamponu ilettiğini kanıtlamak için tamponu işaretle. */
    if((port->return_value == FWU_OK) && (data != NULL) && (length <= TOTAL_SIZE)) {
        for(uint32_t i = 0u; i < length; i++) {
            data[i] = (fwu_byte_t)STUB_MARKER;
        }
    }
    return port->return_value;
}

/** Tam tablo: üç işlev de dolu. */
static const fwu_flash_callback_t g_full_callbacks = {
    .flash_erase = stub_erase, .flash_write = stub_write, .flash_read = stub_read
};
/** Eksik tablolar: tek bir işlev işaretçisi NULL. */
static const fwu_flash_callback_t g_no_erase_callbacks = {
    .flash_erase = NULL, .flash_write = stub_write, .flash_read = stub_read
};
static const fwu_flash_callback_t g_no_write_callbacks = {
    .flash_erase = stub_erase, .flash_write = NULL, .flash_read = stub_read
};
static const fwu_flash_callback_t g_no_read_callbacks = {
    .flash_erase = stub_erase, .flash_write = stub_write, .flash_read = NULL
};

/** Test flash nesnesini ve sahte portu temiz durumda kurar. */
static void make_flash(fwu_flash_t *flash, stub_port_t *port)
{
    port->erase_calls  = 0;
    port->write_calls  = 0;
    port->read_calls   = 0;
    port->last_offset  = 0u;
    port->last_length  = 0u;
    port->last_data    = NULL;
    port->return_value = FWU_OK;

    flash->context                = port;
    flash->callback               = &g_full_callbacks;
    flash->geometry.flash_length  = TOTAL_SIZE;
    flash->geometry.page_length   = PAGE_SIZE;
    flash->geometry.write_granule = WRITE_GRANULE;
    flash->geometry.erased_value  = (fwu_byte_t)0xFFu;
}

/* ------------------------------------------------------------------ test yardımcıları */

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

/** Beklenen port çağrı sayıları (hangi işlev kaç kez çağrılmış olmalı). */
typedef struct {
    int erase;
    int write;
    int read;
} expected_calls_t;

/** Sahte portun sayaçları beklenenle aynı mı? */
static int calls_match(const stub_port_t *port, expected_calls_t expected)
{
    return (port->erase_calls == expected.erase) &&
           (port->write_calls == expected.write) &&
           (port->read_calls  == expected.read);
}

/**
 * Bir sarmalayıcı çağrısının sonucunu kontrol eder: dönen kod ve port çağrı sayıları.
 * Başarısızlıkta beklenen ve gelen değerleri de yazar.
 */
static void expect(const char *name, fwu_return_e got, fwu_return_e want,
                   const stub_port_t *port, expected_calls_t calls)
{
    const int ok = (got == want) && calls_match(port, calls);
    if(ok) {
        printf("[PASS] %s\n", name);
    } else {
        printf("[FAIL] %s  (kod: got %d, expected %d; port cagri e/w/r: %d/%d/%d, expected %d/%d/%d)\n",
               name, (int)got, (int)want,
               port->erase_calls, port->write_calls, port->read_calls,
               calls.erase, calls.write, calls.read);
        g_failures++;
    }
}

/* Geçerli istekte portun beklenen çağrısı: yalnızca ilgili sayaç 1. */
static const expected_calls_t NO_CALLS    = { 0, 0, 0 };
static const expected_calls_t ERASE_ONCE  = { 1, 0, 0 };
static const expected_calls_t WRITE_ONCE  = { 0, 1, 0 };
static const expected_calls_t READ_ONCE   = { 0, 0, 1 };

/** Her senaryodan önce sahte portun sayaçlarını sıfırlar (flash nesnesi aynı kalır). */
static void reset_port(stub_port_t *port)
{
    port->erase_calls  = 0;
    port->write_calls  = 0;
    port->read_calls   = 0;
    port->last_offset  = 0u;
    port->last_length  = 0u;
    port->last_data    = NULL;
    port->return_value = FWU_OK;
}

/* ------------------------------------------------------------------ erase testleri */

static void test_erase(fwu_flash_t *flash, stub_port_t *port)
{
    printf("\n--- erase ---\n");

    /* Geçerli istekler: port 1 kez, parametreler aynen iletilir. */
    reset_port(port);
    expect("erase: (0,32) tek sayfa basindan", fwu_flash_erase(flash, 0u, 32u), FWU_OK, port, ERASE_ONCE);
    check("erase: (0,32) offset/length aynen iletildi", port->last_offset == 0u && port->last_length == 32u);

    reset_port(port);
    expect("erase: (32,32) ikinci sayfa (sinirin tam ustu)", fwu_flash_erase(flash, 32u, 32u), FWU_OK, port, ERASE_ONCE);
    check("erase: (32,32) offset/length aynen iletildi", port->last_offset == 32u && port->last_length == 32u);

    reset_port(port);
    expect("erase: (0,64) tum flash", fwu_flash_erase(flash, 0u, 64u), FWU_OK, port, ERASE_ONCE);

    /* Hiza: yalnızca hiza ihlali, hepsi aralık içinde. Erase hizası page_length'e göre. */
    reset_port(port);
    expect("erase: hizasiz length (32,10)", fwu_flash_erase(flash, 32u, 10u), FWU_ERR_ERASE_ALIGNMENT, port, NO_CALLS);
    expect("erase: hizasiz length (0,40)", fwu_flash_erase(flash, 0u, 40u), FWU_ERR_ERASE_ALIGNMENT, port, NO_CALLS);
    expect("erase: hizasiz offset (16,32)", fwu_flash_erase(flash, 16u, 32u), FWU_ERR_ERASE_ALIGNMENT, port, NO_CALLS);
    expect("erase: ikisi de hizasiz (5,10)", fwu_flash_erase(flash, 5u, 10u), FWU_ERR_ERASE_ALIGNMENT, port, NO_CALLS);
    /* Granül (8) hizalı ama sayfa (32) hizalı değil: erase page_length'e bakmalı, write_granule'a değil. */
    expect("erase: granul hizali ama sayfa hizasiz (8,8)", fwu_flash_erase(flash, 8u, 8u), FWU_ERR_ERASE_ALIGNMENT, port, NO_CALLS);

    /* Geçersiz argüman. */
    expect("erase: length 0", fwu_flash_erase(flash, 0u, 0u), FWU_ERR_INVALID_ARG, port, NO_CALLS);
    expect("erase: self NULL", fwu_flash_erase(NULL, 0u, 32u), FWU_ERR_INVALID_ARG, port, NO_CALLS);
    {
        fwu_flash_t no_table = *flash;
        no_table.callback = NULL;
        expect("erase: callback NULL", fwu_flash_erase(&no_table, 0u, 32u), FWU_ERR_INVALID_ARG, port, NO_CALLS);
    }
    {
        fwu_flash_t no_function = *flash;
        no_function.callback = &g_no_erase_callbacks;
        expect("erase: flash_erase islevi NULL", fwu_flash_erase(&no_function, 0u, 32u), FWU_ERR_INVALID_ARG, port, NO_CALLS);
    }

    /* Aralık: yalnızca aralık ihlali, hepsi sayfa hizalı. */
    expect("erase: aralik disi (64,32) offset == flash_length", fwu_flash_erase(flash, 64u, 32u), FWU_ERR_OUT_OF_RANGE, port, NO_CALLS);
    expect("erase: aralik disi (32,64) offset+length > flash_length", fwu_flash_erase(flash, 32u, 64u), FWU_ERR_OUT_OF_RANGE, port, NO_CALLS);
    expect("erase: tasma (0xFFFFFFE0,64) offset cok buyuk", fwu_flash_erase(flash, 0xFFFFFFE0u, 64u), FWU_ERR_OUT_OF_RANGE, port, NO_CALLS);
    expect("erase: tasma (0,0xFFFFFFE0) length cok buyuk", fwu_flash_erase(flash, 0u, 0xFFFFFFE0u), FWU_ERR_OUT_OF_RANGE, port, NO_CALLS);

    /* Kontrol sırası: hem aralık dışı hem hizasız ise aralık önce. */
    expect("erase: siralama, aralik+hiza birden (64,10) -> OUT_OF_RANGE", fwu_flash_erase(flash, 64u, 10u), FWU_ERR_OUT_OF_RANGE, port, NO_CALLS);

    /* Port hatası aynen yukarı iletilir. */
    reset_port(port);
    port->return_value = FWU_ERR_FLASH_WRITE;
    expect("erase: port hatasi aynen iletilir", fwu_flash_erase(flash, 0u, 32u), FWU_ERR_FLASH_WRITE, port, ERASE_ONCE);
}

/* ------------------------------------------------------------------ write testleri */

static void test_write(fwu_flash_t *flash, stub_port_t *port, fwu_byte_t *buffer)
{
    printf("\n--- write ---\n");

    /* Geçerli istekler: yazma hizası write_granule (8) ile, sayfa (32) ile değil. */
    reset_port(port);
    expect("write: (0,8) tek granul", fwu_flash_write(flash, 0u, 8u, buffer), FWU_OK, port, WRITE_ONCE);
    check("write: (0,8) offset/length/veri aynen iletildi",
          port->last_offset == 0u && port->last_length == 8u && port->last_data == buffer);

    reset_port(port);
    expect("write: (8,8) granul hizali, sayfa hizali DEGIL (gecerli)", fwu_flash_write(flash, 8u, 8u, buffer), FWU_OK, port, WRITE_ONCE);
    check("write: (8,8) offset/length aynen iletildi", port->last_offset == 8u && port->last_length == 8u);

    reset_port(port);
    expect("write: (8,16) cok granul", fwu_flash_write(flash, 8u, 16u, buffer), FWU_OK, port, WRITE_ONCE);

    reset_port(port);
    expect("write: (56,8) sinirin tam ustu", fwu_flash_write(flash, 56u, 8u, buffer), FWU_OK, port, WRITE_ONCE);

    reset_port(port);
    expect("write: (0,64) tum flash", fwu_flash_write(flash, 0u, 64u, buffer), FWU_OK, port, WRITE_ONCE);

    /* Hiza: yalnızca hiza ihlali, hepsi aralık içinde. */
    reset_port(port);
    expect("write: hizasiz offset (4,8)", fwu_flash_write(flash, 4u, 8u, buffer), FWU_ERR_WRITE_ALIGNMENT, port, NO_CALLS);
    expect("write: hizasiz length (0,12)", fwu_flash_write(flash, 0u, 12u, buffer), FWU_ERR_WRITE_ALIGNMENT, port, NO_CALLS);
    expect("write: ikisi de hizasiz (4,12)", fwu_flash_write(flash, 4u, 12u, buffer), FWU_ERR_WRITE_ALIGNMENT, port, NO_CALLS);
    expect("write: hizasiz length (8,1)", fwu_flash_write(flash, 8u, 1u, buffer), FWU_ERR_WRITE_ALIGNMENT, port, NO_CALLS);

    /* Geçersiz argüman. */
    expect("write: length 0", fwu_flash_write(flash, 0u, 0u, buffer), FWU_ERR_INVALID_ARG, port, NO_CALLS);
    expect("write: data NULL", fwu_flash_write(flash, 0u, 8u, NULL), FWU_ERR_INVALID_ARG, port, NO_CALLS);
    expect("write: self NULL", fwu_flash_write(NULL, 0u, 8u, buffer), FWU_ERR_INVALID_ARG, port, NO_CALLS);
    {
        fwu_flash_t no_table = *flash;
        no_table.callback = NULL;
        expect("write: callback NULL", fwu_flash_write(&no_table, 0u, 8u, buffer), FWU_ERR_INVALID_ARG, port, NO_CALLS);
    }
    {
        fwu_flash_t no_function = *flash;
        no_function.callback = &g_no_write_callbacks;
        expect("write: flash_write islevi NULL", fwu_flash_write(&no_function, 0u, 8u, buffer), FWU_ERR_INVALID_ARG, port, NO_CALLS);
    }

    /* Aralık: yalnızca aralık ihlali, hepsi granül hizalı. */
    expect("write: aralik disi (64,8) offset == flash_length", fwu_flash_write(flash, 64u, 8u, buffer), FWU_ERR_OUT_OF_RANGE, port, NO_CALLS);
    expect("write: aralik disi (56,16) offset+length > flash_length", fwu_flash_write(flash, 56u, 16u, buffer), FWU_ERR_OUT_OF_RANGE, port, NO_CALLS);
    expect("write: tasma (0xFFFFFFF8,16) offset cok buyuk", fwu_flash_write(flash, 0xFFFFFFF8u, 16u, buffer), FWU_ERR_OUT_OF_RANGE, port, NO_CALLS);
    expect("write: tasma (0,0xFFFFFFF8) length cok buyuk", fwu_flash_write(flash, 0u, 0xFFFFFFF8u, buffer), FWU_ERR_OUT_OF_RANGE, port, NO_CALLS);

    /* Kontrol sırası: hem aralık dışı hem hizasız ise aralık önce. */
    expect("write: siralama, aralik+hiza birden (64,4) -> OUT_OF_RANGE", fwu_flash_write(flash, 64u, 4u, buffer), FWU_ERR_OUT_OF_RANGE, port, NO_CALLS);

    /* Port hatası aynen yukarı iletilir. */
    reset_port(port);
    port->return_value = FWU_ERR_FLASH_WRITE;
    expect("write: port hatasi aynen iletilir", fwu_flash_write(flash, 0u, 8u, buffer), FWU_ERR_FLASH_WRITE, port, WRITE_ONCE);
}

/* ------------------------------------------------------------------ read testleri */

static void test_read(fwu_flash_t *flash, stub_port_t *port, fwu_byte_t *buffer)
{
    printf("\n--- read ---\n");

    /* Geçerli istekler: okumada HİZA KOŞULU YOKTUR. */
    reset_port(port);
    expect("read: (0,8)", fwu_flash_read(flash, 0u, 8u, buffer), FWU_OK, port, READ_ONCE);
    check("read: (0,8) offset/length/tampon aynen iletildi",
          port->last_offset == 0u && port->last_length == 8u && port->last_data == buffer);
    check("read: tampon port tarafindan dolduruldu", buffer[0] == (fwu_byte_t)STUB_MARKER && buffer[7] == (fwu_byte_t)STUB_MARKER);

    reset_port(port);
    expect("read: (0,64) tum flash", fwu_flash_read(flash, 0u, 64u, buffer), FWU_OK, port, READ_ONCE);

    reset_port(port);
    expect("read: (63,1) son hucre", fwu_flash_read(flash, 63u, 1u, buffer), FWU_OK, port, READ_ONCE);

    reset_port(port);
    expect("read: (3,5) hizasiz offset/length gecerli", fwu_flash_read(flash, 3u, 5u, buffer), FWU_OK, port, READ_ONCE);
    check("read: (3,5) offset/length aynen iletildi", port->last_offset == 3u && port->last_length == 5u);

    /* Geçersiz argüman. */
    reset_port(port);
    expect("read: length 0", fwu_flash_read(flash, 0u, 0u, buffer), FWU_ERR_INVALID_ARG, port, NO_CALLS);
    expect("read: data NULL", fwu_flash_read(flash, 0u, 8u, NULL), FWU_ERR_INVALID_ARG, port, NO_CALLS);
    expect("read: self NULL", fwu_flash_read(NULL, 0u, 8u, buffer), FWU_ERR_INVALID_ARG, port, NO_CALLS);
    {
        fwu_flash_t no_table = *flash;
        no_table.callback = NULL;
        expect("read: callback NULL", fwu_flash_read(&no_table, 0u, 8u, buffer), FWU_ERR_INVALID_ARG, port, NO_CALLS);
    }
    {
        fwu_flash_t no_function = *flash;
        no_function.callback = &g_no_read_callbacks;
        expect("read: flash_read islevi NULL", fwu_flash_read(&no_function, 0u, 8u, buffer), FWU_ERR_INVALID_ARG, port, NO_CALLS);
    }

    /* Aralık. */
    expect("read: aralik disi (64,1) offset == flash_length", fwu_flash_read(flash, 64u, 1u, buffer), FWU_ERR_OUT_OF_RANGE, port, NO_CALLS);
    expect("read: aralik disi (60,8) offset+length > flash_length", fwu_flash_read(flash, 60u, 8u, buffer), FWU_ERR_OUT_OF_RANGE, port, NO_CALLS);
    expect("read: tasma (0xFFFFFFFF,2) offset cok buyuk", fwu_flash_read(flash, 0xFFFFFFFFu, 2u, buffer), FWU_ERR_OUT_OF_RANGE, port, NO_CALLS);
    expect("read: tasma (0,0xFFFFFFFF) length cok buyuk", fwu_flash_read(flash, 0u, 0xFFFFFFFFu, buffer), FWU_ERR_OUT_OF_RANGE, port, NO_CALLS);

    /* Port hatası aynen yukarı iletilir. */
    reset_port(port);
    port->return_value = FWU_ERR_FLASH_WRITE;
    expect("read: port hatasi aynen iletilir", fwu_flash_read(flash, 0u, 8u, buffer), FWU_ERR_FLASH_WRITE, port, READ_ONCE);
}

/* ------------------------------------------------------------------ validate testleri */

/** fwu_flash_validate sonucunu kontrol eder (port çağrı sayacına gerek yok: işlev porta dokunmaz). */
static void expect_validate(const char *name, const fwu_flash_t *flash, fwu_return_e want)
{
    const fwu_return_e got = fwu_flash_validate(flash);
    if(got == want) {
        printf("[PASS] %s\n", name);
    } else {
        printf("[FAIL] %s  (got %d, expected %d)\n", name, (int)got, (int)want);
        g_failures++;
    }
}

/** Her senaryo geçerli bir nesneden başlar ve TEK bir koşulu bozar. */
static void test_validate(const fwu_flash_t *valid)
{
    fwu_flash_t f;

    printf("\n--- validate ---\n");

    expect_validate("validate: gecerli nesne (64/32/8)", valid, FWU_OK);

    /* Gecerli sinir ve gercekci degerler. */
    f = *valid;
    f.geometry.flash_length = 8u; f.geometry.page_length = 8u; f.geometry.write_granule = 8u;
    expect_validate("validate: flash == sayfa == granul (8/8/8) gecerli", &f, FWU_OK);

    f = *valid;
    f.geometry.flash_length = 225280u; f.geometry.page_length = 2048u; f.geometry.write_granule = 8u;
    expect_validate("validate: gercekci STM32G0 bolgesi (110 sayfa x 2048, granul 8)", &f, FWU_OK);

    f = *valid;
    f.geometry.flash_length = 96u; f.geometry.page_length = 48u; f.geometry.write_granule = 16u;
    expect_validate("validate: gecislilik 96/48/16 gecerli", &f, FWU_OK);

    /* Koşul 1: NULL'lar. */
    expect_validate("validate: self NULL", NULL, FWU_ERR_INVALID_ARG);

    f = *valid;
    f.callback = NULL;
    expect_validate("validate: callback NULL", &f, FWU_ERR_INVALID_ARG);

    f = *valid;
    f.callback = &g_no_erase_callbacks;
    expect_validate("validate: flash_erase islevi NULL", &f, FWU_ERR_INVALID_ARG);

    f = *valid;
    f.callback = &g_no_write_callbacks;
    expect_validate("validate: flash_write islevi NULL", &f, FWU_ERR_INVALID_ARG);

    f = *valid;
    f.callback = &g_no_read_callbacks;
    expect_validate("validate: flash_read islevi NULL", &f, FWU_ERR_INVALID_ARG);

    /* Koşul 2: sıfır boyutlar (her biri tek başına). */
    f = *valid;
    f.geometry.flash_length = 0u;
    expect_validate("validate: flash_length 0", &f, FWU_ERR_FLASH_GEOMETRY);

    f = *valid;
    f.geometry.page_length = 0u;
    expect_validate("validate: page_length 0", &f, FWU_ERR_FLASH_GEOMETRY);

    f = *valid;
    f.geometry.write_granule = 0u;
    expect_validate("validate: write_granule 0", &f, FWU_ERR_FLASH_GEOMETRY);

    /* Koşul 3: flash_length sayfanın katı değil (sayfa granülün katı kalır: 32 % 4 == 0). */
    f = *valid;
    f.geometry.flash_length = 60u; f.geometry.write_granule = 4u;
    expect_validate("validate: flash_length sayfanin kati degil (60/32/4)", &f, FWU_ERR_FLASH_GEOMETRY);

    /* flash_length sayfadan küçük: 16 % 32 != 0. */
    f = *valid;
    f.geometry.flash_length = 16u; f.geometry.write_granule = 4u;
    expect_validate("validate: flash_length sayfadan kucuk (16/32/4)", &f, FWU_ERR_FLASH_GEOMETRY);

    /* Koşul 4: sayfa granülün katı değil (flash sayfanın katı kalır: 64 % 32 == 0). */
    f = *valid;
    f.geometry.write_granule = 12u;
    expect_validate("validate: sayfa granulun kati degil (64/32/12)", &f, FWU_ERR_FLASH_GEOMETRY);

    /* Granül sayfadan büyük: 32 % 64 != 0 (64/32 koşulu da bozulmasın diye flash 128). */
    f = *valid;
    f.geometry.flash_length = 128u; f.geometry.write_granule = 64u;
    expect_validate("validate: granul sayfadan buyuk (128/32/64)", &f, FWU_ERR_FLASH_GEOMETRY);
}

/* ------------------------------------------------------------------ main */

int main(void)
{
    stub_port_t port;
    fwu_flash_t flash;
    fwu_byte_t  buffer[TOTAL_SIZE] = {0};

    make_flash(&flash, &port);

    test_erase(&flash, &port);
    test_write(&flash, &port, buffer);
    test_read(&flash, &port, buffer);
    test_validate(&flash);

    printf("\nResult: %s (%d failed)\n", g_failures == 0 ? "ALL TESTS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
