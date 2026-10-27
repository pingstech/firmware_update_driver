/**
 * @file    layout_f280049c.h
 * @brief   TI TMS320F280049C (LAUNCHXL-F280049C, C28x) flash layout: ONLY the board's NUMBERS. The derivations and consistency
 *          checks are in the generic fwu_layout.h (-DFWU_BOARD_F280049C on the build command line).
 *
 * VERIFICATION: the numbers were taken from TI's C2000Ware version 6.00.01.00, file device_support/f28004x/common/cmd/28004x_generic_flash_lnk.cmd
 * (TI's own linker command file): flash base 0x080000 (WORD address), two banks (0x080000 and
 * 0x090000) x 16 sectors, sector = 0x1000 words (4 KW = 8 KB), total 32 sectors = 128 KW = 256 KB. NOT additionally verified
 * against the TRM (therefore a FWU_PRODUCTION build requires -DFWU_BOARD_NUMBERS_VERIFIED).
 *
 * UNIT (IMPORTANT, DIFFERENT from the F2800157 layout): here the unit is the OCTET (8-bit value) = one fwu_byte_t element. The C28x flash is 16-bit WORD
 * addressed but the image and the protocol are an octet stream: one flash word carries TWO octets (low octet first, i.e. little endian:
 * the same order as the hex2000 --binary output). The port (ports/c2000/f28004x_direct/fwu_flash_f28004x.c, and the adapter ports/c2000/f28004x/fwu_flash_adapter.c) converts the octet offset to a word address:
 *      word address = 0x080000 + offset / 2      (offset must be even; the port checks this)
 * So the image uses 2 octets per word, not 1 (open issue 10 was decided this way for this board; see docs/KNOWN_ISSUES.md and docs/DESIGN_DECISIONS.md).
 *   - page (erase unit, sector) = 8192 octets,  total = 32 pages = 262144 octets (= 256 KB)
 *   - write granule = 16 octets (8 words = 128 bits: the largest chunk the TI Flash API programs in one call, 64-bit aligned).
 *     The granule is given in the port's geometry (fwu_flash_f28004x_init), not here.
 *
 * ADDRESSES: the FWU_LAYOUT_*_ADDRESS macros are derived in the generic file as "base + octet offset"; since the base is given as 0 on this board,
 * they are OCTET offsets, NOT real flash addresses. Use FWU_C28_WORD_ADDRESS(offset) for the real WORD address, e.g.
 * FWU_C28_WORD_ADDRESS(FWU_LAYOUT_APP_OFFSET) = word address of the application (linker script, jump, Flash API).
 *
 * REGION SPLIT (sector counts, all can be overridden with -D; the real sizes are tuned by measuring in CCS):
 *   sector:  0..2        3..12       13..22       23..26       27..28     29..31
 *   region:  bootloader  application staging      log/config   COUNTER    SPARE
 *   pages:   3           10          10           4            2          3
 *   size:    12 KW       40 KW       40 KW        16 KW        8 KW       12 KW
 * Word addresses: bootloader 0x080000, application 0x083000 (APP_BEGIN), staging 0x08D000 (manifest) and image 0x08E000,
 * log 0x097000, counter A/B 0x09B000/0x09C000, journal A/B 0x09D000/0x09E000, scratch 0x09F000.
 * General rule (fwu_layout.h): staging is the SAME size as the application; since the first page of staging is the manifest, IMAGE CAPACITY =
 * staging - 1 = 9 sectors = 73728 octets (activation writes to the application region only as much as the image length).
 * Staging 13..22 spans two banks (sectors 13..15 in bank 0, 16..22 in bank 1: from 0x090000). Since the application
 * runs from bank 0, interrupts must be disabled during erase/write and the code must run from RAM (flash_port does this).
 */

#ifndef FWU_LAYOUT_F280049C_H_
#define FWU_LAYOUT_F280049C_H_

/* ------------------------------------------------------------------ hardware facts */

#define FWU_C28_FLASH_WORD_BASE     0x00080000u   /* flash base, WORD address (TI linker command file) */
#define FWU_C28_BANK1_WORD_BASE     0x00090000u   /* start of bank 1, WORD address */
#define FWU_C28_SECTOR_WORDS        0x1000u       /* sector = 4 KW */
#define FWU_C28_OCTETS_PER_WORD     2u

#define FWU_BOARD_FLASH_BASE        0u            /* offset base: octet offsets start at 0 (use FWU_C28_*_WORD_ADDRESS for the real address) */
#define FWU_BOARD_PAGE_SIZE         8192UL        /* erase unit (sector): 4 KW = 8192 octets. UL REQUIRED: int is 16 bits on the C28x, "page * 8192u" silently overflows once it exceeds 65535 */
#define FWU_BOARD_TOTAL_PAGES       32u           /* 256 KB / 8 KB */

/* ------------------------------------------------------------------ page (sector) counts of the regions */

#ifndef FWU_BOARD_BOOT_PAGES
#define FWU_BOARD_BOOT_PAGES        3u
#endif
#ifndef FWU_BOARD_APP_PAGES
#define FWU_BOARD_APP_PAGES         10u
#endif
#ifndef FWU_BOARD_STAGING_PAGES
#define FWU_BOARD_STAGING_PAGES     10u            /* must be the SAME as the application (fwu_layout.h); first page is the manifest, image capacity 9 pages */
#endif
#ifndef FWU_BOARD_SPARE_PAGES
#ifndef FWU_BOARD_SCRATCH_PAGES
#define FWU_BOARD_SCRATCH_PAGES     1u            /* K: number of swap scratch sectors */
#endif
#define FWU_BOARD_SPARE_PAGES       (2u + FWU_BOARD_SCRATCH_PAGES)
#endif
#ifndef FWU_BOARD_COUNTER_PAGES
#define FWU_BOARD_COUNTER_PAGES     2u
#endif
#ifndef FWU_BOARD_LOG_PAGES
#define FWU_BOARD_LOG_PAGES         (FWU_BOARD_TOTAL_PAGES - FWU_BOARD_BOOT_PAGES - FWU_BOARD_APP_PAGES - \
                                     FWU_BOARD_STAGING_PAGES - FWU_BOARD_COUNTER_PAGES - FWU_BOARD_SPARE_PAGES)
#endif

/** Smallest page count the log/config region may have (no use for it yet in this product: 1). */
#define FWU_BOARD_LOG_MIN_PAGES     1u

/* ------------------------------------------------------------------ real WORD addresses (derived from the offset) */

/** Flash WORD address from an octet offset (offset must be even). */
#define FWU_C28_WORD_ADDRESS(octet_offset)  (FWU_C28_FLASH_WORD_BASE + ((octet_offset) / FWU_C28_OCTETS_PER_WORD))

/* ------------------------------------------------------------------ unverified values cannot be used in production */

#if defined(FWU_PRODUCTION) && !defined(FWU_BOARD_NUMBERS_VERIFIED)
#error "FWU layout (F280049C): the numbers have not been verified against the TRM; after verifying them a production build is possible with -DFWU_BOARD_NUMBERS_VERIFIED"
#endif

#endif /* FWU_LAYOUT_F280049C_H_ */
