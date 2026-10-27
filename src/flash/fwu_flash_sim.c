/**
 * @file    fwu_flash_sim.c
 * @brief   Simulated flash implementation (for the rules and the power loss model see fwu_flash_sim.h).
 */

#include <stddef.h>
#include "fwu_flash_sim.h"

/* ------------------------------------------------------------------ helpers */

/** May the basic operation complete? If not, cuts the power. */
static bool sim_operation_allowed(fwu_flash_sim_t *sim)
{
    if(sim->cut_armed && (sim->operations_done >= sim->cut_after)) {
        sim->powered = false;
        return false;
    }
    sim->operations_done++;
    return true;
}

/** Is the range [offset, offset+length) inside the region? (safe against addition overflow) */
static bool sim_range_ok(const fwu_flash_sim_t *sim, uint32_t offset, uint32_t length)
{
    const uint32_t total = sim->flash.geometry.flash_length;
    return (offset <= total) && (length <= (total - offset));
}

/** Common pre-check: is power present, is the length non-zero. */
static fwu_return_e sim_precheck(const fwu_flash_sim_t *sim, uint32_t length)
{
    if(!sim->powered) {
        return FWU_ERR_FLASH_POWER_LOSS;
    }
    if(length == 0u) {
        return FWU_ERR_INVALID_ARG;
    }
    return FWU_OK;
}

/* ------------------------------------------------------------------ operations (callback table) */

static fwu_return_e sim_read(void *context, uint32_t offset, uint32_t length, fwu_byte_t *data)
{
    fwu_flash_sim_t *sim = context;
    fwu_return_e status;

    if((sim == NULL) || (data == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    status = sim_precheck(sim, length);
    if(status != FWU_OK) {
        return status;
    }
    if(!sim_range_ok(sim, offset, length)) {
        return FWU_ERR_OUT_OF_RANGE;
    }
    for(uint32_t i = 0u; i < length; i++) {
        data[i] = sim->memory[offset + i];
    }
    return FWU_OK;
}

static fwu_return_e sim_erase(void *context, uint32_t offset, uint32_t length)
{
    fwu_flash_sim_t *sim = context;
    fwu_return_e status;
    uint32_t page_length;

    if(sim == NULL) {
        return FWU_ERR_INVALID_ARG;
    }
    status = sim_precheck(sim, length);
    if(status != FWU_OK) {
        return status;
    }
    if(!sim_range_ok(sim, offset, length)) {
        return FWU_ERR_OUT_OF_RANGE;
    }
    page_length = sim->flash.geometry.page_length;
    if(((offset % page_length) != 0u) || ((length % page_length) != 0u)) {
        return FWU_ERR_ERASE_ALIGNMENT;
    }

    for(uint32_t done = 0u; done < length; done += page_length) {
        const uint32_t start = offset + done;
        const uint32_t page = start / page_length;

        if(!sim_operation_allowed(sim)) {
            /* Interrupted erase: the first half is erased, the second half keeps the old data. */
            for(uint32_t i = 0u; i < (page_length / 2u); i++) {
                sim->memory[start + i] = sim->flash.geometry.erased_value;
            }
            sim->erase_counts[page]++;
            return FWU_ERR_FLASH_POWER_LOSS;
        }
        for(uint32_t i = 0u; i < page_length; i++) {
            sim->memory[start + i] = sim->flash.geometry.erased_value;
        }
        sim->erase_counts[page]++;
    }
    return FWU_OK;
}

static fwu_return_e sim_write(void *context, uint32_t offset, uint32_t length, const fwu_byte_t *data)
{
    fwu_flash_sim_t *sim = context;
    fwu_return_e status;
    uint32_t granule;

    if((sim == NULL) || (data == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    status = sim_precheck(sim, length);
    if(status != FWU_OK) {
        return status;
    }
    if(!sim_range_ok(sim, offset, length)) {
        return FWU_ERR_OUT_OF_RANGE;
    }
    granule = sim->flash.geometry.write_granule;
    if(((offset % granule) != 0u) || ((length % granule) != 0u)) {
        return FWU_ERR_WRITE_ALIGNMENT;
    }
    /* The whole target must be erased; otherwise NOTHING changes. */
    for(uint32_t i = 0u; i < length; i++) {
        if(sim->memory[offset + i] != sim->flash.geometry.erased_value) {
            return FWU_ERR_FLASH_NOT_ERASED;
        }
    }

    for(uint32_t done = 0u; done < length; done += granule) {
        const bool allowed = sim_operation_allowed(sim);
        /* If interrupted, only the first half of the granule is written. */
        const uint32_t count = allowed ? granule : (granule / 2u);

        for(uint32_t i = 0u; i < count; i++) {
            sim->memory[offset + done + i] = (fwu_byte_t)(data[done + i] & FWU_BYTE_MASK);
        }
        if(!allowed) {
            return FWU_ERR_FLASH_POWER_LOSS;
        }
    }
    return FWU_OK;
}

static const fwu_flash_callback_t g_sim_callbacks = {
    .flash_erase = sim_erase,
    .flash_write = sim_write,
    .flash_read  = sim_read
};

/* ------------------------------------------------------------------ public API */

fwu_return_e fwu_flash_sim_init_raw(fwu_flash_sim_t *sim, fwu_byte_t *memory,
                                    uint32_t *erase_counts, uint32_t page_count,
                                    uint32_t page_length, uint32_t write_granule)
{
    if((sim == NULL) || (memory == NULL) || (erase_counts == NULL) || (page_count == 0u) ||
       (page_length == 0u) || (write_granule == 0u)) {
        return FWU_ERR_INVALID_ARG;
    }
    if(page_count > (UINT32_MAX / page_length)) {
        return FWU_ERR_FLASH_GEOMETRY;
    }

    sim->flash.context                = sim;
    sim->flash.callback               = &g_sim_callbacks;
    sim->flash.geometry.flash_length  = page_count * page_length;
    sim->flash.geometry.page_length   = page_length;
    sim->flash.geometry.write_granule = write_granule;
    sim->flash.geometry.erased_value  = (fwu_byte_t)0xFFu;
    sim->memory                       = memory;
    sim->erase_counts                 = erase_counts;
    sim->page_count                   = page_count;
    sim->operations_done              = 0u;
    sim->cut_after                    = 0u;
    sim->cut_armed                    = false;
    sim->powered                      = true;

    for(uint32_t i = 0u; i < sim->flash.geometry.flash_length; i++) {
        sim->memory[i] = sim->flash.geometry.erased_value;
    }
    for(uint32_t p = 0u; p < page_count; p++) {
        sim->erase_counts[p] = 0u;
    }
    /* Port rule: validate after filling in the object, before using it. */
    return fwu_flash_validate(&sim->flash);
}

void fwu_flash_sim_cut_power_after(fwu_flash_sim_t *sim, uint32_t operations)
{
    if(sim != NULL) {
        sim->cut_after = operations;
        sim->cut_armed = true;
    }
}

void fwu_flash_sim_reset(fwu_flash_sim_t *sim)
{
    if(sim != NULL) {
        sim->powered         = true;
        sim->cut_armed       = false;
        sim->cut_after       = 0u;
        sim->operations_done = 0u;
    }
}

uint32_t fwu_flash_sim_operations(const fwu_flash_sim_t *sim)
{
    return (sim != NULL) ? sim->operations_done : 0u;
}

bool fwu_flash_sim_is_powered(const fwu_flash_sim_t *sim)
{
    return (sim != NULL) && sim->powered;
}

fwu_return_e fwu_flash_sim_get_erase_count(const fwu_flash_sim_t *sim, uint32_t page,
                                           uint32_t *count)
{
    if((sim == NULL) || (count == NULL) || (page >= sim->page_count)) {
        return FWU_ERR_INVALID_ARG;
    }
    *count = sim->erase_counts[page];
    return FWU_OK;
}
