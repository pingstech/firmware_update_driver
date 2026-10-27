/**
 * @file    fwu_counters.h
 * @brief   VERSION COUNTERS (anti-rollback and key revocation): stores the LOWEST image version and the
 *          LOWEST key version that the device accepts in a power-loss-resilient way. The verification pipeline (fwu_verify.h) expects these two
 *          values in the fields `fwu_verify_config_t.min_image_version` / `.min_key_version`; this module is
 *          the PERSISTENT source of those values.
 *
 * WHY: so that an attacker cannot re-install an old, vulnerable but VALIDLY signed version (the signature
 * is also valid on old versions; only the version counter blocks it). The same for the key version: if a leaked key is to be revoked,
 * after a version signed with the new key has been confirmed, images with the old key are rejected.
 *
 * RULES (violating them would either lock the device or open a hole in the protection)
 *   1. The counter ONLY INCREASES (fwu_counters_raise: new = max(old, requested)). There is no way to lower it.
 *   2. The counter increases ONLY AFTER CONFIRMATION: when the new application confirms itself (fwu_activate_confirm ->
 *      on_confirmed hook). If it increased earlier, when a trial boot fails and the device returns to the old version, the old
 *      image would be rejected because of its own counter (device bricked).
 *   3. The counter increases ONLY from a SIGNED manifest: the hook re-verifies the manifest in staging with fwu_verify_manifest
 *      (structure, product, key, signature). Otherwise someone who can write to staging could set the version in the manifest to 0xFFFFFFFF at
 *      confirmation time and permanently block all later updates.
 *   4. "An equal version is accepted" (>=): the confirmed version is exactly the new value of the counter; the same version can be
 *      installed again.
 *
 * HOW IT IS STORED: fwu_journal_ab (A/B alternating journal) in two separate pages, SEPARATE from the activation journal. One record
 * (16 elements) carries ALL the state; if power is lost while a record is being written its CRC does not match, the old record stays valid (safe direction).
 *
 *     journal field   content
 *     ------------    -------------------------------------------------------------------
 *     arg  (32 bit)   lowest accepted IMAGE version
 *     state (8 bit)   high byte of the lowest accepted KEY version
 *     flags (8 bit)   low byte of the lowest accepted KEY version
 *
 *   Counter values come from the current record itself (the last valid record wins). When a region is full the A/B switch copies the last record
 *   to the new region: the values are not lost.
 *
 * POWER LOSS: if a raise is interrupted the counter is either the old or the new value (the two fields are in a SINGLE record, inseparable).
 * If it stays at the old value the confirm hook runs again at the next confirm call and completes it (raise
 * is idempotent; if nothing changes it does NOT write to flash).
 *
 * RAM STATE: if a write returns an error the module tries to re-read flash; if it cannot read, the object becomes "not ready"
 * (get/raise return FWU_ERR_BAD_STATE) and fwu_counters_reload is needed. A not-ready counter NEVER behaves like 0:
 * 0 means "accept every version", so the untrusted state gives an explicit error.
 *
 * LIMITS (deliberate, documented)
 *   - If both regions become unreadable (wear/bit error; not power loss) the counter reads as "never written",
 *     i.e. 0, and the protection is lost. The same limit as in the activation journal (open issue 32).
 *   - On a new device (no record at all) the values are 0: every version is accepted. It rises with the first confirmed version at the factory.
 *   - The signing tool must not produce image_version = 0xFFFFFFFF: if confirmed, the counter stays at the ceiling and no later image is
 *     accepted (permanent lock). This is a RELEASE PROCESS rule, it is not prevented on the device side.
 *   - Preventing the application from writing to the counter pages requires hardware write protection (STM32 WRP option bytes);
 *     this is bootloader/production work, not verified on chip [UNVERIFIED]. On unprotected pages someone who can
 *     run application code can lower the counter.
 *
 * USAGE
 *   - The bootloader and the application call fwu_counters_init at boot (FWU_LAYOUT_COUNTER_A/B_OFFSET, FWU_LAYOUT_COUNTER_LENGTH),
 *     then get the two values with fwu_counters_get and COPY them into fwu_verify_config_t.
 *   - In the activation configuration on_confirmed = fwu_counters_on_confirmed, on_confirmed_context = &fwu_counters_confirm_t.
 *   - Profile: in profiles with FWU_CFG_ANTI_ROLLBACK = 1 the integration uses this module (wired in task G11).
 *
 * Before first use the two counter pages must have been erased (the factory/bootloader provides this; fwu_journal_ab.h).
 */

#ifndef FWU_COUNTERS_H_
#define FWU_COUNTERS_H_

#include <stdbool.h>
#include <stdint.h>
#include "fwu_error.h"
#include "fwu_types.h"
#include "fwu_flash.h"
#include "fwu_journal_ab.h"
#include "fwu_verify.h"

#ifdef __cplusplus
extern "C" {
#endif

/** State of the version counters. Do not touch the fields directly. */
typedef struct {
    fwu_journal_ab_t journal;            /**< A/B journal in which the counters are stored. */
    uint32_t         min_image_version;  /**< Lowest accepted image version (valid only while ready). */
    uint16_t         min_key_version;    /**< Lowest accepted key version (valid only while ready). */
    bool             ready;              /**< Were the values read from flash and are they trusted? */
} fwu_counters_t;

/** Context of the confirm hook (passed as fwu_activate_config_t.on_confirmed_context). */
typedef struct {
    fwu_counters_t            *counters;  /**< Counters to be raised. */
    const fwu_verify_config_t *verify;    /**< For manifest verification: crypto, manifest_offset, product_id, keys, ... */
    const fwu_flash_t         *flash;     /**< Flash from which the staging manifest is read. */
} fwu_counters_confirm_t;

/**
 * @brief  Binds the counter regions and reads the values from flash.
 * @param  counters  Object to initialize (must not be NULL). Stays "not ready" in case of error.
 * @param  flash     Flash object (must stay valid as long as the journal is used).
 * @param  offset_a  Offset of counter region A (FWU_LAYOUT_COUNTER_A_OFFSET).
 * @param  offset_b  Offset of counter region B (FWU_LAYOUT_COUNTER_B_OFFSET).
 * @param  length    Length of each region (FWU_LAYOUT_COUNTER_LENGTH; at least 2 records).
 * @return FWU_OK; FWU_ERR_INVALID_ARG for NULL or an invalid region; FWU_ERR_OUT_OF_RANGE if outside flash; invalid
 *         geometry FWU_ERR_FLASH_GEOMETRY; flash read errors as they are (like fwu_journal_ab_init).
 *         If there is no record at all, FWU_OK and the values are (0, 0).
 */
fwu_return_e fwu_counters_init(fwu_counters_t *counters, const fwu_flash_t *flash,
                               uint32_t offset_a, uint32_t offset_b, uint32_t length);

/**
 * @brief  Re-reads the counters from flash (after a write error or a power loss).
 * @return FWU_OK (the object becomes ready); FWU_ERR_INVALID_ARG for NULL; flash read errors as they are (the object
 *         stays not ready).
 */
fwu_return_e fwu_counters_reload(fwu_counters_t *counters);

/**
 * @brief  Returns the current lowest versions (to copy into the verification configuration).
 * @param  counters       Counter object.
 * @param  image_version  The lowest accepted image version is written here (must not be NULL).
 * @param  key_version    The lowest accepted key version is written here (must not be NULL).
 * @return FWU_OK; FWU_ERR_INVALID_ARG for NULL; FWU_ERR_BAD_STATE if the object is not ready (outputs are NOT CHANGED:
 *         an untrusted zero would make every version acceptable).
 */
fwu_return_e fwu_counters_get(const fwu_counters_t *counters, uint32_t *image_version, uint16_t *key_version);

/**
 * @brief  Raises the counters: new = max(current, requested), separately for the two fields. If both values are already sufficient it does NOT
 *         write to flash (FWU_OK). Otherwise it writes a SINGLE record (both values together, atomic).
 * @param  counters       Counter object.
 * @param  image_version  Confirmed image version.
 * @param  key_version    Confirmed key version.
 * @return FWU_OK; FWU_ERR_INVALID_ARG for NULL; FWU_ERR_BAD_STATE if the object is not ready; flash errors (including power
 *         loss) as they are. On error the module re-reads flash: if it can read, the values are the real values in flash, if it cannot,
 *         the object becomes not ready (fwu_counters_reload is needed).
 */
fwu_return_e fwu_counters_raise(fwu_counters_t *counters, uint32_t image_version, uint16_t key_version);

/**
 * @brief  The `on_confirmed` hook of the activation (fwu_activate_confirmed_fn): verifies the manifest in staging
 *         with fwu_verify_manifest (structure, product, key, signature) and raises the counters to the image and key
 *         version of that manifest. If verification fails the counter is NOT CHANGED and an error is returned (confirm returns an error; if called
 *         again the hook runs again).
 * @param  context  `fwu_counters_confirm_t *`.
 * @return FWU_OK; FWU_ERR_INVALID_ARG for a NULL field; errors of fwu_verify_manifest and fwu_counters_raise as they are.
 */
fwu_return_e fwu_counters_on_confirmed(void *context);

#ifdef __cplusplus
}
#endif

#endif /* FWU_COUNTERS_H_ */
