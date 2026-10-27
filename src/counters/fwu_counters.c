/**
 * @file    fwu_counters.c
 * @brief   Version counters implementation (see fwu_counters.h for the rules, record format and limits).
 */

#include <stddef.h>
#include "fwu_counters.h"

/** Loads the values from the last valid record into RAM; (0, 0) if there is no record. Makes the object ready. */
static void counters_load(fwu_counters_t *counters)
{
    fwu_journal_record_t last;

    if(fwu_journal_ab_get_last(&counters->journal, &last)) {
        counters->min_image_version = last.arg;
        counters->min_key_version = (uint16_t)((((uint32_t)last.state & 0xFFu) << 8) | ((uint32_t)last.flags & 0xFFu));
    }
    else {
        counters->min_image_version = 0u;
        counters->min_key_version = 0u;
    }
    counters->ready = true;
}

fwu_return_e fwu_counters_init(fwu_counters_t *counters, const fwu_flash_t *flash,
                               uint32_t offset_a, uint32_t offset_b, uint32_t length)
{
    fwu_return_e status;

    if(counters == NULL) {
        return FWU_ERR_INVALID_ARG;
    }
    counters->ready = false;                    /* stays untrusted in case of error */
    counters->min_image_version = 0u;
    counters->min_key_version = 0u;

    status = fwu_journal_ab_init(&counters->journal, flash, offset_a, offset_b, length);
    if(status != FWU_OK) {
        return status;
    }
    counters_load(counters);
    return FWU_OK;
}

fwu_return_e fwu_counters_reload(fwu_counters_t *counters)
{
    fwu_return_e status;

    if(counters == NULL) {
        return FWU_ERR_INVALID_ARG;
    }
    status = fwu_journal_ab_scan(&counters->journal);
    if(status != FWU_OK) {
        counters->ready = false;
        return status;
    }
    counters_load(counters);
    return FWU_OK;
}

fwu_return_e fwu_counters_get(const fwu_counters_t *counters, uint32_t *image_version, uint16_t *key_version)
{
    if((counters == NULL) || (image_version == NULL) || (key_version == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    if(!counters->ready) {
        return FWU_ERR_BAD_STATE;
    }
    *image_version = counters->min_image_version;
    *key_version = counters->min_key_version;
    return FWU_OK;
}

fwu_return_e fwu_counters_raise(fwu_counters_t *counters, uint32_t image_version, uint16_t key_version)
{
    uint32_t new_image;
    uint16_t new_key;
    fwu_return_e status;

    if(counters == NULL) {
        return FWU_ERR_INVALID_ARG;
    }
    if(!counters->ready) {
        return FWU_ERR_BAD_STATE;
    }

    new_image = (image_version > counters->min_image_version) ? image_version : counters->min_image_version;
    new_key = (key_version > counters->min_key_version) ? key_version : counters->min_key_version;
    if((new_image == counters->min_image_version) && (new_key == counters->min_key_version)) {
        return FWU_OK;                          /* nothing to raise: do not write to flash (wear, idempotent) */
    }

    status = fwu_journal_ab_append(&counters->journal, (fwu_byte_t)(new_key >> 8), (fwu_byte_t)(new_key & 0xFFu),
                                   new_image);
    if(status != FWU_OK) {
        /* RAM state is untrusted: the record may have completed in flash. Re-read; not ready if it cannot be read. */
        (void)fwu_counters_reload(counters);
        return status;
    }
    counters->min_image_version = new_image;
    counters->min_key_version = new_key;
    return FWU_OK;
}

fwu_return_e fwu_counters_on_confirmed(void *context)
{
    const fwu_counters_confirm_t *confirm = (const fwu_counters_confirm_t *)context;
    fwu_manifest_t manifest;
    fwu_return_e status;

    if((confirm == NULL) || (confirm->counters == NULL) || (confirm->verify == NULL) || (confirm->flash == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    status = fwu_verify_manifest(confirm->verify, confirm->flash, &manifest);
    if(status != FWU_OK) {
        return status;                          /* the counter is not raised from an unsigned/corrupt manifest */
    }
    return fwu_counters_raise(confirm->counters, manifest.image_version, manifest.key_version);
}
