/**
 * @file    fwu_layout.h
 * @brief   GENERIC flash layout: derives the first pages, offsets and addresses of the regions from the
 *          board's numbers (layout_<board>.h) and checks consistency at compile time.
 *          This is the SINGLE SOURCE OF TRUTH of the layout: the numbers are in the board file, the
 *          derivation and checks are here.
 *
 * The board is selected on the build command line (exactly one): -DFWU_BOARD_STM32G0B1, -DFWU_BOARD_F2800157 or -DFWU_BOARD_F280049C
 * Adding a new board = only writing layout_<board>.h and adding it to the two places below (selection
 * and board count); the derivations and checks do not change (STM32G0B1 and F2800157 were added this way).
 *
 * The board file must define these names (unit: fwu_byte_t, addresses in the MCU's address unit):
 *   FWU_BOARD_FLASH_BASE, FWU_BOARD_PAGE_SIZE, FWU_BOARD_TOTAL_PAGES,
 *   FWU_BOARD_BOOT_PAGES, FWU_BOARD_APP_PAGES, FWU_BOARD_STAGING_PAGES,
 *   FWU_BOARD_LOG_PAGES, FWU_BOARD_COUNTER_PAGES, FWU_BOARD_SPARE_PAGES, FWU_BOARD_LOG_MIN_PAGES
 *
 * Region order (increasing page index): bootloader, application, staging, log/config, COUNTER, spare.
 * The counter region (G7d) is 2 pages (A/B): the version counters live here (fwu_counters.h). The
 * addresses of the spare region (journal A/B + swap scratch) are NOT affected by the counter region:
 * the counter region is taken from log/config.
 *
 * The core uses OFFSETS from the start of the flash region, not ABSOLUTE ADDRESSES (fwu_flash.h);
 * the offsets are the FWU_LAYOUT_*_OFFSET constants below (unit: fwu_byte_t). Absolute addresses are
 * used only in the port, in the bootloader and in the linker script.
 */

#ifndef FWU_LAYOUT_H_
#define FWU_LAYOUT_H_

/* ------------------------------------------------------------------ board selection */

#if ((defined(FWU_BOARD_STM32G0B1) ? 1 : 0) + (defined(FWU_BOARD_F2800157) ? 1 : 0) + (defined(FWU_BOARD_F280049C) ? 1 : 0)) != 1
#error "FWU layout: exactly one board must be selected on the compiler command line (example: -DFWU_BOARD_STM32G0B1)"
#endif

#if defined(FWU_BOARD_STM32G0B1)
#include "layout_stm32g0b1.h"
#elif defined(FWU_BOARD_F2800157)
#include "layout_f2800157.h"
#elif defined(FWU_BOARD_F280049C)
#include "layout_f280049c.h"
#endif

/* ------------------------------------------------------------------ first pages (page index), derived */

#define FWU_LAYOUT_BOOT_FIRST_PAGE      0u
#define FWU_LAYOUT_APP_FIRST_PAGE       (FWU_LAYOUT_BOOT_FIRST_PAGE + FWU_BOARD_BOOT_PAGES)
#define FWU_LAYOUT_STAGING_FIRST_PAGE   (FWU_LAYOUT_APP_FIRST_PAGE + FWU_BOARD_APP_PAGES)
#define FWU_LAYOUT_LOG_FIRST_PAGE       (FWU_LAYOUT_STAGING_FIRST_PAGE + FWU_BOARD_STAGING_PAGES)
#define FWU_LAYOUT_COUNTER_FIRST_PAGE   (FWU_LAYOUT_LOG_FIRST_PAGE + FWU_BOARD_LOG_PAGES)
#define FWU_LAYOUT_SPARE_FIRST_PAGE     (FWU_LAYOUT_COUNTER_FIRST_PAGE + FWU_BOARD_COUNTER_PAGES)

/* ------------------------------------------------------------------ offsets (from flash start, fwu_byte_t) */

#define FWU_LAYOUT_OFFSET(page)         ((page) * FWU_BOARD_PAGE_SIZE)

#define FWU_LAYOUT_BOOT_OFFSET          FWU_LAYOUT_OFFSET(FWU_LAYOUT_BOOT_FIRST_PAGE)
#define FWU_LAYOUT_APP_OFFSET           FWU_LAYOUT_OFFSET(FWU_LAYOUT_APP_FIRST_PAGE)
#define FWU_LAYOUT_STAGING_OFFSET       FWU_LAYOUT_OFFSET(FWU_LAYOUT_STAGING_FIRST_PAGE)
#define FWU_LAYOUT_LOG_OFFSET           FWU_LAYOUT_OFFSET(FWU_LAYOUT_LOG_FIRST_PAGE)
#define FWU_LAYOUT_COUNTER_OFFSET       FWU_LAYOUT_OFFSET(FWU_LAYOUT_COUNTER_FIRST_PAGE)
#define FWU_LAYOUT_SPARE_OFFSET         FWU_LAYOUT_OFFSET(FWU_LAYOUT_SPARE_FIRST_PAGE)

/* Region sizes (fwu_byte_t) */
#define FWU_LAYOUT_APP_SIZE             (FWU_BOARD_APP_PAGES * FWU_BOARD_PAGE_SIZE)
#define FWU_LAYOUT_STAGING_SIZE         (FWU_BOARD_STAGING_PAGES * FWU_BOARD_PAGE_SIZE)
#define FWU_LAYOUT_LOG_SIZE             (FWU_BOARD_LOG_PAGES * FWU_BOARD_PAGE_SIZE)
#define FWU_LAYOUT_SPARE_SIZE           (FWU_BOARD_SPARE_PAGES * FWU_BOARD_PAGE_SIZE)

/* ------------------------------------------------------------------ counter region: [counter A] [counter B] */

/* The version counters (fwu_counters.h) use their own A/B journal: two separate pages, one region each. The
 * counters are kept SEPARATE from the activation journal (different lifetime, different write frequency;
 * so that a corruption of one does not affect the other). */
#define FWU_LAYOUT_COUNTER_PAGES        2u
#define FWU_LAYOUT_COUNTER_A_OFFSET     FWU_LAYOUT_COUNTER_OFFSET
#define FWU_LAYOUT_COUNTER_B_OFFSET     (FWU_LAYOUT_COUNTER_OFFSET + FWU_BOARD_PAGE_SIZE)
#define FWU_LAYOUT_COUNTER_LENGTH       FWU_BOARD_PAGE_SIZE   /* each counter region is one page */

/* ------------------------------------------------------------------ inside staging: [manifest page] [image pages] */

/* The FIRST page of staging is reserved for the manifest (138 elements, fits in a page); the image starts at the
 * second page. So the manifest is always in the same place (the bootloader finds it at start-up) and the image
 * offset is shifted by a fixed page. Activation (fwu_activate) sees the image: staging_offset =
 * FWU_LAYOUT_STAGING_IMAGE_OFFSET, region_length = FWU_LAYOUT_IMAGE_MAX_SIZE; the manifest page NEVER takes
 * part in the swap/copy. Image capacity = staging pages - 1. */
#define FWU_LAYOUT_STAGING_MANIFEST_OFFSET  FWU_LAYOUT_STAGING_OFFSET
#define FWU_LAYOUT_STAGING_IMAGE_OFFSET     (FWU_LAYOUT_STAGING_OFFSET + FWU_BOARD_PAGE_SIZE)
#define FWU_LAYOUT_IMAGE_MAX_PAGES          (FWU_BOARD_STAGING_PAGES - 1u)
#define FWU_LAYOUT_IMAGE_MAX_SIZE           (FWU_LAYOUT_IMAGE_MAX_PAGES * FWU_BOARD_PAGE_SIZE)

/* ------------------------------------------------------------------ inside spare region: [journal A] [journal B] [swap scratch] */

/* The power-loss-safe journal exists in EVERY profile (A/B, one page each); swap-based rollback additionally
 * requires a scratch page. Rationale and numbers: header of layout_stm32g0b1.h. */
#define FWU_LAYOUT_SPARE_PAGES_JOURNAL  2u
#define FWU_LAYOUT_SPARE_PAGES_SWAP     3u      /* journal 2 + swap scratch 1 */

#define FWU_LAYOUT_JOURNAL_A_OFFSET     FWU_LAYOUT_SPARE_OFFSET
#define FWU_LAYOUT_JOURNAL_B_OFFSET     (FWU_LAYOUT_SPARE_OFFSET + FWU_BOARD_PAGE_SIZE)
#define FWU_LAYOUT_JOURNAL_LENGTH       FWU_BOARD_PAGE_SIZE   /* each journal region is one page */
#if FWU_BOARD_SPARE_PAGES >= FWU_LAYOUT_SPARE_PAGES_SWAP
/* EVERY page left over in the spare region after the 2 journal pages goes to the swap scratch region. The swap
 * erases the scratch page as many times as there are pages in the update; K pages divide this wear by K
 * (fwu_activate.h). More scratch pages = increasing the number of spare pages (e.g. taking them from the
 * log/config region). */
#define FWU_LAYOUT_SCRATCH_OFFSET       (FWU_LAYOUT_SPARE_OFFSET + (FWU_LAYOUT_SPARE_PAGES_JOURNAL * FWU_BOARD_PAGE_SIZE))
#define FWU_LAYOUT_SCRATCH_PAGES        (FWU_BOARD_SPARE_PAGES - FWU_LAYOUT_SPARE_PAGES_JOURNAL)
#endif

/* ------------------------------------------------------------------ absolute addresses (port/bootloader only) */

#define FWU_LAYOUT_BOOT_ADDRESS         (FWU_BOARD_FLASH_BASE + FWU_LAYOUT_BOOT_OFFSET)
#define FWU_LAYOUT_APP_ADDRESS          (FWU_BOARD_FLASH_BASE + FWU_LAYOUT_APP_OFFSET)
#define FWU_LAYOUT_STAGING_ADDRESS      (FWU_BOARD_FLASH_BASE + FWU_LAYOUT_STAGING_OFFSET)
#define FWU_LAYOUT_LOG_ADDRESS          (FWU_BOARD_FLASH_BASE + FWU_LAYOUT_LOG_OFFSET)
#define FWU_LAYOUT_COUNTER_ADDRESS      (FWU_BOARD_FLASH_BASE + FWU_LAYOUT_COUNTER_OFFSET)
#define FWU_LAYOUT_SPARE_ADDRESS        (FWU_BOARD_FLASH_BASE + FWU_LAYOUT_SPARE_OFFSET)

/* ------------------------------------------------------------------ spare pages: fwu_config.h reads this */

#ifdef FWU_FLASH_SPARE_PAGES
#undef FWU_FLASH_SPARE_PAGES
#endif
#define FWU_FLASH_SPARE_PAGES           FWU_BOARD_SPARE_PAGES

/* ------------------------------------------------------------------ layout consistency (build errors) */

#if (FWU_BOARD_BOOT_PAGES + FWU_BOARD_APP_PAGES + FWU_BOARD_STAGING_PAGES + \
     FWU_BOARD_LOG_PAGES + FWU_BOARD_COUNTER_PAGES + FWU_BOARD_SPARE_PAGES) != FWU_BOARD_TOTAL_PAGES
#error "FWU layout: the page counts of the regions must add up to the total number of flash pages"
#endif

#if FWU_BOARD_COUNTER_PAGES != FWU_LAYOUT_COUNTER_PAGES
#error "FWU layout: the counter region must be exactly 2 pages (A and B)"
#endif

#if FWU_BOARD_STAGING_PAGES != FWU_BOARD_APP_PAGES
#error "FWU layout: the staging region must have the SAME size as the application region"
#endif

#if FWU_BOARD_STAGING_PAGES < 2
#error "FWU layout: staging must be at least 2 pages (1 manifest page + at least 1 image page)"
#endif

#if defined(FWU_BOARD_SCRATCH_PAGES) && ((FWU_BOARD_SCRATCH_PAGES + FWU_LAYOUT_SPARE_PAGES_JOURNAL) != FWU_BOARD_SPARE_PAGES)
#error "FWU layout: FWU_BOARD_SCRATCH_PAGES and FWU_BOARD_SPARE_PAGES conflict (spare = 2 journal + K scratch); give only one of them"
#endif

#if FWU_BOARD_SPARE_PAGES < FWU_LAYOUT_SPARE_PAGES_JOURNAL
#error "FWU layout: a power-loss-safe journal (A/B) needs at least 2 spare pages"
#endif

#if FWU_BOARD_LOG_PAGES < FWU_BOARD_LOG_MIN_PAGES
#error "FWU layout: the log/config region must not shrink below the number of pages the previous application used"
#endif

#endif /* FWU_LAYOUT_H_ */
