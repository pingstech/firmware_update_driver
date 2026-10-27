/**
 * @file    test_boot_profile.c
 * @brief   fwu_boot_profile.h testi: derleme komutundaki profil (-DFWU_PROFILE_...) ve kart (-DFWU_BOARD_...) fwu_boot politikasına
 *          ve düzenine DOĞRU eşlenir. Beklenen değerler komut satırından gelir (-DEXPECT_VERIFY, -DEXPECT_MODE, -DEXPECT_AR);
 *          run_tests.sh her profil için ayrı derler (check_config.sh ile aynı yöntem).
 *
 * Düzen sayıları ELLE türetildi (STM32G0B1KEU6, sayfa 2048): bootloader 0..17, uygulama 18..127, staging 128..237, log 238..249,
 *   sayaç 250..251, yedek 252..255 (günlük A, günlük B, geçici x2).
 *
 * Derleme (boot klasöründen), örnek:
 *   gcc -std=c99 -I. -I.. -I../error -I../bytes -I../crc -I../flash -I../journal -I../activation -I../manifest -I../crypto -I../verify -I../counters -I../config \
 *       -DFWU_BOARD_STM32G0B1 -DFWU_PROFILE_AUTOMOTIVE -DEXPECT_VERIFY=2 -DEXPECT_MODE=1 -DEXPECT_AR=1 test_boot_profile.c -o test_boot_profile && ./test_boot_profile
 */

#include <stdio.h>
#include "fwu_boot_profile.h"

#ifndef EXPECT_VERIFY
#error "EXPECT_VERIFY must be defined (0 NONE, 1 HASH, 2 SIGNED)"
#endif
#ifndef EXPECT_MODE
#error "EXPECT_MODE must be defined (0 COPY, 1 SWAP)"
#endif
#ifndef EXPECT_AR
#error "EXPECT_AR must be defined (0/1)"
#endif

static int g_failures = 0;

static void check(const char *name, int condition)
{
    printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    if(!condition) {
        g_failures++;
    }
}

int main(void)
{
    const fwu_boot_policy_t p = fwu_boot_policy_from_profile();

    check("dogrulama seviyesi profilden", (int)p.verify == EXPECT_VERIFY);
    check("aktivasyon modu profilden", (int)p.mode == EXPECT_MODE);
    check("anti-rollback profilden", (p.anti_rollback ? 1 : 0) == EXPECT_AR);
    check("enum degerleri fwu_config.h degerleriyle ayni (CRC32 -> HASH 1, SIGNED 2; COPY 0, SWAP 1)",
          (int)FWU_BOOT_VERIFY_HASH == FWU_VERIFY_CRC32 && (int)FWU_BOOT_VERIFY_SIGNED == FWU_VERIFY_SIGNED && (int)FWU_BOOT_VERIFY_NONE == FWU_VERIFY_NONE &&
          (int)FWU_ACTIVATE_MODE_COPY == FWU_ACTIVATE_COPY && (int)FWU_ACTIVATE_MODE_SWAP == FWU_ACTIVATE_SWAP_ROLLBACK);
#if defined(FWU_BOARD_STM32G0B1)
    {
        const fwu_boot_layout_t l = fwu_boot_layout_from_board();
        check("STM32: uygulama 18. sayfa = 36864", l.app_offset == 36864u);
        check("STM32: staging manifest 128. sayfa = 262144, imaj bir sayfa sonra = 264192", l.staging_manifest_offset == 262144u && l.staging_image_offset == 264192u);
        check("STM32: imaj kapasitesi 109 sayfa = 223232", l.image_region_length == 223232u);
        check("STM32: gunluk A 252. sayfa = 516096, B = 518144, uzunluk 2048", l.journal_a_offset == 516096u && l.journal_b_offset == 518144u && l.journal_length == 2048u);
        check("STM32: gecici bolge gunlukten sonra = 520192, 2 sayfa", l.scratch_offset == 520192u && l.scratch_pages == 2u);
        check("STM32: sayaclar 250. sayfa = 512000 ve 514048, uzunluk 2048", l.counter_a_offset == 512000u && l.counter_b_offset == 514048u && l.counter_length == 2048u);
    }
#endif

    printf("\nResult: %s (%d failed)\n", g_failures == 0 ? "ALL TESTS PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
