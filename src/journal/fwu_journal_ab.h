/**
 * @file    fwu_journal_ab.h
 * @brief   A/B alternating journal: provides a journal that never fills up by rotating between two separate
 *          flash regions (A and B). It only uses fwu_journal (single region) twice and makes the
 *          TRANSITION between them power-loss safe.
 *
 * WHY: when a single-region journal fills up, append returns FWU_ERR_BUFFER_FULL. A record cannot be erased
 * in flash; only a whole page can be erased. Before erasing a page, the latest state must be safe somewhere
 * else; otherwise, if power is lost during the erase, the state is lost. The solution is two pages:
 * one is "active" (records are appended to it), the other is "empty/old". When the active one fills up:
 *
 *     1. The other page is ERASED         (it holds only OLD records, the active page is untouched)
 *     2. A COPY (snapshot) of the last record is written to the first slot of the new page; its sequence
 *        number is last seq + 1. When this write completes, the new page becomes "newer" and is considered active.
 *     3. The actual record is appended to the new page.
 *
 *   If power is lost in step 1: the active page is intact, the other is half erased (looks empty) -> same state.
 *   If power is lost in step 2: a partial record in the new page (invalid, ignored) -> same state.
 *   If power is lost in step 3: the snapshot is already valid -> same state (the actual record was not written).
 *   In every case the last VALID record in the next scan is either the old state or the new state; nothing is lost.
 *
 * HOW THE ACTIVE PAGE IS FOUND: both pages are scanned; among the pages that have a valid record, the one
 * whose last seq is greater (wrap-around safe comparison over 32 bits) is active. On a tie A (0) is chosen.
 * If neither has a valid record, A is active.
 *
 * RULES
 *   - Regions A and B are regions of the same length, page aligned, and NOT overlapping each other
 *     (usually one page each). Each region must have at least 2 slots (snapshot + actual record).
 *   - Both regions must be erased before first use (factory/bootloader provisioning task).
 *     Random unerased content is tolerated during the scan, but unerased slots that follow erased
 *     slots may later cause a write error (FWU_ERR_FLASH_NOT_ERASED).
 *   - If an append returns an error (e.g. power loss), the object's RAM state is unreliable:
 *     it must be re-read with fwu_journal_ab_scan (or init).
 *   - The meaning of the records (state, flags, arg) is not this module's; the snapshot copies them AS THEY ARE,
 *     so the last record must be a sufficient state on its own (the activation module ensures this).
 */

#ifndef FWU_JOURNAL_AB_H_
#define FWU_JOURNAL_AB_H_

#include <stdbool.h>
#include <stdint.h>
#include "fwu_error.h"
#include "fwu_types.h"
#include "fwu_flash.h"
#include "fwu_journal.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A/B journal state. Do not touch the fields directly. */
typedef struct {
    fwu_journal_t part[2];      /**< 0 = region A, 1 = region B. */
    uint32_t      active;       /**< Active region (0 or 1). */
    uint32_t      region_length;/**< Length of one region (fwu_byte_t). */
} fwu_journal_ab_t;

/**
 * @brief  Binds the two regions, scans both and determines the active one.
 * @param  journal    Object to initialize (must not be NULL).
 * @param  flash      Flash object (must not be NULL; must stay valid as long as the journal is used).
 * @param  offset_a   Offset of region A (fwu_byte_t, page aligned).
 * @param  offset_b   Offset of region B (fwu_byte_t, page aligned).
 * @param  length     Length of each region (fwu_byte_t; a multiple of the page size, at least 2 records).
 * @return FWU_OK; FWU_ERR_INVALID_ARG for NULL, region alignment/length, overlapping regions or space for fewer than
 *         2 slots; FWU_ERR_OUT_OF_RANGE if a region is outside the flash; FWU_ERR_FLASH_GEOMETRY for an invalid
 *         geometry; a flash read error is returned as is.
 */
fwu_return_e fwu_journal_ab_init(fwu_journal_ab_t *journal, const fwu_flash_t *flash,
                                 uint32_t offset_a, uint32_t offset_b, uint32_t length);

/**
 * @brief  Rescans both regions and determines the active one (after a power loss or when append
 *         fails).
 * @return FWU_OK; FWU_ERR_INVALID_ARG if journal is NULL; a flash read error as is.
 */
fwu_return_e fwu_journal_ab_scan(fwu_journal_ab_t *journal);

/**
 * @brief  Appends a record. If the active region is full, it first switches regions (erase + snapshot), then
 *         appends the record to the new region. The module assigns the sequence number (consecutive across a region switch too).
 * @return FWU_OK; FWU_ERR_INVALID_ARG if journal is NULL; a flash error (including power loss) is returned
 *         as is; on error the state is unreliable and must be re-read with fwu_journal_ab_scan.
 */
fwu_return_e fwu_journal_ab_append(fwu_journal_ab_t *journal, fwu_byte_t state, fwu_byte_t flags,
                                   uint32_t arg);

/**
 * @brief  Returns the last valid record (this is a QUERY: it returns a bool, not an error code).
 * @param  out  The last valid record is written here; may be NULL (existence query only).
 * @return true a valid record exists; false no valid record at all or journal is NULL.
 */
bool fwu_journal_ab_get_last(const fwu_journal_ab_t *journal, fwu_journal_record_t *out);

#ifdef __cplusplus
}
#endif

#endif /* FWU_JOURNAL_AB_H_ */
