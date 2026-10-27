/**
 * @file    test_bytes.c
 * @brief   fwu_bytes.h için kendi kendini kontrol eden test programı.
 *
 * Derleme (bytes klasöründen):
 *   gcc -Wall -Wextra -Wpedantic -Wconversion -std=c99 -I. -I.. test_bytes.c -o test_bytes
 *   gcc -DC2000 ...  (aynı komut, C28x'in 16 bitlik bayt tipini PC'de taklit eder)
 */

#include <stdio.h>
#include "fwu_bytes.h"

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

/** Dizinin 4 elemanını 0xAA ile doldurur (taşma/yan etki tespiti için nöbetçi). */
static void fill_guard(fwu_byte_t *buf)
{
    for(int i = 0; i < 4; i++) {
        buf[i] = (fwu_byte_t)0xAA;
    }
}

/** Elemanın 0..255 aralığında olup olmadığını döndürür (üst bit yok). */
static int is_byte_value(fwu_byte_t b)
{
    return (((unsigned int)b & ~(unsigned int)FWU_BYTE_MASK) == 0u);
}

int main(void)
{
    fwu_byte_t buf[4];

    /* 1) Ana örnek: 0x1234 */
    fill_guard(buf);
    fwu_write_u16_big_endian(buf, 0x1234);
    check("big-endian    0x1234 -> {0x12, 0x34}", buf[0] == 0x12 && buf[1] == 0x34);

    fill_guard(buf);
    fwu_write_u16_little_endian(buf, 0x1234);
    check("little-endian 0x1234 -> {0x34, 0x12}", buf[0] == 0x34 && buf[1] == 0x12);

    /* 2) Uç değerler. */
    fill_guard(buf);
    fwu_write_u16_big_endian(buf, 0x0000);
    check("big-endian    0x0000 -> {0x00, 0x00}", buf[0] == 0x00 && buf[1] == 0x00);

    fill_guard(buf);
    fwu_write_u16_big_endian(buf, 0xFFFF);
    check("big-endian    0xFFFF -> {0xFF, 0xFF}", buf[0] == 0xFF && buf[1] == 0xFF);

    fill_guard(buf);
    fwu_write_u16_big_endian(buf, 0x00FF);
    check("big-endian    0x00FF -> {0x00, 0xFF}", buf[0] == 0x00 && buf[1] == 0xFF);

    fill_guard(buf);
    fwu_write_u16_little_endian(buf, 0xFF00);
    check("little-endian 0xFF00 -> {0x00, 0xFF}", buf[0] == 0x00 && buf[1] == 0xFF);

    /* 3) Üst bit (0x80) taşıyan değer: işaret sorunu olmamalı. */
    fill_guard(buf);
    fwu_write_u16_big_endian(buf, 0x8001);
    check("big-endian    0x8001 -> {0x80, 0x01}", buf[0] == 0x80 && buf[1] == 0x01);

    /* 4) Yan etki: sadece ilk iki eleman değişmeli (buf[2], buf[3] nöbetçi kalmalı). */
    fill_guard(buf);
    fwu_write_u16_big_endian(buf, 0x1234);
    check("big-endian    sadece 2 eleman degisir", buf[2] == 0xAA && buf[3] == 0xAA);

    fill_guard(buf);
    fwu_write_u16_little_endian(buf, 0x1234);
    check("little-endian sadece 2 eleman degisir", buf[2] == 0xAA && buf[3] == 0xAA);

    /* 5) Yazılan elemanlar 0..255 aralığında olmalı (C28x'te 16 bitlik kapta üst bit yok). */
    fill_guard(buf);
    fwu_write_u16_big_endian(buf, 0xFFFF);
    check("big-endian    elemanlar 0..255", is_byte_value(buf[0]) && is_byte_value(buf[1]));

    fill_guard(buf);
    fwu_write_u16_little_endian(buf, 0xFFFF);
    check("little-endian elemanlar 0..255", is_byte_value(buf[0]) && is_byte_value(buf[1]));

    /* 6) Okuma: dizi -> sayı. */
    fwu_byte_t be_in[2] = { 0x12, 0x34 };
    check("big-endian oku    {0x12, 0x34} -> 0x1234", fwu_read_u16_big_endian(be_in) == 0x1234);

    fwu_byte_t le_in[2] = { 0x34, 0x12 };
    check("little-endian oku {0x34, 0x12} -> 0x1234", fwu_read_u16_little_endian(le_in) == 0x1234);

    /* Üst baytı 0x80 olan değer: işaret/taşma tuzağı (özellikle 16 bitlik int'te). */
    fwu_byte_t hi_in[2] = { 0x80, 0x01 };
    check("big-endian oku    {0x80, 0x01} -> 0x8001", fwu_read_u16_big_endian(hi_in) == 0x8001);

    /* 7) Gidiş-dönüş: yaz, sonra oku, aynı değer gelmeli. */
    const uint16_t round_trip_values[5] = { 0x0000, 0x0001, 0x1234, 0x8001, 0xFFFF };
    int round_trip_ok = 1;
    for(int i = 0; i < 5; i++) {
        fwu_byte_t tmp[2];

        fwu_write_u16_big_endian(tmp, round_trip_values[i]);
        if(fwu_read_u16_big_endian(tmp) != round_trip_values[i]) {
            round_trip_ok = 0;
        }

        fwu_write_u16_little_endian(tmp, round_trip_values[i]);
        if(fwu_read_u16_little_endian(tmp) != round_trip_values[i]) {
            round_trip_ok = 0;
        }
    }
    check("gidis-donus (yaz sonra oku): 0, 1, 0x1234, 0x8001, 0xFFFF", round_trip_ok);

    /* 8) Üst bitler yok sayılmalı (C28x'te 16 bitlik kapta çöp olabilir).
     *    8 bitlik platformda iki dizi zaten aynıdır, -DC2000 ile gerçek test olur. */
    fwu_byte_t dirty_in[2] = { (fwu_byte_t)0x1FF, 0x34 };
    fwu_byte_t clean_in[2] = { 0xFF, 0x34 };
    check("okuma: elemanin ust bitleri yok sayilir",
          fwu_read_u16_big_endian(dirty_in) == fwu_read_u16_big_endian(clean_in));

    /* 9) 32 bit yazma. */
    fwu_byte_t buf32[4];
    fwu_write_u32_big_endian(buf32, 0x89ABCDEFu);
    check("u32 big-endian    yaz 0x89ABCDEF -> {0x89, 0xAB, 0xCD, 0xEF}",
          buf32[0] == 0x89 && buf32[1] == 0xAB && buf32[2] == 0xCD && buf32[3] == 0xEF);

    fwu_write_u32_little_endian(buf32, 0x89ABCDEFu);
    check("u32 little-endian yaz 0x89ABCDEF -> {0xEF, 0xCD, 0xAB, 0x89}",
          buf32[0] == 0xEF && buf32[1] == 0xCD && buf32[2] == 0xAB && buf32[3] == 0x89);

    /* 10) 32 bit okuma. */
    fwu_byte_t be32[4] = { 0x12, 0x34, 0x56, 0x78 };
    check("u32 big-endian    oku {0x12,0x34,0x56,0x78} -> 0x12345678",
          fwu_read_u32_big_endian(be32) == 0x12345678u);

    fwu_byte_t le32[4] = { 0x78, 0x56, 0x34, 0x12 };
    check("u32 little-endian oku {0x78,0x56,0x34,0x12} -> 0x12345678",
          fwu_read_u32_little_endian(le32) == 0x12345678u);

    /* En üst baytı 0x80 olan değerler: kaydırma/işaret tuzağı. */
    fwu_byte_t hi_be32[4] = { 0x80, 0x00, 0x00, 0x00 };
    check("u32 big-endian    oku {0x80,0,0,0} -> 0x80000000",
          fwu_read_u32_big_endian(hi_be32) == 0x80000000u);

    fwu_byte_t hi_le32[4] = { 0x00, 0x00, 0x00, 0x80 };
    check("u32 little-endian oku {0,0,0,0x80} -> 0x80000000",
          fwu_read_u32_little_endian(hi_le32) == 0x80000000u);

    /* 11) 32 bit gidiş-dönüş. */
    const uint32_t round_trip_values32[5] = { 0x00000000u, 0x00000001u, 0x12345678u, 0x80000000u, 0xFFFFFFFFu };
    int round_trip32_ok = 1;
    for(int i = 0; i < 5; i++) {
        fwu_byte_t tmp32[4];

        fwu_write_u32_big_endian(tmp32, round_trip_values32[i]);
        if(fwu_read_u32_big_endian(tmp32) != round_trip_values32[i]) {
            round_trip32_ok = 0;
        }

        fwu_write_u32_little_endian(tmp32, round_trip_values32[i]);
        if(fwu_read_u32_little_endian(tmp32) != round_trip_values32[i]) {
            round_trip32_ok = 0;
        }
    }
    check("u32 gidis-donus: 0, 1, 0x12345678, 0x80000000, 0xFFFFFFFF", round_trip32_ok);

    printf("\nResult: %s (%d failed)\n", g_failures == 0 ? "ALL TESTS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
