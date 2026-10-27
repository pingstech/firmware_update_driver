/**
 * @file    fwu_activate.c
 * @brief   Activation implementation (see fwu_activate.h for the rules, record format and step layout).
 */

#include <stddef.h>
#include "fwu_activate.h"

/** Number of elements read at a time in the read-back comparison (small buffer on the stack). */
#define ACT_COMPARE_CHUNK   16u

/* ------------------------------------------------------------------ record helpers */

static uint32_t act_pack(uint32_t pages, uint32_t counter)
{
    return (uint32_t)((pages << 16) | (counter & 0xFFFFu));
}

static fwu_byte_t act_flags(const fwu_activate_t *activate)
{
    return (activate->config.mode == FWU_ACTIVATE_MODE_SWAP) ? (fwu_byte_t)FWU_ACT_FLAG_SWAP : (fwu_byte_t)0u;
}

static bool act_is_swap(const fwu_activate_t *activate)
{
    return activate->config.mode == FWU_ACTIVATE_MODE_SWAP;
}

/** Total number of steps for an image: pages in COPY, pages * 3 in SWAP. */
static uint32_t act_total_steps(const fwu_activate_t *activate, uint32_t pages)
{
    return act_is_swap(activate) ? (pages * FWU_ACT_STEPS_PER_PAGE) : pages;
}

/**
 * Does the error code of the verification hook mean "image invalid / unacceptable"? The 3x group consists of
 * integrity errors (CRC, length, frame, manifest, image too large, digest mismatch); from the 5x group: invalid
 * signature (BAD_SIGNATURE), wrong product (PRODUCT_MISMATCH), old version (VERSION_REJECTED) and rejected key
 * (KEY_REJECTED): all of them REJECT the image. Other codes (flash read error, FWU_ERR_CRYPTO_FAILURE, etc.)
 * mean "verification could not be performed" and do NOT reject the image (the error is propagated, retried at the next boot).
 * If a new "unacceptable" code is added it must be added here too (CRYPTO_FAILURE is deliberately excluded).
 */
static bool act_is_rejection(fwu_return_e status)
{
    return (((int)status >= 30) && ((int)status < 40)) || (status == FWU_ERR_BAD_SIGNATURE) ||
           (status == FWU_ERR_PRODUCT_MISMATCH) || (status == FWU_ERR_VERSION_REJECTED) ||
           (status == FWU_ERR_KEY_REJECTED);
}

/** Reads the last journal record and derives the phase, page count and counter; BAD_STATE if it cannot be interpreted. */
static fwu_return_e act_refresh(fwu_activate_t *activate)
{
    fwu_journal_record_t last;
    bool swap_record;
    uint32_t pages;
    uint32_t counter;

    if(!fwu_journal_ab_get_last(&activate->journal, &last)) {
        activate->phase      = FWU_ACT_PHASE_IDLE;
        activate->last_state = 0u;
        activate->page_count = 0u;
        activate->counter    = 0u;
        return FWU_OK;
    }

    pages   = last.arg >> 16;
    counter = last.arg & 0xFFFFu;
    swap_record = (last.flags & FWU_ACT_FLAG_SWAP) != 0u;

    activate->last_state = last.state;
    activate->page_count = pages;
    activate->counter    = counter;

    /* Final states: no pending work. */
    if((last.state == FWU_ACT_STATE_DONE) || (last.state == FWU_ACT_STATE_REJECTED) ||
       (last.state == FWU_ACT_STATE_CONFIRMED) || (last.state == FWU_ACT_STATE_REVERTED)) {
        activate->phase = FWU_ACT_PHASE_IDLE;
        return FWU_OK;
    }

    /* States with pending work: the record must belong to this configuration and be consistent. */
    if((last.state != FWU_ACT_STATE_COMMIT) && (last.state != FWU_ACT_STATE_PROGRESS) &&
       (last.state != FWU_ACT_STATE_TRIAL) && (last.state != FWU_ACT_STATE_REVERT)) {
        return FWU_ERR_BAD_STATE;                       /* unknown state code */
    }
    if(swap_record != act_is_swap(activate)) {
        return FWU_ERR_BAD_STATE;                       /* record of another strategy */
    }
    if((pages == 0u) || (pages > FWU_ACT_MAX_PAGES) || (pages > activate->region_pages)) {
        return FWU_ERR_BAD_STATE;                       /* page count that does not fit the region */
    }

    if(last.state == FWU_ACT_STATE_COMMIT) {
        if(counter != 0u) {
            return FWU_ERR_BAD_STATE;
        }
        activate->phase = FWU_ACT_PHASE_ACTIVATING;
    }
    else if(last.state == FWU_ACT_STATE_PROGRESS) {
        if((counter == 0u) || (counter > act_total_steps(activate, pages))) {
            return FWU_ERR_BAD_STATE;
        }
        activate->phase = FWU_ACT_PHASE_ACTIVATING;
    }
    else if(last.state == FWU_ACT_STATE_TRIAL) {
        if((!act_is_swap(activate)) || (counter == 0u) || (counter > FWU_ACT_MAX_TRIAL_BOOTS)) {
            return FWU_ERR_BAD_STATE;
        }
        activate->phase = FWU_ACT_PHASE_TRIAL;
    }
    else {
        if((!act_is_swap(activate)) || (counter > (pages * FWU_ACT_STEPS_PER_PAGE))) {
            return FWU_ERR_BAD_STATE;
        }
        activate->phase = FWU_ACT_PHASE_REVERTING;
    }
    return FWU_OK;
}

/** Appends a record to the journal and, if successful, updates the phase. */
static fwu_return_e act_append(fwu_activate_t *activate, fwu_byte_t state, uint32_t pages, uint32_t counter)
{
    const fwu_return_e status = fwu_journal_ab_append(&activate->journal, state, act_flags(activate),
                                                      act_pack(pages, counter));
    if(status != FWU_OK) {
        return status;
    }
    return act_refresh(activate);
}

/* ------------------------------------------------------------------ flash steps */

/** Compares `length` elements in flash with `expected`; FWU_ERR_FLASH_WRITE on mismatch. */
static fwu_return_e act_compare(const fwu_flash_t *flash, uint32_t offset, const fwu_byte_t *expected, uint32_t length)
{
    fwu_byte_t actual[ACT_COMPARE_CHUNK];
    uint32_t done = 0u;

    while(done < length) {
        uint32_t part = length - done;
        fwu_return_e status;

        if(part > ACT_COMPARE_CHUNK) {
            part = ACT_COMPARE_CHUNK;
        }
        status = fwu_flash_read(flash, offset + done, part, actual);
        if(status != FWU_OK) {
            return status;
        }
        for(uint32_t i = 0u; i < part; i++) {
            if((actual[i] & FWU_BYTE_MASK) != (expected[done + i] & FWU_BYTE_MASK)) {
                return FWU_ERR_FLASH_WRITE;
            }
        }
        done += part;
    }
    return FWU_OK;
}

/**
 * Copies one page: erases the destination page, writes the source chunk by chunk through the work buffer and reads
 * back and compares every chunk. Source and destination are NOT the same page. If interrupted it can be repeated from the start
 * (the destination is erased and rewritten anyway, the source is not touched).
 */
static fwu_return_e act_copy_page(const fwu_activate_t *activate, uint32_t source, uint32_t destination)
{
    const fwu_flash_t *flash = activate->config.flash;
    const uint32_t page_length = flash->geometry.page_length;
    fwu_byte_t *work = activate->config.work;
    uint32_t done = 0u;
    fwu_return_e status = fwu_flash_erase(flash, destination, page_length);

    if(status != FWU_OK) {
        return status;
    }
    while(done < page_length) {
        uint32_t part = page_length - done;     /* multiple of the granule: page and chunk are multiples of the granule */

        if(part > activate->chunk) {
            part = activate->chunk;
        }
        status = fwu_flash_read(flash, source + done, part, work);
        if(status != FWU_OK) {
            return status;
        }
        status = fwu_flash_write(flash, destination + done, part, work);
        if(status != FWU_OK) {
            return status;
        }
        status = act_compare(flash, destination + done, work, part);
        if(status != FWU_OK) {
            return status;
        }
        done += part;
    }
    return FWU_OK;
}

/** Step `page` of COPY: copies the staging page to the application page. */
static fwu_return_e act_copy_step(const fwu_activate_t *activate, uint32_t page)
{
    const uint32_t page_length = activate->config.flash->geometry.page_length;
    return act_copy_page(activate, activate->config.staging_offset + (page * page_length),
                         activate->config.app_offset + (page * page_length));
}

/** Step `step` of SWAP (page = step / 3, sub-step = step % 3; see the scheme in the header). */
static fwu_return_e act_swap_step(const fwu_activate_t *activate, uint32_t step)
{
    const uint32_t page_length = activate->config.flash->geometry.page_length;
    const uint32_t page = step / FWU_ACT_STEPS_PER_PAGE;
    const uint32_t sub = step % FWU_ACT_STEPS_PER_PAGE;
    const uint32_t app_page = activate->config.app_offset + (page * page_length);
    const uint32_t staging_page = activate->config.staging_offset + (page * page_length);
    /* The scratch page depends on the page number (spreads wear over K pages) and is the same during recovery. */
    const uint32_t scratch_page = activate->config.scratch_offset +
                                  ((page % activate->config.scratch_pages) * page_length);

    if(sub == 0u) {
        return act_copy_page(activate, app_page, scratch_page);                          /* A -> S */
    }
    if(sub == 1u) {
        return act_copy_page(activate, staging_page, app_page);                          /* B -> A */
    }
    return act_copy_page(activate, scratch_page, staging_page);                          /* S -> B */
}

/* ------------------------------------------------------------------ run: phase handlers */

/** ACTIVATING: verification (before the first touch), copy/swap steps, completion record. */
static fwu_return_e act_run_activating(fwu_activate_t *activate, fwu_activate_outcome_e *result)
{
    const uint32_t pages = activate->page_count;
    const uint32_t total = act_total_steps(activate, pages);
    const uint32_t page_length = activate->config.flash->geometry.page_length;
    fwu_return_e status;

    /* Authoritative verification: only at COMMIT (counter 0), while staging/application have not been touched at all.
     * If a progress record exists, staging was already verified and has not changed (never in COPY, in
     * the first 2 steps in SWAP). */
    if((activate->last_state == FWU_ACT_STATE_COMMIT) && (activate->config.verify != NULL)) {
        status = activate->config.verify(activate->config.verify_context, activate->config.flash,
                                         activate->config.staging_offset, pages * page_length);
        if(act_is_rejection(status)) {
            status = act_append(activate, (fwu_byte_t)FWU_ACT_STATE_REJECTED, pages, 0u);
            if(status != FWU_OK) {
                return status;
            }
            *result = FWU_ACT_OUTCOME_REJECTED;
            return FWU_OK;
        }
        if(status != FWU_OK) {
            return status;                              /* verification could not be performed: do not reject, return the error */
        }
    }

    for(uint32_t step = activate->counter; step < total; step++) {
        status = act_is_swap(activate) ? act_swap_step(activate, step) : act_copy_step(activate, step);
        if(status != FWU_OK) {
            return status;
        }
        status = act_append(activate, (fwu_byte_t)FWU_ACT_STATE_PROGRESS, pages, step + 1u);
        if(status != FWU_OK) {
            return status;
        }
    }

    if(act_is_swap(activate)) {
        status = act_append(activate, (fwu_byte_t)FWU_ACT_STATE_TRIAL, pages, 1u);   /* trial 1 started */
        *result = FWU_ACT_OUTCOME_TRIAL;
    }
    else {
        status = act_append(activate, (fwu_byte_t)FWU_ACT_STATE_DONE, pages, 0u);
        *result = FWU_ACT_OUTCOME_ACTIVATED;
    }
    return status;
}

/** REVERTING: the same exchange (with REVERT records), REVERTED at the end. */
static fwu_return_e act_run_reverting(fwu_activate_t *activate, fwu_activate_outcome_e *result)
{
    const uint32_t pages = activate->page_count;
    const uint32_t total = pages * FWU_ACT_STEPS_PER_PAGE;
    fwu_return_e status;

    for(uint32_t step = activate->counter; step < total; step++) {
        status = act_swap_step(activate, step);
        if(status != FWU_OK) {
            return status;
        }
        status = act_append(activate, (fwu_byte_t)FWU_ACT_STATE_REVERT, pages, step + 1u);
        if(status != FWU_OK) {
            return status;
        }
    }
    status = act_append(activate, (fwu_byte_t)FWU_ACT_STATE_REVERTED, pages, 0u);
    *result = FWU_ACT_OUTCOME_REVERTED;
    return status;
}

/* ------------------------------------------------------------------ public API */

/** Do regions a and b overlap? (Called after the regions are validated to be inside flash: the sums do not overflow.) */
static bool act_overlap(uint32_t offset_a, uint32_t length_a, uint32_t offset_b, uint32_t length_b)
{
    return ((offset_a + length_a) > offset_b) && ((offset_b + length_b) > offset_a);
}

/** Is [offset, offset + length) inside flash (without computing the sum, overflow safe)? */
static bool act_in_flash(const fwu_flash_t *flash, uint32_t offset, uint32_t length)
{
    return (offset <= flash->geometry.flash_length) && (length <= (flash->geometry.flash_length - offset));
}

fwu_return_e fwu_activate_init(fwu_activate_t *activate, const fwu_activate_config_t *config)
{
    const fwu_flash_t *flash;
    uint32_t page_length;
    uint32_t granule;
    uint32_t chunk;
    uint32_t scratch_length = 0u;
    fwu_return_e status;

    if((activate == NULL) || (config == NULL) || (config->flash == NULL) || (config->work == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    if(config->require_verify && (config->verify == NULL)) {
        return FWU_ERR_INVALID_ARG;                     /* verification required but no hook */
    }
    flash = config->flash;
    if(fwu_flash_validate(flash) != FWU_OK) {
        return FWU_ERR_FLASH_GEOMETRY;
    }
    page_length = flash->geometry.page_length;
    granule     = flash->geometry.write_granule;

    if((config->mode != FWU_ACTIVATE_MODE_COPY) && (config->mode != FWU_ACTIVATE_MODE_SWAP)) {
        return FWU_ERR_INVALID_ARG;
    }
    if((config->region_length == 0u) || ((config->region_length % page_length) != 0u) ||
       ((config->app_offset % page_length) != 0u) || ((config->staging_offset % page_length) != 0u)) {
        return FWU_ERR_INVALID_ARG;
    }
    if((!act_in_flash(flash, config->app_offset, config->region_length)) ||
       (!act_in_flash(flash, config->staging_offset, config->region_length))) {
        return FWU_ERR_OUT_OF_RANGE;
    }
    if(act_overlap(config->app_offset, config->region_length, config->staging_offset, config->region_length)) {
        return FWU_ERR_INVALID_ARG;
    }

    /* Copy chunk: the work buffer is rounded down to a multiple of the granule and cannot exceed the page. */
    chunk = config->work_length - (config->work_length % granule);
    if(chunk == 0u) {
        return FWU_ERR_INVALID_ARG;                     /* work buffer is smaller than one granule */
    }
    if(chunk > page_length) {
        chunk = page_length;
    }

    if(config->mode == FWU_ACTIVATE_MODE_SWAP) {
        if((config->max_trial_boots == 0u) || (config->max_trial_boots > FWU_ACT_MAX_TRIAL_BOOTS)) {
            return FWU_ERR_INVALID_ARG;
        }
        if((config->scratch_pages == 0u) || ((config->scratch_offset % page_length) != 0u)) {
            return FWU_ERR_INVALID_ARG;
        }
        /* K pages must fit in flash (without computing the product, overflow safe), then the region must be inside flash. */
        if(config->scratch_pages > (flash->geometry.flash_length / page_length)) {
            return FWU_ERR_OUT_OF_RANGE;
        }
        scratch_length = config->scratch_pages * page_length;
        if(!act_in_flash(flash, config->scratch_offset, scratch_length)) {
            return FWU_ERR_OUT_OF_RANGE;
        }
        if(act_overlap(config->scratch_offset, scratch_length, config->app_offset, config->region_length) ||
           act_overlap(config->scratch_offset, scratch_length, config->staging_offset, config->region_length)) {
            return FWU_ERR_INVALID_ARG;
        }
    }

    activate->config         = *config;
    activate->region_pages   = config->region_length / page_length;
    activate->scratch_length = scratch_length;
    activate->chunk          = chunk;

    status = fwu_journal_ab_init(&activate->journal, flash, config->journal_a_offset,
                                 config->journal_b_offset, config->journal_length);
    if(status != FWU_OK) {
        return status;
    }
    /* Journal regions must not overlap the application/staging/scratch pages (the journal regions are now
     * validated to be inside flash). */
    if(act_overlap(config->journal_a_offset, config->journal_length, config->app_offset, config->region_length) ||
       act_overlap(config->journal_a_offset, config->journal_length, config->staging_offset, config->region_length) ||
       act_overlap(config->journal_b_offset, config->journal_length, config->app_offset, config->region_length) ||
       act_overlap(config->journal_b_offset, config->journal_length, config->staging_offset, config->region_length)) {
        return FWU_ERR_INVALID_ARG;
    }
    if(config->mode == FWU_ACTIVATE_MODE_SWAP) {
        if(act_overlap(config->journal_a_offset, config->journal_length, config->scratch_offset, scratch_length) ||
           act_overlap(config->journal_b_offset, config->journal_length, config->scratch_offset, scratch_length)) {
            return FWU_ERR_INVALID_ARG;
        }
    }

    return act_refresh(activate);
}

fwu_return_e fwu_activate_get_phase(const fwu_activate_t *activate, fwu_activate_phase_e *phase)
{
    if((activate == NULL) || (phase == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    *phase = activate->phase;
    return FWU_OK;
}

bool fwu_activate_staging_writable(const fwu_activate_t *activate)
{
    return (activate != NULL) && (activate->phase == FWU_ACT_PHASE_IDLE);
}

fwu_return_e fwu_activate_commit(fwu_activate_t *activate, uint32_t page_count)
{
    if(activate == NULL) {
        return FWU_ERR_INVALID_ARG;
    }
    if((page_count == 0u) || (page_count > activate->region_pages) || (page_count > FWU_ACT_MAX_PAGES)) {
        return FWU_ERR_INVALID_ARG;
    }
    if(activate->phase != FWU_ACT_PHASE_IDLE) {
        return FWU_ERR_BAD_STATE;
    }
    return act_append(activate, (fwu_byte_t)FWU_ACT_STATE_COMMIT, page_count, 0u);
}

fwu_return_e fwu_activate_run(fwu_activate_t *activate, fwu_activate_outcome_e *outcome)
{
    fwu_activate_outcome_e result = FWU_ACT_OUTCOME_NONE;

    if((activate == NULL) || (outcome == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }

    /* Every iteration advances the phase; at most TRIAL -> REVERTING -> IDLE (3 iterations) are needed. */
    for(uint32_t guard = 0u; guard < 4u; guard++) {
        fwu_return_e status;

        if(activate->phase == FWU_ACT_PHASE_IDLE) {
            *outcome = result;
            return FWU_OK;
        }
        if(activate->phase == FWU_ACT_PHASE_ACTIVATING) {
            status = act_run_activating(activate, &result);
            if(status != FWU_OK) {
                return status;
            }
            if(activate->phase == FWU_ACT_PHASE_TRIAL) {   /* swap finished: try the new image (jump now) */
                *outcome = result;
                return FWU_OK;
            }
        }
        else if(activate->phase == FWU_ACT_PHASE_TRIAL) {
            /* The trial started at the previous boot was restarted without being confirmed. */
            if(activate->counter >= activate->config.max_trial_boots) {
                status = act_append(activate, (fwu_byte_t)FWU_ACT_STATE_REVERT, activate->page_count, 0u);
                if(status != FWU_OK) {
                    return status;
                }
            }
            else {
                status = act_append(activate, (fwu_byte_t)FWU_ACT_STATE_TRIAL, activate->page_count,
                                    activate->counter + 1u);
                if(status != FWU_OK) {
                    return status;
                }
                *outcome = FWU_ACT_OUTCOME_TRIAL;
                return FWU_OK;
            }
        }
        else {
            status = act_run_reverting(activate, &result);
            if(status != FWU_OK) {
                return status;
            }
        }
    }
    return FWU_ERR_BAD_STATE;                           /* unreachable: the phase did not advance */
}

fwu_return_e fwu_activate_confirm(fwu_activate_t *activate)
{
    fwu_return_e status;

    if(activate == NULL) {
        return FWU_ERR_INVALID_ARG;
    }
    if(activate->phase == FWU_ACT_PHASE_TRIAL) {
        status = act_append(activate, (fwu_byte_t)FWU_ACT_STATE_CONFIRMED, activate->page_count, 0u);
        if(status != FWU_OK) {
            return status;
        }
    }
    else if(!((activate->phase == FWU_ACT_PHASE_IDLE) && (activate->last_state == FWU_ACT_STATE_CONFIRMED))) {
        return FWU_ERR_BAD_STATE;
    }
    else {
        /* Already CONFIRMED: only call the hook again (the previous hook may have failed). */
    }

    if(activate->config.on_confirmed != NULL) {
        return activate->config.on_confirmed(activate->config.on_confirmed_context);
    }
    return FWU_OK;
}

fwu_return_e fwu_activate_request_revert(fwu_activate_t *activate)
{
    if(activate == NULL) {
        return FWU_ERR_INVALID_ARG;
    }
    if(activate->phase != FWU_ACT_PHASE_TRIAL) {
        return FWU_ERR_BAD_STATE;
    }
    return act_append(activate, (fwu_byte_t)FWU_ACT_STATE_REVERT, activate->page_count, 0u);
}
