/**
 * @file    test_crc16.c
 * @brief   CRC-16/MODBUS kendi kendini kontrol eden test programı (fwu_crc.h için).
 *
 * Her kontrol beklenen değerle karşılaştırılır ve GEÇTİ/KALDI yazdırılır.
 * Herhangi bir kontrol başarısız olursa program sıfırdan farklı çıkış kodu
 * döndürür (otomatik testlerde hatanın fark edilmesi için).
 *
 * CRC-16/MODBUS parametreleri:
 *   - Polinom (yansıtılmış) : 0xA001   (normal gösterim: 0x8005)
 *   - Başlangıç değeri      : 0xFFFF
 *   - Sonda ek işlem (XOR)  : yok
 *   - Doğrulama vektörü     : "123456789" -> 0x4B37
 *
 * Derleme (CRC klasöründen):
 *   gcc -Wall -Wextra -Wpedantic -Wconversion -std=c99 -I. -I.. test_crc16.c -o test_crc16
 *   gcc -DC2000 ...  (aynı komut, C28x'in 16 bitlik bayt tipini PC'de taklit eder)
 */

#include <stdio.h>
#include "fwu_crc.h"

/** Başarısız kontrol sayısı. */
static int g_failures = 0;

/**
 * @brief  Bir kontrolün sonucunu yazdırır ve başarısızlıkları sayar.
 * @param[in] name       Kontrolün adı.
 * @param[in] condition  Doğruysa (sıfırdan farklı) GEÇTİ, değilse KALDI.
 */
static void check(const char *name, int condition)
{
    printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    if(!condition) {
        g_failures++;
    }
}

/** @brief  Bir C metnini fwu_byte_t dizisine kopyalar (her harf bir bayt). */
static void text_to_bytes(const char *text, fwu_byte_t *out, size_t length)
{
    for(size_t i = 0; i < length; i++) {
        out[i] = (fwu_byte_t)text[i];
    }
}

int main(void)
{
    /* 1) Standart doğrulama vektörü. */
    fwu_byte_t vec[9];
    text_to_bytes("123456789", vec, 9);
    uint16_t vec_crc = fwu_crc16_classic_calculate(vec, 9);
    printf("    \"123456789\" CRC = 0x%04X (beklenen 0x4B37)\n", vec_crc);
    check("\"123456789\" -> 0x4B37", vec_crc == 0x4B37);

    /* 2) Boş veri: başlangıç değeri aynen döner. */
    check("bos veri -> 0xFFFF", fwu_crc16_classic_calculate(vec, 0) == FWU_CRC16_INIT);
    check("bos veri, start_value aynen doner",
          fwu_crc16_calculate_w_start_value(0x1234, vec, 0) == 0x1234);

    /* 3) Parça parça hesap, tek seferdekiyle aynı olmalı (parça boyutu 1..9). */
    int chunks_ok = 1;
    for(size_t step = 1; step <= 9; step++) {
        uint16_t crc = FWU_CRC16_INIT;
        for(size_t i = 0; i < 9; i += step) {
            size_t n = (9 - i < step) ? (9 - i) : step;
            crc = fwu_crc16_calculate_w_start_value(crc, &vec[i], n);
        }
        if(crc != vec_crc) {
            chunks_ok = 0;
        }
    }
    check("parca parca hesap == tek seferde (parca boyutu 1..9)", chunks_ok);

    /* 4) Başlangıç değeri taşınmazsa parçalı sonuç tek seferden FARKLI çıkar.
     *    (Bu, başlangıç parametresinin neden gerekli olduğunu gösterir.) */
    uint16_t naive_second = fwu_crc16_classic_calculate(&vec[4], 5);
    check("baslangic tasinmazsa parcali sonuc farkli cikar", naive_second != vec_crc);

    /* 5) Veri + CRC (düşük bayt önce) üzerinden CRC 0x0000 çıkmalı. */
    fwu_byte_t msg[11];
    text_to_bytes("TEST_DATA", msg, 9);
    uint16_t msg_crc = fwu_crc16_classic_calculate(msg, 9);
    printf("    \"TEST_DATA\" CRC = 0x%04X (beklenen 0xF440)\n", msg_crc);
    check("\"TEST_DATA\" -> 0xF440", msg_crc == 0xF440);
    msg[9]  = (fwu_byte_t)(msg_crc & FWU_BYTE_MASK);          /* LSB */
    msg[10] = (fwu_byte_t)((msg_crc >> 8) & FWU_BYTE_MASK);   /* MSB */
    check("veri + CRC -> 0x0000", fwu_crc16_classic_calculate(msg, 11) == 0x0000);

    /* 6) Tek bit değişirse CRC değişmeli. */
    fwu_byte_t flipped[9];
    text_to_bytes("123456789", flipped, 9);
    flipped[4] = (fwu_byte_t)(flipped[4] ^ 0x01u);
    check("tek bit degisince CRC degisir", fwu_crc16_classic_calculate(flipped, 9) != vec_crc);

    /* 7) Üst bitler yok sayılmalı (C28x'te 16 bitlik kapta çöp olabilir).
     *    8 bitlik platformda iki değer zaten aynıdır, -DC2000 ile gerçek test olur. */
    fwu_byte_t clean[2] = { (fwu_byte_t)0x41, (fwu_byte_t)0xFF };
    fwu_byte_t dirty[2] = { (fwu_byte_t)0x41, (fwu_byte_t)0x1FF };
    check("bayt degerinin ust bitleri yok sayilir",
          fwu_crc16_classic_calculate(clean, 2) == fwu_crc16_classic_calculate(dirty, 2));

    printf("\nResult: %s (%d failed)\n", g_failures == 0 ? "ALL TESTS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
