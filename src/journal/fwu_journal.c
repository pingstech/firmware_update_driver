/**
 * @file    fwu_journal.c
 * @brief   Journal implementation (see fwu_journal.h for the format and rules).
 */

#include <stddef.h>
#include "fwu_journal.h"
#include "fwu_bytes.h"
#include "fwu_crc.h"

/* ------------------------------------------------------------------ helpers */

/** Is a slot completely erased (never written)? */
static bool journal_slot_is_erased(const fwu_flash_t *flash, const fwu_byte_t *raw)
{
    for(size_t i = 0u; i < FWU_JOURNAL_RECORD_SIZE; i++) {
        if((raw[i] & FWU_BYTE_MASK) != (flash->geometry.erased_value & FWU_BYTE_MASK)) {
            return false;
        }
    }
    return true;
}

/** Are the raw bytes a valid record (magic and CRC)? If valid, decodes it into `record`. */
static bool journal_parse(const fwu_byte_t *raw, fwu_journal_record_t *record)
{
    const uint16_t crc_calculated = fwu_crc16_classic_calculate(raw, FWU_JOURNAL_CRC_COVERED_SIZE);
    const uint16_t crc_stored = fwu_read_u16_big_endian(&raw[FWU_JOURNAL_OFFSET_CRC]);

    if(fwu_read_u16_big_endian(&raw[FWU_JOURNAL_OFFSET_MAGIC]) != FWU_JOURNAL_MAGIC) {
        return false;
    }
    if(crc_calculated != crc_stored) {
        return false;
    }
    record->state = (fwu_byte_t)(raw[FWU_JOURNAL_OFFSET_STATE] & FWU_BYTE_MASK);
    record->flags = (fwu_byte_t)(raw[FWU_JOURNAL_OFFSET_FLAGS] & FWU_BYTE_MASK);
    record->seq   = fwu_read_u32_big_endian(&raw[FWU_JOURNAL_OFFSET_SEQ]);
    record->arg   = fwu_read_u32_big_endian(&raw[FWU_JOURNAL_OFFSET_ARG]);
    return true;
}

/** Encodes a record into 16 bytes (magic, fields, zero, CRC). */
static void journal_encode(const fwu_journal_record_t *record, fwu_byte_t *raw)
{
    uint16_t crc;

    fwu_write_u16_big_endian(&raw[FWU_JOURNAL_OFFSET_MAGIC], FWU_JOURNAL_MAGIC);
    raw[FWU_JOURNAL_OFFSET_STATE] = (fwu_byte_t)(record->state & FWU_BYTE_MASK);
    raw[FWU_JOURNAL_OFFSET_FLAGS] = (fwu_byte_t)(record->flags & FWU_BYTE_MASK);
    fwu_write_u32_big_endian(&raw[FWU_JOURNAL_OFFSET_SEQ], record->seq);
    fwu_write_u32_big_endian(&raw[FWU_JOURNAL_OFFSET_ARG], record->arg);
    fwu_write_u16_big_endian(&raw[FWU_JOURNAL_OFFSET_RESERVED], 0u);
    crc = fwu_crc16_classic_calculate(raw, FWU_JOURNAL_CRC_COVERED_SIZE);
    fwu_write_u16_big_endian(&raw[FWU_JOURNAL_OFFSET_CRC], crc);
}

/* ------------------------------------------------------------------ public API */

fwu_return_e fwu_journal_scan(fwu_journal_t *journal)
{
    uint32_t valid_slots = 0u;
    uint32_t invalid_slots = 0u;
    uint32_t next_slot;
    bool has_last = false;
    fwu_journal_record_t last = { 0u, 0u, 0u, 0u };

    if((journal == NULL) || (journal->flash == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }

    next_slot = journal->slot_count;       /* if no empty slot is found, the journal is full */
    for(uint32_t slot = 0u; slot < journal->slot_count; slot++) {
        fwu_byte_t raw[FWU_JOURNAL_RECORD_SIZE];
        fwu_journal_record_t candidate;
        const fwu_return_e status = fwu_flash_read(journal->flash,
                                                   journal->offset + (slot * FWU_JOURNAL_RECORD_SIZE),
                                                   FWU_JOURNAL_RECORD_SIZE, raw);
        if(status != FWU_OK) {
            return status;
        }

        if(journal_slot_is_erased(journal->flash, raw)) {
            next_slot = slot;              /* first empty slot: nothing has been written after this point */
            break;
        }

        if(journal_parse(raw, &candidate) &&
           ((!has_last) || (candidate.seq == (uint32_t)(last.seq + 1u)))) {
            last = candidate;
            has_last = true;
            valid_slots++;
        }
        else {
            invalid_slots++;               /* partially written, corrupt or out of sequence: ignore */
        }
    }

    journal->next_slot     = next_slot;
    journal->has_last      = has_last;
    journal->last          = last;
    journal->next_seq      = has_last ? (uint32_t)(last.seq + 1u) : 1u;
    journal->valid_slots   = valid_slots;
    journal->invalid_slots = invalid_slots;
    return FWU_OK;
}

fwu_return_e fwu_journal_init(fwu_journal_t *journal, const fwu_flash_t *flash, uint32_t offset, uint32_t length)
{
    uint32_t page_length;
    uint32_t write_granule;
    uint32_t flash_length;

    if((journal == NULL) || (flash == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }

    page_length   = flash->geometry.page_length;
    write_granule = flash->geometry.write_granule;
    flash_length  = flash->geometry.flash_length;

    /* The flash object must be valid (page_length, write_granule non-zero...). */
    if(fwu_flash_validate(flash) != FWU_OK) {
        return FWU_ERR_FLASH_GEOMETRY;
    }
    /* The record size must be a multiple of the write granule. */
    if((FWU_JOURNAL_RECORD_SIZE % write_granule) != 0u) {
        return FWU_ERR_FLASH_GEOMETRY;
    }
    /* Region: at least one page, page aligned. */
    if((length == 0u) || ((offset % page_length) != 0u) || ((length % page_length) != 0u)) {
        return FWU_ERR_INVALID_ARG;
    }
    /* Region inside the flash (without computing the sum, overflow safe). */
    if((offset > flash_length) || (length > (flash_length - offset))) {
        return FWU_ERR_OUT_OF_RANGE;
    }

    journal->flash         = flash;
    journal->offset        = offset;
    journal->slot_count    = length / FWU_JOURNAL_RECORD_SIZE;
    journal->next_slot     = 0u;
    journal->next_seq      = 1u;
    journal->has_last      = false;
    journal->last.state    = 0u;
    journal->last.flags    = 0u;
    journal->last.seq      = 0u;
    journal->last.arg      = 0u;
    journal->valid_slots   = 0u;
    journal->invalid_slots = 0u;

    return fwu_journal_scan(journal);
}

fwu_return_e fwu_journal_append(fwu_journal_t *journal, fwu_byte_t state, fwu_byte_t flags, uint32_t arg)
{
    fwu_journal_record_t record;
    fwu_byte_t raw[FWU_JOURNAL_RECORD_SIZE];
    fwu_return_e status;

    if((journal == NULL) || (journal->flash == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    if(journal->next_slot >= journal->slot_count) {
        return FWU_ERR_BUFFER_FULL;
    }

    record.state = (fwu_byte_t)(state & FWU_BYTE_MASK);
    record.flags = (fwu_byte_t)(flags & FWU_BYTE_MASK);
    record.seq   = journal->next_seq;
    record.arg   = arg;
    journal_encode(&record, raw);

    status = fwu_flash_write(journal->flash, journal->offset + (journal->next_slot * FWU_JOURNAL_RECORD_SIZE),
                             FWU_JOURNAL_RECORD_SIZE, raw);
    if(status != FWU_OK) {
        return status;     /* state is unreliable: the caller must re-read with fwu_journal_scan */
    }

    /* The RAM state is updated ONLY if the write succeeded. */
    journal->last     = record;
    journal->has_last = true;
    journal->next_slot++;
    journal->next_seq = (uint32_t)(record.seq + 1u);
    journal->valid_slots++;
    return FWU_OK;
}

bool fwu_journal_get_last(const fwu_journal_t *journal, fwu_journal_record_t *out)
{
    if((journal == NULL) || (!journal->has_last)) {
        return false;
    }
    if(out != NULL) {
        *out = journal->last;
    }
    return true;
}
