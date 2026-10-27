/**
 * @file    fwu_boot_profile.h
 * @brief   Derives the POLICY and LAYOUT of fwu_boot from the fwu_config.h profile (-DFWU_PROFILE_... in the build command) and the board layout (-DFWU_BOARD_...).
 *          Header-only (static inline); because fwu_config.h gives #error if no profile is
 *          selected, the core (fwu_boot.h) does NOT include this: only product code does.
 *
 *   fwu_boot_config_t c = { fwu_boot_policy_from_profile(), fwu_boot_layout_from_board(), &flash, ... };
 *
 * Mapping:
 *   FWU_CFG_VERIFY      NONE -> FWU_BOOT_VERIFY_NONE;  CRC32 -> HASH (unsigned: SHA-256 + product/version/size);  SIGNED -> SIGNED
 *   FWU_CFG_ACTIVATION  COPY / SWAP_ROLLBACK -> fwu_activate_mode_e (same values)
 *   FWU_CFG_ANTI_ROLLBACK -> anti_rollback
 * NOTE: the name "CRC32" is a legacy name; the verification pipeline checks a SHA-256 digest, not a CRC-32 (without a signature there is no authority).
 */

#ifndef FWU_BOOT_PROFILE_H_
#define FWU_BOOT_PROFILE_H_

#include "fwu_config.h"
#include "fwu_boot.h"

static inline fwu_boot_policy_t fwu_boot_policy_from_profile(void)
{
    fwu_boot_policy_t policy;

#if FWU_CFG_VERIFY == FWU_VERIFY_NONE
    policy.verify = FWU_BOOT_VERIFY_NONE;
#elif FWU_CFG_VERIFY == FWU_VERIFY_CRC32
    policy.verify = FWU_BOOT_VERIFY_HASH;
#else
    policy.verify = FWU_BOOT_VERIFY_SIGNED;
#endif
#if FWU_CFG_ACTIVATION == FWU_ACTIVATE_SWAP_ROLLBACK
    policy.mode = FWU_ACTIVATE_MODE_SWAP;
#else
    policy.mode = FWU_ACTIVATE_MODE_COPY;
#endif
    policy.anti_rollback = (FWU_CFG_ANTI_ROLLBACK != 0);
    return policy;
}

#if defined(FWU_BOARD_STM32G0B1) || defined(FWU_BOARD_F2800157) || defined(FWU_BOARD_F280049C)
/** Flash layout of the board (fwu_layout.h macros: single source of truth). */
static inline fwu_boot_layout_t fwu_boot_layout_from_board(void)
{
    fwu_boot_layout_t layout;

    layout.app_offset = FWU_LAYOUT_APP_OFFSET;
    layout.staging_manifest_offset = FWU_LAYOUT_STAGING_MANIFEST_OFFSET;
    layout.staging_image_offset = FWU_LAYOUT_STAGING_IMAGE_OFFSET;
    layout.image_region_length = FWU_LAYOUT_IMAGE_MAX_SIZE;
    layout.journal_a_offset = FWU_LAYOUT_JOURNAL_A_OFFSET;
    layout.journal_b_offset = FWU_LAYOUT_JOURNAL_B_OFFSET;
    layout.journal_length = FWU_LAYOUT_JOURNAL_LENGTH;
    layout.scratch_offset = FWU_LAYOUT_SCRATCH_OFFSET;
    layout.scratch_pages = FWU_LAYOUT_SCRATCH_PAGES;
    layout.counter_a_offset = FWU_LAYOUT_COUNTER_A_OFFSET;
    layout.counter_b_offset = FWU_LAYOUT_COUNTER_B_OFFSET;
    layout.counter_length = FWU_LAYOUT_COUNTER_LENGTH;
    return layout;
}
#endif

#endif /* FWU_BOOT_PROFILE_H_ */
