/**
 * @file    hal_stub.c  (PC TEST STUB, not vendor code, never built for a target)
 * @brief   PC imitation of a small subset of the ST HAL flash functions (for behaviour and CAVEATS see stm32g0xx_hal.h).
 */

#include "stm32g0xx_hal.h"

static uint8_t g_memory[HAL_STUB_FLASH_MAX];
static hal_stub_stats_t g_stats;
static uint32_t g_flash_size = HAL_STUB_FLASH_MAX;
static bool g_dual_bank = true;
static bool g_locked = true;
static bool g_powered = true;
static bool g_fail_unlock = false;
static bool g_fail_erase = false;
static bool g_fail_program = false;
static bool g_cut_armed = false;
static uint32_t g_cut_after = 0u;
static uint32_t g_operations = 0u;
static bool g_flags[8];

/* ------------------------------------------------------------------ control API */

void hal_stub_reset(void)
{
    for(uint32_t i = 0u; i < HAL_STUB_FLASH_MAX; i++) {
        g_memory[i] = 0xFFu;
    }
    g_stats.unlock_calls = 0u;
    g_stats.lock_calls = 0u;
    g_stats.erase_calls = 0u;
    g_stats.program_calls = 0u;
    g_stats.optverr_clears = 0u;
    g_stats.last_program_address = 0u;
    g_stats.last_program_data = 0u;
    g_stats.erase_log_count = 0u;
    g_flash_size = HAL_STUB_FLASH_MAX;
    g_dual_bank = true;
    g_locked = true;
    g_powered = true;
    g_fail_unlock = false;
    g_fail_erase = false;
    g_fail_program = false;
    g_cut_armed = false;
    g_cut_after = 0u;
    g_operations = 0u;
    for(uint32_t i = 0u; i < 8u; i++) {
        g_flags[i] = false;
    }
}

void hal_stub_set_chip(uint32_t flash_size, bool dual_bank)
{
    g_flash_size = flash_size;
    g_dual_bank = dual_bank;
}

uint8_t *hal_stub_memory(void) { return g_memory; }
const hal_stub_stats_t *hal_stub_stats(void) { return &g_stats; }
bool hal_stub_is_locked(void) { return g_locked; }
void hal_stub_fail_unlock(bool fail) { g_fail_unlock = fail; }
void hal_stub_fail_erase(bool fail) { g_fail_erase = fail; }
void hal_stub_fail_program(bool fail) { g_fail_program = fail; }
void hal_stub_set_flag(uint32_t flag) { g_flags[flag] = true; }
bool hal_stub_powered(void) { return g_powered; }
uint32_t hal_stub_operations(void) { return g_operations; }

void hal_stub_cut_power_after(uint32_t operations)
{
    g_cut_armed = true;
    g_cut_after = operations;
    g_operations = 0u;
}

void hal_stub_power_on(void)
{
    g_powered = true;
    g_cut_armed = false;
    g_locked = true;                    /* flash is locked after a restart */
    g_operations = 0u;
}

/* ------------------------------------------------------------------ for the HAL macros */

uint32_t hal_stub_flash_size(void) { return g_flash_size; }
uint32_t hal_stub_bank_size(void) { return g_dual_bank ? (g_flash_size >> 1) : g_flash_size; }

bool hal_stub_get_flag(uint32_t flag)
{
    return g_flags[flag];
}

void hal_stub_clear_flag(uint32_t flag)
{
    if(flag == FLASH_FLAG_OPTVERR) {
        g_stats.optverr_clears++;
    }
    g_flags[flag] = false;
}

/* ------------------------------------------------------------------ HAL functions */

HAL_StatusTypeDef HAL_FLASH_Unlock(void)
{
    g_stats.unlock_calls++;
    if(!g_powered || g_fail_unlock) {
        return HAL_ERROR;
    }
    g_locked = false;
    return HAL_OK;
}

HAL_StatusTypeDef HAL_FLASH_Lock(void)
{
    g_stats.lock_calls++;
    g_locked = true;
    return HAL_OK;
}

/** Will the power cut happen in this operation? (n operations completed, the next one is half done). */
static bool cut_now(void)
{
    return g_cut_armed && (g_operations >= g_cut_after);
}

HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *pEraseInit, uint32_t *PageError)
{
    const uint32_t bank_pages = hal_stub_bank_size() / FLASH_PAGE_SIZE;

    g_stats.erase_calls++;
    if(g_stats.erase_log_count < HAL_STUB_ERASE_LOG_MAX) {
        hal_stub_erase_record_t *rec = &g_stats.erase_log[g_stats.erase_log_count];
        rec->banks = pEraseInit->Banks;
        rec->page = pEraseInit->Page;
        rec->nb_pages = pEraseInit->NbPages;
        g_stats.erase_log_count++;
    }
    *PageError = 0xFFFFFFFFU;
    if(!g_powered || g_locked || g_fail_erase) {
        *PageError = pEraseInit->Page;
        return HAL_ERROR;
    }
    if((pEraseInit->TypeErase != FLASH_TYPEERASE_PAGES) ||
       ((pEraseInit->Banks != FLASH_BANK_1) && !((pEraseInit->Banks == FLASH_BANK_2) && g_dual_bank)) ||
       (pEraseInit->NbPages < 1u) || ((pEraseInit->Page + pEraseInit->NbPages) > bank_pages)) {
        *PageError = pEraseInit->Page;
        return HAL_ERROR;
    }

    for(uint32_t index = pEraseInit->Page; index < (pEraseInit->Page + pEraseInit->NbPages); index++) {
        const uint32_t bank_base = (pEraseInit->Banks == FLASH_BANK_2) ? hal_stub_bank_size() : 0u;
        const uint32_t start = bank_base + (index * FLASH_PAGE_SIZE);
        const bool cut = cut_now();
        const uint32_t count = cut ? (FLASH_PAGE_SIZE / 2u) : FLASH_PAGE_SIZE;

        for(uint32_t i = 0u; i < count; i++) {
            g_memory[start + i] = 0xFFu;
        }
        if(cut) {
            g_powered = false;
            *PageError = index;
            return HAL_ERROR;
        }
        g_operations++;
    }
    return HAL_OK;
}

HAL_StatusTypeDef HAL_FLASH_Program(uint32_t TypeProgram, uint32_t Address, uint64_t Data)
{
    const uint32_t flash_base = (uint32_t)FLASH_BASE;
    uint32_t index;

    g_stats.program_calls++;
    g_stats.last_program_address = Address;
    g_stats.last_program_data = Data;
    if(!g_powered || g_locked || g_fail_program || (TypeProgram != FLASH_TYPEPROGRAM_DOUBLEWORD)) {
        return HAL_ERROR;
    }
    if((Address < flash_base) || ((Address - flash_base) > (g_flash_size - 8u)) || (((Address - flash_base) & 7u) != 0u)) {
        return HAL_ERROR;
    }
    index = Address - flash_base;
    for(uint32_t i = 0u; i < 8u; i++) {
        if(g_memory[index + i] != 0xFFu) {
            return HAL_ERROR;           /* double word not erased: a programming error flag on real hardware */
        }
    }
    {
        const bool cut = cut_now();
        const uint32_t count = cut ? 4u : 8u;

        for(uint32_t i = 0u; i < count; i++) {
            g_memory[index + i] = (uint8_t)((Data >> (8u * i)) & 0xFFu);        /* little-endian */
        }
        if(cut) {
            g_powered = false;
            return HAL_ERROR;
        }
        g_operations++;
    }
    return HAL_OK;
}
