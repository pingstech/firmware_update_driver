/**
 * @file    fwu_boot.c
 * @brief   Bootloader core implementation (see fwu_boot.h for the order, policy rules and contract).
 */

#include <stddef.h>
#include "fwu_boot.h"
#include "fwu_bytes.h"
#include "fwu_crc.h"

#define BOOT_FNV_OFFSET     2166136261u
#define BOOT_FNV_PRIME      16777619u

/* ------------------------------------------------------------------ helpers */

static bool boot_verify_valid(fwu_boot_verify_e verify)
{
    return (verify == FWU_BOOT_VERIFY_NONE) || (verify == FWU_BOOT_VERIFY_HASH) || (verify == FWU_BOOT_VERIFY_SIGNED);
}

static bool boot_mode_valid(fwu_activate_mode_e mode)
{
    return (mode == FWU_ACTIVATE_MODE_COPY) || (mode == FWU_ACTIVATE_MODE_SWAP);
}

/** Policy rules and mandatory fields. INVALID_ARG: a bootloader without protection must not be buildable by accident. */
static fwu_return_e boot_validate(const fwu_boot_config_t *c)
{
    if((c->flash == NULL) || (!boot_verify_valid(c->policy.verify)) || (!boot_mode_valid(c->policy.mode))) {
        return FWU_ERR_INVALID_ARG;
    }
    if((c->activate_work == NULL) || (c->activate_work_length == 0u)) {
        return FWU_ERR_INVALID_ARG;
    }
    if(c->policy.verify != FWU_BOOT_VERIFY_NONE) {
        if((c->crypto == NULL) || (c->verify_work == NULL) || (c->verify_work_length == 0u)) {
            return FWU_ERR_INVALID_ARG;
        }
    }
    if(c->policy.verify == FWU_BOOT_VERIFY_SIGNED) {
        if((c->keys == NULL) || (c->key_count == 0u)) {
            return FWU_ERR_INVALID_ARG;
        }
    }
    if(c->policy.anti_rollback) {
        if((c->policy.mode != FWU_ACTIVATE_MODE_SWAP) || (c->policy.verify != FWU_BOOT_VERIFY_SIGNED)) {
            return FWU_ERR_INVALID_ARG;     /* no confirmation in COPY (the counter would not rise); the counter cannot be raised from an unsigned manifest */
        }
    }
    return FWU_OK;
}

/** Wires up counters, verification and activation. On failure `reason` tells the step. */
static fwu_return_e boot_init_stage(fwu_boot_t *boot, const fwu_boot_config_t *config, fwu_boot_reason_e *reason)
{
    const bool verifying = (config->policy.verify != FWU_BOOT_VERIFY_NONE);
    const fwu_boot_layout_t *l = &config->layout;
    fwu_return_e status;
    uint32_t min_image = 0u;
    uint16_t min_key = 0u;

    boot->initialized = false;
    boot->confirm_retry = false;
    *reason = FWU_BOOT_REASON_CONFIG;
    status = boot_validate(config);
    if(status != FWU_OK) {
        return status;
    }
    boot->config = *config;

    if(config->policy.anti_rollback) {
        *reason = FWU_BOOT_REASON_COUNTERS;
        status = fwu_counters_init(&boot->counters, config->flash, l->counter_a_offset, l->counter_b_offset, l->counter_length);
        if(status != FWU_OK) {
            return status;
        }
        status = fwu_counters_get(&boot->counters, &min_image, &min_key);
        if(status != FWU_OK) {
            return status;
        }
    }

    boot->verify.crypto = config->crypto;
    boot->verify.manifest_offset = l->staging_manifest_offset;
    boot->verify.image_offset = l->staging_image_offset;
    boot->verify.max_image_size = l->image_region_length;
    boot->verify.product_id = config->product_id;
    boot->verify.min_image_version = min_image;
    boot->verify.min_key_version = min_key;
    boot->verify.keys = config->keys;
    boot->verify.key_count = config->key_count;
    boot->verify.require_signature = (config->policy.verify == FWU_BOOT_VERIFY_SIGNED);
    boot->verify.work = config->verify_work;
    boot->verify.work_length = config->verify_work_length;

    boot->confirm.counters = &boot->counters;
    boot->confirm.verify = &boot->verify;
    boot->confirm.flash = config->flash;

    boot->activate_config.flash = config->flash;
    boot->activate_config.mode = config->policy.mode;
    boot->activate_config.app_offset = l->app_offset;
    boot->activate_config.staging_offset = l->staging_image_offset;
    boot->activate_config.region_length = l->image_region_length;
    boot->activate_config.journal_a_offset = l->journal_a_offset;
    boot->activate_config.journal_b_offset = l->journal_b_offset;
    boot->activate_config.journal_length = l->journal_length;
    boot->activate_config.scratch_offset = l->scratch_offset;
    boot->activate_config.scratch_pages = l->scratch_pages;
    boot->activate_config.work = config->activate_work;
    boot->activate_config.work_length = config->activate_work_length;
    boot->activate_config.max_trial_boots = config->max_trial_boots;
    boot->activate_config.require_verify = verifying;
    boot->activate_config.verify = verifying ? fwu_verify_activation_hook : NULL;
    boot->activate_config.verify_context = verifying ? (void *)&boot->verify : NULL;
    boot->activate_config.on_confirmed = config->policy.anti_rollback ? fwu_counters_on_confirmed : NULL;
    boot->activate_config.on_confirmed_context = config->policy.anti_rollback ? (void *)&boot->confirm : NULL;

    *reason = FWU_BOOT_REASON_ACTIVATE_INIT;
    status = fwu_activate_init(&boot->activate, &boot->activate_config);
    if(status != FWU_OK) {
        return status;
    }
    boot->initialized = true;
    *reason = FWU_BOOT_REASON_NONE;
    return FWU_OK;
}

/* ------------------------------------------------------------------ public API */

fwu_return_e fwu_boot_init(fwu_boot_t *boot, const fwu_boot_config_t *config)
{
    fwu_boot_reason_e reason;

    if((boot == NULL) || (config == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    return boot_init_stage(boot, config, &reason);
}

fwu_boot_decision_e fwu_boot_start(fwu_boot_t *boot, const fwu_boot_config_t *config, fwu_boot_result_t *result)
{
    fwu_boot_result_t local;
    fwu_boot_result_t *r = (result != NULL) ? result : &local;
    fwu_boot_reason_e reason = FWU_BOOT_REASON_CONFIG;
    fwu_activate_outcome_e outcome = FWU_ACT_OUTCOME_NONE;
    fwu_return_e status;

    r->decision = FWU_BOOT_STAY;
    r->reason = FWU_BOOT_REASON_CONFIG;
    r->status = FWU_ERR_INVALID_ARG;
    r->outcome = FWU_ACT_OUTCOME_NONE;
    r->phase = FWU_ACT_PHASE_IDLE;
    r->trial = false;
    if((boot == NULL) || (config == NULL)) {
        return FWU_BOOT_STAY;
    }

    status = boot_init_stage(boot, config, &reason);
    if(status != FWU_OK) {
        r->reason = reason;
        r->status = status;
        return FWU_BOOT_STAY;
    }

    status = fwu_activate_run(&boot->activate, &outcome);
    if(status != FWU_OK) {
        r->reason = FWU_BOOT_REASON_ACTIVATE_RUN;
        r->status = status;
        return FWU_BOOT_STAY;                       /* the application may be partial: NEVER jump */
    }
    r->outcome = outcome;
    status = fwu_activate_get_phase(&boot->activate, &r->phase);
    if(status != FWU_OK) {
        r->reason = FWU_BOOT_REASON_ACTIVATE_RUN;
        r->status = status;
        return FWU_BOOT_STAY;
    }
    r->trial = (r->phase == FWU_ACT_PHASE_TRIAL);

    if((config->app_valid != NULL) && (!config->app_valid(config->context))) {
        r->reason = FWU_BOOT_REASON_NO_APP;
        r->status = FWU_OK;
        return FWU_BOOT_STAY;
    }
    if(r->trial && (config->watchdog_arm != NULL)) {
        config->watchdog_arm(config->context);      /* if the new application hangs the watchdog resets, rollback at the next boot */
    }
    r->decision = FWU_BOOT_JUMP_APP;
    r->reason = FWU_BOOT_REASON_NONE;
    r->status = FWU_OK;
    return FWU_BOOT_JUMP_APP;
}

fwu_return_e fwu_boot_app_finish_trial(fwu_boot_t *boot, bool healthy)
{
    fwu_activate_phase_e phase = FWU_ACT_PHASE_IDLE;
    fwu_return_e status;

    if((boot == NULL) || (!boot->initialized)) {
        return FWU_ERR_INVALID_ARG;
    }
    status = fwu_activate_get_phase(&boot->activate, &phase);
    if(status != FWU_OK) {
        return status;
    }
    if(phase == FWU_ACT_PHASE_TRIAL) {
        if(!healthy) {
            return fwu_activate_request_revert(&boot->activate);
        }
        status = fwu_activate_confirm(&boot->activate);
        boot->confirm_retry = (status != FWU_OK);   /* if CONFIRMED was written the hook can be retried in this boot */
        return status;
    }
    if(phase == FWU_ACT_PHASE_IDLE) {
        if(healthy && boot->confirm_retry) {
            status = fwu_activate_confirm(&boot->activate);     /* IDLE + CONFIRMED: only calls the hook again */
            boot->confirm_retry = (status != FWU_OK);
            return status;
        }
        return FWU_OK;                              /* no trial (or already confirmed) */
    }
    return FWU_ERR_BAD_STATE;                       /* ACTIVATING / REVERTING: the bootloader should have finished */
}

fwu_activate_t *fwu_boot_activate(fwu_boot_t *boot)
{
    return ((boot != NULL) && boot->initialized) ? &boot->activate : NULL;
}

const fwu_verify_config_t *fwu_boot_verify_config(const fwu_boot_t *boot)
{
    return ((boot != NULL) && boot->initialized) ? &boot->verify : NULL;
}

/* ------------------------------------------------------------------ contract fingerprint */

static uint32_t boot_fnv_u32(uint32_t hash, uint32_t value)
{
    for(uint32_t shift = 24u;; shift -= 8u) {
        hash ^= (value >> shift) & 0xFFu;
        hash *= BOOT_FNV_PRIME;
        if(shift == 0u) {
            break;
        }
    }
    return hash;
}

fwu_return_e fwu_boot_fingerprint(const fwu_boot_config_t *config, uint32_t *fingerprint)
{
    const fwu_boot_layout_t *l;
    uint32_t h;

    if((config == NULL) || (config->flash == NULL) || (fingerprint == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    l = &config->layout;
    h = BOOT_FNV_OFFSET;
    h = boot_fnv_u32(h, FWU_BOOT_CONTRACT_VERSION);
    h = boot_fnv_u32(h, (uint32_t)config->policy.verify);
    h = boot_fnv_u32(h, (uint32_t)config->policy.mode);
    h = boot_fnv_u32(h, config->policy.anti_rollback ? 1u : 0u);
    h = boot_fnv_u32(h, config->product_id);
    h = boot_fnv_u32(h, config->max_trial_boots);
    h = boot_fnv_u32(h, config->flash->geometry.page_length);
    h = boot_fnv_u32(h, config->flash->geometry.write_granule);
    h = boot_fnv_u32(h, config->flash->geometry.flash_length);
    h = boot_fnv_u32(h, l->app_offset);
    h = boot_fnv_u32(h, l->staging_manifest_offset);
    h = boot_fnv_u32(h, l->staging_image_offset);
    h = boot_fnv_u32(h, l->image_region_length);
    h = boot_fnv_u32(h, l->journal_a_offset);
    h = boot_fnv_u32(h, l->journal_b_offset);
    h = boot_fnv_u32(h, l->journal_length);
    h = boot_fnv_u32(h, l->scratch_offset);
    h = boot_fnv_u32(h, l->scratch_pages);
    h = boot_fnv_u32(h, l->counter_a_offset);
    h = boot_fnv_u32(h, l->counter_b_offset);
    h = boot_fnv_u32(h, l->counter_length);
    *fingerprint = h;
    return FWU_OK;
}

fwu_return_e fwu_boot_info_encode(const fwu_boot_config_t *config, fwu_byte_t *out, size_t out_capacity, size_t *out_length)
{
    uint32_t fingerprint = 0u;
    fwu_return_e status;
    uint16_t crc;

    if((config == NULL) || (out == NULL) || (out_length == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    if(out_capacity < FWU_BOOT_INFO_SIZE) {
        return FWU_ERR_BUFFER_TOO_SMALL;
    }
    status = fwu_boot_fingerprint(config, &fingerprint);
    if(status != FWU_OK) {
        return status;
    }
    out[0] = (fwu_byte_t)0x46u;                     /* 'F' */
    out[1] = (fwu_byte_t)0x57u;                     /* 'W' */
    out[2] = (fwu_byte_t)0x42u;                     /* 'B' */
    out[3] = (fwu_byte_t)0x49u;                     /* 'I' */
    fwu_write_u16_big_endian(&out[4], (uint16_t)FWU_BOOT_INFO_VERSION);
    fwu_write_u32_big_endian(&out[6], fingerprint);
    crc = fwu_crc16_calculate_w_start_value(FWU_CRC16_INIT, out, 10u);
    fwu_write_u16_big_endian(&out[10], crc);
    *out_length = FWU_BOOT_INFO_SIZE;
    return FWU_OK;
}

fwu_return_e fwu_boot_info_check(const fwu_boot_config_t *config, const fwu_byte_t *info, size_t length)
{
    fwu_byte_t clean[FWU_BOOT_INFO_SIZE];
    uint32_t expected = 0u;
    fwu_return_e status;

    if((config == NULL) || (info == NULL)) {
        return FWU_ERR_INVALID_ARG;
    }
    if(length < FWU_BOOT_INFO_SIZE) {
        return FWU_ERR_NOT_ENOUGH_DATA;
    }
    for(uint32_t i = 0u; i < FWU_BOOT_INFO_SIZE; i++) {
        clean[i] = (fwu_byte_t)(info[i] & FWU_BYTE_MASK);
    }
    if((clean[0] != 0x46u) || (clean[1] != 0x57u) || (clean[2] != 0x42u) || (clean[3] != 0x49u)) {
        return FWU_ERR_CONTRACT_MISMATCH;
    }
    if(fwu_read_u16_big_endian(&clean[10]) != fwu_crc16_calculate_w_start_value(FWU_CRC16_INIT, clean, 10u)) {
        return FWU_ERR_CONTRACT_MISMATCH;
    }
    if(fwu_read_u16_big_endian(&clean[4]) != (uint16_t)FWU_BOOT_INFO_VERSION) {
        return FWU_ERR_CONTRACT_MISMATCH;
    }
    status = fwu_boot_fingerprint(config, &expected);
    if(status != FWU_OK) {
        return status;
    }
    if(fwu_read_u32_big_endian(&clean[6]) != expected) {
        return FWU_ERR_CONTRACT_MISMATCH;
    }
    return FWU_OK;
}
