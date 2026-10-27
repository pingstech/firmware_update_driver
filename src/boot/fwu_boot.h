/**
 * @file    fwu_boot.h
 * @brief   BOOTLOADER CORE (platform independent): wires counters, verification and activation together in the CORRECT ORDER at boot
 *          and makes the "jump to application / stay in bootloader" decision. Hardware tasks (watchdog timer, jump, reboot,
 *          vector table check) are HOOKS; the core does not know them, so it can be tested entirely on a PC with fake hooks and a fake
 *          flash. The real bootloader project (startup code, jump, linker script) is NOT in this repository.
 *
 * USAGE (bootloader, at every boot, BEFORE jumping to the application)
 *   static fwu_boot_t boot;                       // the structure must stay FIXED: the pointers inside point to its own fields
 *   fwu_boot_result_t result;
 *   if(fwu_boot_start(&boot, &config, &result) == FWU_BOOT_JUMP_APP) { jump_to_app(); }
 *   else { stay_in_bootloader(result.reason); }   // e.g. LED, recovery mode (the core only says "stay")
 *
 * USAGE (application): fwu_boot_init() with the same configuration; the session config gets fwu_boot_activate() and
 *   fwu_boot_verify_config(); when a new version boots it calls fwu_boot_app_finish_trial() after testing itself.
 *
 * ORDER (fwu_boot_start)
 *   1. Configuration and POLICY are validated (rules below).                              error -> STAY (CONFIG)
 *   2. If anti_rollback is set, the counters are read from flash and the minimum versions are copied into the verification
 *      configuration; if unreadable, STAY (COUNTERS): an untrusted zero would make every version acceptable, so it never boots silently.
 *   3. The verification configuration is built from the POLICY (signature requirement, keys, product); activation is set up
 *      (verification hook, and if anti_rollback the confirm hook = counter raise).           error -> STAY (ACTIVATE_INIT)
 *   4. fwu_activate_run: completes the pending work (verify, copy/swap, trial, rollback).    error -> STAY (ACTIVATE_RUN)
 *   5. The app_valid hook (if present) must say the application can be jumped to.            no -> STAY (NO_APP)
 *   6. If a trial boot is in progress, the watchdog_arm hook is called (so that the watchdog resets if the new application hangs,
 *      and the next boot rolls back).  -> JUMP
 *   GOLDEN RULE: if ANY of 1-5 fails the decision is STAY; it NEVER jumps to the application (the application may be partial).
 *
 * POLICY (compile-time decision, cannot be selected over the link; fwu_boot_profile.h derives it from fwu_config.h)
 *   verify:        NONE (development only) | HASH (product/version/size/SHA-256, unsigned) | SIGNED (HASH + signature)
 *   mode:          COPY | SWAP (swap + trial boot + rollback)
 *   anti_rollback: counters are used.
 *   Rules (a violation gives INVALID_ARG; "a bootloader without protection must not be buildable by accident"):
 *     - if verify != NONE, crypto and verify_work must be given; if SIGNED, at least 1 key.
 *     - if anti_rollback, mode must be SWAP and verify must be SIGNED. In COPY there is no confirmation phase, the counter would never rise
 *       (open issue 45); without a signature the counter could be raised through an area the attacker can write.
 *
 * CONTRACT FINGERPRINT (open issue 19): the bootloader and the application must be built with the SAME policy, flash layout and product identity.
 *   fwu_boot_fingerprint produces a 32-bit fingerprint from these values; fwu_boot_info_encode puts it into a 12-byte record
 *   (the bootloader places this as a `const` at a fixed flash address: the linker script is the integrator's job).
 *   At boot the application compares the fingerprint of its own configuration with fwu_boot_info_check; a mismatch gives
 *   FWU_ERR_CONTRACT_MISMATCH. The CONTENT of the keys does not enter the fingerprint (the authoritative copy is in the bootloader).
 */

#ifndef FWU_BOOT_H_
#define FWU_BOOT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fwu_error.h"
#include "fwu_types.h"
#include "fwu_flash.h"
#include "fwu_crypto.h"
#include "fwu_verify.h"
#include "fwu_counters.h"
#include "fwu_activate.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FWU_BOOT_CONTRACT_VERSION   1u      /**< Version of the fingerprint format (increases if the fields change). */
#define FWU_BOOT_INFO_SIZE          12u     /**< fwu_boot_info record: magic(4) version(2) fingerprint(4) CRC-16(2), big endian. */
#define FWU_BOOT_INFO_VERSION       1u

/* ------------------------------------------------------------------ policy and layout */

typedef enum {
    FWU_BOOT_VERIFY_NONE = 0,       /**< No verification (development only). */
    FWU_BOOT_VERIFY_HASH = 1,       /**< Product, version, size, SHA-256; NO signature (no authority). */
    FWU_BOOT_VERIFY_SIGNED = 2      /**< HASH + key + signature. */
} fwu_boot_verify_e;

typedef struct {
    fwu_boot_verify_e   verify;
    fwu_activate_mode_e mode;
    bool                anti_rollback;
} fwu_boot_policy_t;

/** Flash layout (from the fwu_layout.h macros, see fwu_boot_profile.h). All are offsets inside flash (fwu_byte_t). */
typedef struct {
    uint32_t app_offset;                /**< Application region. */
    uint32_t staging_manifest_offset;   /**< First page of staging: manifest. */
    uint32_t staging_image_offset;      /**< After the manifest page: image (the staging offset of the activation). */
    uint32_t image_region_length;       /**< Image capacity (= activation region length = verification max_image_size). */
    uint32_t journal_a_offset;
    uint32_t journal_b_offset;
    uint32_t journal_length;            /**< Length of each activation journal region. */
    uint32_t scratch_offset;            /**< SWAP scratch pages. */
    uint32_t scratch_pages;
    uint32_t counter_a_offset;          /**< Version counters (for anti_rollback). */
    uint32_t counter_b_offset;
    uint32_t counter_length;
} fwu_boot_layout_t;

/* ------------------------------------------------------------------ hooks */

/** Can the application be jumped to? (e.g. valid stack/reset address in the vector table). true = can jump. NULL = no check. */
typedef bool (*fwu_boot_app_valid_fn)(void *context);

/** Called when a trial boot starts, immediately BEFORE the jump: starts the independent watchdog timer. NULL = none. */
typedef void (*fwu_boot_watchdog_fn)(void *context);

typedef struct {
    fwu_boot_policy_t        policy;
    fwu_boot_layout_t        layout;
    const fwu_flash_t       *flash;                 /**< Flash that has passed fwu_flash_validate. */
    fwu_crypto_t            *crypto;                /**< Mandatory if verify != NONE. */
    const fwu_verify_key_t  *keys;                  /**< Mandatory if SIGNED (a `const` array in the bootloader). */
    uint32_t                 key_count;
    uint32_t                 product_id;
    uint32_t                 max_trial_boots;       /**< SWAP: 1..255 (recommended >= 2, open issue 27). */
    fwu_byte_t              *verify_work;           /**< Flash read buffer of the verification (mandatory if verify != NONE). */
    uint32_t                 verify_work_length;
    fwu_byte_t              *activate_work;         /**< Copy buffer of the activation (at least write_granule). Must be SEPARATE from verify_work. */
    uint32_t                 activate_work_length;
    fwu_boot_app_valid_fn    app_valid;             /**< Optional. */
    fwu_boot_watchdog_fn     watchdog_arm;          /**< Optional. */
    void                    *context;               /**< Passed to the two hooks. */
} fwu_boot_config_t;

/* ------------------------------------------------------------------ state and result */

/** State. Must stay FIXED (must not be moved/copied: the structures inside point to each other). Do not touch the fields. */
typedef struct {
    fwu_boot_config_t        config;        /**< Copy of the configuration. */
    bool                     initialized;
    bool                     confirm_retry; /**< The confirm hook failed: in the same boot fwu_boot_app_finish_trial retries the hook (RAM only). */
    fwu_counters_t           counters;
    fwu_counters_confirm_t   confirm;       /**< Context of the confirm hook. */
    fwu_verify_config_t      verify;        /**< Verification configuration (minimum versions from the counter). */
    fwu_activate_config_t    activate_config;
    fwu_activate_t           activate;
} fwu_boot_t;

typedef enum {
    FWU_BOOT_JUMP_APP = 0,          /**< Application consistent: jump. */
    FWU_BOOT_STAY = 1               /**< Do not jump: stay in the bootloader (reason: result.reason). */
} fwu_boot_decision_e;

typedef enum {
    FWU_BOOT_REASON_NONE = 0,
    FWU_BOOT_REASON_CONFIG,         /**< Configuration/policy invalid (like a build error: integration must fix it). */
    FWU_BOOT_REASON_COUNTERS,       /**< anti_rollback is set but the counters could not be read. */
    FWU_BOOT_REASON_ACTIVATE_INIT,  /**< Activation could not be set up (e.g. the last journal record could not be interpreted). */
    FWU_BOOT_REASON_ACTIVATE_RUN,   /**< Activation returned an error (flash error, read-back, "verification could not be performed"). */
    FWU_BOOT_REASON_NO_APP          /**< The app_valid hook found the application not jumpable. */
} fwu_boot_reason_e;

typedef struct {
    fwu_boot_decision_e      decision;
    fwu_boot_reason_e        reason;    /**< Reason if STAY, NONE if JUMP. */
    fwu_return_e             status;    /**< Raw error code of the failed step (FWU_OK on success). */
    fwu_activate_outcome_e   outcome;   /**< Result of fwu_activate_run (NONE if run did not execute/failed). */
    fwu_activate_phase_e     phase;     /**< Phase after run (IDLE if run did not execute). */
    bool                     trial;     /**< A trial boot is in progress (the new application awaits confirmation). */
} fwu_boot_result_t;

/* ------------------------------------------------------------------ API */

/**
 * @brief  Validates the configuration and wires up counters, verification and activation (does NOT RUN the activation). The application uses this;
 *         the bootloader calls fwu_boot_start. Does NOT write to flash.
 * @return FWU_OK; FWU_ERR_INVALID_ARG for NULL, policy rule violation, missing buffer/key/crypto; counter and
 *         activation setup errors as they are (the object does NOT count as set up: initialized false).
 */
fwu_return_e fwu_boot_init(fwu_boot_t *boot, const fwu_boot_config_t *config);

/**
 * @brief  BOOTLOADER: fwu_boot_init + fwu_activate_run + decision (ORDER above). On failure NEVER returns JUMP_APP.
 * @param  result  Optional (may be NULL): reason, raw status code, outcome, phase, trial flag.
 */
fwu_boot_decision_e fwu_boot_start(fwu_boot_t *boot, const fwu_boot_config_t *config, fwu_boot_result_t *result);

/**
 * @brief  APPLICATION: if in a trial boot (phase TRIAL) it confirms when healthy, otherwise requests a rollback. If there is no trial
 *         (phase IDLE) it does nothing (idempotent: can be called at every boot).
 * @param  healthy  true: fwu_activate_confirm (if anti_rollback the counter rises); false: fwu_activate_request_revert
 *                  (the application MUST REBOOT afterwards).
 * @return FWU_OK; FWU_ERR_INVALID_ARG if the object is not set up; FWU_ERR_BAD_STATE if the phase is ACTIVATING/REVERTING (the application
 *         should not have run before the bootloader finished); confirm/revert errors as they are.
 *         CONFIRM HOOK ERROR (e.g. crypto "could not be performed"): the CONFIRMED record has already been written; the error is returned, and if
 *         called again with healthy=true in this boot (remembered in RAM only) the hook runs again. If power is lost/the device
 *         restarts the counter stays behind (the safe direction, open issue 28); there is NO persistent retry: another image that later lands
 *         in staging could raise the counter with its own version.
 */
fwu_return_e fwu_boot_app_finish_trial(fwu_boot_t *boot, bool healthy);

/** The activation object that the session (fwu_session_config_t.activate) and the commit call will use. NULL if not set up. */
fwu_activate_t *fwu_boot_activate(fwu_boot_t *boot);

/** The verification configuration that the session (fwu_session_config_t.verify) will use (minimum versions from the counter). NULL if not set up. */
const fwu_verify_config_t *fwu_boot_verify_config(const fwu_boot_t *boot);

/**
 * @brief  Contract fingerprint: FNV-1a 32 bit over the policy, layout, product id, trial count and flash geometry (page size, write
 *         granule, flash length). Key content, buffers and hooks do not enter.
 * @return FWU_OK; FWU_ERR_INVALID_ARG for NULL (config, flash, fingerprint).
 */
fwu_return_e fwu_boot_fingerprint(const fwu_boot_config_t *config, uint32_t *fingerprint);

/**
 * @brief  Produces the 12-byte contract record that the bootloader will put into flash:
 *         "FWBI" | version (2, big endian) | fingerprint (4) | CRC-16/MODBUS (2, over the previous 10 bytes).
 * @return FWU_OK; INVALID_ARG for NULL; BUFFER_TOO_SMALL for out_capacity < 12.
 */
fwu_return_e fwu_boot_info_encode(const fwu_boot_config_t *config, fwu_byte_t *out, size_t out_capacity, size_t *out_length);

/**
 * @brief  APPLICATION: compares the bootloader's record with its own configuration.
 * @return FWU_OK (matches); INVALID_ARG for NULL; NOT_ENOUGH_DATA for length < 12; FWU_ERR_CONTRACT_MISMATCH if magic/version/CRC are bad
 *         or the fingerprint differs (the application MUST REFUSE to run).
 */
fwu_return_e fwu_boot_info_check(const fwu_boot_config_t *config, const fwu_byte_t *info, size_t length);

#ifdef __cplusplus
}
#endif

#endif /* FWU_BOOT_H_ */
