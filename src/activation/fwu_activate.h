/**
 * @file    fwu_activate.h
 * @brief   Activation: applies the VERIFIED new image from the staging area to the application region in a
 *          power-loss-resilient way. Two strategies, with the same journal and the same recovery logic:
 *            COPY  : staging -> application copy, one-way (NO return to the old version).
 *            SWAP  : application <-> staging exchange + TRIAL BOOT; if the new version does not confirm
 *                    itself, the device returns to the old version (rollback).
 *
 * THREE BASIC RULES (README "power-loss resilience"):
 *   1. The COMMIT record is written LAST: before the commit the application is not touched, the old version stays runnable.
 *   2. Records verify themselves (journal: magic + CRC); a half-written record is ignored.
 *   3. Every step is REPEATABLE (idempotent): if interrupted in the middle of a step, the same step is redone from the start.
 *      Staging is not erased as long as it is the source of a step.
 *
 * WHO CALLS WHAT
 *   - Application (after the download is finished and staging is verified): fwu_activate_commit() -> reboot.
 *     fwu_activate_staging_writable() must be true BEFORE downloading a new image (otherwise the source of a
 *     pending activation would be erased).
 *   - BOOTLOADER (at every boot, BEFORE jumping to the application): fwu_activate_init() + fwu_activate_run().
 *     If run returns FWU_OK the application region is consistent (old or new image, never partial); if run returns
 *     an error DO NOT jump to the application (it may be partial), stay in the bootloader; at the next boot
 *     run continues where it stopped.
 *   - New application (SWAP, in the trial boot, after self-testing): fwu_activate_confirm()
 *     or, if it finds itself inadequate, fwu_activate_request_revert() -> reboot.
 *   - Before jumping, the bootloader must start an independent watchdog timer: if the application in the trial
 *     boot hangs, the reset triggers the return to the old version.
 *
 * JOURNAL RECORDS (fwu_journal_ab; `arg` = (page_count << 16) | counter; `flags` bit 0 = SWAP mode)
 *
 *   state      counter meaning                           next
 *   COMMIT     0                                         PROGRESS ... / REJECTED
 *   PROGRESS   COPY: pages copied; SWAP: steps completed PROGRESS ... / DONE (COPY) / TRIAL (SWAP)
 *   DONE       -                                         (final: COPY complete)
 *   REJECTED   -                                         (final: staging failed verification)
 *   TRIAL      number of trial boots started             TRIAL(+1) / CONFIRMED / REVERT
 *   CONFIRMED  -                                         (final: new version confirmed)
 *   REVERT     rollback steps completed                  REVERT ... / REVERTED
 *   REVERTED   -                                         (final: returned to the old version)
 *
 *   Every record is a sufficient state by itself (page count and counter are inside it): when the journal switches
 *   regions it carries a copy of the last record, no other history is needed.
 *
 * COPY (page count per update N): for page i: erase the application page, write the staging page,
 *   read back and compare, then PROGRESS(i+1). After N pages, DONE. If interrupted: the unfinished page is
 *   redone from the start (staging is untouched, so the source is intact).
 *
 * SCRATCH PAGE WEAR (SWAP): unlike the application and staging pages, which are erased once per page, the
 *   scratch page is erased ONCE FOR EVERY page: a single scratch page is erased N times per update (on STM32
 *   N = 110; with a flash endurance of ~10 thousand erases [UNVERIFIED] it is exhausted after ~90 updates, and rollback doubles this).
 *   Solution: the scratch area is `scratch_pages` (K) consecutive pages; the steps of page number n use scratch
 *   page (n mod K). It is deterministic for recovery (the same page always uses the same scratch page). K = 1
 *   is the single-page behavior above; as K grows, the number of erases per page becomes ~N/K.
 *
 * SWAP (3 steps per page, scratch page S; A = application page, B = staging page):
 *     step 0:  erase S, A -> S          (A is still intact)
 *     step 1:  erase A, B -> A          (S holds the old A)
 *     step 2:  erase B, S -> B          (A is now new; B receives the old A)
 *   PROGRESS after every step. If interrupted, that step is redone from the start: in step 0 the source A, in step 1
 *   the source B, in step 2 the source S is not damaged until the step completes. After 3N steps application = new image,
 *   staging = old image. Then TRIAL(1) is written and the jump to the application is made. Rollback is the SAME exchange
 *   (with REVERT records): application = old, staging = the (unconfirmed) new image.
 *   Only the N pages of the new image are exchanged; if the old image is longer, its tail stays in the application
 *   region as it was, on rollback N pages come back and the old image is complete again.
 *
 * TRIAL BOOT (SWAP): the TRIAL(k) record means "trial number k has started". If the new application calls
 *   fwu_activate_confirm() at boot, CONFIRMED. If it restarts without calling it, the bootloader sees TRIAL(k);
 *   if k < max_trial_boots it writes TRIAL(k+1) and tries again, otherwise it returns to the old version with REVERT.
 *   max_trial_boots = 1: if the new version does not confirm on its first boot (hang, watchdog reset,
 *   power loss), the device returns to the old version at the next boot.
 *
 * ANTI-ROLLBACK VERSION COUNTER: must increase ONLY after confirmation (if it increased earlier, rollback would find the old image
 *   "below the counter" and reject it, bricking the device). fwu_activate_confirm first writes the CONFIRMED record,
 *   THEN calls the on_confirmed hook: if power is lost between the two, the counter stays behind
 *   (the safe direction). If the hook returns an error, confirm returns that error; confirm can be called again (in the
 *   CONFIRMED state the hook is called again), so the hook must be repeatable, like "raise the counter to at least X".
 *
 * VERIFICATION HOOK (verify, optional): called after COMMIT, BEFORE the first touch of the application
 *   (the bootloader's independent, authoritative verification). FWU_OK = valid. Integrity errors of the 3x group
 *   (e.g. FWU_ERR_CRC_MISMATCH, FWU_ERR_BAD_MANIFEST, FWU_ERR_HASH_MISMATCH, FWU_ERR_IMAGE_TOO_LARGE) and
 *   FWU_ERR_BAD_SIGNATURE / PRODUCT_MISMATCH / VERSION_REJECTED / KEY_REJECTED = unacceptable image:
 *   REJECTED is written, the application does not change, the old version keeps running. Any other error code = verification could not be performed
 *   (e.g. flash read error, FWU_ERR_CRYPTO_FAILURE): REJECTED is not written, run returns that error,
 *   and it is retried at the next boot. (If a new "invalid image" code is added, it must also be added to act_is_rejection
 *   in fwu_activate.c.)
 *
 * EVERY WRITE IS READ BACK AND COMPARED: a silent write error (bit rot, ECC) is caught immediately as FWU_ERR_FLASH_WRITE;
 *   the record is not written, the step is repeated at the next boot.
 *
 * KNOWN LIMITS
 *   - If BOTH journal regions become unreadable during a half-finished copy/swap (double
 *     corruption) the state reads as "idle"; the A/B journal design rules this out for power loss, but
 *     for flash wear / random bit errors, ECC or additional measures are outside the driver.
 *   - Record/limit values: page count <= 21845 (3 * pages <= 65535, counter is 16 bit),
 *     max_trial_boots <= 255.
 *   - After an error (except INVALID_ARG and BAD_STATE) the object is untrusted: it must be
 *     re-initialized with fwu_activate_init (on a real device a power loss is a restart anyway).
 */

#ifndef FWU_ACTIVATE_H_
#define FWU_ACTIVATE_H_

#include <stdbool.h>
#include <stdint.h>
#include "fwu_error.h"
#include "fwu_types.h"
#include "fwu_flash.h"
#include "fwu_journal_ab.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ constants */

/* Journal state codes (persistent: the numbers never change / are never reused). */
#define FWU_ACT_STATE_COMMIT        1u
#define FWU_ACT_STATE_PROGRESS      2u
#define FWU_ACT_STATE_DONE          3u
#define FWU_ACT_STATE_REJECTED      4u
#define FWU_ACT_STATE_TRIAL         5u
#define FWU_ACT_STATE_CONFIRMED     6u
#define FWU_ACT_STATE_REVERT        7u
#define FWU_ACT_STATE_REVERTED      8u

#define FWU_ACT_FLAG_SWAP           0x01u      /* record was written in SWAP mode */

#define FWU_ACT_STEPS_PER_PAGE      3u         /* SWAP: steps per page */
#define FWU_ACT_MAX_PAGES           21845u     /* 3 * 21845 = 65535: counter is 16 bit */
#define FWU_ACT_MAX_TRIAL_BOOTS     255u

/** Strategy. The values are THE SAME as FWU_ACTIVATE_COPY (0) / FWU_ACTIVATE_SWAP_ROLLBACK (1) in fwu_config.h:
 *  product code passes (fwu_activate_mode_e)FWU_CFG_ACTIVATION. The mode is a compile-time constant,
 *  it cannot be selected over the communication link (downgrade attack). */
typedef enum {
    FWU_ACTIVATE_MODE_COPY = 0,
    FWU_ACTIVATE_MODE_SWAP = 1
} fwu_activate_mode_e;

/** Phase derived from the last journal record. */
typedef enum {
    FWU_ACT_PHASE_IDLE = 0,         /**< Nothing pending (a new commit can be issued). */
    FWU_ACT_PHASE_ACTIVATING,       /**< COMMIT issued, copy/swap not finished. */
    FWU_ACT_PHASE_TRIAL,            /**< SWAP: new image is in the trial boot, awaiting confirmation. */
    FWU_ACT_PHASE_REVERTING         /**< SWAP: rollback started, not finished. */
} fwu_activate_phase_e;

/** What fwu_activate_run did in this call (together with FWU_OK). */
typedef enum {
    FWU_ACT_OUTCOME_NONE = 0,       /**< Nothing was pending; application left as is. */
    FWU_ACT_OUTCOME_ACTIVATED,      /**< COPY completed: application = new image. */
    FWU_ACT_OUTCOME_TRIAL,          /**< SWAP: application = new image, trial boot; confirm awaited. */
    FWU_ACT_OUTCOME_REVERTED,       /**< SWAP: returned to the old image. */
    FWU_ACT_OUTCOME_REJECTED        /**< Staging failed verification: application = old image (unchanged). */
} fwu_activate_outcome_e;

/** Staging verification hook (see the "VERIFICATION HOOK" section in the header). `length` = N * page size. */
typedef fwu_return_e (*fwu_activate_verify_fn)(void *context, const fwu_flash_t *flash,
                                               uint32_t offset, uint32_t length);

/** Post-confirmation hook (e.g. raises the anti-rollback version counter). Must be repeatable. */
typedef fwu_return_e (*fwu_activate_confirmed_fn)(void *context);

/** Configuration. The caller fills it in; init keeps a COPY (the work buffer stays with the caller). */
typedef struct {
    const fwu_flash_t        *flash;             /**< Flash (must have passed fwu_flash_validate). */
    fwu_activate_mode_e       mode;              /**< COPY or SWAP. */
    uint32_t                  app_offset;        /**< Application region offset (fwu_byte_t, page aligned). */
    uint32_t                  staging_offset;    /**< Staging region offset (page aligned). */
    uint32_t                  region_length;     /**< Application = staging region length (multiple of the page). */
    uint32_t                  journal_a_offset;  /**< Journal A region (page aligned). */
    uint32_t                  journal_b_offset;  /**< Journal B region (page aligned). */
    uint32_t                  journal_length;    /**< Length of each journal region (usually one page). */
    uint32_t                  scratch_offset;    /**< SWAP: offset of the scratch area (page aligned; ignored in COPY). */
    uint32_t                  scratch_pages;     /**< SWAP: number of consecutive pages K >= 1 in the scratch area (for wear; ignored in COPY). */
    fwu_byte_t               *work;              /**< Copy buffer (at least write_granule elements). */
    uint32_t                  work_length;       /**< Number of work elements. */
    uint32_t                  max_trial_boots;   /**< SWAP: 1..255 trial boots (ignored in COPY). */
    bool                      require_verify;    /**< true: `verify` MUST NOT be NULL (init returns FWU_ERR_INVALID_ARG). Integration passes true in verifying profiles (FWU_CFG_VERIFY != NONE), so that a bootloader without verification cannot be built by accident. */
    fwu_activate_verify_fn    verify;            /**< Optional if require_verify is false (NULL = no verification). */
    void                     *verify_context;
    fwu_activate_confirmed_fn on_confirmed;      /**< Optional (NULL = none). */
    void                     *on_confirmed_context;
} fwu_activate_config_t;

/** Activation state. Only READ the fields. */
typedef struct {
    fwu_activate_config_t config;       /**< Copy of the configuration. */
    fwu_journal_ab_t      journal;      /**< A/B journal. */
    fwu_activate_phase_e  phase;        /**< Phase derived from the last record. */
    fwu_byte_t            last_state;   /**< State code of the last record (0: no record). */
    uint32_t              page_count;   /**< Page count in the last record. */
    uint32_t              counter;      /**< Counter in the last record. */
    uint32_t              region_pages; /**< Number of pages in the region. */
    uint32_t              scratch_length; /**< Scratch area length (SWAP: K * page; COPY: 0). */
    uint32_t              chunk;        /**< Copy chunk size (multiple of the granule, <= page). */
} fwu_activate_t;

/* ------------------------------------------------------------------ API */

/**
 * @brief  Validates the configuration, reads the journal and determines the phase. Does NOT write to flash.
 * @return FWU_OK; FWU_ERR_INVALID_ARG for NULL, misalignment, overlapping regions, too small work buffer, invalid mode,
 *         max_trial_boots or scratch_pages (0); FWU_ERR_OUT_OF_RANGE if a region is outside flash;
 *         FWU_ERR_FLASH_GEOMETRY for invalid flash/journal geometry; FWU_ERR_BAD_STATE if the last journal record
 *         cannot be interpreted (unknown state, record of another mode, page count that does not fit the region);
 *         flash read errors are returned as they are.
 */
fwu_return_e fwu_activate_init(fwu_activate_t *activate, const fwu_activate_config_t *config);

/** Returns the phase. @return FWU_OK; FWU_ERR_INVALID_ARG for NULL. */
fwu_return_e fwu_activate_get_phase(const fwu_activate_t *activate, fwu_activate_phase_e *phase);

/** Can a new image be written to staging (phase IDLE)? false for NULL. */
bool fwu_activate_staging_writable(const fwu_activate_t *activate);

/**
 * @brief  APPLICATION side: starts the activation of `page_count` pages of the verified image in staging
 *         with a COMMIT record. This call writes a single journal record to flash; it does not touch the application.
 *         A reboot is required after the call (the bootloader performs the activation).
 * @return FWU_OK; FWU_ERR_INVALID_ARG for NULL or page_count 0 / larger than the region / than FWU_ACT_MAX_PAGES;
 *         FWU_ERR_BAD_STATE if the phase is not IDLE; flash errors as they are.
 */
fwu_return_e fwu_activate_commit(fwu_activate_t *activate, uint32_t page_count);

/**
 * @brief  BOOTLOADER side: completes the pending work (verification, copy/swap, trial, rollback) and writes what it
 *         did to `outcome`. If interrupted, it continues where it stopped at the next boot.
 * @return FWU_OK (application consistent, can be jumped to); FWU_ERR_INVALID_ARG for NULL; flash error,
 *         FWU_ERR_FLASH_WRITE (read-back mismatch) or the error code of the verification hook: in that case
 *         DO NOT jump to the application. `outcome` is written only on FWU_OK.
 */
fwu_return_e fwu_activate_run(fwu_activate_t *activate, fwu_activate_outcome_e *outcome);

/**
 * @brief  NEW APPLICATION (in the trial boot): confirms the new version (CONFIRMED), then calls the on_confirmed
 *         hook. If called again while in the CONFIRMED state it only calls the hook again.
 * @return FWU_OK; FWU_ERR_INVALID_ARG for NULL; FWU_ERR_BAD_STATE if the phase is not TRIAL (and not CONFIRMED);
 *         flash errors or the error code of the hook as they are.
 */
fwu_return_e fwu_activate_confirm(fwu_activate_t *activate);

/**
 * @brief  NEW APPLICATION (in the trial boot): rejects the new version and requests a rollback at the next boot
 *         (REVERT record). A reboot is required after the call.
 * @return FWU_OK; FWU_ERR_INVALID_ARG for NULL; FWU_ERR_BAD_STATE if the phase is not TRIAL; flash errors.
 */
fwu_return_e fwu_activate_request_revert(fwu_activate_t *activate);

#ifdef __cplusplus
}
#endif

#endif /* FWU_ACTIVATE_H_ */
