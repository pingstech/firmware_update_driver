/**
 * @file    test_config.c
 * @brief   fwu_config.h için derleme zamanı testi. check_config.sh tarafından, her kombinasyon
 *          için ayrı bayraklarla derlenir; beklenen değerler -DEXPECT_... ile verilir.
 *
 * Geçerli bir kombinasyonda: türetilen değerler beklenenle birebir aynı olmalı (aksi halde
 * derleme hatası). Geçersiz bir kombinasyonda: fwu_config.h'nin kendisi #error vermeli
 * (script, derlemenin BAŞARISIZ olmasını bekler).
 */

#include "fwu_config.h"

#ifdef EXPECT_VERIFY
typedef char verify_matches[(FWU_CFG_VERIFY == EXPECT_VERIFY) ? 1 : -1];
#endif
#ifdef EXPECT_ACTIVATION
typedef char activation_matches[(FWU_CFG_ACTIVATION == EXPECT_ACTIVATION) ? 1 : -1];
#endif
#ifdef EXPECT_ANTI
typedef char anti_matches[(FWU_CFG_ANTI_ROLLBACK == EXPECT_ANTI) ? 1 : -1];
#endif
typedef char power_safe_always_on[(FWU_CFG_POWER_SAFE == 1) ? 1 : -1];

#if defined(FWU_BOARD_STM32G0B1) && !defined(EXPECT_LAYOUT_SKIP_ADDRESSES)
/* Varsayılan düzen: elle yazılmış adreslere karşı (başlıktaki türetmeler yanlışsa yakalanır). */
typedef char boot_at_08000000[(FWU_LAYOUT_BOOT_ADDRESS == 0x08000000u) ? 1 : -1];
typedef char app_at_08009000[(FWU_LAYOUT_APP_ADDRESS == 0x08009000u) ? 1 : -1];
typedef char staging_at_08040000[(FWU_LAYOUT_STAGING_ADDRESS == 0x08040000u) ? 1 : -1];
typedef char log_at_08077000[(FWU_LAYOUT_LOG_ADDRESS == 0x08077000u) ? 1 : -1];
typedef char spare_at_0807E000[(FWU_LAYOUT_SPARE_ADDRESS == 0x0807E000u) ? 1 : -1];
typedef char layout_ends_at_08080000[((FWU_LAYOUT_SPARE_ADDRESS + (FWU_BOARD_SPARE_PAGES * FWU_BOARD_PAGE_SIZE)) == 0x08080000u) ? 1 : -1];
typedef char spare_pages_feed_config[(FWU_FLASH_SPARE_PAGES == 4u) ? 1 : -1];
typedef char default_scratch_pages_2[(FWU_LAYOUT_SCRATCH_PAGES == 2u) ? 1 : -1];
typedef char default_log_pages_12[(FWU_BOARD_LOG_PAGES == 12u) ? 1 : -1];
typedef char default_counter_pages_2[(FWU_BOARD_COUNTER_PAGES == 2u) ? 1 : -1];
typedef char counter_at_0807D000[(FWU_LAYOUT_COUNTER_ADDRESS == 0x0807D000u) ? 1 : -1];
/* Çekirdeğin göreceği OFSETLER (flash başından, fwu_byte_t) ve boyutlar, elle yazılmış değerlere karşı. */
typedef char app_offset[(FWU_LAYOUT_APP_OFFSET == 0x9000u) ? 1 : -1];
typedef char staging_offset[(FWU_LAYOUT_STAGING_OFFSET == 0x40000u) ? 1 : -1];
typedef char log_offset[(FWU_LAYOUT_LOG_OFFSET == 0x77000u) ? 1 : -1];
typedef char spare_offset[(FWU_LAYOUT_SPARE_OFFSET == 0x7E000u) ? 1 : -1];
typedef char staging_size[(FWU_LAYOUT_STAGING_SIZE == 225280u) ? 1 : -1];
/* Staging'in ilk sayfasi manifest, imaj ikinci sayfadan baslar; kapasite 109 sayfa. */
typedef char manifest_at_staging_start[(FWU_LAYOUT_STAGING_MANIFEST_OFFSET == 0x40000u) ? 1 : -1];
typedef char image_after_manifest_page[(FWU_LAYOUT_STAGING_IMAGE_OFFSET == 0x40800u) ? 1 : -1];
typedef char image_max_109_pages[(FWU_LAYOUT_IMAGE_MAX_PAGES == 109u) ? 1 : -1];
typedef char image_max_size[(FWU_LAYOUT_IMAGE_MAX_SIZE == 223232u) ? 1 : -1];
typedef char app_equals_staging_size[(FWU_LAYOUT_APP_SIZE == FWU_LAYOUT_STAGING_SIZE) ? 1 : -1];
typedef char log_size[(FWU_LAYOUT_LOG_SIZE == 24576u) ? 1 : -1];
typedef char spare_size[(FWU_LAYOUT_SPARE_SIZE == 8192u) ? 1 : -1];
/* Yedek bölgenin içi: [günlük A] [günlük B] [takas geçici x K], her biri bir sayfa (2048). */
typedef char journal_a_offset[(FWU_LAYOUT_JOURNAL_A_OFFSET == 0x7E000u) ? 1 : -1];
typedef char journal_b_offset[(FWU_LAYOUT_JOURNAL_B_OFFSET == 0x7E800u) ? 1 : -1];
typedef char scratch_offset[(FWU_LAYOUT_SCRATCH_OFFSET == 0x7F000u) ? 1 : -1];
typedef char journal_length[(FWU_LAYOUT_JOURNAL_LENGTH == 2048u) ? 1 : -1];
/* Sayaç bölgesi (G7d): [sayaç A] [sayaç B], log/config'in hemen arkasında, yedek bölgenin hemen önünde. */
typedef char counter_offset[(FWU_LAYOUT_COUNTER_OFFSET == 0x7D000u) ? 1 : -1];
typedef char counter_a_offset[(FWU_LAYOUT_COUNTER_A_OFFSET == 0x7D000u) ? 1 : -1];
typedef char counter_b_offset[(FWU_LAYOUT_COUNTER_B_OFFSET == 0x7D800u) ? 1 : -1];
typedef char counter_length[(FWU_LAYOUT_COUNTER_LENGTH == 2048u) ? 1 : -1];
typedef char scratch_ends_at_flash_end[((FWU_LAYOUT_SCRATCH_OFFSET + (FWU_LAYOUT_SCRATCH_PAGES * FWU_BOARD_PAGE_SIZE)) == 0x80000u) ? 1 : -1];
#endif

/* Geçici bölge, 2 günlük sayfasından arta kalan tüm yedek sayfalar (kart + özel düzen için de geçerli). */
#ifdef FWU_LAYOUT_SCRATCH_PAGES
typedef char scratch_pages_are_the_rest_of_spare[((FWU_LAYOUT_SCRATCH_PAGES + 2u) == FWU_BOARD_SPARE_PAGES) ? 1 : -1];
typedef char scratch_pages_at_least_one[(FWU_LAYOUT_SCRATCH_PAGES >= 1u) ? 1 : -1];
#endif
#if defined(EXPECT_SCRATCH_PAGES)
typedef char scratch_pages_expected[(FWU_LAYOUT_SCRATCH_PAGES == EXPECT_SCRATCH_PAGES) ? 1 : -1];
#endif

/* Manifest + imaj bölünmesi her düzende tutarlı: kapasite = staging - 1 sayfa; imaj, manifest sayfasından hemen sonra. */
#if defined(FWU_BOARD_STM32G0B1) || defined(FWU_BOARD_F2800157) || defined(FWU_BOARD_F280049C)
typedef char image_capacity_is_staging_minus_manifest_page[((FWU_LAYOUT_IMAGE_MAX_PAGES + 1u) == FWU_BOARD_STAGING_PAGES) ? 1 : -1];
typedef char image_follows_manifest_page[((FWU_LAYOUT_STAGING_IMAGE_OFFSET - FWU_LAYOUT_STAGING_MANIFEST_OFFSET) == FWU_BOARD_PAGE_SIZE) ? 1 : -1];
/* Sayaç bölgesi her düzende: log/config'in hemen arkasında başlar, yedek bölgenin başında biter, iki sayfadır. */
typedef char counter_follows_log[((FWU_LAYOUT_LOG_OFFSET + FWU_LAYOUT_LOG_SIZE) == FWU_LAYOUT_COUNTER_OFFSET) ? 1 : -1];
typedef char counter_ends_at_spare[((FWU_LAYOUT_COUNTER_B_OFFSET + FWU_LAYOUT_COUNTER_LENGTH) == FWU_LAYOUT_SPARE_OFFSET) ? 1 : -1];
typedef char counter_regions_do_not_overlap[(FWU_LAYOUT_COUNTER_B_OFFSET == (FWU_LAYOUT_COUNTER_A_OFFSET + FWU_LAYOUT_COUNTER_LENGTH)) ? 1 : -1];
#endif

#ifdef EXPECT_NO_SCRATCH
/* 2 yedek sayfa: günlük var, takas geçici sayfası YOK (tanımlı olmamalı: yanlışlıkla kullanılamasın). */
#ifdef FWU_LAYOUT_SCRATCH_OFFSET
#error "FWU_LAYOUT_SCRATCH_OFFSET must not be defined in a layout with 2 spare pages"
#endif
#endif

#if defined(FWU_BOARD_F2800157) && !defined(EXPECT_LAYOUT_SKIP_ADDRESSES)
/* F2800157 (taslak bölüşüm, kelime birimi): elle yazılmış değerlere karşı. */
typedef char c28_page_1024[(FWU_BOARD_PAGE_SIZE == 1024u) ? 1 : -1];
typedef char c28_total_128[(FWU_BOARD_TOTAL_PAGES == 128u) ? 1 : -1];
typedef char c28_boot_at_80000[(FWU_LAYOUT_BOOT_ADDRESS == 0x00080000u) ? 1 : -1];
typedef char c28_app_at_82000[(FWU_LAYOUT_APP_ADDRESS == 0x00082000u) ? 1 : -1];
typedef char c28_staging_at_8FC00[(FWU_LAYOUT_STAGING_ADDRESS == 0x0008FC00u) ? 1 : -1];
typedef char c28_log_at_9D800[(FWU_LAYOUT_LOG_ADDRESS == 0x0009D800u) ? 1 : -1];
typedef char c28_counter_at_9EC00[(FWU_LAYOUT_COUNTER_ADDRESS == 0x0009EC00u) ? 1 : -1];
typedef char c28_spare_at_9F400[(FWU_LAYOUT_SPARE_ADDRESS == 0x0009F400u) ? 1 : -1];
typedef char c28_ends_at_A0000[((FWU_LAYOUT_SPARE_ADDRESS + (FWU_BOARD_SPARE_PAGES * FWU_BOARD_PAGE_SIZE)) == 0x000A0000u) ? 1 : -1];
typedef char c28_app_offset[(FWU_LAYOUT_APP_OFFSET == 0x2000u) ? 1 : -1];
typedef char c28_staging_offset[(FWU_LAYOUT_STAGING_OFFSET == 0xFC00u) ? 1 : -1];
typedef char c28_app_size[(FWU_LAYOUT_APP_SIZE == 56320u) ? 1 : -1];
typedef char c28_staging_equals_app[(FWU_LAYOUT_STAGING_SIZE == FWU_LAYOUT_APP_SIZE) ? 1 : -1];
typedef char c28_image_offset[(FWU_LAYOUT_STAGING_IMAGE_OFFSET == 0x10000u) ? 1 : -1];
typedef char c28_image_max_54_pages[(FWU_LAYOUT_IMAGE_MAX_PAGES == 54u) ? 1 : -1];
typedef char c28_image_max_size[(FWU_LAYOUT_IMAGE_MAX_SIZE == 55296u) ? 1 : -1];
typedef char c28_spare_feeds_config[(FWU_FLASH_SPARE_PAGES == 3u) ? 1 : -1];
typedef char c28_log_size[(FWU_LAYOUT_LOG_SIZE == 5120u) ? 1 : -1];
typedef char c28_counter_a_offset[(FWU_LAYOUT_COUNTER_A_OFFSET == 0x1EC00u) ? 1 : -1];
typedef char c28_counter_b_offset[(FWU_LAYOUT_COUNTER_B_OFFSET == 0x1F000u) ? 1 : -1];
typedef char c28_counter_length[(FWU_LAYOUT_COUNTER_LENGTH == 1024u) ? 1 : -1];
/* Yedek bölgenin içi: [günlük A] [günlük B] [takas geçici], her biri bir sektör (1024 kelime). */
typedef char c28_journal_a_offset[(FWU_LAYOUT_JOURNAL_A_OFFSET == 0x1F400u) ? 1 : -1];
typedef char c28_journal_b_offset[(FWU_LAYOUT_JOURNAL_B_OFFSET == 0x1F800u) ? 1 : -1];
typedef char c28_scratch_offset[(FWU_LAYOUT_SCRATCH_OFFSET == 0x1FC00u) ? 1 : -1];
typedef char c28_journal_length[(FWU_LAYOUT_JOURNAL_LENGTH == 1024u) ? 1 : -1];
#endif

int fwu_config_test_dummy(void);
int fwu_config_test_dummy(void)
{
    return FWU_CFG_VERIFY + FWU_CFG_ACTIVATION + FWU_CFG_ANTI_ROLLBACK;
}

#if defined(FWU_BOARD_F280049C) && !defined(EXPECT_LAYOUT_SKIP_ADDRESSES)
/* F280049C (LAUNCHXL, oktet birimi, 32 sektor x 4 KW): elle yazilmis degerlere karsi. Sektor sinirlari 0x1000 kelime. */
typedef char f49_page_8192[(FWU_BOARD_PAGE_SIZE == 8192u) ? 1 : -1];
typedef char f49_total_32[(FWU_BOARD_TOTAL_PAGES == 32u) ? 1 : -1];
typedef char f49_flash_is_256kb[((FWU_BOARD_PAGE_SIZE * FWU_BOARD_TOTAL_PAGES) == 262144u) ? 1 : -1];
typedef char f49_boot_at_80000[(FWU_C28_WORD_ADDRESS(FWU_LAYOUT_BOOT_OFFSET) == 0x00080000u) ? 1 : -1];
typedef char f49_app_at_83000[(FWU_C28_WORD_ADDRESS(FWU_LAYOUT_APP_OFFSET) == 0x00083000u) ? 1 : -1];
typedef char f49_staging_at_8D000[(FWU_C28_WORD_ADDRESS(FWU_LAYOUT_STAGING_OFFSET) == 0x0008D000u) ? 1 : -1];
typedef char f49_image_at_8E000[(FWU_C28_WORD_ADDRESS(FWU_LAYOUT_STAGING_IMAGE_OFFSET) == 0x0008E000u) ? 1 : -1];
typedef char f49_image_capacity_73728[(FWU_LAYOUT_IMAGE_MAX_SIZE == 73728u) ? 1 : -1];
typedef char f49_log_at_97000[(FWU_C28_WORD_ADDRESS(FWU_LAYOUT_LOG_OFFSET) == 0x00097000u) ? 1 : -1];
typedef char f49_counter_at_9B000[(FWU_C28_WORD_ADDRESS(FWU_LAYOUT_COUNTER_OFFSET) == 0x0009B000u) ? 1 : -1];
typedef char f49_journal_a_at_9D000[(FWU_C28_WORD_ADDRESS(FWU_LAYOUT_JOURNAL_A_OFFSET) == 0x0009D000u) ? 1 : -1];
typedef char f49_journal_b_at_9E000[(FWU_C28_WORD_ADDRESS(FWU_LAYOUT_JOURNAL_B_OFFSET) == 0x0009E000u) ? 1 : -1];
typedef char f49_ends_at_A0000[(FWU_C28_WORD_ADDRESS(FWU_LAYOUT_SPARE_OFFSET + (FWU_BOARD_SPARE_PAGES * FWU_BOARD_PAGE_SIZE)) == 0x000A0000u) ? 1 : -1];
/* Staging iki bankaya yayilir: sektor 13..15 banka 0'da, sektor 16 (banka 1 basi, 0x090000) staging'in icinde. */
typedef char f49_staging_spans_bank_boundary[((FWU_C28_WORD_ADDRESS(FWU_LAYOUT_STAGING_OFFSET) < FWU_C28_BANK1_WORD_BASE) &&
    (FWU_C28_WORD_ADDRESS(FWU_LAYOUT_STAGING_OFFSET + FWU_LAYOUT_STAGING_SIZE) > FWU_C28_BANK1_WORD_BASE)) ? 1 : -1];
#endif
