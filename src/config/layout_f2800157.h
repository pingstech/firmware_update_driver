/**
 * @file    layout_f2800157.h
 * @brief   TI TMS320F2800157-Q1 (C2000, C28x) flash layout: ONLY the board's NUMBERS. The derivations and
 *          consistency checks are in the generic fwu_layout.h (-DFWU_BOARD_F2800157 on the build command line).
 *
 * !!! UNVERIFIED VALUES [UNVERIFIED] !!!
 * The hardware facts were compiled from online sources and were NOT verified against the TRM/data sheet:
 *   - 256 KB = 128K x 16-bit words of flash, single bank (distributor listings and the TI F280013x/15x
 *     comparison table).
 *   - Sector (erase unit) size: 2 KB = 1024 words according to the family table. Caution: in the F28003x
 *     family the sector is 4 KB; it must be verified MANUALLY from the flash memory map of the F280015x TRM.
 *     If the sector is 2 KW, FWU_BOARD_PAGE_SIZE becomes 2048u, FWU_BOARD_TOTAL_PAGES becomes 64u and the
 *     region counts must be redistributed.
 *   - Flash base 0x00080000 (WORD address): this is the case in the F28002x/F28003x families; for F28001x it
 *     must be verified from the TRM.
 *   - Programming (TI Flash API, F280015x family): at most 128 bits (8 words) per call, 64-bit aligned; ECC
 *     is generated for each 64-bit aligned group; each 64-bit word can be programmed ONLY ONCE per erase
 *     cycle. The write granule (write_granule in the fwu_flash geometry) must therefore be at least 4 words
 *     (64 bits), mostly 8 words (128 bits); this value is given in the PORT's geometry (fwu_flash_c2000_init),
 *     not in the layout file.
 * Once verified, -DFWU_BOARD_NUMBERS_VERIFIED must be given for a PRODUCTION build; if it is not given, a
 * build with FWU_PRODUCTION is rejected (below).
 *
 * UNIT: on the C28x fwu_byte_t is 16 bits and the flash is WORD addressed; in this file the page size and
 * sizes are in units of fwu_byte_t = 1 WORD, and addresses are word addresses. (On STM32 the unit is a byte.)
 * Open design question (README open issue 10): is the image payload stored as 1 byte or 2 bytes per word?
 * This changes the real image capacity of the regions below by a factor of 2.
 *
 * !!! THE REGION SPLIT IS A DRAFT (product decision) !!!
 * The bootloader and application sizes of the C2000 product are not known in this repository. The split
 * below is only an EXAMPLE layout that consistently divides the total of 128 sectors; it must be changed
 * according to the real sizes.
 *
 *   sector:  0..7        8..62       63..117      118..122     123..124   125..127
 *   region:  bootloader  application staging      log/config   COUNTER    SPARE
 *   size:    8           55          55           5            2          3
 *
 * COUNTER (G7d): A/B 2 sectors for the version counters (fwu_counters.h), taken from log/config; the spare addresses are the same.
 *
 * Spare 3 sectors: 2 journal (A/B) + 1 swap scratch (rationale: layout_stm32g0b1.h). A journal sector of
 * 1024 words = 64 slots; a swap-based update produces ~165 step records (55 pages x 3), so rotation is mandatory.
 *
 * The numbers can be overridden with -D ONLY for tests and for a new product layout; in production the
 * bootloader, driver and CI must be built with the same values.
 */

#ifndef FWU_LAYOUT_F2800157_H_
#define FWU_LAYOUT_F2800157_H_

/* ------------------------------------------------------------------ hardware facts [UNVERIFIED] */

#define FWU_BOARD_FLASH_BASE        0x00080000u   /* flash base, WORD address */
#define FWU_BOARD_PAGE_SIZE         1024u         /* erase unit (sector): 2 KB = 1024 words (fwu_byte_t) */
#define FWU_BOARD_TOTAL_PAGES       128u          /* 256 KB / 2 KB */

/* ------------------------------------------------------------------ page (sector) counts of the regions: DRAFT */

#ifndef FWU_BOARD_BOOT_PAGES
#define FWU_BOARD_BOOT_PAGES        8u
#endif
#ifndef FWU_BOARD_APP_PAGES
#define FWU_BOARD_APP_PAGES         55u
#endif
#ifndef FWU_BOARD_STAGING_PAGES
#define FWU_BOARD_STAGING_PAGES     55u           /* must be the SAME size as the application */
#endif
/* Spare = 2 journal sectors + K swap scratch sectors (setting of K and rationale: layout_stm32g0b1.h). In the
 * draft K = 1; once the real size of the C2000 application is known, K = ceil(N / 26) should be chosen. */
#ifndef FWU_BOARD_SPARE_PAGES
#ifndef FWU_BOARD_SCRATCH_PAGES
#define FWU_BOARD_SCRATCH_PAGES     1u            /* K: number of swap scratch sectors (wear spreading) */
#endif
#define FWU_BOARD_SPARE_PAGES       (2u + FWU_BOARD_SCRATCH_PAGES)
#endif
/* Counter region: A/B 2 sectors for the version counters (G7d). The number is fixed (fwu_layout.h rejects any other value). */
#ifndef FWU_BOARD_COUNTER_PAGES
#define FWU_BOARD_COUNTER_PAGES     2u
#endif
/* log/config = the remaining sectors (if given by hand, the total check catches a mismatch). */
#ifndef FWU_BOARD_LOG_PAGES
#define FWU_BOARD_LOG_PAGES         (FWU_BOARD_TOTAL_PAGES - FWU_BOARD_BOOT_PAGES - FWU_BOARD_APP_PAGES - \
                                     FWU_BOARD_STAGING_PAGES - FWU_BOARD_COUNTER_PAGES - FWU_BOARD_SPARE_PAGES)
#endif

/** Smallest page count the log/config region may have (no use for it yet in the C2000 product: 1). */
#define FWU_BOARD_LOG_MIN_PAGES     1u

/* ------------------------------------------------------------------ unverified values cannot be used in production */

#if defined(FWU_PRODUCTION) && !defined(FWU_BOARD_NUMBERS_VERIFIED)
#error "FWU layout (F2800157): the numbers have not been verified against the TRM; after verifying them a production build is possible with -DFWU_BOARD_NUMBERS_VERIFIED"
#endif

#endif /* FWU_LAYOUT_F2800157_H_ */
