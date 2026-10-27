/**
 * @file    fwu_platform.h
 * @brief   Shared platform settings of the STM32G0B1KEU6 (512 KB flash) reference integration. The bootloader and the application
 *          MUST use the same file: the "contract fingerprint" of fwu_boot.h is derived from these values, and an application built
 *          with different values gets CONTRACT_MISMATCH at start-up.
 *
 * Include this file BEFORE fwu_boot_profile.h (the profile and board macros are defined here); this file includes it after the macros.
 */

#ifndef FWU_PLATFORM_H_
#define FWU_PLATFORM_H_

#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ protection profile (fwu_config.h) */

#define FWU_PROFILE_DEV                 /* DEV: SHA-256 digest check PRESENT, signature ABSENT (no Ed25519 port yet). MUST NOT be used in PRODUCTION. */
#define FWU_BOARD_STM32G0B1             /* flash layout: config/layout_stm32g0b1.h */

/* Activation: COPY by default (one-way, NO trial boot). To get SWAP + ROLLBACK, uncomment the line:
 * the application must self-test on every trial boot and call fwu_boot_app_finish_trial(), otherwise after MAX_TRIAL_BOOTS it reverts to the old version. */
/* #define FWU_ACTIVATION_SWAP_ROLLBACK */

/* ------------------------------------------------------------------ product and protocol */

#define FWU_PLATFORM_PRODUCT_ID         1001u   /* must be the SAME as product_id in the manifest (see the protocol command reference) */
#define FWU_PLATFORM_MAX_TRIAL_BOOTS    3u      /* used only in swap mode */
#define FWU_PLATFORM_CHUNK_SIZE         256u    /* multiple of the granule (8), divides the page (2048), <= 508 */
#define FWU_PLATFORM_IDLE_TIMEOUT_MS    30000u  /* the session is dropped if no frame is received for this time */
#define FWU_PLATFORM_WORK_SIZE          256u    /* verify and activation buffers (RAM) */

/* ------------------------------------------------------------------ memory map */

#define FWU_PLATFORM_APP_ADDRESS        0x08009000u     /* page 18 (bootloader 18 pages = 36 KB) */
#define FWU_PLATFORM_IMAGE_MAX_PAGES    78u             /* image limit: pages 18..95. Pages 96..107 hold the application's own flash data (flash_control) and
                                                         * activation erases the application region up to the image length: the limit must not reach this data. */
#define FWU_PLATFORM_APP_MAX_SIZE       (FWU_PLATFORM_IMAGE_MAX_PAGES * 2048u)   /* 0x27000; SAME as linker FLASH LENGTH = 156K */
#define FWU_PLATFORM_RAM_BASE           0x20000000u
#define FWU_PLATFORM_RAM_END            0x20024000u     /* 144 KB */

#include "fwu_boot_profile.h"    /* AFTER the profile and board macros above */

/* ------------------------------------------------------------------ bootloader -> application RAM mailbox */

/* The LAST 32 bytes of RAM: the linker scripts of both projects define the RAM length 32 bytes lower (144K - 32), so the stack and
 * .bss never touch this area. The bootloader writes here (1) what the activation did and (2) the contract fingerprint. The application
 * reads and clears (1) at start-up (FwuResult) and compares (2) with its own configuration (fwu_boot_info_check). */
#define FWU_BOOTINFO_ADDRESS            0x20023FE0u
#define FWU_BOOTINFO_MAGIC              0x46574249u     /* "FWBI" */

typedef struct {
    uint32_t magic;         /**< FWU_BOOTINFO_MAGIC */
    uint32_t outcome;       /**< fwu_activate_outcome_e (0 NONE, 1 ACTIVATED, 2 TRIAL, 3 REVERTED, 4 REJECTED) */
    uint32_t reason;        /**< fwu_boot_reason_e */
    uint32_t check;         /**< ~(magic ^ outcome ^ reason) */
    uint8_t  contract[FWU_BOOT_INFO_SIZE];  /**< fwu_boot_info_encode record (has its own magic and CRC); written by the bootloader, NOT cleared by the application */
    uint32_t reserved;
} fwu_bootinfo_t;

static inline volatile fwu_bootinfo_t *fwu_bootinfo(void)
{
    return (volatile fwu_bootinfo_t *)FWU_BOOTINFO_ADDRESS;
}

/** Bootloader: writes the result. */
static inline void fwu_bootinfo_put(uint32_t outcome, uint32_t reason)
{
    volatile fwu_bootinfo_t *b = fwu_bootinfo();
    b->magic = FWU_BOOTINFO_MAGIC;
    b->outcome = outcome;
    b->reason = reason;
    b->check = ~(FWU_BOOTINFO_MAGIC ^ outcome ^ reason);
}

/** Application: if a valid record exists, returns `outcome` and `reason`, CLEARS the record and returns 1; otherwise returns 0 (the record is not cleared). */
static inline int fwu_bootinfo_take(uint32_t *outcome, uint32_t *reason)
{
    volatile fwu_bootinfo_t *b = fwu_bootinfo();
    int valid = (b->magic == FWU_BOOTINFO_MAGIC) && (b->check == (uint32_t)~(b->magic ^ b->outcome ^ b->reason));

    if(valid) {
        *outcome = b->outcome;
        *reason = b->reason;
        b->magic = 0u;
        b->check = 0u;
    }
    return valid;
}

/** Bootloader: writes the contract record (fwu_boot_info_encode). */
static inline void fwu_bootinfo_put_contract(const uint8_t *record)
{
    volatile fwu_bootinfo_t *b = fwu_bootinfo();
    uint32_t i;

    for(i = 0u; i < (uint32_t)FWU_BOOT_INFO_SIZE; i++) {
        b->contract[i] = record[i];
    }
    b->reserved = 0u;
}

/** Application: address of the contract record written by the bootloader (fwu_boot_info_check tells whether it is valid). */
static inline const volatile uint8_t *fwu_bootinfo_contract(void)
{
    return fwu_bootinfo()->contract;
}

/** The flash layout that BOTH projects must use: board layout + image limit (it enters the fingerprint; if it differs the application rejects). */
static inline fwu_boot_layout_t fwu_platform_boot_layout(void)
{
    fwu_boot_layout_t layout = fwu_boot_layout_from_board();

    layout.image_region_length = FWU_PLATFORM_IMAGE_MAX_PAGES * (uint32_t)FWU_BOARD_PAGE_SIZE;
    return layout;
}

#endif /* FWU_PLATFORM_H_ */
