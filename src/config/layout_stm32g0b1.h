/**
 * @file    layout_stm32g0b1.h
 * @brief   STM32G0B1 flash layout: ONLY the board's NUMBERS. The derivations and consistency checks are
 *          in the generic fwu_layout.h (selected with -DFWU_BOARD_STM32G0B1 on the build command line).
 *
 * This file is board specific: for another chip a separate layout_<board>.h is written (e.g. layout_f2800157.h),
 * defining the same FWU_BOARD_* names; the generic file and the core do not change.
 *
 * PART: STM32G0B1KEU6 (K = 32 pins, E = 512 KB flash, U = UFQFPN32, 6 = -40..85 C; according to
 * distributor listings). The part number was given as "KBEU6" in the requirements, which is not a standard
 * code; "KEU6" was assumed, because the project's linker script is for 512 KB (0x0807FFFF). The xB (128 KB)
 * and xC (256 KB) parts of the same family have a DIFFERENT total page count: a separate numbers file is
 * needed for them.
 * Chip properties (2 KB page, dual bank, 64-bit programming) [UNVERIFIED]: verify from the data sheet.
 *
 * Unit: the page size and sizes are counts of fwu_byte_t (1 byte on STM32). Addresses are in the MCU's own
 * address unit (byte address on STM32).
 *
 * Layout (2 KB pages, total 256 pages = 512 KB). Comparison with the old layout (18 + 110 + 110 + 18):
 *
 *   page:    0..17      18..127    128..237     238..249     250..251   252..255
 *   region:  bootloader application staging      log/config   COUNTER    SPARE
 *   size:    18         110        110          12 (was 18)  2 (new)    4 (new; with K = 2)
 *   address: 0x08000000 0x08009000 0x08040000   0x08077000   0x0807D000 0x0807E000
 *
 * COUNTER region (G7d, 2 pages = A/B): the lowest accepted image version and the key version (fwu_counters.h).
 * Kept separate from the activation journal. Taken from the log/config region (its start 0x08077000 is the
 * same; the old application used only its first 7 pages); the addresses of the spare region did NOT change.
 *
 * Why it changed: the power-loss-safe journal and swap-based rollback need extra space; the old layout had
 * no spare pages. SPARE = 2 + K PAGES (decision G8, with numbers):
 *   - 2 journal pages (A and B): for rotation between A/B when the journal is full (fwu_journal_ab.h). A
 *     single page is not enough: while a page is being erased, the latest state must be safe somewhere
 *     else. A swap-based update produces ~330 step records (110 pages x 3 steps), 128 slots per page:
 *     rotation is mandatory.
 *   - K pages of swap scratch area (only for swap-based rollback: holds one page while two pages are being
 *     exchanged). The copy strategy does not use this area, but the space is always reserved so that the
 *     profile can be changed.
 *
 * K (FWU_BOARD_SCRATCH_PAGES) is ADJUSTABLE: the swap erases each page of the scratch area 1/K as many
 * times as there are pages in the update (page p -> scratch page p mod K). K is for flash LIFETIME only,
 * not for speed (the number of operations is independent of K). Rule of thumb: K = ceil(N / 26), N =
 * the REAL number of image pages (not the region size). E.g. N = 52 -> K = 2: each scratch page is erased
 * ~26 times per update; if the endurance is ~10 thousand erases [UNVERIFIED: not verified against the data
 * sheet] that is ~385 updates. Each additional scratch page costs 2 KB of flash. For the 128/256 KB parts a
 * separate board file is written and K is chosen there (K = 1 may be enough for a small image).
 * The spare page count and the log/config page count are DERIVED from K (below), not kept in sync by hand.
 *
 * The log/config region of the old application used only the first 7 pages (0..6); 7..17 were not
 * referenced anywhere; the start of log/config (0x08077000) does not change, only its end gets shorter
 * (K = 2: 12 pages, 5 pages stay unused). The bootloader and application addresses and staging
 * (the start of the 2nd bank, 0x08040000) do not change for any value of K.
 *
 * Order inside the spare region (derived by fwu_layout.h): [journal A] [journal B] [swap scratch x K].
 *
 * The region page counts can be overridden with -D ONLY for tests and for a new product layout;
 * in production the bootloader, driver and CI must be built with the same values.
 */

#ifndef FWU_LAYOUT_STM32G0B1_H_
#define FWU_LAYOUT_STM32G0B1_H_

/* ------------------------------------------------------------------ hardware facts */

#define FWU_BOARD_FLASH_BASE        0x08000000u   /* start address of the flash */
#define FWU_BOARD_PAGE_SIZE         2048u         /* erase unit (fwu_byte_t) */
#define FWU_BOARD_TOTAL_PAGES       256u          /* 512 KB / 2 KB */

/* ------------------------------------------------------------------ page counts of the regions */

#ifndef FWU_BOARD_BOOT_PAGES
#define FWU_BOARD_BOOT_PAGES        18u
#endif
#ifndef FWU_BOARD_APP_PAGES
#define FWU_BOARD_APP_PAGES         110u
#endif
#ifndef FWU_BOARD_STAGING_PAGES
#define FWU_BOARD_STAGING_PAGES     110u          /* must be the SAME size as the application */
#endif
/* Spare = 2 journal pages + K swap scratch pages. If K is given, spare is derived from it; if
 * FWU_BOARD_SPARE_PAGES is given directly (test), K = spare - 2 (fwu_layout.h). */
#ifndef FWU_BOARD_SPARE_PAGES
#ifndef FWU_BOARD_SCRATCH_PAGES
#define FWU_BOARD_SCRATCH_PAGES     2u            /* K: number of swap scratch pages (wear spreading) */
#endif
#define FWU_BOARD_SPARE_PAGES       (2u + FWU_BOARD_SCRATCH_PAGES)
#endif
/* Counter region: A/B 2 pages for the version counters (G7d). The number is fixed (fwu_layout.h rejects any other value). */
#ifndef FWU_BOARD_COUNTER_PAGES
#define FWU_BOARD_COUNTER_PAGES     2u
#endif
/* log/config = the remaining pages (default: total - the other regions); if given by hand, the total check catches a mismatch. */
#ifndef FWU_BOARD_LOG_PAGES
#define FWU_BOARD_LOG_PAGES         (FWU_BOARD_TOTAL_PAGES - FWU_BOARD_BOOT_PAGES - FWU_BOARD_APP_PAGES - \
                                     FWU_BOARD_STAGING_PAGES - FWU_BOARD_COUNTER_PAGES - FWU_BOARD_SPARE_PAGES)
#endif

/** Number of pages the old application used in the log/config region (0..6). The region cannot go below this. */
#define FWU_BOARD_LOG_MIN_PAGES     7u

#endif /* FWU_LAYOUT_STM32G0B1_H_ */
