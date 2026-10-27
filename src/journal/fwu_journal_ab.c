/**
 * @file    fwu_journal_ab.c
 * @brief   A/B alternating journal implementation (see fwu_journal_ab.h for the rules and the safety rationale).
 */

#include <stddef.h>
#include "fwu_journal_ab.h"

/* ------------------------------------------------------------------ helpers */

/** Is `a` STRICTLY newer than `b`? (32-bit wrap-around safe: newer if the difference is 1..0x7FFFFFFF.) */
static bool seq_is_newer(uint32_t a, uint32_t b)
{
    const uint32_t difference = (uint32_t)(a - b);
    return (difference != 0u) && (difference <= 0x7FFFFFFFu);
}

/** Chooses the active region when both regions have been scanned. */
static void choose_active(fwu_journal_ab_t *journal)
{
    const fwu_journal_t *a = &journal->part[0];
    const fwu_journal_t *b = &journal->part[1];

    if(a->has_last && b->has_last) {
        journal->active = seq_is_newer(b->last.seq, a->last.seq) ? 1u : 0u;
    }
    else if(b->has_last) {
        journal->active = 1u;
    }
    else {
        journal->active = 0u;
    }
}

/**
 * The active region is full: makes room. Normally it erases the other region and writes a snapshot of the
 * last record there (see the header for the steps). If the active region has no valid record at all (it is
 * full of only garbage/partial records; in that case the other region has no valid record either), there is
 * nothing to lose, so the active region itself is erased.
 */
static fwu_return_e journal_ab_rotate(fwu_journal_ab_t *journal)
{
    fwu_journal_t *active = &journal->part[journal->active];
    fwu_journal_t *target;
    fwu_return_e status;

    if(!active->has_last) {
        status = fwu_flash_erase(active->flash, active->offset, journal->region_length);
        if(status != FWU_OK) {
            return status;
        }
        return fwu_journal_scan(active);
    }

    target = &journal->part[1u - journal->active];

    /* 1) Erase the old region. The active region is untouched, the last state is safe. */
    status = fwu_flash_erase(target->flash, target->offset, journal->region_length);
    if(status != FWU_OK) {
        return status;
    }
    status = fwu_journal_scan(target);          /* reset the RAM state to match the erased region */
    if(status != FWU_OK) {
        return status;
    }

    /* 2) Snapshot: a copy of the last record, consecutive sequence number. When the write completes this region
     *    becomes "newer" and is considered active. */
    target->next_seq = (uint32_t)(active->last.seq + 1u);
    status = fwu_journal_append(target, active->last.state, active->last.flags, active->last.arg);
    if(status != FWU_OK) {
        return status;
    }

    journal->active = 1u - journal->active;
    return FWU_OK;
}

/* ------------------------------------------------------------------ public API */

fwu_return_e fwu_journal_ab_scan(fwu_journal_ab_t *journal)
{
    fwu_return_e status;

    if(journal == NULL) {
        return FWU_ERR_INVALID_ARG;
    }
    status = fwu_journal_scan(&journal->part[0]);
    if(status != FWU_OK) {
        return status;
    }
    status = fwu_journal_scan(&journal->part[1]);
    if(status != FWU_OK) {
        return status;
    }
    choose_active(journal);
    return FWU_OK;
}

fwu_return_e fwu_journal_ab_init(fwu_journal_ab_t *journal, const fwu_flash_t *flash,
                                 uint32_t offset_a, uint32_t offset_b, uint32_t length)
{
    fwu_return_e status;

    if((journal == NULL) || (flash == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }

    status = fwu_journal_init(&journal->part[0], flash, offset_a, length);
    if(status != FWU_OK) {
        return status;
    }
    status = fwu_journal_init(&journal->part[1], flash, offset_b, length);
    if(status != FWU_OK) {
        return status;
    }

    /* The regions were verified to be inside the flash (the sums do not overflow). They must not overlap. */
    if(((offset_a + length) > offset_b) && ((offset_b + length) > offset_a)) {
        return FWU_ERR_INVALID_ARG;
    }
    /* At least 2 slots for the snapshot + the actual record. */
    if(journal->part[0].slot_count < 2u) {
        return FWU_ERR_INVALID_ARG;
    }

    journal->region_length = length;
    choose_active(journal);
    return FWU_OK;
}

fwu_return_e fwu_journal_ab_append(fwu_journal_ab_t *journal, fwu_byte_t state, fwu_byte_t flags,
                                   uint32_t arg)
{
    fwu_journal_t *active;
    fwu_return_e status;

    if(journal == NULL) {
        return FWU_ERR_INVALID_ARG;
    }

    active = &journal->part[journal->active];
    if(active->next_slot >= active->slot_count) {
        status = journal_ab_rotate(journal);
        if(status != FWU_OK) {
            return status;
        }
    }

    return fwu_journal_append(&journal->part[journal->active], state, flags, arg);
}

bool fwu_journal_ab_get_last(const fwu_journal_ab_t *journal, fwu_journal_record_t *out)
{
    if(journal == NULL) {
        return false;
    }
    return fwu_journal_get_last(&journal->part[journal->active], out);
}
