/**
 * @file    fwu_journal.h
 * @brief   Journal: small, self-validating records appended one after another in flash.
 *          It is the "where am I" ledger of the power-loss-safe update.
 *
 * This module provides ONLY A RELIABLE LEDGER: appending a record (fwu_journal_append) and reading the
 * ledger to find the state (fwu_journal_init / fwu_journal_scan). The MEANING of the records (COMMIT,
 * DONE, progress...) is not this module's business but that of the activation logic (G8b); here `state`,
 * `flags` and `arg` are carried opaquely.
 *
 * RECORD (16 fwu_byte_t, multi-byte fields big endian):
 *
 *   offset size  field
 *     0      2    magic   (0x4A52)
 *     2      1    state   step code (opaque)
 *     3      1    flags   flags (opaque)
 *     4      4    seq     sequence number (each record is the previous one + 1, 32 bit)
 *     8      4    arg     progress data (opaque, e.g. page number)
 *    12      2    reserved (zero)
 *    14      2    crc     CRC-16 (fwu_crc16), covers offsets 0..13
 *
 * The record size must be a MULTIPLE of the flash write granule (write_granule) (init checks this).
 *
 * RULES
 *   - A record is ALWAYS appended to the first empty slot; nothing is erased (flash is written only
 *     in erased areas). Empty slot = all bytes at the erased value.
 *   - A partially written record (power was lost during the write) does not match magic/CRC and is IGNORED;
 *     its slot is wasted, the next record is written to the next slot.
 *   - The sequence numbers of valid records must be consecutive (seq == previous seq + 1, including
 *     32-bit wrap-around); a record that is not consecutive is considered invalid.
 *   - Atomicity: wherever power is lost during an append, the next scan yields either the OLD state or
 *     the NEW state, and never counts a corrupt record as valid.
 *   - If the journal fills up, append returns FWU_ERR_BUFFER_FULL. Page switching (A/B alternating erase) is OUTSIDE
 *     the scope of this module: fwu_journal_ab.h rotates two regions on top of this module (activation uses that).
 *   - The region must be ALIGNED to flash pages (the erase unit); the contents of the region may be random
 *     at the start, the scan tolerates this.
 *   - If a fwu_journal_append returns an error (e.g. power loss), the caller must re-read the state with
 *     fwu_journal_scan (or init); the object's RAM state is unreliable then.
 */

#ifndef FWU_JOURNAL_H_
#define FWU_JOURNAL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "fwu_error.h"
#include "fwu_types.h"
#include "fwu_flash.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ record format */

#define FWU_JOURNAL_MAGIC               0x4A52u    /* "JR" */
#define FWU_JOURNAL_RECORD_SIZE         16u        /* fwu_byte_t */

#define FWU_JOURNAL_OFFSET_MAGIC        0u
#define FWU_JOURNAL_OFFSET_STATE        2u
#define FWU_JOURNAL_OFFSET_FLAGS        3u
#define FWU_JOURNAL_OFFSET_SEQ          4u
#define FWU_JOURNAL_OFFSET_ARG          8u
#define FWU_JOURNAL_OFFSET_RESERVED     12u
#define FWU_JOURNAL_OFFSET_CRC          14u
#define FWU_JOURNAL_CRC_COVERED_SIZE    14u        /* crc covers offsets 0..13 */

/** Content of a record (its meaning belongs to the upper layer). */
typedef struct {
    fwu_byte_t state;   /**< Step code (opaque). */
    fwu_byte_t flags;   /**< Flags (opaque). */
    uint32_t   seq;     /**< Sequence number (assigned by the module, +1 for each record). */
    uint32_t   arg;     /**< Progress data (opaque). */
} fwu_journal_record_t;

/** Journal state. Do not touch the fields directly (the counters may be read only). */
typedef struct {
    const fwu_flash_t   *flash;          /**< Flash the journal is written to. */
    uint32_t             offset;         /**< Offset of the region from the flash start (fwu_byte_t). */
    uint32_t             slot_count;     /**< Number of slots in the region. */
    uint32_t             next_slot;      /**< First empty slot (journal is full if slot_count). */
    uint32_t             next_seq;       /**< Sequence number to be given to the next record. */
    bool                 has_last;       /**< Whether a valid record exists. */
    fwu_journal_record_t last;           /**< Last valid record (if has_last). */
    uint32_t             valid_slots;    /**< Number of valid records in the last scan. */
    uint32_t             invalid_slots;  /**< Number of invalid (partially written/corrupt) slots in the last scan. */
} fwu_journal_t;

/**
 * @brief  Binds the journal to a flash region and scans the region (reads the state).
 * @param  journal  Journal to initialize (must not be NULL).
 * @param  flash    Flash object (must not be NULL, must have passed fwu_flash_validate; must stay
 *                  valid as long as the journal is used).
 * @param  offset   Offset of the region from the flash start (fwu_byte_t); must be page aligned.
 * @param  length   Length of the region (fwu_byte_t); a multiple of the page size, at least one page.
 * @return FWU_OK; FWU_ERR_INVALID_ARG for NULL or page misalignment/length 0; FWU_ERR_OUT_OF_RANGE if the
 *         region is outside the flash; FWU_ERR_FLASH_GEOMETRY if the record size is not a multiple of the
 *         write granule; a flash read error is returned as is.
 */
fwu_return_e fwu_journal_init(fwu_journal_t *journal, const fwu_flash_t *flash, uint32_t offset, uint32_t length);

/**
 * @brief  Rescans the region: finds the last valid record, the first empty slot and the counters. To
 *         re-read the state from flash after a power loss (or when append fails).
 * @return FWU_OK; a flash read error as is; FWU_ERR_INVALID_ARG if journal is NULL.
 */
fwu_return_e fwu_journal_scan(fwu_journal_t *journal);

/**
 * @brief  Appends a record to the journal (to the first empty slot). The module assigns the sequence number.
 * @param  journal  Journal (must not be NULL).
 * @param  state    Step code (low 8 bits).
 * @param  flags    Flags (low 8 bits).
 * @param  arg      Progress data.
 * @return FWU_OK (record was written to flash and can be verified); FWU_ERR_BUFFER_FULL (no empty slot);
 *         FWU_ERR_INVALID_ARG (journal NULL); a flash write error is returned as is (e.g. power
 *         loss). On error the state is unreliable: it must be re-read with fwu_journal_scan.
 */
fwu_return_e fwu_journal_append(fwu_journal_t *journal, fwu_byte_t state, fwu_byte_t flags, uint32_t arg);

/**
 * @brief  Returns the last valid record (this is a QUERY: it returns a bool, not an error code).
 * @param  out  The last valid record is written here (if has_last); may be NULL (existence query only).
 * @return true  a valid record exists (out was filled); false the journal is empty (no valid record at all) or
 *         journal is NULL.
 */
bool fwu_journal_get_last(const fwu_journal_t *journal, fwu_journal_record_t *out);

#ifdef __cplusplus
}
#endif

#endif /* FWU_JOURNAL_H_ */
