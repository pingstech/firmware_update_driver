/**
 * @file    test_error.c
 * @brief   fwu_error.h için kendi kendini kontrol eden test programı.
 *
 * Derleme (error klasöründen):
 *   gcc -Wall -Wextra -Wpedantic -Wconversion -std=c99 -I. test_error.c -o test_error && ./test_error
 */

#include <stdio.h>
#include "fwu_error.h"

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

int main(void)
{
    /* Yalnızca HATA kodları (FWU_OK hariç). Yeni kod eklenince buraya da eklenmeli. */
    const fwu_return_e errors[] = {
        FWU_ERR_INVALID_ARG,
        FWU_ERR_BUFFER_TOO_SMALL,
        FWU_ERR_BAD_STATE,
        FWU_ERR_BUFFER_FULL,
        FWU_ERR_BUFFER_EMPTY,
        FWU_ERR_NOT_ENOUGH_DATA,
        FWU_ERR_CRC_MISMATCH,
        FWU_ERR_BAD_LENGTH,
        FWU_ERR_BAD_FRAME,
        FWU_ERR_BAD_MANIFEST,
        FWU_ERR_IMAGE_TOO_LARGE,
        FWU_ERR_HASH_MISMATCH,
        FWU_ERR_FLASH_WRITE,
        FWU_ERR_OUT_OF_RANGE,
        FWU_ERR_WRITE_ALIGNMENT,
        FWU_ERR_ERASE_ALIGNMENT,
        FWU_ERR_FLASH_GEOMETRY,
        FWU_ERR_FLASH_NOT_ERASED,
        FWU_ERR_FLASH_POWER_LOSS,
        FWU_ERR_FLASH_ERASE,
        FWU_ERR_BAD_SIGNATURE,
        FWU_ERR_CRYPTO_FAILURE,
        FWU_ERR_PRODUCT_MISMATCH,
        FWU_ERR_VERSION_REJECTED,
        FWU_ERR_KEY_REJECTED,
        FWU_ERR_UNKNOWN_MESSAGE,
        FWU_ERR_BAD_BLOCK,
        FWU_ERR_LOCKED_OUT,
        FWU_ERR_CONTRACT_MISMATCH,
        FWU_ERR_PULL_PROTOCOL,
        FWU_ERR_PULL_HTTP_STATUS,
        FWU_ERR_PULL_TIMEOUT,
        FWU_ERR_PULL_SIZE_MISMATCH
    };
    const int count = (int)(sizeof(errors) / sizeof(errors[0]));

    /* 1) Tek başarı değeri 0. */
    check("FWU_OK == 0", (int)FWU_OK == 0);

    /* 2) Her hata kodu 0'dan farklı (hiçbir hata başarı sanılmaz). */
    int all_nonzero = 1;
    for(int i = 0; i < count; i++) {
        if((int)errors[i] == 0) {
            all_nonzero = 0;
        }
    }
    check("her hata kodu 0'dan farkli", all_nonzero);

    /* 3) Hata kodları birbirinden farklı. */
    int all_unique = 1;
    for(int i = 0; i < count; i++) {
        for(int j = i + 1; j < count; j++) {
            if(errors[i] == errors[j]) {
                all_unique = 0;
            }
        }
    }
    check("hata kodlari birbirinden farkli", all_unique);

    /* 4) Değerler int16_t aralığına sığar (C28x'te int 16 bit). */
    int fits_int16 = 1;
    for(int i = 0; i < count; i++) {
        if((int)errors[i] < 0 || (int)errors[i] > 32767) {
            fits_int16 = 0;
        }
    }
    check("tum kodlar 0..32767 araligina sigar (16 bit int)", fits_int16);

    /* 4b) Oturum protokolü hata kodunu TEK BAYTTA taşır (fwu_proto.h): hiçbir kod 255'i aşmamalı. */
    int fits_byte = 1;
    for(int i = 0; i < count; i++) {
        if((int)errors[i] > 255) {
            fits_byte = 0;
        }
    }
    check("tum kodlar 0..255 araligina sigar (protokol hata kodunu 1 baytta tasir)", fits_byte);

    /* 5) Gruplama: onluk grup numarası beklenen kategoriyle uyuşur. */
    check("INVALID_ARG, BUFFER_TOO_SMALL ve BAD_STATE 1x grubunda",
          (int)FWU_ERR_INVALID_ARG / 10 == 1 && (int)FWU_ERR_BUFFER_TOO_SMALL / 10 == 1 &&
          (int)FWU_ERR_BAD_STATE / 10 == 1);
    check("BUFFER_FULL, BUFFER_EMPTY ve NOT_ENOUGH_DATA 2x grubunda",
          (int)FWU_ERR_BUFFER_FULL / 10 == 2 && (int)FWU_ERR_BUFFER_EMPTY / 10 == 2 &&
          (int)FWU_ERR_NOT_ENOUGH_DATA / 10 == 2);
    check("CRC_MISMATCH, BAD_LENGTH, BAD_FRAME, BAD_MANIFEST, IMAGE_TOO_LARGE ve HASH_MISMATCH 3x grubunda",
          (int)FWU_ERR_CRC_MISMATCH / 10 == 3 && (int)FWU_ERR_BAD_LENGTH / 10 == 3 &&
          (int)FWU_ERR_BAD_FRAME / 10 == 3 && (int)FWU_ERR_BAD_MANIFEST / 10 == 3 &&
          (int)FWU_ERR_IMAGE_TOO_LARGE / 10 == 3 && (int)FWU_ERR_HASH_MISMATCH / 10 == 3);
    check("FLASH_WRITE, OUT_OF_RANGE, ALIGNMENT, GEOMETRY, NOT_ERASED ve POWER_LOSS 4x grubunda",
          (int)FWU_ERR_FLASH_WRITE / 10 == 4 && (int)FWU_ERR_OUT_OF_RANGE / 10 == 4 &&
          (int)FWU_ERR_WRITE_ALIGNMENT / 10 == 4 && (int)FWU_ERR_ERASE_ALIGNMENT / 10 == 4 &&
          (int)FWU_ERR_FLASH_GEOMETRY / 10 == 4 && (int)FWU_ERR_FLASH_NOT_ERASED / 10 == 4 &&
          (int)FWU_ERR_FLASH_POWER_LOSS / 10 == 4 && (int)FWU_ERR_FLASH_ERASE / 10 == 4);

    check("BAD_SIGNATURE, CRYPTO_FAILURE, PRODUCT_MISMATCH, VERSION_REJECTED ve KEY_REJECTED 5x grubunda",
          (int)FWU_ERR_BAD_SIGNATURE / 10 == 5 && (int)FWU_ERR_CRYPTO_FAILURE / 10 == 5 &&
          (int)FWU_ERR_PRODUCT_MISMATCH / 10 == 5 && (int)FWU_ERR_VERSION_REJECTED / 10 == 5 &&
          (int)FWU_ERR_KEY_REJECTED / 10 == 5);
    check("UNKNOWN_MESSAGE, BAD_BLOCK ve LOCKED_OUT 6x grubunda",
          (int)FWU_ERR_UNKNOWN_MESSAGE / 10 == 6 && (int)FWU_ERR_BAD_BLOCK / 10 == 6 &&
          (int)FWU_ERR_LOCKED_OUT / 10 == 6);
    check("kalici numaralar: UNKNOWN_MESSAGE 60, BAD_BLOCK 61, LOCKED_OUT 62",
          (int)FWU_ERR_UNKNOWN_MESSAGE == 60 && (int)FWU_ERR_BAD_BLOCK == 61 && (int)FWU_ERR_LOCKED_OUT == 62);
    check("CONTRACT_MISMATCH 7x grubunda, kalici numara 70, tek bayta sigar (protokol hata kodunu 1 bayt tasir)",
          (int)FWU_ERR_CONTRACT_MISMATCH / 10 == 7 && (int)FWU_ERR_CONTRACT_MISMATCH == 70 && (int)FWU_ERR_CONTRACT_MISMATCH <= 255);

    printf("\nResult: %s (%d failed)\n", g_failures == 0 ? "ALL TESTS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
