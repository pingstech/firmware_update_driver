/**
 * @file    fwu_config.h
 * @brief   Compile-time PROTECTION PROFILE: verification and activation options, and detection of
 *          forbidden combinations as build errors.
 *
 * The selection is made by the INTEGRATING DEVELOPER on the build command line; it cannot be selected
 * at run time over the communication link (otherwise an attacker could select the "no protection"
 * mode: a downgrade attack).
 *
 * THREE AXES
 *   1. Power-loss-safe update (journal + commit record LAST):
 *        mandatory in EVERY profile, not an option (FWU_CFG_POWER_SAFE is always 1).
 *   2. Verification:  FWU_VERIFY_NONE | FWU_VERIFY_CRC32 | FWU_VERIFY_SIGNED (SHA-256 + signature).
 *   3. Activation:    FWU_ACTIVATE_COPY (copy from staging, one-way) |
 *                     FWU_ACTIVATE_SWAP_ROLLBACK (swap + trial boot, return to the old version on failure).
 *      SPARE PAGES: the power-loss-safe journal requires 2 spare pages in EVERY profile (A/B journal);
 *      swap-based rollback additionally requires 1 scratch page (3 in total). This number comes from the
 *      flash layout and is given on the build command line (-DFWU_FLASH_SPARE_PAGES=n, 0 if not given).
 *
 * PROFILES (EXACTLY ONE on the build command line: -DFWU_PROFILE_...):
 *   FWU_PROFILE_DEV         verification CRC-32, activation copy, no anti-rollback.
 *                           CANNOT BE BUILT FOR PRODUCTION (error if given together with -DFWU_PRODUCTION).
 *   FWU_PROFILE_INDUSTRIAL  verification signature (SHA-256), activation copy by default;
 *                           -DFWU_ACTIVATION_SWAP_ROLLBACK enables swap-based rollback (3 spare pages required).
 *   FWU_PROFILE_AUTOMOTIVE  verification signature, swap-based rollback MANDATORY, anti-rollback version counter present.
 *
 * IMPORTANT (two separate "rollbacks"): FWU_CFG_ACTIVATION = return to the old version (from a failed
 * new version), FWU_CFG_ANTI_ROLLBACK = an attacker must not be able to install an old, vulnerable
 * version (version counter). The version counter must be incremented ONLY after the new version has
 * confirmed itself, otherwise the device's own recovery would be rejected by anti-rollback.
 *
 * The bootloader and the CI signing tool MUST be built with the SAME profile and layout (otherwise the
 * device may be bricked); this header is part of that shared contract.
 */

#ifndef FWU_CONFIG_H_
#define FWU_CONFIG_H_

/* ------------------------------------------------------------------ value definitions */

#define FWU_VERIFY_NONE             0   /* development only */
#define FWU_VERIFY_CRC32            1   /* integrity (accidental corruption), no authorization */
#define FWU_VERIFY_SIGNED           2   /* SHA-256 + signature (integrity and authorization) */

#define FWU_ACTIVATE_COPY           0   /* copy from staging, one-way */
#define FWU_ACTIVATE_SWAP_ROLLBACK  1   /* swap + trial boot + return to the old version */

/* ------------------------------------------------------------------ number of spare pages */

/* If a board is selected (-DFWU_BOARD_...), the number of spare pages comes from the flash layout (single source of truth). */
#if defined(FWU_BOARD_STM32G0B1) || defined(FWU_BOARD_F2800157) || defined(FWU_BOARD_F280049C)
#include "fwu_layout.h"
#endif

#ifndef FWU_FLASH_SPARE_PAGES
#define FWU_FLASH_SPARE_PAGES       0   /* 0 if the flash layout reserves no spare pages */
#endif

/* ------------------------------------------------------------------ profile selection */

#if ((defined(FWU_PROFILE_DEV) ? 1 : 0) + (defined(FWU_PROFILE_INDUSTRIAL) ? 1 : 0) + \
     (defined(FWU_PROFILE_AUTOMOTIVE) ? 1 : 0)) != 1
#error "FWU: exactly one profile must be selected on the compiler command line: -DFWU_PROFILE_DEV, -DFWU_PROFILE_INDUSTRIAL or -DFWU_PROFILE_AUTOMOTIVE"
#endif

#if defined(FWU_PROFILE_DEV)

#  if defined(FWU_PRODUCTION)
#    error "FWU: the DEV profile cannot be used in a production build (FWU_PRODUCTION is defined)"
#  endif
#  define FWU_CFG_VERIFY            FWU_VERIFY_CRC32
#  define FWU_CFG_ANTI_ROLLBACK     0
#  if defined(FWU_ACTIVATION_SWAP_ROLLBACK)
#    define FWU_CFG_ACTIVATION      FWU_ACTIVATE_SWAP_ROLLBACK
#  else
#    define FWU_CFG_ACTIVATION      FWU_ACTIVATE_COPY
#  endif

#elif defined(FWU_PROFILE_INDUSTRIAL)

#  define FWU_CFG_VERIFY            FWU_VERIFY_SIGNED
#  define FWU_CFG_ANTI_ROLLBACK     0
#  if defined(FWU_ACTIVATION_SWAP_ROLLBACK)
#    define FWU_CFG_ACTIVATION      FWU_ACTIVATE_SWAP_ROLLBACK
#  else
#    define FWU_CFG_ACTIVATION      FWU_ACTIVATE_COPY
#  endif

#elif defined(FWU_PROFILE_AUTOMOTIVE)

#  define FWU_CFG_VERIFY            FWU_VERIFY_SIGNED
#  define FWU_CFG_ANTI_ROLLBACK     1
#  define FWU_CFG_ACTIVATION        FWU_ACTIVATE_SWAP_ROLLBACK     /* mandatory in this profile */

#endif

/* ------------------------------------------------------------------ fixed in every profile */

#define FWU_CFG_POWER_SAFE          1   /* power-loss-safe update: NOT an option */

/* ------------------------------------------------------------------ forbidden combinations */

#define FWU_CFG_SPARE_PAGES_JOURNAL 2   /* journal A + B: in every profile */
#define FWU_CFG_SPARE_PAGES_SWAP    3   /* journal A + B + swap scratch page */

#if FWU_FLASH_SPARE_PAGES < FWU_CFG_SPARE_PAGES_JOURNAL
#error "FWU: a power-loss-safe journal (A/B) needs at least 2 spare pages (-DFWU_FLASH_SPARE_PAGES=n); this flash layout does not provide them"
#endif

#if (FWU_CFG_ACTIVATION == FWU_ACTIVATE_SWAP_ROLLBACK) && (FWU_FLASH_SPARE_PAGES < FWU_CFG_SPARE_PAGES_SWAP)
#error "FWU: swap with rollback needs at least 3 spare pages (2 journal + 1 swap scratch); this flash layout does not provide them"
#endif

#endif /* FWU_CONFIG_H_ */
